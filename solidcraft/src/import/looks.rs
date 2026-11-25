//! Materials and appearances (FORMAT-SPEC §4 "Materials", "Appearances"): what a part is made of and what it
//! looks like. They are not features, so they are applied to a part's bodies once its shape is in place, whether
//! it was rebuilt or brought in as its geometry.
//!
//! - A material is used by name when SolidCraft has one of that name; otherwise it is recreated from the file's
//!   `definition` under the source's own name (its density and colour), so "AISI 304" stays AISI 304. A material
//!   with neither is reported.
//! - A colour goes on its target: the whole part (`document`), a body (found by name, or by volume and centroid
//!   for a `geom` reference), or a face (picked at its descriptor's middle sample). Textured library appearances
//!   are SolidWorks' own files: their colour is used where the file gives one, else they are reported.

use std::collections::HashSet;

use anyhow::Result;
use serde_json::{json, Value};

use super::features::pick;
use crate::geom::{self, v3};
use crate::report::{Report, Status};
use crate::sc::{self, Session};

/// Materials already made in this design (one per name).
#[derive(Default)]
pub struct Made {
    pub materials: HashSet<String>,
    pub builtin: Option<Vec<String>>,
}

/// The bodies of a component (its definition's names).
fn bodies_of(s: &mut Session, component: &Value) -> Result<Vec<String>> {
    let list = s.exec("component.list", json!({"compact": true, "bodies": true}))?;
    let c = list["components"].as_array().and_then(|a| a.iter().find(|c| &c["id"] == component).cloned());
    Ok(c.and_then(|c| c["bodies"].as_array().cloned())
        .unwrap_or_default()
        .iter()
        .filter_map(|b| b.as_str().map(String::from))
        .collect())
}

fn rgb255(c: &Value) -> Option<[u8; 3]> {
    let a = c.as_array()?;
    if a.len() < 3 {
        return None;
    }
    let f = |v: &Value| (v.as_f64().unwrap_or(0.0).clamp(0.0, 1.0) * 255.0).round() as u8;
    Some([f(&a[0]), f(&a[1]), f(&a[2])])
}

/// Applies a part document's material and colours to its component's bodies.
pub fn apply(s: &mut Session, doc: &Value, component: &Value, made: &mut Made, report: &mut Report) -> Result<()> {
    let name = doc["name"].as_str().unwrap_or("Part").to_string();
    let has_material = doc["materials"].as_array().map(|m| !m.is_empty()).unwrap_or(false);
    let has_looks = doc["appearances"].as_array().map(|m| !m.is_empty()).unwrap_or(false);
    if !has_material && !has_looks {
        return Ok(());
    }
    let bodies = bodies_of(s, component)?;
    if bodies.is_empty() {
        return Ok(());
    }
    if has_material {
        if let Err(e) = material(s, doc, &bodies, made, report, &name) {
            if sc::is_cancelled(&e) {
                return Err(e);
            }
            report.add(&name, "material", "material", Status::Failed, format!("{e:#}"));
        }
    }
    if has_looks {
        let mut done = 0;
        let mut skipped: Vec<String> = Vec::new();
        for a in doc["appearances"].as_array().into_iter().flatten() {
            match appearance(s, a, &bodies) {
                Ok(true) => done += 1,
                Ok(false) => skipped.push(describe(a)),
                Err(e) if sc::is_cancelled(&e) => return Err(e),
                Err(e) => skipped.push(format!("{} ({})", describe(a), one_line(&e.to_string()))),
            }
        }
        if done > 0 {
            report.add(&name, "colours", "appearance", Status::Rebuilt, format!("{done} applied"));
        }
        if !skipped.is_empty() {
            report.add(&name, "colours", "appearance", Status::Approximate, format!("not carried: {}", skipped.join("; ")));
        }
    }
    Ok(())
}

fn one_line(s: &str) -> String {
    let mut t = s.lines().next().unwrap_or("").to_string();
    t.truncate(140);
    t
}

fn describe(a: &Value) -> String {
    match a["kind"].as_str() {
        Some("appearance") => format!("appearance {}", a["file"].as_str().unwrap_or("?")),
        _ => format!("colour on {}", a["target"]["kind"].as_str().unwrap_or("?")),
    }
}

