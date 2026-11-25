//! Mates (FORMAT-SPEC §4 assemblies, §5 references) as SolidCraft assembly constraints.
//!
//! The parts are already where the file places them. Each mate becomes the SolidCraft constraint of the same
//! rule (mate/flush for coincident planes, concentric, distance, angle, parallel, perpendicular, tangent, a
//! rigid joint for lock), made between the two placed parts. A mate's references are geometric: a face, edge or
//! vertex descriptor in its part's own frame (turned into a world point through the placements above it), or
//! one of the part's or the assembly's origin planes.
//!
//! Which way two faces of a mate face, and on which side a distance lies, is the source system's convention.
//! SolidCraft makes a constraint "where the parts are", so the importer makes it, checks that neither part
//! moved, and otherwise undoes it and tries the other facing (and, for a distance, the other side). A mate that
//! cannot be made without moving a part is left out and reported; the assembly check after the mates confirms
//! nothing moved.

use std::collections::HashMap;

use anyhow::{anyhow, bail, Result};
use serde_json::{json, Value};

use super::features::pick;
use super::num;
use crate::report::{Report, Status};
use crate::sc::{self, Session};

/// Where an assembly document's components were placed.
pub struct Placed {
    /// (assembly doc id, component entry id) -> SolidCraft occurrence id
    pub occ: HashMap<(String, String), Value>,
    /// assembly doc id -> occurrence ids from the root down to its first placement
    pub path: HashMap<String, Vec<Value>>,
    /// assembly doc id -> its first placement's transform in the root assembly's frame (row-major 4x4)
    pub world: HashMap<String, [f64; 16]>,
    /// part doc id -> payload feature id -> the SolidCraft name of what it built (reference planes)
    pub features: HashMap<String, HashMap<String, String>>,
}

type M4 = [f64; 16];

pub fn identity() -> M4 {
    [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
}

pub fn mul(a: &M4, b: &M4) -> M4 {
    let mut r = [0.0; 16];
    for i in 0..4 {
        for j in 0..4 {
            r[i * 4 + j] = (0..4).map(|k| a[i * 4 + k] * b[k * 4 + j]).sum();
        }
    }
    r
}

pub fn m4(v: &Value) -> M4 {
    let t: Vec<f64> = v.as_array().into_iter().flatten().filter_map(Value::as_f64).collect();
    if t.len() == 16 {
        let mut r = [0.0; 16];
        r.copy_from_slice(&t);
        r
    } else {
        identity()
    }
}

fn apply(m: &M4, p: [f64; 3]) -> [f64; 3] {
    [
        m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
        m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
        m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11],
    ]
}

/// One side of a mate as SolidCraft takes it, whether that part is fixed, and what kind of geometry it is.
struct Side {
    origin: Value,
    occurrence: Value,
    fixed: bool,
    /// plane, circle (a round face or edge: an axis), line, point
    geo: &'static str,
}

