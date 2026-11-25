//! Parts made from another document (FORMAT-SPEC §4 "Inserted / derived parts"), which SolidCraft has no feature
//! for, rebuilt as their functional equivalent:
//!
//! - `insertPart` (a layout's sketches and planes brought into a part): the transferred sketches are rebuilt in
//!   the part itself, with the layout's parameters and equations, so the layout's numbers still drive the part (the
//!   spec's suggestion for a system without derived parts). Features of the part that use a transferred item use
//!   the copy.
//! - `mirrorPart` (the mirror image of another part): the source part's features, then its bodies mirrored about
//!   the plane and the originals removed.
//!
//! Both are rewrites of the document before it is built: the rest of the import sees an ordinary part. A transform
//! other than identity, or transferred bodies, keep the old behaviour (the part's shape from the file).

use std::collections::HashMap;

use serde_json::{json, Map, Value};

/// The document rewritten as an ordinary part, or None when it is not made from another document (or cannot be).
/// Err(why) when it is, but this rewrite does not cover it.
pub fn inline(doc: &Value, docs: &HashMap<String, Value>) -> Option<Result<Value, String>> {
    let feats = doc["features"].as_array()?;
    let first = feats.iter().find(|f| f["type"] == "insertPart" || f["type"] == "mirrorPart")?;
    Some(if first["type"] == "insertPart" { insert_part(doc, first, docs) } else { mirror_part(doc, first, docs) })
}

fn identity(t: &Value) -> bool {
    match t.as_array() {
        None => true,
        Some(a) if a.len() == 16 => {
            let id = [1.0, 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.];
            a.iter().zip(id.iter()).all(|(v, w)| (v.as_f64().unwrap_or(f64::NAN) - w).abs() < 1e-9)
        }
        _ => false,
    }
}

/// The source document's id for one of its features, inside the part that copies it.
fn copy_id(src: &str, fid: &str) -> String {
    format!("{src}|{fid}")
}

/// Every reference into the source (by id, or as a transferred item) rewritten to the copies.
fn rewrite_refs(v: &mut Value, src: &str, own: &std::collections::HashSet<String>) {
    match v {
        Value::Object(o) => {
            let kind = o.get("kind").and_then(|k| k.as_str()).map(String::from);
            match kind.as_deref() {
                Some("transferred") if o.get("document").and_then(|d| d.as_str()) == Some(src) => {
                    if let Some(f) = o.get("feature").and_then(|f| f.as_str()) {
                        let id = copy_id(src, f);
                        *v = json!({"kind": "feature", "by": "id", "id": id});
                    } else if let Some(p) = o.get("plane").and_then(|p| p.as_str()) {
                        *v = json!({"kind": "originPlane", "by": "id", "plane": p.trim_start_matches("plane:")});
                    }
                    return;
                }
                Some("sketchEntity") if o.get("document").and_then(|d| d.as_str()) == Some(src) => {
                    if let Some(f) = o.get("feature").and_then(|f| f.as_str()).map(String::from) {
                        o.insert("feature".into(), json!(copy_id(src, &f)));
                        o.remove("document");
                    }
                    return;
                }
                Some("feature") | Some("sketchEntity") => {
                    // a reference inside a copied source feature to another source feature
                    let key = if kind.as_deref() == Some("feature") { "id" } else { "feature" };
                    if let Some(f) = o.get(key).and_then(|f| f.as_str()).map(String::from) {
                        if own.contains(&f) {
                            o.insert(key.into(), json!(copy_id(src, &f)));
                        }
                    }
                }
                _ => {}
            }
            for (_, x) in o.iter_mut() {
                rewrite_refs(x, src, own);
            }
        }
        Value::Array(a) => {
            for x in a.iter_mut() {
                rewrite_refs(x, src, own);
            }
        }
        _ => {}
    }
}

/// The source's parameters first (the part's own after, a name both have taken from the part), and its
/// equations, re-keyed to the copies.
fn merge_params(doc: &mut Map<String, Value>, src_doc: &Value, src: &str, own: &std::collections::HashSet<String>) {
    let mut params: Vec<Value> = src_doc["parameters"].as_array().cloned().unwrap_or_default();
    for p in doc.get("parameters").and_then(|p| p.as_array()).cloned().unwrap_or_default() {
        params.retain(|q| q["name"] != p["name"]);
        params.push(p);
    }
    doc.insert("parameters".into(), json!(params));
    let mut eqs: Vec<Value> = Vec::new();
    for e in src_doc["equations"].as_array().into_iter().flatten() {
        let mut e = e.clone();
        if let Some(f) = e["targetRef"]["feature"].as_str().map(String::from) {
            if own.contains(&f) {
                e["targetRef"]["feature"] = json!(copy_id(src, &f));
            }
        }
        eqs.push(e);
    }
    eqs.extend(doc.get("equations").and_then(|e| e.as_array()).cloned().unwrap_or_default());
    doc.insert("equations".into(), json!(eqs));
}