fn material(s: &mut Session, doc: &Value, bodies: &[String], made: &mut Made, report: &mut Report, part: &str) -> Result<()> {
    let mats = doc["materials"].as_array().cloned().unwrap_or_default();
    let active = doc["activeConfiguration"].as_str().unwrap_or("");
    let active_name = doc["configurations"]
        .as_array()
        .and_then(|c| c.iter().find(|c| c["id"] == active))
        .and_then(|c| c["name"].as_str())
        .unwrap_or("");
    let m = mats
        .iter()
        .find(|m| m["configuration"] == active || m["configuration"] == active_name)
        .or(mats.first())
        .cloned()
        .unwrap_or(Value::Null);
    let src_name = m["name"].as_str().unwrap_or("").to_string();
    if src_name.is_empty() {
        return Ok(());
    }
    if made.builtin.is_none() {
        let list = s.exec("material.list", json!({}))?;
        made.builtin = Some(
            list["materials"].as_array().into_iter().flatten().filter_map(|m| m["name"].as_str().map(String::from)).collect(),
        );
    }
    let known = made.builtin.as_ref().and_then(|b| b.iter().find(|n| n.eq_ignore_ascii_case(&src_name)).cloned());
    let (use_name, how) = if let Some(n) = known {
        (n, "SolidCraft's own".to_string())
    } else if made.materials.contains(&src_name) {
        (src_name.clone(), "recreated from the file".to_string())
    } else {
        let def = &m["definition"];
        let density = def["density"].as_f64().filter(|d| *d > 0.0);
        let Some(density) = density else {
            // the functional equivalent: SolidCraft's material of that kind (another system's name for it)
            if let Some(n) = nearest_material(&src_name) {
                s.exec("material.assign", json!({"bodies": bodies, "material": n}))?;
                report.add(part, &src_name, "material", Status::Approximate,
                           format!("SolidCraft has no material of this name and the file does not define it: {n} used, the nearest kind"));
            } else {
                report.add(part, &src_name, "material", Status::Failed, "SolidCraft has no material of this name and the file does not define it");
            }
            return Ok(());
        };
        let mut p = json!({"name": src_name, "density_g_cm3": density / 1000.0});
        if let Some(c) = rgb255(&def["color"]) {
            p["color"] = json!(c);
        }
        s.exec("material.create", p)?;
        made.materials.insert(src_name.clone());
        let partial = if def["complete"].as_bool() == Some(false) { ", density and colour only" } else { "" };
        (src_name.clone(), format!("recreated from the file ({:.3} g/cm³{partial})", density / 1000.0))
    };
    s.exec("material.assign", json!({"bodies": bodies, "material": use_name}))?;
    report.add(part, &src_name, "material", Status::Rebuilt, how);
    Ok(())
}

/// SolidCraft's built-in material of the same kind as another system's material name (FORMAT-SPEC 4 Materials:
/// "the importer's nearest material"), by the words such names use.
fn nearest_material(name: &str) -> Option<&'static str> {
    let n = name.to_lowercase();
    let has = |w: &[&str]| w.iter().any(|x| n.contains(x));
    Some(if has(&["stainless", "aisi 3", "aisi 4", "316", "304", "inox"]) {
        "Stainless Steel"
    } else if has(&["alumin", "6061", "6082", "7075", "5083", "2024", "1060", "3003", "5052"]) {
        "Aluminum"
    } else if has(&["cast iron", "grey iron", "gray iron", "ductile iron", "malleable"]) {
        "Cast Iron"
    } else if has(&["titan"]) {
        "Titanium"
    } else if has(&["brass"]) {
        "Brass"
    } else if has(&["copper", "bronze"]) {
        "Copper"
    } else if has(&["steel", "aisi", "s235", "s275", "s355", "en8", "en24", "mild", "carbon"]) {
        "Steel"
    } else if has(&["abs"]) {
        "ABS Plastic"
    } else if has(&["pla"]) && !has(&["plate", "plastic"]) {
        "PLA"
    } else if has(&["nylon", "polyamide", "pa6", "pa66"]) {
        "Nylon"
    } else if has(&["polycarbonate", "pc "]) || n == "pc" {
        "Polycarbonate"
    } else if has(&["oak", "wood", "timber", "beech", "pine", "ash"]) {
        "Oak"
    } else if has(&["glass"]) {
        "Glass"
    } else {
        return None;
    })
}