/// Resolves a mate reference `{component, path?, ref}` (or the assembly's own `{ref}`) in assembly doc `asm`.
fn side(r: &Value, asm: &str, docs: &HashMap<String, Value>, placed: &Placed) -> Result<Side> {
    let base_path = placed.path.get(asm).cloned().ok_or_else(|| anyhow!("its assembly was not placed"))?;
    let mut world = *placed.world.get(asm).unwrap_or(&identity());
    let mut path = base_path.clone();
    let mut fixed = false;
    let mut doc_id = asm.to_string();
    if let Some(c) = r["component"].as_str() {
        // the component, then down its path through sub-assemblies to the part
        let mut chain = vec![c.to_string()];
        chain.extend(r["path"].as_array().into_iter().flatten().filter_map(|x| x.as_str().map(String::from)));
        for cid in chain {
            let d = docs.get(&doc_id).ok_or_else(|| anyhow!("document missing"))?;
            let entry = d["components"]
                .as_array()
                .and_then(|a| a.iter().find(|e| e["id"] == cid.as_str()))
                .ok_or_else(|| anyhow!("component {cid} not found"))?;
            let occ = placed.occ.get(&(doc_id.clone(), cid.clone())).ok_or_else(|| anyhow!("component {cid} not placed"))?;
            path.push(occ.clone());
            world = mul(&world, &m4(&entry["transform"]));
            fixed = entry["fixed"].as_bool() == Some(true);
            doc_id = entry["document"].as_str().unwrap_or("").to_string();
        }
    }
    let occurrence = path.last().cloned().ok_or_else(|| anyhow!("no placement"))?;
    let rf = &r["ref"];
    let mut o = json!({"occurrence": occurrence, "path": path});
    let geo: &'static str;
    match rf["kind"].as_str() {
        Some("feature") => {
            // a reference plane the part has: by the name it was built under
            let fid = rf["id"].as_str().unwrap_or("");
            let name = placed
                .features
                .get(&doc_id)
                .and_then(|m| m.get(fid))
                .ok_or_else(|| anyhow!("its reference feature was not built here"))?;
            o["plane"] = json!(name);
            geo = "plane";
        }
        Some("originPlane") => {
            let p = match rf["plane"].as_str() {
                Some("front") => "XY",
                Some("top") => "XZ",
                Some("right") => "YZ",
                other => bail!("origin plane {:?}", other),
            };
            o["plane"] = json!(p);
            geo = "plane";
        }
        Some("originAxis") => {
            let a = rf["axis"].as_str().unwrap_or("").to_uppercase();
            o["axis"] = json!(a);
            geo = "circle";
        }
        Some("face") | Some("edge") | Some("vertex") => {
            let p = pick(rf).ok_or_else(|| anyhow!("its reference has no position"))?;
            let w = apply(&world, p);
            let ty = rf["geom"]["type"].as_str().unwrap_or("");
            let (key, g) = match (rf["kind"].as_str(), ty) {
                (Some("face"), "plane") => ("face", "plane"),
                (Some("face"), "cylinder" | "cone") => ("circle", "circle"),
                (Some("face"), _) => ("face", "plane"),
                (Some("edge"), "circle" | "arc") => ("circle", "circle"),
                (Some("edge"), _) => ("edge", "line"),
                _ => ("point", "point"),
            };
            o[key] = json!(w);
            geo = g;
        }
        other => bail!("a {:?} reference is not carried by this version", other),
    }
    Ok(Side { origin: o, occurrence, fixed, geo })
}

/// The occurrences' placements now (component.list), to tell whether a constraint moved anything.
fn placements(s: &mut Session) -> Result<HashMap<String, Vec<f64>>> {
    let list = s.exec("component.list", json!({"compact": true}))?;
    let mut out = HashMap::new();
    for c in list["components"].as_array().into_iter().flatten() {
        for o in c["occurrences"].as_array().into_iter().flatten() {
            let t: Vec<f64> = o["transform"]
                .as_array()
                .map(|rows| rows.iter().flat_map(|r| r.as_array().cloned().unwrap_or_default()).filter_map(|x| x.as_f64()).collect())
                .unwrap_or_else(|| identity().to_vec());
            out.insert(o["id"].to_string(), t);
        }
    }
    Ok(out)
}

fn moved(before: &HashMap<String, Vec<f64>>, after: &HashMap<String, Vec<f64>>) -> bool {
    before.iter().any(|(k, t)| match after.get(k) {
        Some(u) => t.iter().zip(u.iter()).enumerate().any(|(i, (a, b))| {
            // rotation entries absolute, translation in mm
            let tol = if i >= 12 { 1e-3 } else { 1e-6 };
            (a - b).abs() > tol
        }),
        None => false,
    })
}

/// Makes every mate of every assembly. Returns (made, left out).
pub fn make_all(
    s: &mut Session,
    assemblies: &[String],
    docs: &HashMap<String, Value>,
    placed: &Placed,
    report: &mut Report,
) -> Result<(usize, usize)> {
    let (mut made, mut left) = (0usize, 0usize);
    for asm in assemblies {
        let doc = &docs[asm];
        let asm_name = doc["name"].as_str().unwrap_or("assembly").to_string();
        // mates that couple motion go last: they link the joints the other mates make
        let mut ordered: Vec<&Value> = doc["mates"].as_array().into_iter().flatten().filter(|m| !is_coupling(m)).collect();
        ordered.extend(doc["mates"].as_array().into_iter().flatten().filter(|m| is_coupling(m)));
        for m in ordered {
            let name = m["name"].as_str().unwrap_or("Mate").to_string();
            let kind = m["type"].as_str().unwrap_or("").to_string();
            let r = if kind == "hinge" {
                hinge(s, m, asm, docs, placed)
            } else if is_coupling(m) {
                coupling(s, m, asm, docs, placed)
            } else if kind == "width" {
                width(s, m, asm, docs, placed)
            } else if kind == "slot" {
                slot(s, m, asm, docs, placed)
            } else {
                one(s, m, asm, docs, placed)
            };
            match r {
                Ok(note) => {
                    made += 1;
                    let st = if note.is_empty() { Status::Rebuilt } else { Status::Approximate };
                    report.add(&asm_name, &name, &format!("mate {kind}"), st, note);
                }
                Err(e) if sc::is_cancelled(&e) => return Err(e),
                Err(e) => {
                    left += 1;
                    report.add(&asm_name, &name, &format!("mate {kind}"), Status::Failed, format!("{e:#}"));
                }
            }
        }
    }
    Ok((made, left))
}