fn insert_part(doc: &Value, ins: &Value, docs: &HashMap<String, Value>) -> Result<Value, String> {
    let def = &ins["def"];
    let src = def["source"].as_str().ok_or("no source document")?.to_string();
    let src_doc = docs.get(&src).ok_or("its source document is not in the file")?;
    if !identity(&def["transform"]) {
        return Err("its source is moved or turned (only an Insert Part in place is rebuilt)".into());
    }
    let transfer: Vec<&str> = def["transfer"].as_array().into_iter().flatten().filter_map(|t| t.as_str()).collect();
    if transfer.iter().any(|t| t.contains("Bodies") || *t == "bodies") {
        return Err("it brings in bodies (only sketches and planes are rebuilt from the source)".into());
    }
    let src_feats: Vec<Value> = src_doc["features"].as_array().cloned().unwrap_or_default();
    let own: std::collections::HashSet<String> = src_feats.iter().filter_map(|f| f["id"].as_str().map(String::from)).collect();
    // the transferred source features (sketches, reference planes), in the source's order, under their names here
    let mut copies = Vec::new();
    for item in def["items"].as_array().into_iter().flatten() {
        let Some(fid) = item["source"].as_str().filter(|s| s.starts_with("f:") || own.contains(*s)) else { continue };
        let Some(f) = src_feats.iter().find(|f| f["id"] == fid) else { continue };
        if !["sketch", "plane", "axis", "point", "sketch3d"].contains(&f["type"].as_str().unwrap_or("")) {
            continue;
        }
        let mut c = f.clone();
        c["id"] = json!(copy_id(&src, fid));
        if let Some(n) = item["name"].as_str() {
            c["name"] = json!(n);
        }
        if item["hidden"].as_bool() == Some(true) {
            c["def"]["hidden"] = json!(true);
        }
        rewrite_refs(&mut c["def"], &src, &own);
        copies.push(c);
    }
    let mut out = doc.as_object().cloned().unwrap_or_default();
    merge_params(&mut out, src_doc, &src, &own);
    let mut feats = copies;
    for f in doc["features"].as_array().into_iter().flatten() {
        if f["id"] == ins["id"] {
            continue;
        }
        let mut f = f.clone();
        rewrite_refs(&mut f, &src, &own);
        feats.push(f);
    }
    out.insert("features".into(), json!(feats));
    Ok(Value::Object(out))
}

fn mirror_part(doc: &Value, mp: &Value, docs: &HashMap<String, Value>) -> Result<Value, String> {
    let def = &mp["def"];
    let src = def["source"].as_str().ok_or("no source document")?.to_string();
    let src_doc = docs.get(&src).ok_or("its source document is not in the file")?;
    if src_doc["features"].as_array().into_iter().flatten().any(|f| f["type"] == "insertPart" || f["type"] == "mirrorPart") {
        return Err("its source is itself made from another document".into());
    }
    let plane = def["plane"].clone();
    if plane["kind"] != "originPlane" {
        return Err("it is mirrored about something other than an origin plane".into());
    }
    let src_feats: Vec<Value> = src_doc["features"].as_array().cloned().unwrap_or_default();
    let own: std::collections::HashSet<String> = src_feats.iter().filter_map(|f| f["id"].as_str().map(String::from)).collect();
    let mut out = doc.as_object().cloned().unwrap_or_default();
    merge_params(&mut out, src_doc, &src, &own);
    let mut feats = Vec::new();
    for f in &src_feats {
        let mut c = f.clone();
        if let Some(id) = f["id"].as_str() {
            c["id"] = json!(copy_id(&src, id));
        }
        rewrite_refs(&mut c, &src, &own);
        feats.push(c);
    }
    // the source's bodies, mirrored (as new bodies), then the originals removed
    let body_refs: Vec<Value> = src_doc["bodies"].as_array().into_iter().flatten().filter_map(|b| {
        let fp = &b["fingerprint"];
        Some(json!({"kind": "body", "by": "geom", "geom": {"volume": fp["volume"], "centroid": fp["centroid"], "bbox": fp["bbox"]}}))
    }).collect();
    if body_refs.is_empty() {
        return Err("its source has no bodies".into());
    }
    let name = mp["name"].as_str().unwrap_or("Mirror Part");
    feats.push(json!({"id": format!("{}|mirror", mp["id"].as_str().unwrap_or("f")), "type": "mirrorBody", "name": format!("{name} (mirror)"),
                      "status": "mapped", "def": {"bodies": body_refs, "plane": plane, "merge": false, "knit": false}}));
    feats.push(json!({"id": format!("{}|remove", mp["id"].as_str().unwrap_or("f")), "type": "deleteBody", "name": format!("{name} (originals)"),
                      "status": "mapped", "def": {"bodies": body_refs, "keep": false}}));
    for f in doc["features"].as_array().into_iter().flatten() {
        if f["id"] != mp["id"] {
            let mut f = f.clone();
            rewrite_refs(&mut f, &src, &own);
            feats.push(f);
        }
    }
    out.insert("features".into(), json!(feats));
    Ok(Value::Object(out))
}
