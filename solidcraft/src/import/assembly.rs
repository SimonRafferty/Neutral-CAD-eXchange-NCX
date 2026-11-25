//! Assemblies (FORMAT-SPEC §4 "Documents", components).
//!
//! Every part document placed in the assembly becomes one SolidCraft component, built at the design's origin
//! (its own frame = world, as for a single part) and checked against its own fingerprint. A part that cannot be
//! rebuilt, or that is made from another document (a layout), takes its shape from the file's own B-rep: the
//! file is imported once, the part's component is copied out of it as an independent component
//! (`component.paste_new`), and the import is deleted. Then each assembly document becomes a component holding
//! placements (occurrences) of its children at their transforms, innermost first; `fixed` components are
//! grounded. The whole assembly is checked against the file's fingerprint (every body where the file puts it),
//! and the top assembly is stood upright.
//!
//! Mates become SolidCraft constraints between the placed parts (mates.rs).

use std::collections::{HashMap, HashSet};

use anyhow::{anyhow, Result};
use serde_json::{json, Value};

use super::{apply_looks, body_names, looks, mates, rebuild_part, set_progress, Job, PartState};
use crate::ncx;
use crate::report::{Report, Status};
use crate::sc::{self, Session};

/// A document as a SolidCraft component.
struct Comp {
    id: Value,
    /// its first occurrence while it still sits at the design's root, unused: the first placement takes it
    free_occurrence: Option<Value>,
}

