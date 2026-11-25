//! Materials, colours and custom properties of a SolidCraft design, as NCX writes them (FORMAT-SPEC §4
//! "Materials", "Appearances", "Custom properties").
//!
//! - A part's material: the material its bodies use (SolidCraft assigns materials per body; NCX per part: the one
//!   most of the part's volume has), with its density and colour as the `definition`, so a system without a
//!   material of that name recreates it.
//! - Colours: each body's own appearance (`appearance.list`), as a colour on that body.
//! - Custom properties: SolidCraft's design properties (part number, description...) on the root document, and any
//!   properties an NCX import kept in the design's metadata (`io.github.simonrafferty.ncx.properties`), per document.

use anyhow::Result;
use serde_json::{json, Value};

use crate::sc::Session;

/// The key under which an NCX import keeps a file's custom properties in a design's metadata.
pub const META_KEY: &str = "io.github.simonrafferty.ncx.properties";

fn hex_rgb(h: &str) -> Option<[f64; 3]> {
    let h = h.trim_start_matches('#');
    if h.len() < 6 {
        return None;
    }
    let p = |i: usize| u8::from_str_radix(&h[i..i + 2], 16).ok().map(|v| v as f64 / 255.0);
    Some([p(0)?, p(2)?, p(4)?])
}

/// The part's material (NCX `materials`), from its bodies (component.properties rows).
pub fn materials(s: &mut Session, bodies: &[Value]) -> Result<Vec<Value>> {
    let mut by: std::collections::HashMap<String, f64> = std::collections::HashMap::new();
    for b in bodies {
        if let Some(m) = b["material"].as_str().filter(|m| !m.is_empty() && *m != "Default") {
            *by.entry(m.to_string()).or_default() += b["volume_mm3"].as_f64().unwrap_or(0.0);
        }
    }
    let Some((name, _)) = by.into_iter().max_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal)) else {
        return Ok(Vec::new());
    };
    let info = s.exec("material.inspect", json!({"material": name})).unwrap_or(Value::Null);
    let mut def = json!({"complete": false});
    if let Some(d) = info["density_g_cm3"].as_f64() {
        def["density"] = json!(d * 1000.0); // g/cm3 -> kg/m3
    }
    if let Some(c) = info["appearance"]["color"].as_str().and_then(hex_rgb) {
        def["color"] = json!(c);
    }
    Ok(vec![json!({"configuration": "Default", "name": name, "database": "SolidCraft", "definition": def})])
}

/// Colours given to bodies (by their reference: volume, centroid, box), read once for the design.
pub fn appearances(looks: &Value, bodies: &[Value]) -> Vec<Value> {
    let mut out = Vec::new();
    for b in bodies {
        let Some(name) = b["name"].as_str() else { continue };
        let Some(l) = looks["bodies"].get(name) else { continue };
        let Some(c) = l["color"].as_str().and_then(hex_rgb) else { continue };
        let bb = &b["bbox"];
        let (mn, mx) = (bb["min"].clone(), bb["max"].clone());
        let bbox = match (mn.as_array(), mx.as_array()) {
            (Some(a), Some(z)) if a.len() == 3 && z.len() == 3 => json!([a[0], a[1], a[2], z[0], z[1], z[2]]),
            _ => Value::Null,
        };
        let mut a = json!({"target": {"kind": "body", "by": "geom", "geom": {"volume": b["volume_mm3"], "centroid": b["center_of_mass"], "bbox": bbox}},
                           "kind": "color", "color": c});
        if let Some(o) = l["opacity"].as_f64().filter(|o| *o < 1.0) {
            a["optical"] = json!({"transparency": 1.0 - o});
        }
        out.push(a);
    }
    out
}

/// Custom properties of a document: the design's own properties (for the root document) and those an NCX import
/// kept for this document.
pub fn properties(meta: &Value, doc_name: &str, root: bool) -> Vec<Value> {
    let mut out: Vec<Value> = Vec::new();
    let mut seen = std::collections::HashSet::new();
    if let Some(kept) = meta["metadata"][META_KEY][doc_name].as_object() {
        for (k, v) in kept {
            let val = v.as_str().map(String::from).unwrap_or_else(|| v.to_string());
            seen.insert(k.to_lowercase());
            out.push(json!({"name": k, "type": "text", "value": val, "resolved": val}));
        }
    }
    if root {
        for (key, name) in [("part_number", "Part Number"), ("description", "Description"), ("revision", "Revision"),
                            ("author", "Author"), ("project", "Project"), ("organization", "Organization"), ("license", "License")] {
            if let Some(v) = meta["properties"][key].as_str().filter(|v| !v.is_empty()) {
                // already there under the source's own name (PartNo for part_number)
                if seen.contains(&name.to_lowercase()) || seen.contains(key) || out.iter().any(|p| p["value"].as_str() == Some(v)) {
                    continue;
                }
                out.push(json!({"name": name, "type": "text", "value": v, "resolved": v}));
            }
        }
    }
    out
}

/// Adds a part's material, colours and custom properties to its NCX document (the part placed at identity).
pub fn add_to_part(s: &mut Session, c: i64, doc: &mut Value, meta: &Value, root: bool) -> Result<()> {
    let props = s.exec("component.properties", json!({"component": c, "descendants": false}))?;
    let bodies: Vec<Value> = props["bodies"].as_array().cloned().unwrap_or_default();
    let mats = materials(s, &bodies)?;
    if !mats.is_empty() {
        doc["materials"] = json!(mats);
    }
    let looks = s.exec("appearance.list", json!({})).unwrap_or(Value::Null);
    let app = appearances(&looks, &bodies);
    if !app.is_empty() {
        doc["appearances"] = json!(app);
    }
    add_properties(doc, meta, root);
    Ok(())
}

/// Adds a document's custom properties.
pub fn add_properties(doc: &mut Value, meta: &Value, root: bool) {
    let name = doc["name"].as_str().unwrap_or("").to_string();
    let p = properties(meta, &name, root);
    if !p.is_empty() {
        doc["customProperties"] = json!(p);
    }
}
