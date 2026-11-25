//! More features (FORMAT-SPEC §4): shell, draft, patterns, mirror, holes, reference axes, sweeps, lofts, body
//! operations, sheet metal, weldment members and surfaces.
//!
//! Where the source system's convention is not pinned down by the payload (which side a sheet's thickness or a
//! flange goes, which way a draft pulls), the importer builds the likeliest variant; a feature that carries its
//! `result` is then checked against it (mod.rs) and the other variants tried, or the result used.

use std::collections::HashMap;

use anyhow::{anyhow, bail, Result};
use serde_json::{json, Value};

use super::features::{pick, Built};
use super::{num, PartState};
use crate::geom::{self, v3};
use crate::sc::Session;

pub fn build(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Option<Built>> {
    let kind = f["type"].as_str().unwrap_or("");
    let r = match kind {
        "shell" => shell(s, st, f)?,
        "draft" => draft(s, st, f)?,
        "linearPattern" => linear_pattern(s, st, f)?,
        "circularPattern" => circular_pattern(s, st, f)?,
        "mirror" | "mirrorBody" => mirror(s, st, f)?,
        "hole" => hole(s, st, f)?,
        "axis" => axis(s, st, f)?,
        "point" => Built::Skipped("reference point: not made (nothing here uses it)".into()),
        "coordinateSystem" => Built::Skipped("coordinate system: not made (nothing here uses it)".into()),
        "sweep" | "sweepCut" => sweep(s, st, f, kind == "sweepCut")?,
        "loft" | "loftCut" => loft(s, st, f, kind == "loftCut")?,
        "combine" => combine(s, st, f)?,
        "deleteBody" => delete_body(s, st, f)?,
        "moveBody" => move_body(s, st, f)?,
        "split" => split(s, st, f)?,
        "sheetMetal" => sheet_rule(s, st, f)?,
        "baseFlange" => base_flange(s, st, f)?,
        "edgeFlange" => edge_flange(s, st, f, false)?,
        "convertToSheetMetal" => convert_sheet(s, st, f)?,
        "structuralMember" => member(s, st, f)?,
        "extrudedSurface" => surface_extrude(s, st, f)?,
        "revolvedSurface" => surface_revolve(s, st, f)?,
        "planarSurface" => planar_surface(s, st, f)?,
        "knit" => knit(s, st, f)?,
        "rib" => rib(s, st, f)?,
        "unfold" | "fold" => {
            // SolidCraft unfolds (and refolds) every bend of the sheet at once
            let all = bodies(s, st)?;
            if all.len() != 1 {
                bail!("{kind} of one of several bodies is not rebuilt by this version");
            }
            let cmd = if kind == "unfold" { "sheet.unfold" } else { "sheet.refold" };
            s.exec(cmd, json!({"body": all[0].0}))?;
            Built::Approx(format!("{kind}s every bend of the sheet (SolidCraft has no single-bend {kind}): checked by the part's shape"))
        }
        _ => return Ok(None),
    };
    Ok(Some(r))
}

// ---------------------------------------------------------------------------------------------------------
// helpers

/// The bodies of the part now (other parts' left out), with their measurements.
pub(crate) fn bodies(s: &mut Session, st: &PartState) -> Result<Vec<(String, Value)>> {
    super::measure_fingerprints_with(s, false, &st.exclude)
}

/// The body a `{kind: body, by: geom|name}` reference names: by name, else by volume and centroid.
pub(crate) fn body_of(s: &mut Session, st: &PartState, r: &Value) -> Result<String> {
    let all = bodies(s, st)?;
    if let Some(n) = r["name"].as_str() {
        if all.iter().any(|(b, _)| b == n) {
            return Ok(n.to_string());
        }
    }
    let g = &r["geom"];
    let v = num(&g["volume"]);
    let c = v3(&g["centroid"]).unwrap_or([0.0; 3]);
    all.iter()
        .map(|(n, fp)| {
            let dv = (num(&fp["volume"]) - v).abs() / v.abs().max(1e-9);
            let dc = geom::norm(geom::sub(v3(&fp["centroid"]).unwrap_or([0.0; 3]), c));
            (n.clone(), dv * 1e3 + dc, dv, dc)
        })
        .filter(|x| x.2 < 1e-2 && x.3 < 1.0)
        .min_by(|a, b| a.1.partial_cmp(&b.1).unwrap())
        .map(|x| x.0)
        .ok_or_else(|| anyhow!("its body was not found"))
}

/// The SolidCraft name of a payload feature reference.
fn feature_name(st: &PartState, r: &Value) -> Result<String> {
    let id = r["id"].as_str().unwrap_or("");
    st.names.get(id).cloned().ok_or_else(|| anyhow!("the feature it uses was not built"))
}

/// A direction from a reference: a straight edge's direction, a plane's normal, an axis feature's line.
fn direction(st: &PartState, r: &Value) -> Result<geom::V3> {
    let g = &r["geom"];
    if let Some(d) = v3(&g["params"]["direction"]) {
        return Ok(geom::unit(d));
    }
    if let Some(n) = v3(&g["params"]["normal"]) {
        return Ok(geom::unit(n));
    }
    if let Some(a) = v3(&g["params"]["axis"]) {
        return Ok(geom::unit(a));
    }
    if r["kind"] == "originPlane" {
        return Ok(match r["plane"].as_str() {
            Some("front") => [0.0, 0.0, 1.0],
            Some("top") => [0.0, 1.0, 0.0],
            _ => [1.0, 0.0, 0.0],
        });
    }
    if r["kind"] == "originAxis" {
        return Ok(match r["axis"].as_str() {
            Some("x") | Some("X") => [1.0, 0.0, 0.0],
            Some("y") | Some("Y") => [0.0, 1.0, 0.0],
            _ => [0.0, 0.0, 1.0],
        });
    }
    if let Some(line) = st.axis_lines.get(r["id"].as_str().unwrap_or("")) {
        return Ok(geom::unit(geom::sub(line.1, line.0)));
    }
    bail!("its direction reference has no direction")
}

/// A plane for mirror, split, draft: an origin plane, a built reference plane, or a planar face.
fn plane(st: &PartState, r: &Value) -> Result<Value> {
    match r["kind"].as_str() {
        Some("originPlane") => Ok(json!(match r["plane"].as_str() {
            Some("front") => "XY",
            Some("top") => "XZ",
            _ => "YZ",
        })),
        Some("feature") => Ok(json!(feature_name(st, r)?)),
        Some("face") => {
            let n = v3(&r["geom"]["params"]["normal"]).ok_or_else(|| anyhow!("its plane face has no normal"))?;
            let o = v3(&r["geom"]["params"]["point"]).or_else(|| pick(r)).ok_or_else(|| anyhow!("its plane face has no point"))?;
            Ok(json!({"origin": o, "normal": n}))
        }
        other => bail!("a {:?} plane is not rebuilt by this version", other),
    }
}

fn picks(def: &Value, key: &str) -> Result<Vec<geom::V3>> {
    let mut v = Vec::new();
    for r in def[key].as_array().into_iter().flatten() {
        v.push(pick(r).ok_or_else(|| anyhow!("a {key} reference has no position"))?);
    }
    Ok(v)
}

fn name_of(st: &mut PartState, f: &Value, default: &str) -> String {
    st.unique_name(f["name"].as_str().unwrap_or(default))
}

fn remember(st: &mut PartState, f: &Value, name: &str) {
    st.names.insert(f["id"].as_str().unwrap_or("").to_string(), name.to_string());
}

/// The seeds of a pattern or mirror: features (by their SolidCraft names) or bodies.
fn seeds(s: &mut Session, st: &PartState, def: &Value) -> Result<(String, Vec<String>)> {
    let feats: Vec<String> = def["seeds"].as_array().into_iter().flatten().filter_map(|r| feature_name(st, r).ok()).collect();
    if !feats.is_empty() {
        return Ok(("features".into(), feats));
    }
    let mut bs = Vec::new();
    for r in def["bodies"].as_array().into_iter().flatten() {
        bs.push(body_of(s, st, r)?);
    }
    if bs.is_empty() {
        bail!("nothing to pattern (its seeds were not built)");
    }
    Ok(("bodies".into(), bs))
}

// ---------------------------------------------------------------------------------------------------------
// solid features

fn shell(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Shell");
    let faces = picks(def, "faces")?;
    let dir = if def["outward"].as_i64().unwrap_or(0) != 0 || def["outward"].as_bool() == Some(true) { "outside" } else { "inside" };
    s.exec("solid.shell", json!({"faces": faces, "thickness": format!("{} mm", num(&def["thickness"])), "direction": dir, "name": name}))?;
    remember(st, f, &name);
    Ok(Built::Done)
}

fn draft(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Draft");
    let faces = picks(def, "faces")?;
    let neutral = plane(st, &def["neutral"])?;
    let mut n = v3(&def["neutral"]["geom"]["params"]["normal"]).unwrap_or([0.0, 0.0, 1.0]);
    if def["reverse"].as_bool() == Some(true) {
        n = geom::scale(n, -1.0);
    }
    let fid = f["id"].as_str().unwrap_or("").to_string();
    st.uncertain.insert(fid.clone());
    if st.flips.contains(&fid) {
        n = geom::scale(n, -1.0);
    }
    // SolidWorks pulls away from the neutral face's outward normal; checked by the part's shape
    let pull = geom::scale(n, -1.0);
    s.exec("solid.draft", json!({"faces": faces, "angle": format!("{} deg", num(&def["angle"])), "neutral": neutral, "pull": pull, "name": name}))?;
    remember(st, f, &name);
    Ok(Built::Approx("which way it pulls checked by the part's shape".into()))
}

fn linear_pattern(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Pattern");
    let (key, list) = seeds(s, st, def)?;
    let d1 = &def["dir1"];
    let skipped: Vec<u64> = def["skipped"].as_array().into_iter().flatten().filter_map(Value::as_u64).collect();
    let mut dir1 = direction(st, &d1["ref"])?;
    if d1["reverse"].as_bool() == Some(true) {
        dir1 = geom::scale(dir1, -1.0);
    }
    let mut p = json!({key.clone(): list, "dir1": dir1, "count1": d1["count"].as_u64().unwrap_or(2),
                        "spacing1": format!("{} mm", num(&d1["spacing"])), "name": name});
    if let Some(d2) = def.get("dir2").filter(|v| v.is_object()) {
        let mut dir2 = direction(st, &d2["ref"])?;
        if d2["reverse"].as_bool() == Some(true) {
            dir2 = geom::scale(dir2, -1.0);
        }
        p["dir2"] = json!(dir2);
        p["count2"] = json!(d2["count"].as_u64().unwrap_or(2));
        p["spacing2"] = json!(format!("{} mm", num(&d2["spacing"])));
    }
    let seed_centroid = if key == "bodies" && !skipped.is_empty() {
        let all = bodies(s, st)?;
        all.iter().find(|(n, _)| *n == list[0]).and_then(|(_, fp)| v3(&fp["centroid"]))
    } else {
        None
    };
    s.exec("solid.pattern.rectangular", p)?;
    remember(st, f, &name);
    if !skipped.is_empty() {
        // the instances the source skipped: the copies nearest where they would be (one direction, body seeds)
        let Some(c0) = seed_centroid else { bail!("skipped instances of a feature pattern are not rebuilt by this version") };
        if def.get("dir2").map(|v| v.is_object()).unwrap_or(false) {
            bail!("skipped instances of a two-direction pattern are not rebuilt by this version");
        }
        let all = bodies(s, st)?;
        let mut gone = Vec::new();
        for k in &skipped {
            let at = geom::add(c0, geom::scale(dir1, *k as f64 * num(&d1["spacing"])));
            if let Some((n, _)) = all
                .iter()
                .filter(|(n, _)| !gone.contains(n))
                .map(|(n, fp)| (n.clone(), geom::norm(geom::sub(v3(&fp["centroid"]).unwrap_or([0.0; 3]), at))))
                .min_by(|a, b| a.1.partial_cmp(&b.1).unwrap())
                .filter(|(_, d)| *d < 1e-3 * num(&d1["spacing"]).max(1.0))
            {
                gone.push(n);
            }
        }
        if gone.len() != skipped.len() {
            bail!("the skipped instances were not found");
        }
        s.exec("solid.remove", json!({"bodies": gone}))?;
        return Ok(Built::Approx(format!("{} skipped instance(s) removed after patterning", skipped.len())));
    }
    Ok(Built::Done)
}

fn circular_pattern(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    if def["skipped"].as_array().map(|a| !a.is_empty()).unwrap_or(false) {
        bail!("a pattern with skipped instances is not rebuilt by this version");
    }
    let name = name_of(st, f, "Pattern");
    let (key, list) = seeds(s, st, def)?;
    let ax = &def["axis"];
    let axis = match ax["kind"].as_str() {
        Some("face") => json!({"face": pick(ax).ok_or_else(|| anyhow!("its axis face has no position"))?}),
        Some("edge") => json!({"edge": pick(ax).ok_or_else(|| anyhow!("its axis edge has no position"))?}),
        Some("feature") => json!(feature_name(st, ax)?),
        Some("originAxis") => json!(ax["axis"].as_str().unwrap_or("z").to_uppercase()),
        other => bail!("a {:?} pattern axis is not rebuilt by this version", other),
    };
    let count = def["count"].as_u64().unwrap_or(2);
    let a = num(&def["angle"]);
    // SolidWorks: with equal spacing the angle is the total; otherwise the step between instances
    let total = if def["equalSpacing"].as_bool() == Some(false) { a * (count as f64 - 1.0) } else { a };
    let mut p = json!({key: list, "axis": axis, "count": count, "angle": format!("{total} deg"), "name": name});
    if def["reverse"].as_bool() == Some(true) {
        p["reversed"] = json!(true);
    }
    s.exec("solid.pattern.circular", p)?;
    remember(st, f, &name);
    Ok(Built::Done)
}

fn mirror(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Mirror");
    let (key, list) = seeds(s, st, def)?;
    let pl = plane(st, &def["plane"])?;
    let mut p = json!({key: list, "plane": pl, "name": name});
    if def["merge"].as_bool() == Some(true) {
        p["combine"] = json!(true);
    }
    s.exec("solid.mirror", p)?;
    remember(st, f, &name);
    Ok(Built::Done)
}

fn hole(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Hole");
    let sizes = &def["sizes"];
    let mut p = json!({"name": name});
    let d = num(&sizes["holeDiameter"]).max(num(&sizes["diameter"]));
    let measured: Vec<f64> = sizes["measuredDiameters"].as_array().into_iter().flatten().filter_map(Value::as_f64).collect();
    let d = if d > 0.0 { d } else { measured.first().copied().unwrap_or(0.0) };
    if d <= 0.0 {
        bail!("its diameter is not in the file");
    }
    p["diameter"] = json!(format!("{d} mm"));
    let mut notes = Vec::new();
    match def["holeType"].as_str().unwrap_or("simple") {
        "counterbore" | "counterboreSlot" => {
            p["type"] = json!("counterbore");
            p["cb_diameter"] = json!(format!("{} mm", num(&sizes["counterBoreDiameter"])));
            p["cb_depth"] = json!(format!("{} mm", num(&sizes["counterBoreDepth"])));
        }
        "countersink" | "countersinkSlot" => {
            p["type"] = json!("countersink");
            p["cs_diameter"] = json!(format!("{} mm", num(&sizes["counterSinkDiameter"])));
            p["cs_angle"] = json!(format!("{} deg", num(&sizes["counterSinkAngle"])));
        }
        "tapped" | "pipeTap" => {
            let td = num(&sizes["tapDrillDiameter"]);
            if td > 0.0 {
                p["diameter"] = json!(format!("{td} mm"));
            }
            notes.push("tapped: drilled at its tap drill size (the thread is SolidWorks' table data)".to_string());
        }
        _ => {}
    }
    match def["end"]["type"].as_str().unwrap_or("throughAll") {
        "blind" => p["depth"] = json!(format!("{} mm", num(&def["end"]["depth"]).max(num(&sizes["holeDepth"])))),
        "throughAll" | "throughAllBoth" => {}
        other => notes.push(format!("{other} end: drilled through all")),
    }
    let a = num(&sizes["drillAngle"]);
    if a > 0.0 && def["end"]["type"] == "blind" {
        p["tip_angle"] = json!(format!("{a} deg"));
    }
    // one hole per centre, at its point on the face
    let pts: Vec<geom::V3> = def["points"].as_array().into_iter().flatten().filter_map(v3).collect();
    if pts.is_empty() {
        bail!("it has no hole centres");
    }
    for (i, pt) in pts.iter().enumerate() {
        let mut q = p.clone();
        q["position"] = json!(pt);
        if i > 0 {
            q["name"] = json!(format!("{name} ({})", i + 1));
        }
        s.exec("solid.hole", q)?;
    }
    remember(st, f, &name);
    if pts.len() > 1 {
        notes.push(format!("{} holes, one feature each", pts.len()));
    }
    Ok(if notes.is_empty() { Built::Done } else { Built::Approx(notes.join("; ")) })
}

fn axis(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Axis");
    if let (Some(a), Some(b)) = (v3(&def["line"]["start"]), v3(&def["line"]["end"])) {
        st.axis_lines.insert(f["id"].as_str().unwrap_or("").to_string(), (a, b));
    }
    let r0 = def["references"].as_array().and_then(|a| a.first()).cloned().unwrap_or(Value::Null);
    match def["axisType"].as_str() {
        Some("cylindricalFace") => {
            let p = pick(&r0).ok_or_else(|| anyhow!("its face has no position"))?;
            s.exec("construct.axis.through_cylinder", json!({"face": {"face": p}, "name": name}))?;
        }
        Some("line") if r0["kind"] == "edge" => {
            let p = pick(&r0).ok_or_else(|| anyhow!("its edge has no position"))?;
            s.exec("construct.axis.edge", json!({"edge": {"edge": p}, "name": name}))?;
        }
        _ => {
            return Ok(Built::Skipped("reference axis: kept as a line for the features that use it, not made".into()));
        }
    }
    remember(st, f, &name);
    Ok(Built::Done)
}

fn sketch_name(st: &PartState, r: &Value) -> Result<String> {
    let id = r["id"].as_str().unwrap_or("");
    st.sketches.get(id).map(|b| b.name.clone()).ok_or_else(|| anyhow!("its sketch was not built"))
}

/// The drawn curves of a sketch in the payload's order (a path or a rail).
fn curves_of(st: &PartState, r: &Value) -> Result<(String, Vec<String>)> {
    let id = r["id"].as_str().unwrap_or("");
    let sk = st.sketches.get(id).ok_or_else(|| anyhow!("its path sketch was not built"))?;
    let mut ids: Vec<(String, String)> = sk
        .ents
        .iter()
        .filter(|(_, e)| e.kind != "point")
        .map(|(pid, e)| (pid.clone(), e.sc.clone()))
        .collect();
    // payload ids s1, s2, ... in order
    ids.sort_by_key(|(pid, _)| pid.trim_start_matches(|c: char| !c.is_ascii_digit()).parse::<u64>().unwrap_or(0));
    Ok((sk.name.clone(), ids.into_iter().map(|(_, sc)| sc).collect()))
}

fn sweep(s: &mut Session, st: &mut PartState, f: &Value, cut: bool) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Sweep");
    let profile = sketch_name(st, &def["profile"])?;
    let (path_sk, path) = curves_of(st, &def["path"])?;
    let mut p = json!({"sketch": profile, "path_sketch": path_sk, "path": path, "name": name,
                       "operation": if cut { "cut" } else if def["merge"].as_bool() == Some(false) { "new" } else { "join" }});
    let mut rails = Vec::new();
    for g in def["guides"].as_array().into_iter().flatten() {
        let (sk, cs) = curves_of(st, g)?;
        rails.push(json!({"sketch": sk, "curves": cs}));
    }
    if !rails.is_empty() {
        p["rails"] = json!(rails);
    }
    if let Err(e) = s.exec("solid.sweep", p.clone()) {
        // the first body in the part: join has nothing to join to
        if p["operation"] == "join" {
            p["operation"] = json!("new");
            s.exec("solid.sweep", p).map_err(|_| e)?;
        } else {
            return Err(e);
        }
    }
    remember(st, f, &name);
    Ok(Built::Done)
}

