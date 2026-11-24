//! Features (FORMAT-SPEC §4 "Feature definitions"), mapped onto SolidCraft's commands.
//!
//! References to model faces and edges are geometric in NCX (spec §5). SolidCraft picks a face or an edge
//! by a world point and snaps to the nearest one, so a descriptor's middle sample is the pick. The part's
//! final fingerprint check catches a pick that found the wrong edge.

use anyhow::{anyhow, bail, Result};
use serde_json::{json, Value};

use super::{num, sketch, PartState};
use crate::geom::{self, v3};
use crate::sc::Session;

pub enum Built {
    Done,
    Approx(String),
    Skipped(String),
}

pub fn build(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let kind = f["type"].as_str().unwrap_or("");
    let def = &f["def"];
    if f["status"] == "opaque" {
        bail!("the source could not describe it ({})", f["reason"].as_str().unwrap_or("opaque"));
    }
    match kind {
        "sketch" => {
            let o = sketch::build(s, st, f)?;
            let id = f["id"].as_str().unwrap_or("").to_string();
            st.names.insert(id.clone(), o.built.name.clone());
            st.sketches.insert(id, o.built);
            let mut why: Vec<String> = o.missing;
            if let Some(l) = o.less_defined {
                why.push(l);
            }
            Ok(if why.is_empty() { Built::Done } else { Built::Approx(why.join("; ")) })
        }
        "extrude" | "cut" => extrude(s, st, f, kind == "cut"),
        "revolve" | "revolveCut" => revolve(s, st, f, kind == "revolveCut"),
        "fillet" => fillet(s, st, f),
        "chamfer" => chamfer(s, st, f),
        "plane" => plane(s, st, f),
        "flatPattern" => Ok(Built::Skipped("flat pattern: SolidCraft makes its own".into())),
        "weldment" => Ok(Built::Skipped("weldment settings: used by the members that follow".into())),
        "cosmeticThread" => Ok(Built::Skipped("cosmetic thread: annotation only, not carried".into())),
        other => {
            let _ = def;
            match super::features2::build(s, st, f)? {
                Some(b) => Ok(b),
                None => bail!("{other} is not rebuilt by this version"),
            }
        }
    }
}

/// The world point that picks a face, edge or vertex descriptor: its middle sample.
pub fn pick(r: &Value) -> Option<geom::V3> {
    let g = &r["geom"];
    if let Some(samples) = g["samples"].as_array() {
        if !samples.is_empty() {
            return v3(&samples[samples.len() / 2]);
        }
    }
    if let Some(p) = v3(&g["params"]["point"]) {
        return Some(p);
    }
    let b = g["bbox"].as_array()?;
    if b.len() == 6 {
        let f = |i: usize| b[i].as_f64().unwrap_or(0.0);
        return Some([(f(0) + f(3)) / 2.0, (f(1) + f(4)) / 2.0, (f(2) + f(5)) / 2.0]);
    }
    None
}

fn sketch_of<'a>(st: &'a PartState, r: &Value) -> Result<&'a sketch::Built> {
    let id = r["id"].as_str().ok_or_else(|| anyhow!("no profile sketch"))?;
    st.sketches.get(id).ok_or_else(|| anyhow!("its sketch was not built"))
}

/// A length for a feature field: an equation's expression where one drives it, else the number.
fn length(st: &PartState, fname: &str, field: &str, v: f64, unit: &str) -> String {
    for key in [format!("{fname}:{field}")] {
        if let Some(e) = st.equations.get(&key) {
            if let Some(x) = translate_expression(e, st) {
                return x;
            }
        }
    }
    format!("{v} {unit}")
}

fn total_volume(s: &mut Session) -> Result<f64> {
    let m = s.tool("measure", json!({}))?;
    Ok(m["total"]["volume_mm3"].as_f64().unwrap_or(0.0))
}