/// The distance between two parallel face descriptors' planes (in their own part's frame).
fn plane_gap(a: &Value, b: &Value) -> Option<f64> {
    let pa = crate::geom::v3(&a["ref"]["geom"]["params"]["point"])?;
    let pb = crate::geom::v3(&b["ref"]["geom"]["params"]["point"])?;
    let n = crate::geom::unit(crate::geom::v3(&a["ref"]["geom"]["params"]["normal"])?);
    Some(crate::geom::dot(crate::geom::sub(pb, pa), n).abs())
}

/// A width mate, centred (SolidCraft has none): the tab held at the gap a centred tab leaves, as a distance from
/// one outer face to the tab face beside it. Same placement and the same freedom left; the tab no longer re-centres
/// when the widths change.
fn width(s: &mut Session, m: &Value, asm: &str, docs: &HashMap<String, Value>, placed: &Placed) -> Result<String> {
    let refs = m["refs"].as_array().cloned().unwrap_or_default();
    let outer: Vec<&Value> = refs.iter().filter(|r| r["role"] == "width").collect();
    let tab: Vec<&Value> = refs.iter().filter(|r| r["role"] == "tab").collect();
    if outer.len() != 2 || tab.len() != 2 {
        bail!("a width mate with {} outer and {} tab faces", outer.len(), tab.len());
    }
    let c = m["params"]["constraint"].as_str().unwrap_or("centered");
    let (wo, wt) = (plane_gap(outer[0], outer[1]).ok_or_else(|| anyhow!("its outer faces are not planes"))?,
                    plane_gap(tab[0], tab[1]).ok_or_else(|| anyhow!("its tab faces are not planes"))?);
    let gap = match c {
        "centered" => (wo - wt) / 2.0,
        "distance" => num(&m["params"]["distance"]),
        "percent" => (wo - wt) * num(&m["params"]["percent"]) / 100.0,
        _ => bail!("a free width mate holds nothing a constraint can say"),
    };
    let mut last = None;
    for t in &tab {
        let mut d = m.clone();
        d["type"] = json!("distance");
        d["refs"] = json!([outer[0], t]);
        d["params"] = json!({"distance": gap});
        match one(s, &d, asm, docs, placed) {
            Ok(_) => return Ok(format!("as a {gap} mm distance from an outer face to the tab (SolidCraft has no width mate; the tab does not re-centre if the widths change)")),
            Err(e) if sc::is_cancelled(&e) => return Err(e),
            Err(e) => last = Some(e),
        }
    }
    Err(last.unwrap_or_else(|| anyhow!("not made")))
}