fn loft(s: &mut Session, st: &mut PartState, f: &Value, cut: bool) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Loft");
    let mut sections = Vec::new();
    for r in def["sections"].as_array().into_iter().flatten() {
        sections.push(json!({"sketch": sketch_name(st, r)?}));
    }
    let mut p = json!({"sections": sections, "name": name,
                       "operation": if cut { "cut" } else if def["merge"].as_bool() == Some(false) { "new" } else { "join" }});
    let mut rails = Vec::new();
    for g in def["guides"].as_array().into_iter().flatten() {
        let (sk, cs) = curves_of(st, g)?;
        rails.push(json!({"sketch": sk, "curves": cs}));
    }
    if !rails.is_empty() {
        p["rails"] = json!(rails);
    }
    if let Err(e) = s.exec("solid.loft", p.clone()) {
        if p["operation"] == "join" {
            p["operation"] = json!("new");
            s.exec("solid.loft", p).map_err(|_| e)?;
        } else {
            return Err(e);
        }
    }
    remember(st, f, &name);
    Ok(Built::Approx("how the sections line up checked by the part's shape".into()))
}

fn combine(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Combine");
    let target = body_of(s, st, &def["main"])?;
    let mut tools = Vec::new();
    for r in def["bodies"].as_array().into_iter().flatten() {
        tools.push(body_of(s, st, r)?);
    }
    let op = match def["operation"].as_str().unwrap_or("add") {
        "subtract" => "cut",
        "common" => "intersect",
        _ => "join",
    };
    s.exec("solid.combine", json!({"target": target, "tools": tools, "operation": op, "touching": "one_body", "name": name}))?;
    remember(st, f, &name);
    Ok(Built::Done)
}