fn extrude(s: &mut Session, st: &mut PartState, f: &Value, cut: bool) -> Result<Built> {
    let def = &f["def"];
    let name = &st.unique_name(f["name"].as_str().unwrap_or("Extrude"));
    let fid = f["id"].as_str().unwrap_or("");
    let sk = sketch_of(st, &def["profile"])?.clone();
    let mut notes: Vec<String> = Vec::new();
    let mut p = json!({"sketch": sk.name, "name": name});

    // profiles: picked contours (by their segments) and regions (by a point inside)
    let mut profiles: Vec<Value> = Vec::new();
    let mut loops: Vec<Vec<String>> = Vec::new();
    for c in def["contours"].as_array().into_iter().flatten() {
        let curves: Vec<String> = c
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|id| id.as_str().and_then(|i| sk.ents.get(i)).map(|e| e.sc.clone()))
            .collect();
        if !curves.is_empty() {
            loops.push(curves);
        }
    }
    // NCX lists a contour's holes as contours of their own; SolidCraft's loop already leaves out the loops inside
    // it, and picking a hole as well would fill it: holes of another listed contour's region are not passed
    if loops.len() > 1 {
        if let Ok(info) = s.exec("sketch.inspect", json!({"sketch": sk.name})) {
            let set = |v: &Vec<String>| v.iter().cloned().collect::<std::collections::BTreeSet<String>>();
            let mut holes: Vec<std::collections::BTreeSet<String>> = Vec::new();
            for pr in info["profiles"].as_array().into_iter().flatten() {
                let outer: std::collections::BTreeSet<String> =
                    pr["curves"].as_array().into_iter().flatten().filter_map(|c| c.as_str().map(String::from)).collect();
                if loops.iter().any(|l| set(l) == outer) {
                    for h in pr["holes"].as_array().into_iter().flatten() {
                        holes.push(h.as_array().into_iter().flatten().filter_map(|c| c.as_str().map(String::from)).collect());
                    }
                }
            }
            loops.retain(|l| !holes.contains(&set(l)));
        }
    }
    for curves in loops {
        profiles.push(json!({"curves": curves}));
    }
    for r in def["regions"].as_array().into_iter().flatten() {
        if let Some(pt) = geom::v2(&r["point"]) {
            profiles.push(json!({"point": pt}));
        }
    }
    if !profiles.is_empty() {
        p["profiles"] = json!(profiles);
        if def["regions"].as_array().map(|r| !r.is_empty()).unwrap_or(false) {
            p["exact_regions"] = json!(true);
        }
    }

    // end conditions
    let end = &def["end"];
    let reverse = def["reverse"].as_bool().unwrap_or(false) ^ end["reverse"].as_bool().unwrap_or(false);
    // extrude runs along the sketch normal, a cut against it; `reverse` flips either (spec)
    let along = if cut { reverse } else { !reverse };
    // from a start face, which way it runs is not pinned down: guessed, then checked (mod.rs)
    let from_face = def.get("start").map(|v| v["type"] == "surface" || v["type"] == "vertex").unwrap_or(false);
    let along = if from_face {
        st.uncertain.insert(fid.to_string());
        along ^ st.flips.contains(fid)
    } else {
        along
    };
    let mut direction = if along { "positive" } else { "negative" };
    match end["type"].as_str().unwrap_or("blind") {
        "blind" => p["distance"] = json!(length(st, fid, "end.depth", num(&end["depth"]), "mm")),
        "throughAll" => p["through_all"] = json!(true),
        "midPlane" => {
            direction = "symmetric";
            p["distance"] = json!(length(st, fid, "end.depth", num(&end["depth"]), "mm"));
        }
        "upToVertex" | "upToSurface" | "offsetFromSurface" => {
            let t = pick(&end["target"]).ok_or_else(|| anyhow!("its end target has no position"))?;
            p["to"] = json!(t);
            if let Some(o) = end["offset"].as_f64() {
                if o != 0.0 {
                    p["to_offset"] = json!(format!("{o} mm"));
                }
            }
        }
        "throughAllBoth" => {
            direction = "symmetric";
            p["through_all"] = json!(true);
        }
        other => bail!("end condition {other} is not rebuilt by this version"),
    }
    if let Some(e2) = def.get("end2").filter(|v| v.is_object()) {
        match e2["type"].as_str().unwrap_or("blind") {
            "blind" => p["distance2"] = json!(format!("{} mm", num(&e2["depth"]))),
            "throughAll" => {
                if p.get("through_all").is_some() {
                    direction = "symmetric";
                } else {
                    notes.push("second side through all built as 10 m".into());
                    p["distance2"] = json!("10000 mm");
                }
            }
            other => bail!("second-side end condition {other} is not rebuilt by this version"),
        }
    }
    p["direction"] = json!(direction);
    if let Some(start) = def.get("start").filter(|v| v["type"] == "offset") {
        let d = num(&start["distance"]) * if start["reverse"].as_bool() == Some(true) { -1.0 } else { 1.0 };
        p["start_offset"] = json!(format!("{d} mm"));
    }
    // starts at a face or vertex (From Object)
    if let Some(start) = def.get("start").filter(|v| v["type"] == "surface" || v["type"] == "vertex") {
        let at = pick(&start["target"]).ok_or_else(|| anyhow!("its start face has no position"))?;
        p["from"] = json!(at);
    }
    if let Some(thin) = def.get("thin").filter(|v| v.is_object()) {
        p["thin"] = json!(true);
        p["thickness"] = json!(format!("{} mm", num(&thin["thickness"])));
        p["side"] = json!(match thin["type"].as_str().unwrap_or("") {
            "midPlane" => "center",
            "twoDirection" => "two_sides",
            _ => "outside",
        });
        if thin["type"] == "twoDirection" {
            p["thickness2"] = json!(format!("{} mm", num(&thin["thickness2"])));
        }
        notes.push("thin wall: its side checked by the part's shape".into());
    }
    p["operation"] = json!(if cut {
        "cut"
    } else if def["merge"].as_bool() == Some(false) {
        "new"
    } else {
        "auto"
    });
    if !cut && def["merge"].as_bool() != Some(false) {
        // merged in the source: pieces that touch the body only along an edge stay one body, as there
        p["touching"] = json!("one_body");
    }
    if let Some(d) = def.get("draft").filter(|v| v.is_object()) {
        let a = num(&d["angle"]) * if d["outward"].as_bool() == Some(true) { 1.0 } else { -1.0 };
        p["taper"] = json!(format!("{a} deg"));
    }

    let before = if cut { Some(total_volume(s)?) } else { None };
    s.exec("solid.extrude", p.clone())?;
    if let Some(v0) = before {
        // a cut that leaves the volume as it was has gone the wrong way: try the other way
        let v1 = total_volume(s)?;
        if (v1 - v0).abs() <= 1e-9 * v0.max(1.0) && direction != "symmetric" {
            s.tool("undo", json!({}))?;
            p["direction"] = json!(if direction == "positive" { "negative" } else { "positive" });
            s.exec("solid.extrude", p)?;
            notes.push("built in the other direction (the first removed nothing)".into());
        }
    }
    st.names.insert(fid.to_string(), name.to_string());
    Ok(if notes.is_empty() { Built::Done } else { Built::Approx(notes.join("; ")) })
}