pub fn import_documents(s: &mut Session, job: &Job, model: &Value, header: &Value, up: &str, report: &mut Report) -> Result<()> {
    let docs: HashMap<String, Value> = model["documents"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|d| d["id"].as_str().map(|id| (id.to_string(), d.clone())))
        .collect();
    let root_id = model["root"].as_str().ok_or_else(|| anyhow!("the file names no root document"))?.to_string();

    let name_of = |id: &str| docs.get(id).and_then(|d| d["name"].as_str()).unwrap_or(id).to_string();

    // The documents placed in the assembly, innermost first.
    let mut order: Vec<String> = Vec::new();
    postorder(&root_id, &docs, &mut order, &mut HashSet::new());
    let parts: Vec<String> = order.iter().filter(|id| docs[*id]["kind"] == "part").cloned().collect();
    let assemblies: Vec<String> = order.iter().filter(|id| docs[*id]["kind"] == "assembly").cloned().collect();

    // 1. Parts: rebuilt natively, else from the file's own geometry.
    let mut comps: HashMap<String, Comp> = HashMap::new();
    let mut plain: Vec<(String, String)> = Vec::new(); // (doc id, why)
    let mut taken: HashSet<String> = HashSet::new();
    let mut made = looks::Made::default();
    // part doc -> payload feature id -> SolidCraft name (mates refer to reference planes by them)
    let mut built_features: HashMap<String, HashMap<String, String>> = HashMap::new();
    for (i, id) in parts.iter().enumerate() {
        let doc = &docs[id];
        let name = name_of(id);
        set_progress(&job.progress, "Rebuilding the parts", &format!("{name} ({} of {})", i + 1, parts.len()), i as f32 / parts.len().max(1) as f32);
        if doc["plain"].as_bool() == Some(true) {
            report.add(&name, &name, "part", Status::Note, "no feature history in the source: brought in as its shape, as it was there");
            plain.push((id.clone(), String::new()));
            continue;
        }
        // made from another document (a layout, a mirrored part): rewritten as an ordinary part (derived.rs)
        let inlined;
        let doc = match super::derived::inline(doc, &docs) {
            None => doc,
            Some(Ok(d)) => {
                let how = if d["features"].as_array().into_iter().flatten().any(|f| f["type"] == "mirrorBody" && f["id"].as_str().map(|i| i.ends_with("|mirror")).unwrap_or(false)) {
                    "a mirror image of another part: rebuilt as that part's features, its bodies mirrored"
                } else {
                    "made from a layout: the layout's sketches and parameters rebuilt in the part (SolidCraft has no derived parts)"
                };
                report.add(&name, &name, "part", Status::Note, how);
                inlined = d;
                &inlined
            }
            Some(Err(why)) => {
                let why = format!("made from another document: {why}");
                report.add(&name, &name, "part", Status::Fallback, &why);
                plain.push((id.clone(), why));
                continue;
            }
        };
        let exclude = body_names(s)?;
        let c = s.exec("component.create", json!({"name": name, "parent": 0, "activate": true, "grounded": false}))?;
        let mut st = PartState::new(&name, exclude);
        st.taken_names = super::feature_names(s)?;
        st.component = c["component"].clone();
        let r = rebuild_part(s, job, doc, report, &mut st, &mut taken);
        s.exec("component.activate", json!({"component": 0}))?;
        match r {
            Ok(()) => {
                built_features.insert(id.clone(), st.names.clone());
                apply_looks(s, doc, &c["component"], &mut made, report, false)?;
                comps.insert(id.clone(), Comp { id: c["component"].clone(), free_occurrence: Some(c["occurrence"].clone()) });
            }
            Err(e) if sc::is_cancelled(&e) => return Err(e),
            Err(e) => {
                let why = format!("{e:#}");
                // take the half-built part away again, with the parameters it added
                let _ = s.exec("component.delete", json!({"component": c["component"]}));
                for p in &st.added_params {
                    let _ = s.exec("parameters.delete", json!({"name": p}));
                    taken.remove(p);
                }
                report.add(&name, &name, "part", Status::Fallback, &why);
                plain.push((id.clone(), why));
            }
        }
    }
    if !plain.is_empty() {
        set_progress(&job.progress, "Bringing in parts as their geometry", "", -1.0);
        copy_parts_from_file(s, job, &plain, &docs, &mut comps, report)?;
        for (id, _) in &plain {
            if let Some(c) = comps.get(id) {
                let cid = c.id.clone();
                apply_looks(s, &docs[id], &cid, &mut made, report, false)?;
            }
        }
        let named: Vec<String> = plain.iter().map(|(id, _)| name_of(id)).collect();
        report.notes.push(format!(
            "Brought in with their shape but without feature history: {}. The report lists why for each.",
            named.join(", ")
        ));
    }

    // 2. Assemblies, innermost first: each a component holding placements of its children.
    let mut placed_at = mates::Placed {
        occ: HashMap::new(),
        path: HashMap::new(),
        world: HashMap::new(),
        features: std::mem::take(&mut built_features),
    };
    for (i, id) in assemblies.iter().enumerate() {
        let doc = &docs[id];
        let name = name_of(id);
        set_progress(&job.progress, "Placing the components", &name, i as f32 / assemblies.len().max(1) as f32);
        let c = s.exec("component.create", json!({"name": name, "parent": 0, "activate": false, "grounded": false}))?;
        let asm_comp = c["component"].clone();
        let mut placed = 0usize;
        for entry in doc["components"].as_array().into_iter().flatten() {
            let child_doc = entry["document"].as_str().unwrap_or("");
            let label = entry["name"].as_str().unwrap_or(child_doc).to_string();
            let Some(child) = comps.get_mut(child_doc) else {
                report.add(&name, &label, "component", Status::Failed, "its document could not be brought in");
                continue;
            };
            let occ = match child.free_occurrence.take() {
                Some(o) => {
                    s.exec("occurrence.reparent", json!({"occurrence": o, "parent": asm_comp}))?;
                    o
                }
                None => s.exec("occurrence.copy", json!({"component": child.id, "parent": asm_comp}))?["occurrence"].clone(),
            };
            let t: Vec<f64> = entry["transform"].as_array().into_iter().flatten().filter_map(Value::as_f64).collect();
            if t.len() >= 12 {
                s.exec("occurrence.move", json!({"occurrence": occ, "to_matrix": &t[..12]}))?;
            }
            if entry["fixed"].as_bool() == Some(true) {
                let _ = s.exec("occurrence.ground", json!({"occurrence": occ, "grounded": true}));
            }
            if let Some(cid) = entry["id"].as_str() {
                placed_at.occ.insert((id.clone(), cid.to_string()), occ.clone());
            }
            placed += 1;
        }
        comps.insert(id.clone(), Comp { id: asm_comp, free_occurrence: Some(c["occurrence"].clone()) });
        report.add(&name, &name, "assembly", Status::Rebuilt, format!("{placed} components placed as in the file"));
        super::looks::keep_properties(s, doc, *id == root_id, report)?;
    }

    // 3. Mates, as SolidCraft constraints between the placed parts (made where the parts are).
    if let Some(root_occ) = comps.get(&root_id).and_then(|c| c.free_occurrence.clone()) {
        index_placements(&root_id, vec![root_occ], mates::identity(), &docs, &mut placed_at);
    }
    let mates_total: usize = assemblies.iter().map(|a| docs[a]["mates"].as_array().map(|m| m.len()).unwrap_or(0)).sum();
    if mates_total > 0 {
        set_progress(&job.progress, "Making the mates", &format!("{mates_total} mates"), -1.0);
        let (made, left) = mates::make_all(s, &assemblies, &docs, &placed_at, report)?;
        if left > 0 {
            report.notes.push(format!(
                "{made} of the {mates_total} mates were made as SolidCraft constraints; the other {left} are not carried (the report says why for each): those parts stay where the file puts them."
            ));
        }
    }

    // 3. Check every body where the file puts it (before standing the model up), then stand it up.
    set_progress(&job.progress, "Checking the assembly", "", 1.0);
    check_assembly(s, model, report)?;
    let root_name = name_of(&root_id);
    if let Some(occ) = comps.get(&root_id).and_then(|c| c.free_occurrence.clone()) {
        super::stand_upright(s, &root_name, &occ, up, report)?;
    }
    let _ = header;
    Ok(())
}