fn delete_body(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let mut bs = Vec::new();
    for r in def["bodies"].as_array().into_iter().flatten() {
        bs.push(body_of(s, st, r)?);
    }
    if def["keep"].as_bool() == Some(true) {
        let all = bodies(s, st)?;
        bs = all.into_iter().map(|(n, _)| n).filter(|n| !bs.contains(n)).collect();
    }
    s.exec("solid.remove", json!({"bodies": bs}))?;
    Ok(Built::Done)
}

fn move_body(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Move");
    let mut bs = Vec::new();
    for r in def["bodies"].as_array().into_iter().flatten() {
        bs.push(body_of(s, st, r)?);
    }
    let copy = def["copy"].as_bool() == Some(true);
    let copies = def["copies"].as_u64().unwrap_or(1).max(1);
    let mut notes = Vec::new();
    let mut base = json!({"bodies": bs, "name": name, "copy": copy});
    if let Some(t) = v3(&def["translate"]) {
        base["translate"] = json!(t);
    } else if def["rotate"].is_object() {
        let r = &def["rotate"];
        let angles: Vec<f64> = r["angles"].as_array().into_iter().flatten().filter_map(Value::as_f64).collect();
        let nz: Vec<(usize, f64)> = angles.iter().copied().enumerate().filter(|(_, a)| a.abs() > 1e-12).collect();
        if nz.len() != 1 {
            bail!("a turn about more than one axis is not rebuilt by this version");
        }
        let mut ax = [0.0; 3];
        ax[nz[0].0] = 1.0;
        base["axis"] = json!(ax);
        let fid = f["id"].as_str().unwrap_or("").to_string();
        st.uncertain.insert(fid.clone());
        let a = if st.flips.contains(&fid) { -nz[0].1 } else { nz[0].1 };
        base["angle"] = json!(format!("{a} deg"));
        base["origin"] = json!(v3(&r["origin"]).unwrap_or([0.0; 3]));
        notes.push("turning sense checked by the part's shape".to_string());
    }
    if copy && copies > 1 {
        bail!("{copies} copies at once are not rebuilt by this version");
    }
    s.exec("solid.move", base)?;
    remember(st, f, &name);
    Ok(if notes.is_empty() { Built::Done } else { Built::Approx(notes.join("; ")) })
}