/// A slot mate: the riding part's round face tangent to the slot's face (it slides along the slot and turns in it,
/// as in the source); a centred slot wider than the pin keeps its distance instead.
fn slot(s: &mut Session, m: &Value, asm: &str, docs: &HashMap<String, Value>, placed: &Placed) -> Result<String> {
    // SolidCraft's pin-in-slot joint, at the mate's frame (FORMAT-SPEC mate `frame`: origin on the pin's axis, x along
    // the slot, z along the pin), both parts' joint origins there, so nothing moves
    if let (Some(fo), Some(fx), Some(fz)) = (crate::geom::v3(&m["frame"]["origin"]), crate::geom::v3(&m["frame"]["x"]), crate::geom::v3(&m["frame"]["z"])) {
        let refs = m["refs"].as_array().cloned().unwrap_or_default();
        if refs.len() >= 2 {
            let a = side(&refs[0], asm, docs, placed)?;
            let b = side(&refs[1], asm, docs, placed)?;
            let world = *placed.world.get(asm).unwrap_or(&identity());
            let o = apply(&world, fo);
            let dir = |d: [f64; 3]| crate::geom::unit(crate::geom::sub(apply(&world, crate::geom::add(fo, d)), o));
            let at = |side: &Side| {
                let mut j = json!({"occurrence": side.occurrence, "point": o, "z": dir(fz), "x": dir(fx)});
                if let Some(p) = side.origin.get("path") {
                    j["path"] = p.clone();
                }
                j
            };
            let name = m["name"].as_str().unwrap_or("Slot");
            let before = placements(s)?;
            if let Ok(res) = s.exec("joint.create", json!({"type": "pin_slot", "a": at(&a), "b": at(&b), "flip": true, "name": name})) {
                let after = placements(s)?;
                let conflict = res["conflicts"].as_array().map(|c| !c.is_empty()).unwrap_or(false);
                if !moved(&before, &after) && !conflict {
                    return Ok("as SolidCraft's pin-in-slot joint at the mate's frame".into());
                }
                if let Some(j) = res.get("joint").filter(|j| !j.is_null()) {
                    let _ = s.exec("joint.delete", json!({"joint": j}));
                }
            }
        }
    }
    let mut t = m.clone();
    t["type"] = json!("tangent");
    match one(s, &t, asm, docs, placed) {
        Ok(_) => Ok("as the pin tangent to the slot's face (SolidCraft has no slot mate)".into()),
        Err(e) if sc::is_cancelled(&e) => Err(e),
        Err(_) => {
            let mut c = m.clone();
            c["type"] = json!("coincident");
            one(s, &c, asm, docs, placed).map(|_| "as the pin on the slot's face (SolidCraft has no slot mate)".into())
        }
    }
}

fn is_coupling(m: &Value) -> bool {
    matches!(m["type"].as_str(), Some("gear" | "screw" | "rackPinion"))
}

/// A gear, screw or rack-and-pinion mate as SolidCraft's motion link between the joints the parts move on (made
/// by the other mates: a turning joint for a gear or pinion, a sliding one for a rack, a cylindrical one for a
/// screw).
fn coupling(s: &mut Session, m: &Value, asm: &str, docs: &HashMap<String, Value>, placed: &Placed) -> Result<String> {
    let kind = m["type"].as_str().unwrap_or("");
    let refs = m["refs"].as_array().cloned().unwrap_or_default();
    if refs.len() < 2 {
        bail!("it has {} references where a {kind} mate has 2", refs.len());
    }
    let a = side(&refs[0], asm, docs, placed)?;
    let b = side(&refs[1], asm, docs, placed)?;
    let jl = s.exec("joint.list", json!({}))?;
    let joints: Vec<Value> = jl["joints"].as_array().cloned().unwrap_or_default();
    let on = |occ: &Value, types: &[&str]| -> Option<Value> {
        joints.iter().find(|j| (j["a"] == *occ || j["b"] == *occ) && types.contains(&j["type"].as_str().unwrap_or("")) && j["suppressed"] != true)
            .map(|j| j["id"].clone())
    };
    let p = &m["params"];
    let reverse = p["reverse"].as_bool().unwrap_or(false);
    let name = m["name"].as_str().unwrap_or(kind);
    match kind {
        "gear" => {
            let ja = on(&a.occurrence, &["revolute", "cylindrical"]).ok_or_else(|| anyhow!("the first gear turns on no joint here"))?;
            let jb = on(&b.occurrence, &["revolute", "cylindrical"]).ok_or_else(|| anyhow!("the second gear turns on no joint here"))?;
            let ratio: Vec<f64> = p["ratio"].as_array().into_iter().flatten().filter_map(Value::as_f64).collect();
            let (ra, rb) = if ratio.len() == 2 && ratio[0] > 0.0 && ratio[1] > 0.0 { (ratio[0], ratio[1]) } else { (1.0, 1.0) };
            s.exec("joint.gear", json!({"a": ja, "b": jb, "ra": ra, "rb": rb, "internal": reverse, "name": name}))?;
            Ok(format!("a SolidCraft gear link, ratio {ra}:{rb}"))
        }
        "screw" => {
            // the cylindrical joint between the two parts: its turn drives its travel
            let j = joints.iter().find(|j| {
                ((j["a"] == a.occurrence && j["b"] == b.occurrence) || (j["a"] == b.occurrence && j["b"] == a.occurrence)) && j["type"] == "cylindrical"
            }).map(|j| j["id"].clone()).ok_or_else(|| anyhow!("the two parts share no cylindrical joint here"))?;
            let pitch = num(&p["pitch"]);
            let pitch = if p["pitchAsTurnsPerMm"].as_bool() == Some(true) && pitch > 0.0 { 1.0 / pitch } else { pitch };
            s.exec("joint.screw", json!({"joint": j, "pitch": format!("{pitch} mm"), "reverse": reverse, "name": name}))?;
            Ok(format!("a SolidCraft screw link, {pitch} mm per turn"))
        }
        _ => {
            // rack first, pinion second (either order in the file: the pinion is the side on a round face)
            let (rack, pinion) = if a.geo == "circle" && b.geo != "circle" { (b, a) } else { (a, b) };
            let jp = on(&pinion.occurrence, &["revolute", "cylindrical"]).ok_or_else(|| anyhow!("the pinion turns on no joint here"))?;
            let jr = on(&rack.occurrence, &["slider", "cylindrical", "planar"]).ok_or_else(|| anyhow!("the rack slides on no joint here"))?;
            let radius = num(&p["pitchDiameter"]) / 2.0;
            s.exec("joint.rack_pinion", json!({"a": jp, "b": jr, "radius": format!("{radius} mm"), "reverse": reverse, "name": name}))?;
            Ok(format!("a SolidCraft rack-and-pinion link, pitch radius {radius} mm"))
        }
    }
}

