//! Sheet-metal and weldment features SolidCraft has under other names or not at all, rebuilt as their functional
//! equivalent (FORMAT-SPEC §4; the user's rule: the destination's own feature where it exists, else the feature
//! that makes the same shape, else the carried shape):
//!
//! - `hem` → `sheet.hem` (closed and open hems; teardrop and rolled hems keep their carried shape);
//! - `sketchedBend` → `sheet.fold` on the sketch's line;
//! - `jog` → two `sheet.fold`s (the jog's two bends, at the bend axes the file records);
//! - `breakCorner` → a chamfer or fillet on the corner's edges;
//! - `endCap` → `frame.end_cap`.
//!
//! Where SolidCraft's placement words differ from the source's (where a fold's line sits in the bend, which side
//! stays), each builder has variants: the importer tries them in turn until one gives the carried result.

use anyhow::{anyhow, bail, Result};
use serde_json::{json, Value};

use super::features::{pick, Built};
use super::{num, PartState};
use crate::geom::{self, v3, V3};
use crate::sc::Session;

/// How many placement variants a feature type has (variant 0 is the file's own words).
pub fn variant_count(kind: &str) -> usize {
    match kind {
        "hem" => 2,
        "sketchedBend" => 8,
        "jog" => 4,
        _ => 1,
    }
}

pub fn build(s: &mut Session, st: &mut PartState, f: &Value, variant: usize) -> Result<Option<Built>> {
    let r = match f["type"].as_str().unwrap_or("") {
        "hem" => hem(s, st, f, variant)?,
        "sketchedBend" => sketched_bend(s, st, f, variant)?,
        "jog" => jog(s, st, f, variant)?,
        "breakCorner" => break_corner(s, st, f)?,
        "endCap" => end_cap(s, st, f)?,
        "weldmentTrim" => weldment_trim(s, st, f)?,
        _ => return Ok(None),
    };
    Ok(Some(r))
}

fn name_of(st: &mut PartState, f: &Value, default: &str) -> String {
    st.unique_name(f["name"].as_str().unwrap_or(default))
}

fn remember(st: &mut PartState, f: &Value, name: &str) {
    st.names.insert(f["id"].as_str().unwrap_or("").to_string(), name.to_string());
}

fn mm(v: f64) -> String {
    format!("{v} mm")
}

/// The part's own sheet-metal rule, named, where it has one.
fn with_rule(st: &PartState, mut p: Value) -> Value {
    if let Some((rule, _)) = &st.sheet_rule {
        p["rule"] = json!(rule);
    }
    p
}

fn hem(s: &mut Session, st: &mut PartState, f: &Value, variant: usize) -> Result<Built> {
    let def = &f["def"];
    match def["hemType"].as_str().unwrap_or("closed") {
        "closed" | "open" => {}
        other => bail!("a {other} hem is not rebuilt by this version"),
    }
    let mut edges = Vec::new();
    for e in def["edges"].as_array().into_iter().flatten() {
        edges.push(pick(e).ok_or_else(|| anyhow!("a hem edge has no position"))?);
    }
    if edges.is_empty() {
        bail!("no edges");
    }
    let name = name_of(st, f, "Hem");
    let mut p = json!({"edges": edges, "length": mm(num(&def["length"])), "name": name});
    // the gap between the leg and the face it folds over (a closed hem's is SolidWorks' tiny gap)
    let gap = num(&def["gap"]);
    if gap > 0.0 {
        p["gap"] = json!(mm(gap));
    }
    if def["reverse"].as_bool().unwrap_or(false) ^ (variant == 1) {
        p["flip"] = json!(true);
    }
    s.exec("sheet.hem", p)?;
    remember(st, f, &name);
    Ok(if variant == 0 { Built::Approx("as SolidCraft's hem (length to the outer face): checked by the carried result".into()) } else {
        Built::Approx("as SolidCraft's hem, folded the other way from the file's words: matches its carried shape".into())
    })
}

/// The line of a sketched bend or jog: (sketch name, curve id) of the sketch's one (non-construction) line.
fn bend_line(st: &PartState, def: &Value) -> Result<(String, String)> {
    let id = def["line"]["id"].as_str().ok_or_else(|| anyhow!("its bend line names no sketch"))?;
    let sk = st.sketches.get(id).ok_or_else(|| anyhow!("its bend line's sketch was not built"))?;
    let lines: Vec<&super::sketch::Ent> = sk.ents.values().filter(|e| e.kind == "line").collect();
    match lines.as_slice() {
        [l] => Ok((sk.name.clone(), l.sc.clone())),
        _ => bail!("its sketch has {} lines (one bend line expected)", lines.len()),
    }
}