fn split(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let tools = def["tools"].as_array().cloned().unwrap_or_default();
    if tools.len() != 1 {
        bail!("a split by {} tools is not rebuilt by this version", tools.len());
    }
    let t = &tools[0];
    let all = bodies(s, st)?;
    if all.len() != 1 {
        bail!("splitting one of several bodies is not rebuilt by this version");
    }
    let mut p = json!({"body": all[0].0});
    if t["kind"] == "feature" && st.sketches.contains_key(t["id"].as_str().unwrap_or("")) {
        let (sk, cs) = curves_of(st, t)?;
        if cs.len() != 1 {
            bail!("a split by a sketch of {} curves is not rebuilt by this version", cs.len());
        }
        p["sketch"] = json!(sk);
        p["line"] = json!(cs[0]);
    } else {
        p["plane"] = plane(st, t)?;
    }
    s.exec("solid.split_body", p)?;
    Ok(Built::Done)
}

fn rib(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Rib");
    let (sk, cs) = curves_of(st, &def["profile"])?;
    let p = json!({"sketch": sk, "curves": cs, "thickness": format!("{} mm", num(&def["thickness"])),
                   "direction": if def["extrusion"] == "normalToSketch" { "normal" } else { "in_plane" },
                   "flip": def["flipMaterial"].as_bool() == Some(true), "name": name});
    s.exec("solid.rib", p)?;
    remember(st, f, &name);
    Ok(Built::Done)
}

