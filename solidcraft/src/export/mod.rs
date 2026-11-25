//! SolidCraft design -> NCX file.
//!
//! The design is opened in a private headless session (never saved), its feature history read feature by
//! feature (`features.rs`: sketches, extrudes, revolves, fillets, chamfers, primitives, holes, patterns, mirrors,
//! shells and planes natively, every other feature as its carried shape) with geometric references measured in the
//! state each feature saw (`refs.rs`), and written as NCX documents: one part per SolidCraft component with
//! bodies, one assembly per component holding others. The STEP geometry comes from SolidCraft's own STEP writer
//! and the NCX core embeds the payload with the notice body.
//!
//! A design an NCX import made from a Y-up file sits in a component turned upright: that turn is undone first,
//! so the file is written in its own frame again (FORMAT-SPEC §4 orientation: an exporter writing a model back
//! keeps its frame).

mod expr;
mod features;
mod looks;
mod mates;
mod refs;
mod sketch;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;

use anyhow::{bail, Context, Result};
use serde_json::{json, Value};

use crate::import::{scratch_dir, set_progress, ProgressHandle};
use crate::ncx;
use crate::report::{Report, Status};
use crate::sc::{self, Session};

pub struct Job {
    pub input: PathBuf,
    pub output: PathBuf,
    pub cli: PathBuf,
    pub cancel: Arc<AtomicBool>,
    pub progress: ProgressHandle,
    pub log: bool,
}

pub struct Outcome {
    pub report: Report,
    pub report_path: Option<PathBuf>,
    pub message: String,
    pub problem: bool,
}

pub fn run(job: &Job) -> Result<Outcome> {
    let mut report = Report::default();
    report.title = format!(
        "NCX export of {} ({})",
        job.input.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(),
        env!("CARGO_PKG_VERSION")
    );
    let mut allow = vec![scratch_dir()];
    for p in [&job.input, &job.output] {
        if let Some(d) = p.parent().and_then(|d| d.canonicalize().ok()) {
            allow.push(d);
        }
    }
    let version = sc::cli_version(&job.cli).unwrap_or_default();
    let mut s = Session::start(&job.cli, &allow, job.cancel.clone())?;
    s.keep_log = job.log;
    let r = export_from(&mut s, job, &version, &mut report);
    if job.log {
        let _ = std::fs::write(job.output.with_extension("log.txt"), s.log.join("\n"));
    }
    drop(s);
    match r {
        Ok(()) => {}
        Err(e) if sc::is_cancelled(&e) => {
            let _ = std::fs::remove_file(&job.output);
            return Ok(Outcome { report, report_path: None, message: "The export was cancelled. No file was written.".into(), problem: true });
        }
        Err(e) => return Err(e),
    }
    let report_path = report.save_beside(&job.output).ok();
    let (message, problem) = if report.serious.is_empty() {
        ("Your design has been saved as an NCX file.".to_string(), false)
    } else {
        (format!("The NCX file was written, but:\n- {}", report.serious.join("\n- ")), true)
    };
    Ok(Outcome { report, report_path, message, problem })
}

/// A SolidCraft component as `component.list` reports it.
pub(crate) struct Comp {
    name: String,
    bodies: usize,
    /// placements of other components inside it: (occurrence id, component id, name, transform, grounded)
    children: Vec<(i64, i64, String, Value, bool)>,
    /// its own placements: (occurrence id, parent component id)
    occurrences: Vec<(i64, i64)>,
}

const IDENTITY: [[f64; 4]; 3] = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0]];