const FOLD_POSITIONS: [&str; 4] = ["centerline", "mould", "start", "end"];

/// The fold position SolidCraft calls the file's word (the line's place in the bend).
fn fold_position(word: &str) -> &'static str {
    match word {
        "bendCenterline" => "centerline",
        "materialInside" | "materialOutside" => "mould",
        "bendOutside" => "end",
        _ => "centerline",
    }
}

fn sketched_bend(s: &mut Session, st: &mut PartState, f: &Value, variant: usize) -> Result<Built> {
    let def = &f["def"];
    let (sketch, curve) = bend_line(st, def)?;
    let own = fold_position(def["position"].as_str().unwrap_or(""));
    // variants: the file's word, then the others; each both ways round
    let mut order = vec![own];
    order.extend(FOLD_POSITIONS.iter().copied().filter(|p| *p != own));
    let pos = order[(variant / 2) % order.len()];
    let flip = def["reverse"].as_bool().unwrap_or(false) ^ (variant % 2 == 1);
    let name = name_of(st, f, "Sketched Bend");
    let mut p = json!({"sketch": sketch, "curve": curve, "angle": format!("{} deg", num(&def["angle"])), "position": pos, "name": name});
    if def["useDefaultRadius"].as_bool() == Some(false) {
        p["radius"] = json!(mm(num(&def["radius"])));
    }
    if let Some(fx) = pick(&def["fixedFace"]) {
        p["fixed"] = json!(fx);
    }
    if flip {
        p["flip"] = json!(true);
    }
    s.exec("sheet.fold", with_rule(st, p))?;
    remember(st, f, &name);
    Ok(Built::Approx(format!("as a SolidCraft fold (line at the bend's {pos}{}): checked by the carried result", if flip { ", folded the other way" } else { "" })))
}

/// A jog: two folds. The file records both bends' axes; each fold's line is where its bend's axis meets the flat
/// sheet before that fold (the axis moved onto the sheet's face, centre of the bend), the second at the jog's
/// offset, folded back the other way.
fn jog(s: &mut Session, st: &mut PartState, f: &Value, variant: usize) -> Result<Built> {
    let def = &f["def"];
    let bends: Vec<&Value> = def["bendGeometry"].as_array().into_iter().flatten().collect();
    if bends.len() != 2 {
        bail!("a jog without its two bends' axes is not rebuilt by this version");
    }
    let (sketch, curve) = bend_line(st, def)?;
    let angle = num(&def["angle"]);
    let r = if def["useDefaultRadius"].as_bool() == Some(false) { num(&def["radius"]) } else { num(&bends[0]["innerRadius"]) };
    let name = name_of(st, f, "Jog");
    let flip = variant % 2 == 1;
    let pos = if variant / 2 == 0 { "centerline" } else { "mould" };
    // the first bend on the sketch's line
    let mut p = json!({"sketch": sketch, "curve": curve, "angle": format!("{angle} deg"), "position": pos, "name": format!("{name} (1)")});
    p["radius"] = json!(mm(r));
    if let Some(fx) = pick(&def["fixedFace"]) {
        p["fixed"] = json!(fx);
    }
    if flip {
        p["flip"] = json!(true);
    }
    s.exec("sheet.fold", with_rule(st, p))?;
    // the second: a line on the sheet's face across the jog's leg, at the second bend's axis
    let ax = &bends[1]["axis"];
    let (Some(o), Some(d)) = (v3(&ax["point"]), v3(&ax["direction"])) else { bail!("its second bend has no axis") };
    let d = geom::unit(d);
    // not found: the first fold went the other way (the shape then differs and the next variant is tried)
    let Ok(line) = face_line_at(s, st, o, d, r) else {
        remember(st, f, &format!("{name} (1)"));
        return Ok(Built::Approx("first fold only".into()));
    };
    let mut q = json!({"points": line, "angle": format!("{angle} deg"), "position": pos, "name": format!("{name} (2)")});
    q["radius"] = json!(mm(r));
    if let Some(fx) = pick(&def["fixedFace"]) {
        q["fixed"] = json!(fx);
    }
    if !flip {
        q["flip"] = json!(true);
    }
    s.exec("sheet.fold", with_rule(st, q))?;
    remember(st, f, &format!("{name} (1)"));
    Ok(Built::Approx("as two SolidCraft folds (SolidCraft has no jog): checked by the carried result".into()))
}