// ---------------------------------------------------------------------------------------------------------
// sheet metal

/// The part's sheet-metal rule: thickness, bend radius and K-factor (sheet.manage_rules), made active.
fn sheet_rule(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    set_rule(s, st, num(&def["thickness"]), num(&def["bendRadius"]), def["kFactor"].as_f64())?;
    Ok(Built::Done)
}

fn set_rule(s: &mut Session, st: &mut PartState, thickness: f64, radius: f64, k: Option<f64>) -> Result<()> {
    if thickness <= 0.0 {
        return Ok(());
    }
    let name = format!("{} sheet", st.doc_name);
    let mut p = json!({"name": name, "thickness": format!("{thickness} mm"), "active": true});
    if radius > 0.0 {
        p["bend_radius"] = json!(format!("{radius} mm"));
    }
    if let Some(k) = k.filter(|k| *k > 0.0) {
        p["k_factor"] = json!(format!("{k}"));
    }
    s.exec("sheet.manage_rules", p)?;
    st.sheet_rule = Some((name, thickness));
    Ok(())
}

fn base_flange(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let thickness = num(&def["thickness"]);
    if st.sheet_rule.as_ref().map(|r| (r.1 - thickness).abs() > 1e-9).unwrap_or(true) {
        set_rule(s, st, thickness, num(&def["bendRadius"]), def["kFactor"].as_f64())?;
    }
    let name = name_of(st, f, "Base-Flange");
    let sk = st.sketches.get(def["profile"]["id"].as_str().unwrap_or("")).cloned().ok_or_else(|| anyhow!("its sketch was not built"))?;
    // closed profile: a plate; open profile: a contour flange as deep as `end.depth`
    let closed = def["end"].get("depth").is_none() || num(&def["end"]["depth"]) == 0.0;
    let fid = f["id"].as_str().unwrap_or("").to_string();
    st.uncertain.insert(fid.clone());
    let flip = (def["reverseThickness"].as_bool() == Some(true) || def["reverse"].as_bool() == Some(true)) ^ st.flips.contains(&fid);
    let mut p = if closed {
        json!({"type": "base", "base": {"sketch": sk.name}, "flip": flip, "name": name})
    } else {
        let (_, cs) = curves_of(st, &def["profile"])?;
        json!({"type": "contour", "contour": {"sketch": sk.name, "curves": cs, "distance": format!("{} mm", num(&def["end"]["depth"]))},
               "flip": flip, "name": name})
    };
    if let Some((rule, _)) = &st.sheet_rule {
        p["rule"] = json!(rule);
    }
    s.exec("sheet.flange", flatten_flange(p))?;
    remember(st, f, &name);
    Ok(Built::Approx("which side the thickness goes checked by the part's shape".into()))
}