fn export_from(s: &mut Session, job: &Job, sc_version: &str, report: &mut Report) -> Result<()> {
    set_progress(&job.progress, "Opening the design", "", -1.0);
    s.tool("open", json!({"path": job.input.to_string_lossy()})).context("SolidCraft could not open the design")?;
    let design = s.tool("inspect_design", json!({"measure": false, "sketches": false, "timeline": true, "bodies": "names", "joints": false}))?;
    let file_stem = job.input.file_stem().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "Design".into());
    let design_name = design["name"].as_str().filter(|n| !n.is_empty()).map(String::from).unwrap_or(file_stem);

    // the components and their placements, as they are before the export moves anything
    let list = s.exec("component.list", json!({}))?;
    let mut comps: HashMap<i64, Comp> = HashMap::new();
    for c in list["components"].as_array().into_iter().flatten() {
        let id = c["id"].as_i64().unwrap_or(0);
        comps.insert(id, Comp {
            name: c["name"].as_str().unwrap_or("").to_string(),
            bodies: c["bodies"].as_array().map(|b| b.len()).unwrap_or(0),
            children: c["children"].as_array().into_iter().flatten().map(|o| (
                o["id"].as_i64().unwrap_or(0), o["component"].as_i64().unwrap_or(0),
                o["name"].as_str().unwrap_or("").to_string(), o["transform"].clone(), o["grounded"].as_bool() == Some(true),
            )).collect(),
            occurrences: c["occurrences"].as_array().into_iter().flatten().map(|o| (o["id"].as_i64().unwrap_or(0), o["parent"].as_i64().unwrap_or(0))).collect(),
        });
    }
    if !comps.contains_key(&0) {
        comps.insert(0, Comp { name: design_name.clone(), bodies: 0, children: Vec::new(), occurrences: Vec::new() });
    }

    // parameters: the user's (written as global variables) and every value (for the numbers behind expressions)
    let mut params = std::collections::HashSet::new();
    let mut values = HashMap::new();
    let mut user_params = Vec::new();
    for p in design["params"].as_array().into_iter().flatten() {
        let name = p["name"].as_str().unwrap_or("").to_string();
        if p["model"].as_bool() != Some(true) {
            params.insert(name.clone());
            user_params.push(p.clone());
        }
    }
    // every parameter's value, the features' own inputs too
    if let Ok(pl) = s.exec("parameters.list", json!({})) {
        for p in pl["parameters"].as_array().into_iter().flatten() {
            if let (Some(n), Some(v)) = (p["name"].as_str(), p["value"].as_f64()) {
                values.insert(n.to_string(), v);
            }
        }
    }

    // the timeline, each feature with its definition (which component it belongs to)
    set_progress(&job.progress, "Reading the feature history", &design_name, -1.0);
    let mut items: Vec<(i64, features::Item)> = Vec::new();
    let timeline: Vec<Value> = design["timeline"].as_array().cloned().unwrap_or_default();
    for (k, t) in timeline.iter().enumerate() {
        let id = t["id"].as_i64().unwrap_or(-1);
        let info = s.exec("timeline.inspect", json!({"feature": id})).unwrap_or(Value::Null);
        let comp = info["definition"]["component"].as_i64().unwrap_or(0);
        items.push((comp, features::Item {
            k, id,
            name: t["name"].as_str().unwrap_or("").to_string(),
            type_name: t["type"].as_str().unwrap_or("").to_string(),
            suppressed: t["suppressed"].as_bool() == Some(true) || t["rolled_back"].as_bool() == Some(true),
            error: t["error"].as_str().map(String::from),
            warning: t["warning"].as_str().map(String::from),
            info,
        }));
    }
    let own_features = |c: i64| items.iter().filter(|(ic, _)| *ic == c).count();

    // which component is the model: a design an NCX import turned upright has one placement, turned +90° about X,
    // holding the model; it is written in its own (Y-up) frame
    let root0 = &comps[&0];
    let mut up = "z";
    let mut root = 0;
    let mut strip = 0;
    if root0.bodies == 0 && own_features(0) == 0 && root0.children.len() == 1 && is_upright_turn(&root0.children[0].3) {
        let (occ, child, ..) = root0.children[0].clone();
        place_at_identity(s, &[occ])?;
        up = "y";
        root = child;
        strip = 1;
    }

    // what holds content: bodies, or placements of components that do (a component left empty, such as the one a
    // carried shape was imported into before its bodies moved into the part, is not written)
    fn has_content(c: i64, comps: &HashMap<i64, Comp>, depth: usize) -> bool {
        let Some(k) = comps.get(&c) else { return false };
        depth < 32 && (k.bodies > 0 || k.children.iter().any(|ch| has_content(ch.1, comps, depth + 1)))
    }
    let content = |c: i64| has_content(c, &comps, 0);
    if !content(root) {
        bail!("the design has nothing to write");
    }

    // the file's geometry and fingerprint, in the model's frame
    set_progress(&job.progress, "Measuring", &design_name, -1.0);
    // the mates (joints), read before anything moves; then removed in this session so that each part can be put
    // in its own frame (the design is never saved)
    let joint_info = s.exec("joint.inspect", json!({})).unwrap_or(Value::Null);
    let links = s.exec("joint.list", json!({})).map(|l| l["links"].clone()).unwrap_or(Value::Null);
    let (joints, skipped) = mates::read(&joint_info, &links);
    for (n, why) in skipped {
        report.add(&design_name, &n, "mate", Status::Note, why);
    }
    for j in joint_info["joints"].as_array().into_iter().flatten() {
        if let Some(id) = j["id"].as_i64() {
            let _ = s.exec("joint.delete", json!({"joint": id}));
        }
    }
    // an imported STEP assembly holds its parts in a rigid group: unlocked, so they can be moved too
    if let Ok(jl) = s.exec("joint.list", json!({})) {
        for g in jl["rigid_groups"].as_array().into_iter().flatten() {
            if let Some(c) = g["component"].as_i64() {
                let _ = s.exec("component.unlock", json!({"component": c}));
            }
        }
    }
    let meta = s.exec("document.metadata", json!({})).unwrap_or(Value::Null);
    let wanted = mates::faces_wanted(&joints);
    let mut mate_faces: HashMap<(i64, String), (Value, Option<crate::geom::V3>)> = HashMap::new();
    let all = s.exec("component.properties", json!({"component": 0, "descendants": true}))?;
    let counts = s.exec("inspect.measure", json!({}))?;
    let mut file_fps = Vec::new();
    let mut used = std::collections::HashSet::new();
    for b in all["bodies"].as_array().into_iter().flatten() {
        let name = b["name"].as_str().unwrap_or("");
        let vol = b["volume_mm3"].as_f64();
        // face and edge counts: the measured body of that name (and volume, when names repeat)
        let m = counts["bodies"].as_array().into_iter().flatten().enumerate()
            .find(|(i, m)| !used.contains(i) && m["name"] == name && (vol.is_none() || (m["volume_mm3"].as_f64().unwrap_or(0.0) - vol.unwrap()).abs() <= 1e-6 * vol.unwrap().abs().max(1.0)))
            .map(|(i, m)| {
                used.insert(i);
                m.clone()
            })
            .unwrap_or(Value::Null);
        let closed = b["closed"].as_bool().unwrap_or(true) && vol.is_some();
        // surface bodies are not fingerprinted (as the SolidWorks exporter writes them)
        if closed {
            let mut fp = features::fingerprint(b, &m, closed);
            if let Some(mo) = features::mesh_moments(s, name, vol.unwrap_or(0.0)) {
                fp["moments"] = json!(mo);
            }
            file_fps.push(fp);
        }
    }
    if all["bodies"].as_array().map(|b| b.is_empty()).unwrap_or(true) {
        bail!("the design has no bodies to write");
    }
    set_progress(&job.progress, "Writing the geometry", &design_name, -1.0);
    // named after the design: SolidCraft names the STEP product after the file
    let dir = scratch_dir().join(format!("export-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    let step_path = dir.join(format!("{}.step", file_safe(&comps[&root].name).unwrap_or_else(|| file_safe(&design_name).unwrap_or_else(|| "Design".into()))));
    s.exec("file.export_step", json!({"path": step_path.to_string_lossy()}))?;
    let step = std::fs::read(&step_path).context("SolidCraft wrote no STEP file")?;
    let _ = std::fs::remove_file(&step_path);
    let _ = std::fs::remove_dir(&dir);
    let step = one_brep(&as_ap214(&step));

    // documents: depth first, parts and sub-assemblies before the assemblies that place them
    let mut order: Vec<i64> = Vec::new();
    fn visit(c: i64, comps: &HashMap<i64, Comp>, content: &dyn Fn(i64) -> bool, order: &mut Vec<i64>, depth: usize) {
        if order.contains(&c) || depth > 32 {
            return;
        }
        if let Some(k) = comps.get(&c) {
            for ch in &k.children {
                if content(ch.1) {
                    visit(ch.1, comps, content, order, depth + 1);
                }
            }
        }
        order.push(c);
    }
    visit(root, &comps, &content, &mut order, 0);
    let mut doc_ids: HashMap<i64, String> = HashMap::new();
    let mut taken = std::collections::HashSet::new();
    for c in &order {
        let base = format!("doc:{}", if comps[c].name.is_empty() { format!("Component{c}") } else { comps[c].name.clone() });
        let mut id = base.clone();
        let mut i = 2;
        while !taken.insert(id.clone()) {
            id = format!("{base} ({i})");
            i += 1;
        }
        doc_ids.insert(*c, id);
    }

    let parameters: Vec<Value> = user_params.iter().map(|p| {
        let ex = p["expression"].as_str().unwrap_or("");
        let neutral = expr::neutral(ex, &params).map(|x| x.0).unwrap_or_else(|| p["value"].to_string());
        json!({"name": p["name"], "value": p["value"], "unit": "none", "expression": neutral})
    }).collect();
    let source_file = job.input.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let mut documents = Vec::new();
    let total = order.len().max(1);
    for (n, c) in order.iter().enumerate() {
        let comp = &comps[c];
        let doc_id = doc_ids[c].clone();
        let name = if comp.name.is_empty() { design_name.clone() } else { comp.name.clone() };
        let assembly = comp.children.iter().any(|ch| content(ch.1));
        if !assembly {
            set_progress(&job.progress, "Writing the feature history", &name, n as f32 / total as f32);
            let want: Vec<String> = wanted.get(c).map(|w| w.iter().cloned().collect()).unwrap_or_default();
            let (mut doc, faces) = write_part(s, &comps, *c, root, &doc_id, &name, &source_file, &items, &params, &values, &parameters, &want, report)?;
            if let Err(e) = looks::add_to_part(s, *c, &mut doc, &meta, *c == root) {
                report.add(&name, &name, "material", Status::Note, format!("material and colours not written ({e})"));
            }
            for (f, d) in faces {
                mate_faces.insert((*c, f), d);
            }
            documents.push(doc);
            continue;
        }
        if comp.bodies > 0 || own_features(*c) > 0 {
            report.add(&name, &name, "assembly", Status::Note, "the assembly's own bodies are not written separately (its parts are)");
        }
        let mut mate_notes = Vec::new();
        let mates_here = mates::for_assembly(*c, root, strip, &comps, &joints, &mate_faces, &mut mate_notes);
        for (n, why) in mate_notes {
            report.add(&name, &n, "mate", Status::Note, why);
        }
        let mut components = Vec::new();
        for (occ, child, oname, tr, grounded) in &comp.children {
            if !content(*child) {
                continue;
            }
            components.push(json!({"id": format!("c:{oname}"), "document": doc_ids[child], "name": oname,
                                   "configuration": "Default", "transform": ncx_transform(tr), "fixed": grounded}));
            let _ = occ;
        }
        let mut adoc = json!({"id": doc_id, "kind": "assembly", "name": name, "sourceFile": source_file,
                              "configurations": [{"id": "cfg:Default", "name": "Default", "values": {}}],
                              "activeConfiguration": "cfg:Default", "components": components, "mates": mates_here, "features": []});
        looks::add_properties(&mut adoc, &meta, *c == root);
        documents.push(adoc);
    }

    let model = json!({
        "format": "ncx",
        "schema": ncx::schema_version(),
        "generator": {"app": "SolidCraft", "version": sc_version.trim_start_matches("solidcraft-cli ").to_string(),
                      "exporter": format!("ncx-solidcraft {}", env!("CARGO_PKG_VERSION"))},
        "units": {"length": "mm", "angle": "deg", "mass": "kg"},
        "orientation": {"up": up},
        "root": doc_ids[&root],
        "documents": documents,
        "fingerprint": {"bodies": file_fps}
    });
    let v = ncx::validate(&model)?;
    if v["status"].as_str().map(|s| s != "OK").unwrap_or(false) {
        bail!("the NCX data failed its own check: {}", v["message"].as_str().unwrap_or(""));
    }
    let out = ncx::payload_write(&step, &model, &format!("ncx-solidcraft {}", env!("CARGO_PKG_VERSION")))?;
    std::fs::write(&job.output, out).with_context(|| format!("cannot write {}", job.output.display()))?;
    report.notes.push(format!("{} bodies written, in {} document(s).", file_fps.len(), order.len()));
    Ok(())
}

/// One part document: the component's features, written in its own frame (its placement moved to identity
/// first, in this session only).
#[allow(clippy::too_many_arguments)]
fn write_part(
    s: &mut Session,
    comps: &HashMap<i64, Comp>,
    c: i64,
    root: i64,
    doc_id: &str,
    name: &str,
    source_file: &str,
    items: &[(i64, features::Item)],
    params: &std::collections::HashSet<String>,
    values: &HashMap<String, f64>,
    parameters: &[Value],
    mate_faces: &[String],
    report: &mut Report,
) -> Result<(Value, Vec<(String, (Value, Option<crate::geom::V3>))>)> {
    if c != 0 && c != root {
        if let Some(path) = occurrence_path(comps, c) {
            place_at_identity(s, &path)?;
        }
    }
    let mut w = refs::Walk::new(s, c);
    let mut part = features::Part::new(&mut w, name, params.clone(), values.clone());
    let mine: Vec<&features::Item> = items.iter().filter(|(ic, _)| *ic == c).map(|(_, it)| it).collect();
    for (i, it) in mine.iter().enumerate() {
        part.next = mine.get(i + 1).map(|n| (n.k, n.type_name.clone(), n.info.clone()));
        part.write(it, report)?;
    }
    part.w.at(None)?;
    let fps = part.fingerprints_with(true)?;
    // the faces its mates were made on, described in its own frame
    let mut faces = Vec::new();
    for f in mate_faces {
        if let Some(d) = part.w.face_by_name(f)? {
            faces.push((f.clone(), d));
        }
    }
    report.notes.push(format!("{name}: {} features written natively, {} as their finished shape.", part.mapped, part.opaque));
    // nothing written natively (an imported shape, or only features without a counterpart): a plain part
    let plain = part.mapped == 0;
    let features = if plain { Vec::new() } else { std::mem::take(&mut part.features) };
    let equations = std::mem::take(&mut part.equations);
    let bodies: Vec<Value> = fps.iter().filter(|(_, _, sheet)| !sheet).enumerate().map(|(i, (n, fp, _))| {
        let mut fp = fp.clone();
        if plain {
            fp["plain"] = json!(true);
        }
        json!({"id": format!("body:{}", i + 1), "name": n, "fingerprint": fp})
    }).collect();
    if plain {
        report.add(name, name, "part", Status::Note, "written as its shape (it has no feature history SolidCraft can describe)");
    }
    let mut doc = json!({
        "id": doc_id, "kind": "part", "name": name, "sourceFile": source_file,
        "configurations": [{"id": "cfg:Default", "name": "Default", "values": {}}],
        "activeConfiguration": "cfg:Default",
        "parameters": parameters, "equations": if plain { Vec::new() } else { equations }, "features": features, "bodies": bodies,
    });
    if plain {
        doc["plain"] = json!(true);
    }
    Ok((doc, faces))
}

/// Moves a placement (given by its occurrence path) so that its component's frame is the world's: released from
/// the ground first (in this session only: the design is never saved).
fn place_at_identity(s: &mut Session, path: &[i64]) -> Result<()> {
    let occ = *path.last().unwrap_or(&0);
    let _ = s.exec("occurrence.ground", json!({"occurrence": occ, "path": path, "grounded": false}));
    s.exec("occurrence.move", json!({"occurrence": occ, "path": path, "space": "world", "to_matrix": IDENTITY}))?;
    Ok(())
}

/// The occurrence ids from the top down to the component's first placement.
fn occurrence_path(comps: &HashMap<i64, Comp>, c: i64) -> Option<Vec<i64>> {
    let mut path = Vec::new();
    let mut cur = c;
    for _ in 0..32 {
        let (occ, parent) = *comps.get(&cur)?.occurrences.first()?;
        path.push(occ);
        if parent == 0 {
            path.reverse();
            return Some(path);
        }
        cur = parent;
    }
    None
}

/// SolidCraft's placement (rows: the axes' images, then the translation) as NCX's row-major 4x4 (column vectors).
fn ncx_transform(t: &Value) -> Value {
    let m = |i: usize, j: usize| t[i][j].as_f64().unwrap_or(if i == j { 1.0 } else { 0.0 });
    let mut out = Vec::with_capacity(16);
    for i in 0..4 {
        for j in 0..4 {
            out.push(if i == 3 { if j == 3 { 1.0 } else { 0.0 } } else { m(j, i) });
        }
    }
    json!(out)
}

/// The turn an NCX import gives a Y-up model: +90° about X.
fn is_upright_turn(t: &Value) -> bool {
    let want = [[1.0, 0.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, -1.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0]];
    (0..4).all(|i| (0..4).all(|j| (t[i][j].as_f64().unwrap_or(f64::NAN) - want[i][j]).abs() < 1e-9))
}

/// A name usable as a file name, or None.
fn file_safe(name: &str) -> Option<String> {
    let s: String = name.chars().map(|c| if "<>:\"/\\|?*".contains(c) || c.is_control() { '_' } else { c }).collect();
    let s = s.trim().trim_end_matches('.').to_string();
    if s.is_empty() {
        None
    } else {
        Some(s)
    }
}

/// SolidCraft writes a part with several bodies as one B-rep representation per body, each tied to the part's
/// shape by a SHAPE_REPRESENTATION_RELATIONSHIP. NCX writers put a part's solids in one representation (as
/// SolidWorks does); the core places the notice body beside them there. The solids are moved into the first
/// representation and the others, with their relationships, removed. Files with more than one product (assemblies)
/// are left as they are.
fn one_brep(step: &[u8]) -> Vec<u8> {
    let text = String::from_utf8_lossy(step).into_owned();
    let lines: Vec<&str> = text.split_inclusive('\n').collect();
    let head = |l: &str, kw: &str| -> Option<String> {
        let t = l.trim_start();
        let eq = t.find('=')?;
        let rest = t[eq + 1..].trim_start();
        if rest.starts_with(kw) && rest[kw.len()..].starts_with('(') {
            Some(t[..eq].trim().to_string())
        } else {
            None
        }
    };
    if lines.iter().filter(|l| head(l, "PRODUCT").is_some()).count() != 1 {
        return step.to_vec();
    }
    let reps: Vec<(usize, String)> = lines.iter().enumerate().filter_map(|(i, l)| head(l, "ADVANCED_BREP_SHAPE_REPRESENTATION").map(|id| (i, id))).collect();
    if reps.len() < 2 || lines.iter().any(|l| !l.trim_end().ends_with(';') && !l.trim().is_empty() && head(l, "ADVANCED_BREP_SHAPE_REPRESENTATION").is_some()) {
        return step.to_vec();
    }
    // the items of each representation: ('name',(#a,#b,...),#ctx);
    let items_of = |l: &str| -> Option<Vec<String>> {
        let open = l.find(",(")? + 2;
        let close = open + l[open..].find(')')?;
        Some(l[open..close].split(',').map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect())
    };
    let first = &reps[0];
    let Some(mut items) = items_of(lines[first.0]) else { return step.to_vec() };
    let others: std::collections::HashSet<String> = reps[1..].iter().map(|r| r.1.clone()).collect();
    for (i, _) in &reps[1..] {
        match items_of(lines[*i]) {
            Some(more) => {
                for m in more {
                    if !items.contains(&m) {
                        items.push(m);
                    }
                }
            }
            None => return step.to_vec(),
        }
    }
    let mut out = String::with_capacity(text.len());
    for (i, l) in lines.iter().enumerate() {
        if i == first.0 {
            let open = l.find(",(").unwrap_or(0) + 2;
            let close = open + l[open..].find(')').unwrap_or(0);
            out.push_str(&l[..open]);
            out.push_str(&items.join(","));
            out.push_str(&l[close..]);
        } else if reps[1..].iter().any(|r| r.0 == i) {
            continue;
        } else if head(l, "SHAPE_REPRESENTATION_RELATIONSHIP").is_some()
            && others.iter().any(|o| l.contains(&format!("{o},")) || l.contains(&format!("{o})")))
        {
            continue;
        } else {
            out.push_str(l);
        }
    }
    out.into_bytes()
}

/// SolidCraft's STEP writer labels its files AP242. NCX files must declare AP214 (FORMAT-SPEC §1: SolidWorks
/// 2018 never returns from importing an AP242 assembly). The entities SolidCraft writes (B-rep geometry and
/// product structure) are AP214's too, so only the labels change: the schema, the application context and
/// protocol, written as SolidWorks writes them.
pub(crate) fn as_ap214(step: &[u8]) -> Vec<u8> {
    let text = String::from_utf8_lossy(step);
    let mut out = String::with_capacity(text.len());
    for line in text.split_inclusive('\n') {
        let t = line.trim_start();
        if t.starts_with("FILE_SCHEMA") && t.contains("AP242") {
            out.push_str("FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\n");
        } else if t.contains("=APPLICATION_CONTEXT('managed model based 3d engineering')") {
            out.push_str(&line.replace("managed model based 3d engineering", "core data for automotive mechanical design processes"));
        } else if t.contains("=APPLICATION_PROTOCOL_DEFINITION(") && t.contains("ap242_managed_model_based_3d_engineering") {
            out.push_str(&line.replace("'ap242_managed_model_based_3d_engineering',2014", "'automotive_design',2000"));
        } else {
            out.push_str(line);
        }
    }
    out.into_bytes()
}