fn revolve(s: &mut Session, st: &mut PartState, f: &Value, cut: bool) -> Result<Built> {
    let def = &f["def"];
    let name = &st.unique_name(f["name"].as_str().unwrap_or("Revolve"));
    let fid = f["id"].as_str().unwrap_or("");
    let sk = sketch_of(st, &def["profile"])?.clone();
    let mut notes = Vec::new();
    let axis_ref = &def["axis"];
    let axis = match axis_ref["kind"].as_str() {
        Some("sketchEntity") => {
            let feat = axis_ref["feature"].as_str().unwrap_or("");
            let id = axis_ref["id"].as_str().unwrap_or("");
            let other = st.sketches.get(feat).ok_or_else(|| anyhow!("its axis sketch was not built"))?;
            let e = other.ents.get(id).ok_or_else(|| anyhow!("its axis line was not drawn"))?;
            if other.name == sk.name {
                json!(e.sc)
            } else {
                json!({"sketch": other.name, "curve": e.sc})
            }
        }
        // an origin axis: SolidCraft's world axis
        Some("originAxis") => json!(axis_ref["axis"].as_str().unwrap_or("z").to_uppercase()),
        Some("feature") => {
            let id = axis_ref["id"].as_str().unwrap_or("");
            // a construction axis by its name, else the axis feature's line
            if let Some(n) = st.names.get(id) {
                json!(n)
            } else if let Some((a, b)) = st.axis_lines.get(id) {
                json!({"origin": a, "dir": crate::geom::unit(crate::geom::sub(*b, *a))})
            } else {
                bail!("its axis was not built")
            }
        }
        _ => bail!("its axis reference is not rebuilt by this version"),
    };
    let angle = num(&def["angle"]);
    // revolve takes new, join, cut or intersect: join where the part already has a body
    let has_body = !super::measure_fingerprints_with(s, false, &st.exclude)?.is_empty();
    let op = if cut {
        "cut"
    } else if def["merge"].as_bool() == Some(false) || !has_body {
        "new"
    } else {
        "join"
    };
    let mut p = json!({"sketch": sk.name, "axis": axis, "name": name, "operation": op});
    let reverse = def["reverse"].as_bool().unwrap_or(false);
    match def["mode"].as_str().unwrap_or("oneDirection") {
        "midPlane" => {
            p["direction"] = json!("symmetric");
            p["angle"] = json!(length(st, fid, "angle", angle, "deg"));
        }
        "twoDirection" => {
            p["angle"] = json!(format!("{angle} deg"));
            p["angle2"] = json!(format!("{} deg", num(&def["angle2"])));
        }
        _ => {
            // NCX: unreversed turns clockwise about the axis as recorded; checked by the part's shape
            let fid = f["id"].as_str().unwrap_or("").to_string();
            let mut a = if reverse { angle } else { -angle };
            if (angle - 360.0).abs() >= 1e-9 {
                st.uncertain.insert(fid.clone());
                if st.flips.contains(&fid) {
                    a = -a;
                }
            }
            p["angle"] = json!(if (angle - 360.0).abs() < 1e-9 { "360 deg".to_string() } else { format!("{a} deg") });
            if (angle - 360.0).abs() >= 1e-9 {
                notes.push("turning sense checked by the part's shape".to_string());
            }
        }
    }
    if let Some(thin) = def.get("thin").filter(|v| v.is_object()) {
        bail!("thin revolve ({} mm) is not rebuilt by this version", num(&thin["thickness"]));
    }
    s.exec("solid.revolve", p)?;
    st.names.insert(fid.to_string(), name.to_string());
    Ok(if notes.is_empty() { Built::Done } else { Built::Approx(notes.join("; ")) })
}