/// One appearance; false when it has no counterpart here.
fn appearance(s: &mut Session, a: &Value, bodies: &[String]) -> Result<bool> {
    let Some(color) = rgb255(&a["color"]) else { return Ok(false) };
    let mut p = json!({"color": color});
    if let Some(t) = a["optical"]["transparency"].as_f64() {
        if t > 0.0 {
            p["opacity"] = json!((1.0 - t).clamp(0.0, 1.0));
        }
    }
    let t = &a["target"];
    match t["kind"].as_str() {
        Some("document") | Some("component") => p["bodies"] = json!(bodies),
        Some("body") => {
            let b = if let Some(n) = t["name"].as_str().filter(|n| bodies.iter().any(|b| b == n)) {
                n.to_string()
            } else {
                match_body(s, &t["geom"], bodies)?.ok_or_else(|| anyhow::anyhow!("its body was not found"))?
            };
            p["bodies"] = json!([b]);
        }
        Some("face") => {
            let pt = pick(t).ok_or_else(|| anyhow::anyhow!("its face has no position"))?;
            p["faces"] = json!([pt]);
        }
        _ => return Ok(false),
    }
    s.exec("appearance.assign", p)?;
    Ok(true)
}

/// The body whose volume and centroid are nearest a `geom` body reference.
fn match_body(s: &mut Session, g: &Value, bodies: &[String]) -> Result<Option<String>> {
    if bodies.len() == 1 {
        return Ok(Some(bodies[0].clone()));
    }
    let v = g["volume"].as_f64().unwrap_or(0.0);
    let c = v3(&g["centroid"]).unwrap_or([0.0; 3]);
    let mut best: Option<(String, f64)> = None;
    for b in bodies {
        let m = s.tool("measure", json!({"body": b}))?;
        let body = m["bodies"].as_array().and_then(|a| a.first()).cloned().unwrap_or(Value::Null);
        let dv = (body["volume_mm3"].as_f64().unwrap_or(0.0) - v).abs() / v.abs().max(1e-9);
        let dc = geom::norm(geom::sub(v3(&body["center_of_mass"]).unwrap_or([0.0; 3]), c));
        let cost = dv * 1e3 + dc;
        if best.as_ref().map(|(_, bc)| cost < *bc).unwrap_or(true) {
            best = Some((b.clone(), cost));
        }
    }
    Ok(best.map(|(n, _)| n))
}

/// The key under which a file's custom properties are kept in the design's metadata (the exporter writes them
/// back from there). SolidCraft has design properties only for the whole design: the root document's part
/// number, description and the like also go into those.
const META_KEY: &str = "io.github.simonrafferty.ncx.properties";

/// Keeps a document's custom properties (FORMAT-SPEC §4 "Custom properties") in the design.
pub fn keep_properties(s: &mut Session, doc: &Value, root: bool, report: &mut Report) -> Result<()> {
    let props = doc["customProperties"].as_array().cloned().unwrap_or_default();
    if props.is_empty() {
        return Ok(());
    }
    let name = doc["name"].as_str().unwrap_or("?").to_string();
    let meta = s.exec("document.metadata", json!({}))?;
    let mut kept = meta["metadata"][META_KEY].as_object().cloned().unwrap_or_default();
    let mut mine = serde_json::Map::new();
    let mut own = serde_json::Map::new();
    for p in &props {
        let Some(k) = p["name"].as_str() else { continue };
        // a linked value names the source system's own items: its evaluated text is kept
        let v = match p["value"].as_str() {
            Some(v) if !v.contains('@') && !v.contains('"') => v.to_string(),
            _ => p["resolved"].as_str().map(String::from).unwrap_or_else(|| p["value"].to_string()),
        };
        if root {
            let key = match k.to_lowercase().replace([' ', '_'], "").as_str() {
                "partnumber" | "partno" => Some("part_number"),
                "description" => Some("description"),
                "revision" | "rev" => Some("revision"),
                "author" | "drawnby" => Some("author"),
                "project" => Some("project"),
                _ => None,
            };
            if let Some(key) = key {
                own.insert(key.into(), json!(v));
            }
        }
        mine.insert(k.to_string(), json!(v));
    }
    kept.insert(name.clone(), Value::Object(mine.clone()));
    own.insert(META_KEY.into(), Value::Object(kept));
    match s.exec("document.metadata.set", json!({"values": own})) {
        Ok(_) => report.add(&name, "custom properties", "property", Status::Rebuilt, format!("{} kept in the design's properties", mine.len())),
        Err(e) if sc::is_cancelled(&e) => return Err(e),
        Err(e) => report.add(&name, "custom properties", "property", Status::Failed, format!("{e:#}")),
    }
    Ok(())
}