/// Each assembly's first placement, from the root down: its occurrence path and its transform in the root
/// assembly's frame (mate references are turned into world points through it).
fn index_placements(asm: &str, path: Vec<Value>, world: [f64; 16], docs: &HashMap<String, Value>, placed: &mut mates::Placed) {
    if placed.path.contains_key(asm) {
        return;
    }
    placed.path.insert(asm.to_string(), path.clone());
    placed.world.insert(asm.to_string(), world);
    let Some(d) = docs.get(asm) else { return };
    for e in d["components"].as_array().into_iter().flatten() {
        let child = e["document"].as_str().unwrap_or("");
        if docs.get(child).map(|c| c["kind"] == "assembly").unwrap_or(false) {
            if let Some(occ) = e["id"].as_str().and_then(|cid| placed.occ.get(&(asm.to_string(), cid.to_string()))).cloned() {
                let mut p = path.clone();
                p.push(occ);
                index_placements(child, p, mates::mul(&world, &mates::m4(&e["transform"])), docs, placed);
            }
        }
    }
}

/// Depth first through the components: children before the assemblies that hold them.
fn postorder(id: &str, docs: &HashMap<String, Value>, out: &mut Vec<String>, seen: &mut HashSet<String>) {
    if !seen.insert(id.to_string()) {
        return;
    }
    if let Some(d) = docs.get(id) {
        for c in d["components"].as_array().into_iter().flatten() {
            if let Some(child) = c["document"].as_str() {
                postorder(child, docs, out, seen);
            }
        }
    }
    out.push(id.to_string());
}