fn edge_picks(def: &Value, key: &str) -> Result<Vec<geom::V3>> {
    let mut v = Vec::new();
    for e in def[key].as_array().into_iter().flatten() {
        v.push(pick(e).ok_or_else(|| anyhow!("an edge has no position"))?);
    }
    if v.is_empty() {
        bail!("no edges");
    }
    Ok(v)
}

fn fillet(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = &st.unique_name(f["name"].as_str().unwrap_or("Fillet"));
    match def["filletType"].as_str().unwrap_or("constant") {
        "constant" => {}
        other => bail!("{other} fillet is not rebuilt by this version"),
    }
    let edges = edge_picks(def, "edges")?;
    let propagate = def["propagate"].as_bool().unwrap_or(true);
    let radii: Vec<f64> = def["radii"].as_array().map(|r| r.iter().map(num).collect()).unwrap_or_default();
    if radii.len() == edges.len() && radii.iter().any(|r| (r - radii[0]).abs() > 1e-9) {
        // one feature per radius
        let mut groups: Vec<(f64, Vec<geom::V3>)> = Vec::new();
        for (e, r) in edges.iter().zip(radii.iter()) {
            match groups.iter_mut().find(|g| (g.0 - r).abs() < 1e-9) {
                Some(g) => g.1.push(*e),
                None => groups.push((*r, vec![*e])),
            }
        }
        for (i, (r, es)) in groups.iter().enumerate() {
            let n = if i == 0 { name.to_string() } else { format!("{name} ({})", i + 1) };
            s.exec("solid.fillet", json!({"edges": es, "radius": format!("{r} mm"), "tangent_chain": propagate, "name": n}))?;
        }
        return Ok(Built::Approx(format!("built as {} fillets, one per radius", groups.len())));
    }
    let r = num(&def["radius"]);
    s.exec("solid.fillet", json!({"edges": edges, "radius": format!("{r} mm"), "tangent_chain": propagate, "name": name}))?;
    Ok(Built::Done)
}

fn chamfer(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = &st.unique_name(f["name"].as_str().unwrap_or("Chamfer"));
    let edges = edge_picks(def, "edges")?;
    let propagate = def["propagate"].as_bool().unwrap_or(true);
    let flipped: Vec<bool> = def["flipped"].as_array().map(|v| v.iter().map(|b| b.as_bool().unwrap_or(false)).collect()).unwrap_or_default();
    let mut p = json!({"edges": edges, "distance": format!("{} mm", num(&def["distance"])), "tangent_chain": propagate, "name": name});
    let mut notes = Vec::new();
    match def["chamferType"].as_str().unwrap_or("equalDistance") {
        "equalDistance" => {}
        "distanceDistance" => {
            p["distance2"] = json!(format!("{} mm", num(&def["distance2"])));
            notes.push("which face takes which distance checked by the part's shape".to_string());
        }
        "angleDistance" => {
            p["angle"] = json!(format!("{} deg", num(&def["angle"])));
            if (num(&def["angle"]) - 45.0).abs() > 1e-9 {
                notes.push("which face the angle is from checked by the part's shape".to_string());
            }
        }
        other => bail!("{other} chamfer is not rebuilt by this version"),
    }
    if flipped.iter().any(|b| *b) {
        p["flip"] = json!(true);
    }
    s.exec("solid.chamfer", p)?;
    Ok(if notes.is_empty() { Built::Done } else { Built::Approx(notes.join("; ")) })
}