/// sheet.flange takes its inputs at the top level (base: sketch, contour: sketch + curves + distance).
fn flatten_flange(p: Value) -> Value {
    let mut out = json!({});
    for (k, v) in p.as_object().cloned().unwrap_or_default() {
        if (k == "base" || k == "contour") && v.is_object() {
            for (kk, vv) in v.as_object().cloned().unwrap_or_default() {
                out[kk] = vv;
            }
        } else {
            out[k] = v;
        }
    }
    out
}

/// SolidCraft's position words for an edge flange (where the flange sits relative to the edge).
pub(crate) const FLANGE_POSITIONS: [&str; 5] = ["inside", "material_outside", "outside", "middle", "tangent"];

/// The position word for the file's (FORMAT-SPEC 4 `edgeFlange.position`).
pub(crate) fn flange_position(def: &Value) -> &'static str {
    match def["position"].as_str() {
        Some("materialOutside") => "material_outside",
        Some("bendOutside") => "outside",
        Some("bendCenterline") => "middle",
        Some("bendTangent") => "tangent",
        _ => "inside",
    }
}

pub(crate) fn edge_flange(s: &mut Session, st: &mut PartState, f: &Value, flip: bool) -> Result<Built> {
    edge_flange_at(s, st, f, flip, None)
}

/// An edge flange with its position word given (None: the file's).
pub(crate) fn edge_flange_at(s: &mut Session, st: &mut PartState, f: &Value, flip: bool, position: Option<&str>) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Edge-Flange");
    let mut edges = Vec::new();
    let all_edges: Vec<Value> = if def["edges"].as_array().map(|a| !a.is_empty()).unwrap_or(false) {
        def["edges"].as_array().cloned().unwrap_or_default()
    } else {
        vec![def["edge"].clone()]
    };
    for e in &all_edges {
        edges.push(pick(e).ok_or_else(|| anyhow!("its edge has no position"))?);
    }
    // "material inside" at an angle other than 90 degrees: the source runs the flange's outer face, extended, through
    // the edge, t*tan(angle - 90) further out than SolidCraft's "inside" (FORMAT-SPEC 4 edgeFlange.position), so the
    // sheet's end face is first moved by that much (back, below 90 degrees) and the flange made "inside" at the new
    // edge. Measured on the user's Rear AC Half Shell (45 degrees, t 1.5): same volume and bend axis as SolidWorks.
    let angle = num(&def["angle"]);
    let mut shifted = false;
    if position.is_none() && def["position"] == "materialInside" && (angle - 90.0).abs() > 1e-6 && edges.len() == 1 {
        if let Some((t, _)) = st.sheet_rule.clone().map(|r| (r.1, r.0)) {
            let delta = t * (angle - 90.0).to_radians().tan();
            if let Some((face_pt, n)) = end_face_at(s, st, edges[0])? {
                s.exec("solid.offset_face", json!({"faces": [face_pt], "distance": format!("{delta} mm")}))?;
                edges[0] = [edges[0][0] + n[0] * delta, edges[0][1] + n[1] * delta, edges[0][2] + n[2] * delta];
                shifted = true;
            }
        }
    }
    let len = &def["length"];
    let mut p = json!({"type": "edge", "edges": edges, "height": format!("{} mm", num(&len["distance"])),
                       "angle": format!("{} deg", num(&def["angle"])), "name": name});
    p["height_from"] = json!(match len["datum"].as_str() {
        Some("innerVirtualSharp") => "inner",
        Some("bendTangent") => "tangent",
        _ => "outer",
    });
    p["position"] = json!(if shifted { "inside" } else { position.unwrap_or_else(|| flange_position(def)) });
    if def["useDefaultRadius"].as_bool() == Some(false) {
        p["radius"] = json!(format!("{} mm", num(&def["bendRadius"])));
    }
    if flip || len["reverse"].as_bool() == Some(true) {
        p["flip"] = json!(true);
    }
    if def["positionOffset"].is_object() && num(&def["positionOffset"]["distance"]) != 0.0 {
        bail!("a flange offset along its face is not rebuilt by this version");
    }
    // the part's own rule, named (the active rule changes as later parts are built, and a rebuild would follow it)
    if let Some((rule, _)) = &st.sheet_rule {
        let mut with_rule = p.clone();
        with_rule["rule"] = json!(rule);
        if s.exec("sheet.flange", with_rule).is_ok() {
            remember(st, f, &name);
            return Ok(Built::Done);
        }
    }
    s.exec("sheet.flange", p)?;
    remember(st, f, &name);
    Ok(Built::Done)
}