/// Two points of a line on the sheet's face where a bend of inner radius r about the axis (o, d) will be: the
/// axis moved onto the nearest face of the sheet, across the sheet's width.
fn face_line_at(s: &mut Session, st: &PartState, o: V3, d: V3, r: f64) -> Result<Vec<V3>> {
    let m = s.tool("measure", json!({"names_only": true}))?;
    let names: Vec<String> = m["bodies"].as_array().into_iter().flatten()
        .filter_map(|b| b["name"].as_str().map(String::from)).filter(|n| !st.exclude.contains(n)).collect();
    for n in &names {
        // the sheet's extent along the axis, from its box
        let t = s.tool("body_topology", json!({"body": n}))?;
        let mut lo = f64::INFINITY;
        let mut hi = f64::NEG_INFINITY;
        for e in t["edges"].as_array().into_iter().flatten() {
            for k in ["start", "end"] {
                if let Some(p) = v3(&e[k]) {
                    let x = geom::dot(geom::sub(p, o), d);
                    lo = lo.min(x);
                    hi = hi.max(x);
                }
            }
        }
        if !lo.is_finite() {
            continue;
        }
        // the face nearest the axis, r away from it: try the directions across the axis
        for k in 0..16 {
            let a = k as f64 * std::f64::consts::PI / 8.0;
            let (u, v) = perp_basis(d);
            let dir = geom::add(geom::scale(u, a.cos()), geom::scale(v, a.sin()));
            let at = geom::add(o, geom::scale(dir, r));
            let res = s.exec("inspect.classify_points", json!({"body": n, "points": [at]}))?;
            if res["results"][0]["class"] == "on" {
                let mid = (lo + hi) / 2.0;
                let half = (hi - lo) / 2.0 * 0.9;
                return Ok(vec![geom::add(at, geom::scale(d, mid - half)), geom::add(at, geom::scale(d, mid + half))]);
            }
        }
    }
    bail!("the jog's second bend line was not found on the sheet")
}

/// Two unit vectors square to d and to each other.
fn perp_basis(d: V3) -> (V3, V3) {
    let a = if d[0].abs() < 0.9 { [1.0, 0.0, 0.0] } else { [0.0, 1.0, 0.0] };
    let u = geom::unit(geom::cross(d, a));
    (u, geom::unit(geom::cross(d, u)))
}

fn break_corner(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let mut edges = Vec::new();
    for e in def["entities"].as_array().into_iter().flatten() {
        if e["kind"] == "edge" {
            edges.push(pick(e).ok_or_else(|| anyhow!("a corner edge has no position"))?);
        }
    }
    if edges.is_empty() {
        bail!("its corners are not edges (faces are not rebuilt by this version)");
    }
    let name = name_of(st, f, "Break Corner");
    match def["breakType"].as_str().unwrap_or("chamfer") {
        "chamfer" => {
            s.exec("solid.chamfer", json!({"edges": edges, "distance": mm(num(&def["distance"])), "tangent_chain": false, "name": name}))?;
        }
        _ => {
            let r = if def["radius"].is_number() { num(&def["radius"]) } else { num(&def["distance"]) };
            s.exec("solid.fillet", json!({"edges": edges, "radius": mm(r), "name": name}))?;
        }
    }
    remember(st, f, &name);
    Ok(Built::Approx("as a chamfer or fillet on the sheet's corner edges (SolidCraft has no break corner)".into()))
}

/// A weldment trim: each trimmed member cut by the boundary members (kept), and the pieces of it beyond them
/// removed (the largest piece stays: the member itself).
fn weldment_trim(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    use super::features2::{bodies, body_of};
    let def = &f["def"];
    let mut tools = Vec::new();
    for r in def["boundary"].as_array().into_iter().flatten() {
        tools.push(body_of(s, st, r)?);
    }
    let mut trimmed = Vec::new();
    for r in def["trimmed"].as_array().into_iter().flatten() {
        trimmed.push(body_of(s, st, r)?);
    }
    if tools.is_empty() || trimmed.is_empty() {
        bail!("no members to trim, or nothing to trim them to");
    }
    let name = name_of(st, f, "Trim");
    for t in &trimmed {
        let before: Vec<String> = bodies(s, st)?.into_iter().map(|b| b.0).collect();
        s.exec("solid.combine", json!({"target": t, "tools": tools, "operation": "cut", "keep_tools": true, "touching": "separate", "name": name}))?;
        let after = bodies(s, st)?;
        // the trimmed member's pieces: the new bodies and the one that kept its name
        let mut pieces: Vec<(String, f64)> = after.iter().filter(|(n, _)| n == t || !before.contains(n))
            .map(|(n, fp)| (n.clone(), num(&fp["volume"]))).collect();
        pieces.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
        let stubs: Vec<String> = pieces.iter().skip(1).map(|p| p.0.clone()).collect();
        if !stubs.is_empty() {
            s.exec("solid.remove", json!({"bodies": stubs}))?;
        }
    }
    remember(st, f, &name);
    Ok(Built::Approx("as the member cut by the members it is trimmed to (SolidCraft trims only its own frame's corners)".into()))
}