/// A reference plane. Planes parallel to an origin plane are made as offset planes; others are left
/// out (sketches are placed by their own frames, so nothing else depends on them in this version).
fn plane(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = &st.unique_name(f["name"].as_str().unwrap_or("Plane"));
    let origin = v3(&def["origin"]).ok_or_else(|| anyhow!("no origin"))?;
    let n = geom::unit(v3(&def["normal"]).ok_or_else(|| anyhow!("no normal"))?);
    let base = [("XY", [0.0, 0.0, 1.0], 2usize), ("XZ", [0.0, 1.0, 0.0], 1), ("YZ", [1.0, 0.0, 0.0], 0)]
        .into_iter()
        .find(|(_, a, _)| geom::dot(n, *a).abs() > 1.0 - 1e-9);
    let Some((b, _axis, k)) = base else {
        return Ok(Built::Skipped("a plane at an angle is not made by this version (nothing here uses it)".into()));
    };
    // offset from an origin plane: made so, following its distance (and the parameter driving it); the distance
    // runs along the origin plane's normal, or against it when flipped
    let refs = def["references"].as_array().cloned().unwrap_or_default();
    if refs.len() == 1 && refs[0]["constraint"] == "distance" {
        let r = &refs[0]["ref"];
        let fid = f["id"].as_str().unwrap_or("");
        let base_of = match r["kind"].as_str() {
            Some("originPlane") => match r["plane"].as_str() {
                Some("front") => Some(("XY".to_string(), [0.0, 0.0, 1.0])),
                Some("top") => Some(("XZ".to_string(), [0.0, 1.0, 0.0])),
                Some("right") => Some(("YZ".to_string(), [1.0, 0.0, 0.0])),
                _ => None,
            },
            _ => None,
        };
        if let Some((bname, _)) = base_of {
            let v = num(&refs[0]["value"]);
            let flip = refs[0]["flip"].as_bool() == Some(true);
            let mut ex = length(st, fid, "references.0.value", v, "mm");
            if flip {
                ex = format!("-({ex})");
            }
            if s.exec("construct.plane.offset", json!({"base": bname, "offset": ex, "name": name})).is_ok() {
                st.names.insert(fid.to_string(), name.to_string());
                return Ok(Built::Done);
            }
        }
    }
    // the plane holds the origin, whichever way its normal points: offset = that coordinate
    s.exec("construct.plane.offset", json!({"base": b, "offset": format!("{} mm", origin[k]), "name": name}))?;
    st.names.insert(f["id"].as_str().unwrap_or("").to_string(), name.to_string());
    Ok(Built::Approx("placed at its position; not linked to the references it was made from".into()))
}