/// The sheet's end face at an edge (the thinner of the two faces the edge joins): a point on it and its outward
/// normal. None where there is no such face.
fn end_face_at(s: &mut Session, st: &PartState, at: [f64; 3]) -> Result<Option<([f64; 3], [f64; 3])>> {
    let m = s.tool("measure", json!({"names_only": true}))?;
    let names: Vec<String> = m["bodies"].as_array().into_iter().flatten()
        .filter_map(|b| b["name"].as_str().map(String::from)).filter(|n| !st.exclude.contains(n)).collect();
    let mut best: Option<(f64, String, Value)> = None;
    for n in &names {
        let t = s.tool("body_topology", json!({"body": n}))?;
        for e in t["edges"].as_array().into_iter().flatten() {
            let (Some(a), Some(b)) = (v3(&e["start"]), v3(&e["end"])) else { continue };
            let ab = geom::sub(b, a);
            let l2 = geom::dot(ab, ab).max(1e-18);
            let u = (geom::dot(geom::sub(at, a), ab) / l2).clamp(0.0, 1.0);
            let d = geom::norm(geom::sub(at, geom::add(a, geom::scale(ab, u))));
            if best.as_ref().map(|x| d < x.0).unwrap_or(true) {
                best = Some((d, n.clone(), json!({"edge": e, "topo": t})));
            }
        }
    }
    let Some((d, body, x)) = best else { return Ok(None) };
    if d > 0.01 {
        return Ok(None);
    }
    let ename = x["edge"]["name"].as_str().unwrap_or("").to_string();
    // the two face names, split at the top-level '|'
    let mut faces = Vec::new();
    let (mut depth, mut cur) = (0i32, String::new());
    for c in ename.chars() {
        match c {
            '(' => depth += 1,
            ')' => depth -= 1,
            '|' if depth == 0 => {
                faces.push(std::mem::take(&mut cur));
                continue;
            }
            _ => {}
        }
        cur.push(c);
    }
    faces.push(cur);
    let thin = x["topo"]["faces"].as_array().into_iter().flatten()
        .filter(|f| faces.iter().any(|n| f["name"] == n.as_str()) && f["plane_normal"].is_array())
        .min_by(|a, b| a["area"].as_f64().unwrap_or(0.0).partial_cmp(&b["area"].as_f64().unwrap_or(0.0)).unwrap_or(std::cmp::Ordering::Equal))
        .cloned();
    let Some(f) = thin else { return Ok(None) };
    let Some(pt) = v3(&f["point"]) else { return Ok(None) };
    let r = s.exec("inspect.classify_points", json!({"body": body, "points": [pt]}))?;
    Ok(v3(&r["results"][0]["normal"]).map(|n| (pt, geom::unit(n))))
}

fn convert_sheet(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    set_rule(s, st, num(&def["thickness"]), num(&def["bendRadius"]), None)?;
    let all = bodies(s, st)?;
    if all.len() != 1 {
        bail!("converting one of several bodies is not rebuilt by this version");
    }
    let face = pick(&def["fixedFace"]).ok_or_else(|| anyhow!("its fixed face has no position"))?;
    let bends = picks(def, "bendEdges")?;
    let mut p = json!({"body": all[0].0, "face": face, "direction": if def["reverse"].as_bool() == Some(true) { "outside" } else { "inside" }});
    if !bends.is_empty() {
        p["bends"] = json!(bends);
    }
    if let Some((rule, _)) = &st.sheet_rule {
        p["rule"] = json!(rule);
    }
    s.exec("sheet.convert", p)?;
    Ok(Built::Done)
}

// ---------------------------------------------------------------------------------------------------------
// weldment members

/// A SolidWorks profile ("square tube", "20 x 20 x 2") as one of SolidCraft's built-in kinds.
fn profile_of(pr: &Value) -> Result<Value> {
    let ty = pr["type"].as_str().unwrap_or("").to_lowercase();
    let nums: Vec<f64> = pr["size"]
        .as_str()
        .unwrap_or("")
        .split(|c: char| !(c.is_ascii_digit() || c == '.'))
        .filter_map(|x| x.parse::<f64>().ok())
        .collect();
    let mm = |v: f64| json!(format!("{v} mm"));
    Ok(match (ty.as_str(), nums.as_slice()) {
        (t, [w, h, wall, ..]) if t.contains("square tube") || t.contains("rectangular tube") || t.contains("rect") => {
            json!({"kind": "rect_tube", "width": mm(*w), "height": mm(*h), "wall": mm(*wall)})
        }
        (t, [w, wall]) if t.contains("square tube") => json!({"kind": "rect_tube", "width": mm(*w), "height": mm(*w), "wall": mm(*wall)}),
        (t, [d, wall, ..]) if t.contains("pipe") || t.contains("round tube") => json!({"kind": "round_tube", "diameter": mm(*d), "wall": mm(*wall)}),
        (t, [w, h, th, ..]) if t.contains("angle") => json!({"kind": "angle", "width": mm(*w), "height": mm(*h), "thickness": mm(*th)}),
        (t, [w, h]) if t.contains("rect") || t.contains("bar") => json!({"kind": "bar", "width": mm(*w), "height": mm(*h)}),
        (t, [d]) if t.contains("round") => json!({"kind": "round_bar", "diameter": mm(*d)}),
        _ => bail!("the profile {} {} has no counterpart here", pr["type"].as_str().unwrap_or("?"), pr["size"].as_str().unwrap_or("?")),
    })
}