fn end_cap(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Built> {
    let def = &f["def"];
    let face = pick(&def["face"]).ok_or_else(|| anyhow!("its face has no position"))?;
    let name = name_of(st, f, "End Cap");
    // a plain plate over the member's end, inset from its outer box: SolidWorks' end cap without corner treatment
    // has square corners where SolidCraft's follows the profile's rounds (an end face square to an axis)
    if !def["cornerTreatment"].as_bool().unwrap_or(false) && def["offsetByRatio"].as_bool() != Some(true) && def["reverse"].as_bool() != Some(true) {
        if let (Some(n), Some(bb)) = (v3(&def["face"]["geom"]["params"]["normal"]), def["face"]["geom"]["bbox"].as_array()) {
            let bb: Vec<f64> = bb.iter().filter_map(Value::as_f64).collect();
            let axis = (0..3).find(|&i| (n[i].abs() - 1.0).abs() < 1e-9);
            if let (Some(ax), 6) = (axis, bb.len()) {
                let (inset, depth) = (num(&def["offset"]), num(&def["depth"]));
                let mut lo = [bb[0], bb[1], bb[2]];
                let mut hi = [bb[3], bb[4], bb[5]];
                for i in 0..3 {
                    if i == ax {
                        if n[i] > 0.0 { hi[i] = lo[i] + depth } else { lo[i] = hi[i] - depth }
                    } else {
                        lo[i] += inset;
                        hi[i] -= inset;
                    }
                }
                let p = json!({"length": mm(hi[0] - lo[0]), "width": mm(hi[1] - lo[1]), "height": mm(hi[2] - lo[2]), "corner": lo,
                               "operation": "new", "name": name});
                if s.exec("solid.box", p).is_ok() {
                    remember(st, f, &name);
                    return Ok(Built::Approx("as a plate over the member's end (square corners, as the source's end cap)".into()));
                }
            }
        }
    }
    let pickj = face_pick(s, st, face)?;
    let mut p = json!({"faces": [pickj], "thickness": mm(num(&def["depth"])), "name": name});
    if def["offsetByRatio"].as_bool() == Some(true) {
        p["ratio"] = json!(num(&def["ratio"]));
    } else if num(&def["offset"]) != 0.0 {
        p["offset"] = json!(mm(num(&def["offset"])));
    }
    if def["chamfer"].as_bool() == Some(true) && num(&def["chamferDistance"]) > 0.0 {
        p["chamfer"] = json!(mm(num(&def["chamferDistance"])));
    }
    if def["reverse"].as_bool() == Some(true) {
        p["direction"] = json!("inward");
    }
    s.exec("frame.end_cap", p)?;
    remember(st, f, &name);
    Ok(Built::Done)
}

/// A face pick as SolidCraft's view makes it: the point, its body and the face's index in the body's topology.
fn face_pick(s: &mut Session, st: &PartState, at: V3) -> Result<Value> {
    let m = s.tool("measure", json!({"names_only": true}))?;
    let names: Vec<String> = m["bodies"].as_array().into_iter().flatten()
        .filter_map(|b| b["name"].as_str().map(String::from)).filter(|n| !st.exclude.contains(n)).collect();
    for n in &names {
        let r = s.exec("inspect.classify_points", json!({"body": n, "points": [at]}))?;
        let hit = &r["results"][0];
        if hit["class"] != "on" {
            continue;
        }
        let Some(fname) = hit["face"].as_str() else { continue };
        let t = s.tool("body_topology", json!({"body": n}))?;
        if let Some(i) = t["faces"].as_array().and_then(|a| a.iter().position(|f| f["name"] == fname)) {
            return Ok(json!({"face": at, "body": n, "index": i}));
        }
    }
    bail!("its face was not found")
}