/// Uses a feature's carried result (FORMAT-SPEC §4 "Feature results"): the bodies it changed are replaced
/// by the bodies in its STEP data.
pub fn apply_result(s: &mut Session, st: &mut PartState, f: &Value) -> Result<()> {
    let r = &f["result"];
    let replaces: Vec<Value> = r["replaces"].as_array().cloned().unwrap_or_default();
    let have = super::measure_fingerprints(s, st)?;
    let mut remove: Vec<String> = Vec::new();
    // surface bodies (volume 0) are matched by area and box among the part's open bodies
    let open: Vec<Value> = if replaces.iter().any(|fp| num(&fp["volume"]) <= 0.0) {
        let m = s.tool("measure", json!({}))?;
        m["bodies"].as_array().into_iter().flatten()
            .filter(|b| b["closed"].as_bool() == Some(false) && !st.exclude.contains(b["name"].as_str().unwrap_or("")))
            .cloned().collect()
    } else {
        Vec::new()
    };
    let box_mid = |b: &Value| -> Option<geom::V3> {
        if let Some(a) = b.as_array().filter(|a| a.len() == 6) {
            let n: Vec<f64> = a.iter().map(|x| x.as_f64().unwrap_or(0.0)).collect();
            return Some([(n[0] + n[3]) / 2.0, (n[1] + n[4]) / 2.0, (n[2] + n[5]) / 2.0]);
        }
        let (mn, mx) = (v3(&b["min"])?, v3(&b["max"])?);
        Some([(mn[0] + mx[0]) / 2.0, (mn[1] + mx[1]) / 2.0, (mn[2] + mx[2]) / 2.0])
    };
    for fp in &replaces {
        let v = num(&fp["volume"]);
        if v <= 0.0 {
            let a = num(&fp["area"]);
            let want = box_mid(&fp["bbox"]).unwrap_or([0.0; 3]);
            let hit = open.iter().filter(|b| !remove.iter().any(|r| b["name"] == r.as_str())).find(|b| {
                (num(&b["area_mm2"]) - a).abs() <= 2e-3 * a.max(1.0)
                    && box_mid(&b["bbox"]).map(|m| geom::norm(geom::sub(m, want)) < 0.05).unwrap_or(false)
            });
            match hit.and_then(|b| b["name"].as_str()) {
                Some(n) => remove.push(n.to_string()),
                None => bail!("the part as rebuilt so far differs from the file just before this feature (a surface)"),
            }
            continue;
        }
        let c = v3(&fp["centroid"]).unwrap_or([0.0; 3]);
        let best = have
            .iter()
            .filter(|(n, _)| !remove.contains(n))
            .map(|(n, a)| {
                let dv = (num(&a["volume"]) - v).abs() / v.abs().max(1e-9);
                let dc = geom::norm(geom::sub(v3(&a["centroid"]).unwrap_or([0.0; 3]), c));
                (n.clone(), dv, dc)
            })
            .min_by(|a, b| (a.1 * 1e3 + a.2).partial_cmp(&(b.1 * 1e3 + b.2)).unwrap());
        match best {
            Some((n, dv, dc)) if dv < 1e-3 && dc < 0.05 => remove.push(n),
            _ => bail!("the part as rebuilt so far differs from the file just before this feature"),
        }
    }
    if !remove.is_empty() {
        s.exec("solid.remove", json!({"bodies": remove}))?;
    }
    let data = r["data"].as_str().unwrap_or("");
    if !data.is_empty() {
        let path = super::scratch_dir().join(format!("result-{}.step", sanitize(f["name"].as_str().unwrap_or("feature"))));
        std::fs::write(&path, data)?;
        let ins = s.exec("file.insert_step", json!({"path": path.to_string_lossy(), "flatten": true, "names": "solid"}))?;
        // the result's bodies (exactly those the import made) belong to the part, not to a component of their own
        let comp = ins["component"].clone();
        let names: Vec<String> =
            ins["created_bodies"].as_array().into_iter().flatten().filter_map(|b| b.as_str().map(String::from)).collect();
        if !names.is_empty() && !comp.is_null() {
            let target = if st.component.is_null() { json!(st.doc_name) } else { st.component.clone() };
            s.exec("component.move_bodies", json!({"bodies": names, "component": target}))?;
            // the import's own component stays (it holds the import the bodies came from): hidden
            let _ = s.exec("browser.visibility", json!({"visible": false, "items": [comp]}));
        }
        let _ = std::fs::remove_file(&path);
    }
    st.used_results = true;
    Ok(())
}

fn sanitize(s: &str) -> String {
    s.chars().map(|c| if c.is_alphanumeric() { c } else { '_' }).collect()
}

/// Translates an expression in the neutral grammar (names in double quotes) into SolidCraft's, naming
/// SolidCraft parameters. None when a name is unknown here.
pub fn translate_expression(e: &str, st: &PartState) -> Option<String> {
    let mut out = String::new();
    let mut chars = e.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '"' {
            let mut name = String::new();
            for d in chars.by_ref() {
                if d == '"' {
                    break;
                }
                name.push(d);
            }
            if let Some(p) = st.params.get(&name) {
                out.push_str(p);
            } else if let Some(p) = st.dim_params.get(&name) {
                out.push_str(p);
            } else {
                // D1@SK_Base@Part.SLDPRT -> D1@SK_Base
                let short: Vec<&str> = name.split('@').take(2).collect();
                out.push_str(st.dim_params.get(&short.join("@"))?);
            }
        } else {
            out.push(c);
        }
    }
    Some(out)
}