fn member(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let profile = profile_of(&def["profile"])?;
    let groups = def["groups"].as_array().cloned().unwrap_or_default();
    let mut made = 0;
    let mut notes = Vec::new();
    for (gi, g) in groups.iter().enumerate() {
        let segs = g["segments"].as_array().cloned().unwrap_or_default();
        let sk_id = segs.first().and_then(|r| r["feature"].as_str()).unwrap_or("").to_string();
        let sk = st.sketches.get(&sk_id).cloned().ok_or_else(|| anyhow!("its path sketch was not built"))?;
        let mut lines = Vec::new();
        for r in &segs {
            let e = sk.ents.get(r["id"].as_str().unwrap_or("")).ok_or_else(|| anyhow!("a path segment was not drawn"))?;
            lines.push(e.sc.clone());
        }
        let base = f["name"].as_str().unwrap_or("Member");
        let name = st.unique_name(&if groups.len() > 1 { format!("{base} ({})", gi + 1) } else { base.to_string() });
        let corner = match g["cornerTreatment"].as_str().unwrap_or("none") {
            "miter" => "mitre",
            "butt1" | "butt2" => "butt",
            _ => "none",
        };
        let mut p = json!({"sketch": sk.name, "lines": lines, "profile": profile, "corner": corner, "name": name,
                           "angle": format!("{} deg", num(&g["angle"]))});
        if g["cornerTreatment"] == "butt2" {
            p["flip"] = json!(true);
        }
        let gap = num(&g["gapWithinGroup"]);
        if gap > 0.0 {
            p["gap"] = json!(format!("{gap} mm"));
        }
        if def["connectedSegments"] == "copedCut" {
            p["coped"] = json!(true);
        }
        s.exec("frame.member", p)?;
        made += 1;
        if gi == 0 {
            remember(st, f, &name);
        }
    }
    notes.push(format!("profile from SolidCraft's own kinds ({})", profile["kind"].as_str().unwrap_or("?")));
    let _ = made;
    Ok(Built::Approx(notes.join("; ")))
}

// ---------------------------------------------------------------------------------------------------------
// surfaces

fn surface_extrude(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Surface-Extrude");
    let (sk, cs) = curves_of(st, &def["profile"])?;
    let dir = if def["reverse"].as_bool() == Some(true) { "negative" } else { "positive" };
    let depth = num(&def["end"]["depth"]);
    let mut p = json!({"sketch": sk, "curves": cs, "distance": format!("{depth} mm"), "direction": dir, "name": name});
    if def["end"]["type"] == "midPlane" {
        p["direction"] = json!("symmetric");
    }
    s.exec("surface.extrude", p)?;
    remember(st, f, &name);
    Ok(Built::Done)
}

fn surface_revolve(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Surface-Revolve");
    let (sk, cs) = curves_of(st, &def["profile"])?;
    let ax = &def["axis"];
    let axis_sk = st.sketches.get(ax["feature"].as_str().unwrap_or("")).ok_or_else(|| anyhow!("its axis sketch was not built"))?;
    let axis_line = axis_sk.ents.get(ax["id"].as_str().unwrap_or("")).ok_or_else(|| anyhow!("its axis line was not drawn"))?.sc.clone();
    let curves: Vec<String> = cs.into_iter().filter(|c| *c != axis_line).collect();
    let p = json!({"sketch": sk, "curves": curves, "axis": axis_line, "angle": format!("{} deg", num(&def["angle"])), "name": name});
    s.exec("surface.revolve", p)?;
    remember(st, f, &name);
    Ok(Built::Done)
}

fn planar_surface(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let name = name_of(st, f, "Surface-Plane");
    let sk = sketch_name(st, &def["profile"])?;
    let r = s.exec("surface.patch", json!({"sketch": sk, "name": name}))?;
    st.surface_bodies.insert(f["id"].as_str().unwrap_or("").to_string(), created(&r));
    remember(st, f, &name);
    Ok(Built::Done)
}

fn created(r: &Value) -> Vec<String> {
    r["created_bodies"].as_array().into_iter().flatten().filter_map(|b| b.as_str().map(String::from)).collect()
}

fn knit(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let mut bodies: Vec<String> = Vec::new();
    for r in def["surfaces"].as_array().into_iter().flatten() {
        let id = r["id"].as_str().unwrap_or("");
        let b = st.surface_bodies.get(id).cloned().ok_or_else(|| anyhow!("a surface it knits was not built"))?;
        bodies.extend(b);
    }
    if bodies.len() < 2 {
        bail!("fewer than two surfaces to knit");
    }
    s.exec("surface.stitch", json!({"bodies": bodies}))?;
    Ok(Built::Done)
}

#[allow(dead_code)]
fn unused(_: HashMap<String, String>) {}
