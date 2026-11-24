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
        for m in doc["mates"].as_array().into_iter().flatten() {
            let name = m["name"].as_str().unwrap_or("Mate").to_string();
            let kind = m["type"].as_str().unwrap_or("").to_string();
            let r = if kind == "hinge" { hinge(s, m, asm, docs, placed) } else { one(s, m, asm, docs, placed) };
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