/// Parts that come in as their shape: the file is imported once, each part's component is copied out of it
/// as an independent component, and the import is deleted (with the notice body in it).
fn copy_parts_from_file(
    s: &mut Session,
    job: &Job,
    plain: &[(String, String)],
    docs: &HashMap<String, Value>,
    comps: &mut HashMap<String, Comp>,
    report: &mut Report,
) -> Result<()> {
    let ins = s.exec("file.insert_step", json!({"path": job.input.to_string_lossy(), "names": "product"}))?;
    let import_root = ins["component"].clone();
    let list = s.exec("component.list", json!({"compact": true}))?;
    let all: Vec<Value> = list["components"].as_array().cloned().unwrap_or_default();
    // the import's components: those whose parents lead to its root
    let parent_of: HashMap<String, String> =
        all.iter().map(|c| (c["id"].to_string(), c["parent"].to_string())).collect();
    let in_import = |cid: &str| {
        let mut cur = cid.to_string();
        for _ in 0..64 {
            if cur == import_root.to_string() {
                return true;
            }
            match parent_of.get(&cur) {
                Some(p) if p != "null" => cur = p.clone(),
                _ => return false,
            }
        }
        false
    };
    for (id, _) in plain {
        let doc = &docs[id];
        let name = doc["name"].as_str().unwrap_or("");
        let stem = doc["sourceFile"].as_str().map(|f| f.rsplit_once('.').map(|(a, _)| a).unwrap_or(f)).unwrap_or(name);
        let found = all
            .iter()
            .filter(|c| in_import(&c["id"].to_string()) && c["id"] != import_root)
            .find(|c| {
                let n = c["name"].as_str().unwrap_or("");
                n == name || n == stem || n.eq_ignore_ascii_case(name)
            })
            .or_else(|| {
                all.iter().filter(|c| in_import(&c["id"].to_string())).find(|c| {
                    let n = c["name"].as_str().unwrap_or("").to_lowercase();
                    n.starts_with(&name.to_lowercase())
                })
            });
        let Some(src) = found else {
            report.add(name, name, "part", Status::Failed, "its geometry was not found in the file");
            continue;
        };
        // a part placed more than once needs one of its placements named, by its full path
        let mut params = json!({"component": src["id"], "parent": 0});
        if let Some(path) = first_path(&all, &src["id"]) {
            params["occurrence"] = json!(path.last());
            params["path"] = json!(path);
        }
        let copy = s.exec("component.paste_new", params)?;
        let _ = s.exec("component.rename", json!({"component": copy["component"], "name": name}));
        comps.insert(id.clone(), Comp { id: copy["component"].clone(), free_occurrence: Some(copy["occurrence"].clone()) });
    }
    s.exec("component.delete", json!({"component": import_root}))?;
    // the copies' bodies are named "<name> (2)" while the import still had the originals
    if let Ok(names) = body_names(s) {
        for n in names {
            if let Some(base) = n.strip_suffix(" (2)") {
                let _ = s.exec("body.rename", json!({"body": n, "name": base}));
            }
        }
    }
    Ok(())
}

/// The occurrence ids from the design's root down to the first placement of a component (component.list).
fn first_path(all: &[Value], comp: &Value) -> Option<Vec<Value>> {
    let c = all.iter().find(|c| &c["id"] == comp)?;
    let occ = c["occurrences"].as_array()?.first()?;
    let parent = &occ["parent"];
    if parent.as_u64() == Some(0) {
        return Some(vec![occ["id"].clone()]);
    }
    let mut p = first_path(all, parent)?;
    p.push(occ["id"].clone());
    Some(p)
}

/// Every body where the file puts it: the file's fingerprint (assembly frame) against the design's bodies.
fn check_assembly(s: &mut Session, model: &Value, report: &mut Report) -> Result<()> {
    let expected = model["fingerprint"].clone();
    let n_expected = expected["bodies"].as_array().map(|b| b.len()).unwrap_or(0);
    if n_expected == 0 {
        return Ok(());
    }
    let actual = super::measure_fingerprints_with(s, false, &HashSet::new())?;
    let act = json!({"bodies": actual.iter().map(|(_, f)| f.clone()).collect::<Vec<_>>()});
    let top = model["documents"]
        .as_array()
        .and_then(|d| d.iter().find(|d| d["id"] == model["root"]))
        .and_then(|d| d["name"].as_str())
        .unwrap_or("assembly")
        .to_string();
    if actual.len() != n_expected {
        report.serious.push(format!(
            "The assembly has {} solid bodies where the file has {n_expected}: some parts are missing or extra.",
            actual.len()
        ));
        return Ok(());
    }
    let rep = ncx::fingerprint_compare(&expected, &act, "exchange")?;
    let result = rep.get("result").or_else(|| rep.get("status")).and_then(Value::as_str).unwrap_or("");
    if result == "mismatch" {
        let n = rep["differences"].as_array().map(|d| d.iter().filter(|x| x["severity"] != "info").count()).unwrap_or(0);
        report.serious.push(format!("Some parts of the assembly differ from the file in shape or position ({n} differences: see the report)."));
        for d in rep["differences"].as_array().into_iter().flatten().filter(|d| d["severity"] != "info").take(12) {
            report.notes.push(format!(
                "assembly check: body {} {}: {} where the file has {}",
                d["body"], d["quantity"].as_str().unwrap_or("?"), d["actual"], d["expected"]
            ));
        }
    } else {
        report.add(&top, &top, "check", Status::Rebuilt, format!("all {n_expected} bodies match the file in shape and position"));
    }
    Ok(())
}