/// A hinge (round faces + a face pair, in that order): a concentric constraint on the round faces and a
/// coincident one on the faces, which together turn as the hinge does.
fn hinge(s: &mut Session, m: &Value, asm: &str, docs: &HashMap<String, Value>, placed: &Placed) -> Result<String> {
    let refs = m["refs"].as_array().cloned().unwrap_or_default();
    if refs.len() < 4 {
        bail!("it has {} references where a hinge has 4", refs.len());
    }
    let name = m["name"].as_str().unwrap_or("Hinge");
    let mut axis = m.clone();
    axis["type"] = json!("concentric");
    axis["refs"] = json!([refs[0], refs[1]]);
    axis["name"] = json!(format!("{name} (axis)"));
    let mut faces = m.clone();
    faces["type"] = json!("coincident");
    faces["refs"] = json!([refs[2], refs[3]]);
    faces["name"] = json!(format!("{name} (faces)"));
    one(s, &axis, asm, docs, placed)?;
    if let Err(e) = one(s, &faces, asm, docs, placed) {
        if sc::is_cancelled(&e) {
            return Err(e);
        }
        return Ok(format!("its axis is held; its faces were not ({e:#}): the part can also slide along it"));
    }
    Ok("made as a concentric and a coincident constraint".into())
}

/// One mate; Ok(note) when made.
fn one(s: &mut Session, m: &Value, asm: &str, docs: &HashMap<String, Value>, placed: &Placed) -> Result<String> {
    let kind = m["type"].as_str().unwrap_or("");
    let refs = m["refs"].as_array().cloned().unwrap_or_default();
    if refs.len() < 2 {
        bail!("it has {} references; this version carries mates between two", refs.len());
    }
    let (rule, value): (&str, Option<(&str, String)>) = match kind {
        "coincident" => ("mate", None),
        "concentric" => ("concentric", None),
        "hinge" => ("insert", None),
        "distance" => ("distance", Some(("offset", format!("{} mm", m["params"]["distance"].as_f64().unwrap_or(0.0))))),
        "angle" => ("angle", Some(("angle", format!("{} deg", m["params"]["angle"].as_f64().unwrap_or(0.0))))),
        "parallel" => ("parallel", None),
        "perpendicular" => ("perpendicular", None),
        "tangent" => ("tangent", None),
        "lock" => ("rigid", None),
        "gear" | "screw" | "rackPinion" | "cam" => bail!("a {kind} mate couples motion: the parts are placed, not coupled"),
        other => bail!("{other} mates are not carried by this version"),
    };
    let mut a = side(&refs[0], asm, docs, placed)?;
    let mut b = side(&refs[1], asm, docs, placed)?;
    if a.occurrence == b.occurrence {
        bail!("both references are on the same part");
    }
    // b is the side that may move: keep a fixed part on side a
    if b.fixed && !a.fixed {
        std::mem::swap(&mut a, &mut b);
    }
    let name = m["name"].as_str().unwrap_or("Mate");
    let suppressed = m["suppressed"].as_bool() == Some(true);

    // the variants to try, in order: as the file's alignment suggests first
    let aligned = m["alignment"] == "aligned";
    let pair = (a.geo, b.geo);
    let rules: Vec<&str> = if kind == "coincident" {
        match pair {
            // planes that face the same way are flush; facing each other, mated
            ("plane", "plane") => if aligned { vec!["flush", "mate"] } else { vec!["mate", "flush"] },
            ("plane", "point") | ("point", "plane") => vec!["rel:point_on_plane"],
            ("plane", "line") | ("line", "plane") => vec!["rel:line_on_plane"],
            ("plane", "circle") | ("circle", "plane") => vec!["rel:point_on_plane", "mate", "flush"],
            ("circle", "circle") => vec!["insert", "concentric"],
            ("point", "point") => vec!["rel:ball"],
            ("line", "line") | ("circle", "line") | ("line", "circle") => vec!["concentric", "rel:point_on_line"],
            _ => vec!["mate", "flush"],
        }
    } else {
        vec![rule]
    };
    let flips = [false, true];
    let signs: Vec<f64> = if kind == "distance" { vec![1.0, -1.0] } else { vec![1.0] };
    let before = placements(s)?;
    let mut last_err = String::new();
    for r in &rules {
        for flip in flips {
            for sign in &signs {
                // a relation (joint.create) may want its sides the other way round: the flip variant swaps them
                let (sa, sb) = if r.starts_with("rel:") && flip { (&b.origin, &a.origin) } else { (&a.origin, &b.origin) };
                let mut p = if *r == "rigid" {
                    json!({"type": "rigid", "a": a.origin, "b": b.origin, "name": name})
                } else if let Some(rel) = r.strip_prefix("rel:") {
                    json!({"type": rel, "a": sa, "b": sb, "name": name})
                } else {
                    json!({"type": r, "a": a.origin, "b": b.origin, "name": name, "flip": flip})
                };
                if let Some((k, v)) = &value {
                    let v = if *sign < 0.0 { format!("-{v}") } else { v.clone() };
                    p[*k] = json!(v);
                }
                let cmd = if *r == "rigid" || r.starts_with("rel:") { "joint.create" } else { "assembly.constraint.create" };
                match s.exec(cmd, p) {
                    Ok(res) => {
                        let after = placements(s)?;
                        // between two fixed parts nothing can move: a wrong facing shows as a conflict instead
                        let jid = res.get("joint").cloned().unwrap_or(Value::Null);
                        let conflict = res["conflicts"]
                            .as_array()
                            .into_iter()
                            .flatten()
                            .find(|c| (!jid.is_null() && c["joint"] == jid) || c["name"] == name)
                            .map(|c| c["why"].as_str().unwrap_or("conflicts with the other joints").to_string());
                        if conflict.is_none() && !moved(&before, &after) {
                            if suppressed {
                                if let Some(j) = res.get("joint").filter(|j| !j.is_null()) {
                                    let _ = s.exec("assembly.constraint.edit", json!({"constraint": j, "suppressed": true}));
                                }
                                return Ok("suppressed, as in the source".into());
                            }
                            let shown = r.strip_prefix("rel:").unwrap_or(r);
                            let note = if rules.len() > 1 && *r != rules[0] { format!("made as {shown}") } else { String::new() };
                            return Ok(note);
                        }
                        // it conflicts or moved a part: take it back (once) and try the next variant
                        s.tool("undo", json!({}))?;
                        last_err = match conflict {
                            Some(why) => format!("it conflicts with the other mates as the parts are placed ({why})"),
                            None => "every way of making it moves a part".into(),
                        };
                    }
                    Err(e) if sc::is_cancelled(&e) => return Err(e),
                    Err(e) => last_err = e.to_string().lines().next().unwrap_or("").to_string(),
                }
                if *r == "rigid" {
                    break;
                }
            }
            if *r == "rigid" {
                break;
            }
        }
    }
    bail!("not made: {last_err}")
}
