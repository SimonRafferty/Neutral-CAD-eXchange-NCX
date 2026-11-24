//! NCX file -> SolidCraft design.
//!
//! The payload's modelling log is replayed, feature by feature, in a private headless SolidCraft session
//! (see `sc.rs`). Every part is verified against the file's own fingerprints (FORMAT-SPEC §6.1); a part that
//! does not match, or that has a feature which can be neither rebuilt nor carried as its result, comes in as
//! the file's own geometry instead ("geometry wins"). The design is saved as a SolidCraft `.sc` file.

mod assembly;
mod features;
mod features2;
mod looks;
mod mates;
mod sketch;

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};

use anyhow::{anyhow, bail, Context, Result};
use serde_json::{json, Value};

use crate::ncx;
use crate::report::{Report, Status};
use crate::sc::{self, Session};

/// What the UI shows while a job runs.
#[derive(Clone, Default)]
pub struct Progress {
    pub phase: String,
    pub detail: String,
    /// 0..1, or negative when unknown.
    pub fraction: f32,
}

pub type ProgressHandle = Arc<Mutex<Progress>>;

pub fn set_progress(p: &ProgressHandle, phase: &str, detail: &str, fraction: f32) {
    if let Ok(mut g) = p.lock() {
        g.phase = phase.to_string();
        g.detail = detail.to_string();
        g.fraction = fraction;
    }
}

pub struct Job {
    pub input: PathBuf,
    pub output: PathBuf,
    pub cli: PathBuf,
    pub cancel: Arc<AtomicBool>,
    pub progress: ProgressHandle,
    /// Developer option: write every SolidCraft command and answer to `<output>.log.txt`.
    pub log: bool,
    /// Developer option: skip the native rebuild and bring the file in as plain geometry.
    pub plain_only: bool,
}

pub struct Outcome {
    pub report: Report,
    pub report_path: Option<PathBuf>,
    /// What to tell the user, in plain words.
    pub message: String,
    /// True when the message is about a problem the user should know of.
    pub problem: bool,
}

/// Imports an NCX file into a new SolidCraft design file.
pub fn run(job: &Job) -> Result<Outcome> {
    set_progress(&job.progress, "Reading the file", "", -1.0);
    let bytes = std::fs::read(&job.input).with_context(|| format!("cannot read {}", job.input.display()))?;
    let payload = ncx::payload_read(&bytes)?;

    let mut report = Report::default();
    report.title = format!(
        "NCX import of {} ({})",
        job.input.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(),
        env!("CARGO_PKG_VERSION")
    );

    let allow = allowed_folders(job);
    let mut s = Session::start(&job.cli, &allow, job.cancel.clone())?;
    s.keep_log = job.log;
    let result = import_into(&mut s, job, payload, &mut report);
    if job.log {
        let _ = std::fs::write(job.output.with_extension("log.txt"), s.log.join("\n"));
    }
    drop(s);

    match result {
        Ok(()) => {}
        Err(e) if sc::is_cancelled(&e) => {
            let _ = std::fs::remove_file(&job.output);
            report.serious.push("The import was cancelled. No design was saved.".into());
            let path = report.save_beside(&job.output).ok();
            return Ok(Outcome { report, report_path: path, message: "The import was cancelled.".into(), problem: true });
        }
        Err(e) => return Err(e),
    }

    let report_path = report.save_beside(&job.output).ok();
    let (message, problem) = if report.serious.is_empty() {
        ("Your model has been imported.".to_string(), false)
    } else {
        let mut m = String::from("Your model has been imported, but:\n");
        for p in &report.serious {
            m.push_str(&format!("- {p}\n"));
        }
        if let Some(p) = &report_path {
            m.push_str(&format!("\nThe full report, for anyone helping you, is saved as\n{}", p.display()));
        }
        (m, true)
    };
    Ok(Outcome { report, report_path, message, problem })
}

fn allowed_folders(job: &Job) -> Vec<PathBuf> {
    let mut v = Vec::new();
    for p in [&job.input, &job.output] {
        if let Some(d) = p.parent() {
            let d = if d.as_os_str().is_empty() { Path::new(".") } else { d };
            if let Ok(c) = d.canonicalize() {
                v.push(c);
            }
        }
    }
    v.push(scratch_dir());
    v
}

/// The translator's own scratch folder (result STEP snippets).
pub fn scratch_dir() -> PathBuf {
    let d = std::env::temp_dir().join("ncx-solidcraft");
    let _ = std::fs::create_dir_all(&d);
    d.canonicalize().unwrap_or(d)
}

fn import_into(s: &mut Session, job: &Job, payload: ncx::Payload, report: &mut Report) -> Result<()> {
    let (model, header) = match payload {
        ncx::Payload::Model { model, header } => (model, header),
        ncx::Payload::None => {
            report.notes.push("The file carries no NCX data: it came in as plain geometry.".into());
            plain_import(s, job, None, "y", report, "the file has no NCX data")?;
            return save(s, &job.output);
        }
    };
    let up = model["orientation"]["up"].as_str().unwrap_or("y").to_string();
    let docs = model["documents"].as_array().cloned().unwrap_or_default();
    let root_id = model["root"].as_str().unwrap_or("");
    let root = docs.iter().find(|d| d["id"] == root_id).cloned().ok_or_else(|| anyhow!("the file names no root document"))?;

    // A part with no history in its source (spec §4 `plain`): its geometry is all there is.
    if root["kind"] == "part" && docs.len() == 1 && root["plain"].as_bool() == Some(true) {
        let name = root["name"].as_str().unwrap_or("Part");
        report.add(name, name, "part", Status::Note, "no feature history in the source: brought in as its shape, as it was there");
        plain_import(s, job, Some(&header), &up, report, "the part has no history in its source")?;
        return save(s, &job.output);
    }
    if job.plain_only {
        let why = "plain import was asked for";
        for d in &docs {
            report.add(d["name"].as_str().unwrap_or("?"), d["name"].as_str().unwrap_or("?"), "document", Status::Fallback, why);
        }
        plain_import(s, job, Some(&header), &up, report, why)?;
        return save(s, &job.output);
    }
    // Assemblies, and parts made from other documents (layouts): every part in a component of its own.
    if !(root["kind"] == "part" && docs.len() == 1) {
        assembly::import_documents(s, job, &model, &header, &up, report)?;
        return save(s, &job.output);
    }

    let doc_name = root["name"].as_str().unwrap_or("Part").to_string();
    // The part is built in a component of its own, placed at the origin while it is built (so the file's
    // coordinates are SolidCraft's world coordinates for every pick) and turned upright at the end.
    let c = s.exec("component.create", json!({"name": doc_name, "activate": true, "grounded": false}))?;
    let occurrence = c["occurrence"].clone();
    let mut st = PartState::new(&doc_name, Default::default());
    st.component = c["component"].clone();
    match rebuild_part(s, job, &root, report, &mut st, &mut Default::default()) {
        Ok(()) => {
            apply_looks(s, &root, &c["component"], &mut looks::Made::default(), report)?;
            stand_upright(s, &doc_name, &occurrence, &up, report)?;
        }
        Err(e) if sc::is_cancelled(&e) => return Err(e),
        Err(e) => {
            let why = format!("{e:#}");
            report.add(&doc_name, &doc_name, "part", Status::Fallback, &why);
            report.notes.push(format!("{doc_name} came in as its geometry, without history: {why}"));
            s.tool("new_design", json!({}))?;
            plain_import(s, job, Some(&header), &up, report, &why)?;
        }
    }
    save(s, &job.output)
}

fn save(s: &mut Session, output: &Path) -> Result<()> {
    let _ = std::fs::remove_file(output);
    s.exec("file.save_as", json!({"path": output.to_string_lossy(), "format": "container"}))?;
    Ok(())
}

/// The file's own B-rep, without the notice body, upright.
fn plain_import(s: &mut Session, job: &Job, header: Option<&Value>, up: &str, report: &mut Report, why: &str) -> Result<()> {
    set_progress(&job.progress, "Bringing in the geometry", why, -1.0);
    let mut params = json!({"path": job.input.to_string_lossy(), "names": "solid"});
    if up == "y" {
        params["up"] = json!("y");
    }
    s.exec("file.insert_step", params)?;
    let notice = header.and_then(|h| h.get("notice")).and_then(Value::as_str).unwrap_or("READ ME");
    remove_notice_bodies(s, notice)?;
    report.notes.push("Parts brought in as geometry have the right shape but no feature history.".into());
    Ok(())
}

/// Deletes the "READ ME" notice body (FORMAT-SPEC §2.6) from imported geometry.
fn remove_notice_bodies(s: &mut Session, notice: &str) -> Result<()> {
    let m = s.tool("measure", json!({}))?;
    let key: String = notice.chars().take(12).collect();
    let names: Vec<String> = m["bodies"]
        .as_array()
        .map(|b| {
            b.iter()
                .filter_map(|x| x["name"].as_str())
                .filter(|n| n.starts_with(&key) || n.contains("READ ME - NCX"))
                .map(String::from)
                .collect()
        })
        .unwrap_or_default();
    if !names.is_empty() {
        s.exec("solid.remove", json!({"bodies": names}))?;
    }
    Ok(())
}

/// What the importer knows of the design it is building.
#[derive(Clone)]
pub(crate) struct PartState {
    pub doc_name: String,
    /// payload sketch feature id -> the sketch as built
    pub sketches: HashMap<String, sketch::Built>,
    /// payload feature id -> SolidCraft feature or construction name
    pub names: HashMap<String, String>,
    /// dimension full name (D1@SK_Base) -> SolidCraft parameter name (d3)
    pub dim_params: HashMap<String, String>,
    /// equations by target (full name or "feature:id") -> neutral expression
    pub equations: HashMap<String, String>,
    /// payload parameter name -> its name in the design (renamed when another part already uses it)
    pub params: HashMap<String, String>,
    /// the parameters this part added (removed again if the part falls back to plain geometry)
    pub added_params: Vec<String>,
    /// bodies that were in the design before this part was built (other parts): left out of its checks
    pub exclude: std::collections::HashSet<String>,
    /// at least one feature came in as its carried result (verification at exchange tolerance)
    pub used_results: bool,
    /// feature names already used in the design (SolidCraft's are unique across the whole design)
    pub taken_names: std::collections::HashSet<String>,
    /// the part's SolidCraft component (its id)
    pub component: Value,
    /// payload axis feature id -> its line (start, end), for the features that use it as a direction
    pub axis_lines: HashMap<String, (crate::geom::V3, crate::geom::V3)>,
    /// the part's active sheet-metal rule (name, thickness)
    pub sheet_rule: Option<(String, f64)>,
    /// payload surface feature id -> the surface bodies it made
    pub surface_bodies: HashMap<String, Vec<String>>,
    /// features built with a guessed side or direction (checked afterwards)
    pub uncertain: std::collections::BTreeSet<String>,
    /// features to build the other way round from the likeliest guess
    pub flips: std::collections::HashSet<String>,
}

impl PartState {
    pub fn new(doc_name: &str, exclude: std::collections::HashSet<String>) -> PartState {
        PartState {
            doc_name: doc_name.to_string(),
            sketches: HashMap::new(),
            names: HashMap::new(),
            dim_params: HashMap::new(),
            equations: HashMap::new(),
            params: HashMap::new(),
            added_params: Vec::new(),
            exclude,
            used_results: false,
            taken_names: Default::default(),
            component: Value::Null,
            axis_lines: HashMap::new(),
            sheet_rule: None,
            surface_bodies: HashMap::new(),
            uncertain: Default::default(),
            flips: Default::default(),
        }
    }

    /// The source's feature name, or, when another part already has a feature of that name, the name with
    /// this part's name after it: "Sketch1 (Fan Cowl)".
    pub fn unique_name(&mut self, name: &str) -> String {
        let mut n = name.to_string();
        if self.taken_names.contains(&n) {
            n = format!("{name} ({})", self.doc_name);
            let mut i = 2;
            while self.taken_names.contains(&n) {
                n = format!("{name} ({} {i})", self.doc_name);
                i += 1;
            }
        }
        self.taken_names.insert(n.clone());
        n
    }
}

/// The names of every feature and sketch in the design now.
pub(crate) fn feature_names(s: &mut Session) -> Result<std::collections::HashSet<String>> {
    let d = s.tool("inspect_design", json!({"measure": false, "sketches": false, "timeline": "summary", "bodies": false}))?;
    let mut names: std::collections::HashSet<String> =
        d["timeline"].as_array().into_iter().flatten().filter_map(|t| t["name"].as_str().map(String::from)).collect();
    names.extend(d["sketches"].as_array().into_iter().flatten().filter_map(|t| t["name"].as_str().map(String::from)));
    Ok(names)
}

/// A parameter name free in the design: the payload's own, else made unique with the part's name.
fn free_param_name(name: &str, doc_name: &str, taken: &std::collections::HashSet<String>) -> String {
    if !taken.contains(name) {
        return name.to_string();
    }
    let part: String = doc_name.chars().map(|c| if c.is_alphanumeric() { c } else { '_' }).collect();
    let base = format!("{name}_{part}");
    let mut n = base.clone();
    let mut i = 2;
    while taken.contains(&n) {
        n = format!("{base}_{i}");
        i += 1;
    }
    n
}

pub(crate) fn rebuild_part(
    s: &mut Session,
    job: &Job,
    doc: &Value,
    report: &mut Report,
    st: &mut PartState,
    taken: &mut std::collections::HashSet<String>,
) -> Result<()> {
    let doc_name = st.doc_name.clone();

    // Global variables first: dimensions and features may use them.
    for p in doc["parameters"].as_array().into_iter().flatten() {
        let name = p["name"].as_str().unwrap_or("");
        if name.is_empty() {
            continue;
        }
        let sc_name = free_param_name(name, &doc_name, taken);
        let expr = p["expression"]
            .as_str()
            .map(|e| features::translate_expression(e, st))
            .unwrap_or(None)
            .unwrap_or_else(|| num(&p["value"]).to_string());
        let renamed = if sc_name != name { format!(" (named {sc_name} here: another part has a {name})") } else { String::new() };
        match s.exec("parameters.add", json!({"name": sc_name, "expression": expr})) {
            Ok(_) => {
                st.params.insert(name.to_string(), sc_name.clone());
                st.added_params.push(sc_name.clone());
                taken.insert(sc_name.clone());
                let status = if renamed.is_empty() { Status::Rebuilt } else { Status::Approximate };
                report.add(&doc_name, name, "parameter", status, renamed.trim());
            }
            Err(e) => {
                // a value the user can still edit, rather than none
                let v = num(&p["value"]).to_string();
                if s.exec("parameters.add", json!({"name": sc_name, "expression": v})).is_ok() {
                    st.params.insert(name.to_string(), sc_name.clone());
                    st.added_params.push(sc_name.clone());
                    taken.insert(sc_name.clone());
                    report.add(&doc_name, name, "parameter", Status::Approximate, format!("its expression was not taken ({e}); its value was{renamed}"));
                } else {
                    report.add(&doc_name, name, "parameter", Status::Failed, e.to_string());
                }
            }
        }
    }
    for e in doc["equations"].as_array().into_iter().flatten() {
        if let (Some(t), Some(x)) = (e["target"].as_str(), e["expression"].as_str()) {
            st.equations.insert(t.to_string(), x.to_string());
            if let Some(r) = e.get("targetRef") {
                // keyed "feature:dimension id" for sketch dimensions, "feature:field" for feature fields
                let key = match r["kind"].as_str() {
                    Some("dimension") => r["feature"].as_str().zip(r["id"].as_str()),
                    Some("featureField") => r["feature"].as_str().zip(r["field"].as_str()),
                    _ => None,
                };
                if let Some((f, k)) = key {
                    st.equations.insert(format!("{f}:{k}"), x.to_string());
                }
            }
        }
    }

    // Features whose side or direction the file does not state (a sheet's thickness, a draft's pull) are built
    // the likeliest way and checked: against the next carried result's record of the part just before it, else
    // by the part's final check, which rebuilds the part with each of them turned the other way until one fits.
    let start = s.exec("edit.checkpoint", json!({}))?["checkpoint"].clone();
    let snapshot = st.clone();
    let n_items = report.items.len();
    let first = build_features(s, job, doc, report, st);
    let err = match first {
        Ok(()) => return Ok(()),
        Err(e) if sc::is_cancelled(&e) => return Err(e),
        Err(e) => e,
    };
    let guessed: Vec<String> = st.uncertain.iter().cloned().collect();
    if guessed.is_empty() || !format!("{err:#}").contains("differs") {
        return Err(err);
    }
    let mut cp = start;
    // each guess turned round alone, then (few guesses) every pair, then all of them
    let mut combos: Vec<Vec<String>> = guessed.iter().take(6).map(|g| vec![g.clone()]).collect();
    if guessed.len() <= 4 {
        for i in 0..guessed.len() {
            for j in i + 1..guessed.len() {
                combos.push(vec![guessed[i].clone(), guessed[j].clone()]);
            }
        }
    }
    if guessed.len() > 2 {
        combos.push(guessed.clone());
    }
    for combo in combos.iter() {
        let g = &combo[0];
        s.exec("edit.restore_checkpoint", json!({"checkpoint": cp}))?;
        cp = s.exec("edit.checkpoint", json!({}))?["checkpoint"].clone();
        let mut flips = snapshot.flips.clone();
        flips.extend(combo.iter().cloned());
        *st = snapshot.clone();
        st.flips = flips;
        report.items.truncate(n_items);
        match build_features(s, job, doc, report, st) {
            Ok(()) => {
                let fname = doc["features"]
                    .as_array()
                    .and_then(|a| a.iter().find(|f| f["id"] == g.as_str()))
                    .and_then(|f| f["name"].as_str())
                    .unwrap_or("?");
                report.add(&doc_name, fname, "check", Status::Note, "built the other way round from the first try: the part then matches the file");
                return Ok(());
            }
            Err(e) if sc::is_cancelled(&e) => return Err(e),
            Err(_) => {}
        }
    }
    s.exec("edit.restore_checkpoint", json!({"checkpoint": cp}))?;
    *st = snapshot;
    report.items.truncate(n_items);
    Err(err)
}

/// Feature types that add no geometry of their own.
fn adds_no_geometry(kind: &str) -> bool {
    matches!(kind, "sketch" | "sketch3d" | "plane" | "axis" | "point" | "coordinateSystem" | "sheetMetal" | "flatPattern"
        | "weldment" | "cosmeticThread")
}

/// The record of the part just before the next feature that carries a result, if only features that add no
/// geometry lie between: what the part must be now.
fn next_replaces(features: &[Value], i: usize) -> Option<Vec<Value>> {
    for f in &features[i + 1..] {
        let kind = f["type"].as_str().unwrap_or("");
        if let Some(r) = f.get("result") {
            let v: Vec<Value> = r["replaces"].as_array().cloned().unwrap_or_default();
            return if v.is_empty() { None } else { Some(v) };
        }
        if !adds_no_geometry(kind) {
            return None;
        }
    }
    None
}

fn build_features(s: &mut Session, job: &Job, doc: &Value, report: &mut Report, st: &mut PartState) -> Result<()> {
    let doc_name = st.doc_name.clone();
    let features = doc["features"].as_array().cloned().unwrap_or_default();
    let total = features.len().max(1);
    for (i, f) in features.iter().enumerate() {
        let name = f["name"].as_str().unwrap_or("?").to_string();
        let kind = f["type"].as_str().unwrap_or("?").to_string();
        set_progress(&job.progress, &format!("Rebuilding {doc_name}"), &name, i as f32 / total as f32);
        if is_suppressed(f, doc) {
            report.add(&doc_name, &name, &kind, Status::Note, "suppressed in the source: left out");
            continue;
        }
        if f["status"] == "opaque" && f.get("noGeometry").and_then(Value::as_bool) == Some(true) {
            let why = f["reason"].as_str().filter(|r| !r.is_empty()).unwrap_or("another program's reference data");
            report.add(&doc_name, &name, &kind, Status::Note, format!("adds no geometry ({why}): left out"));
            continue;
        }
        // A feature that carries its result is checked against it: built natively first, and where that differs
        // the other variant is tried (a flange's other side), else the result is used.
        let checked = f.get("result").is_some();
        let fid = f["id"].as_str().unwrap_or("").to_string();
        let cp = s.exec("edit.checkpoint", json!({}))?["checkpoint"].clone();
        let (names0, taken0) = (st.names.clone(), st.taken_names.clone());
        let mut outcome = features::build(s, st, f);
        // a guessed side checked against the next carried result's record of the part before it
        if outcome.is_ok() && st.uncertain.contains(&fid) {
            if let Some(before) = next_replaces(&features, i) {
                if !matches_fingerprints(s, st, &before)? {
                    s.exec("edit.restore_checkpoint", json!({"checkpoint": cp}))?;
                    st.names = names0.clone();
                    st.taken_names = taken0.clone();
                    let flipped = !st.flips.contains(&fid);
                    if flipped {
                        st.flips.insert(fid.clone());
                    } else {
                        st.flips.remove(&fid);
                    }
                    let _cp_again = s.exec("edit.checkpoint", json!({}))?;
                    outcome = features::build(s, st, f).map(|_| {
                        features::Built::Approx("built the other way round from the first try: matches the part the file records after it".into())
                    });
                }
            }
        }
        let cp = if checked { Some(cp) } else { None };
        if let (Ok(_), Some(cp)) = (&outcome, &cp) {
            if !matches_result(s, st, f)? {
                s.exec("edit.restore_checkpoint", json!({"checkpoint": cp}))?;
                let _ = s.exec("edit.checkpoint", json!({}));
                st.names = names0.clone();
                st.taken_names = taken0.clone();
                let mut ok = false;
                if kind == "edgeFlange" {
                    let cp2 = s.exec("edit.checkpoint", json!({}))?["checkpoint"].clone();
                    if features2::edge_flange(s, st, f, true).is_ok() && matches_result(s, st, f)? {
                        outcome = Ok(features::Built::Approx("built with its flange the other way round from the source's words; matches its carried shape".into()));
                        ok = true;
                    } else {
                        s.exec("edit.restore_checkpoint", json!({"checkpoint": cp2}))?;
                        st.names = names0.clone();
                        st.taken_names = taken0.clone();
                    }
                }
                if !ok {
                    outcome = Err(anyhow!("built, but it differs from the shape the file carries for it"));
                }
            }
        }
        match outcome {
            Ok(features::Built::Done) => report.add(&doc_name, &name, &kind, Status::Rebuilt, ""),
            Ok(features::Built::Approx(why)) => report.add(&doc_name, &name, &kind, Status::Approximate, why),
            Ok(features::Built::Skipped(why)) => report.add(&doc_name, &name, &kind, Status::Note, why),
            Err(e) if sc::is_cancelled(&e) => return Err(e),
            Err(e) => {
                let why = format!("{e:#}");
                if f.get("result").is_some() {
                    match features::apply_result(s, st, f) {
                        Ok(()) => {
                            report.add(&doc_name, &name, &kind, Status::Result, format!("not rebuilt ({why}): its shape was carried"));
                            continue;
                        }
                        Err(e2) if sc::is_cancelled(&e2) => return Err(e2),
                        Err(e2) => bail!("{name} ({kind}) could not be built ({why}), nor its carried shape used ({e2:#})"),
                    }
                }
                if kind == "sketch" || kind == "sketch3d" {
                    // a sketch adds no geometry: the features that use it will say so
                    report.add(&doc_name, &name, &kind, Status::Failed, why);
                    continue;
                }
                bail!("{name} ({kind}) could not be built: {why}");
            }
        }
    }

    set_progress(&job.progress, &format!("Checking {doc_name}"), "", 1.0);
    verify_part(s, st, doc, report)
}

fn is_suppressed(f: &Value, doc: &Value) -> bool {
    let active = doc["activeConfiguration"].as_str().unwrap_or("");
    match f.get("suppressed") {
        Some(Value::Bool(b)) => *b,
        Some(Value::Object(m)) => m.get(active).and_then(Value::as_bool).unwrap_or(false),
        _ => false,
    }
}

/// Whether the part's bodies now include the bodies a feature's carried result has (exchange tolerance).
fn matches_result(s: &mut Session, st: &PartState, f: &Value) -> Result<bool> {
    let want: Vec<Value> = f["result"]["bodies"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|b| b["sheet"].as_bool() != Some(true))
        .filter_map(|b| b.get("fingerprint").cloned())
        .collect();
    matches_fingerprints(s, st, &want)
}

/// Whether the part's bodies include bodies with these fingerprints (each against its nearest; exchange tolerance).
fn matches_fingerprints(s: &mut Session, st: &PartState, want: &[Value]) -> Result<bool> {
    let want: Vec<Value> = want.to_vec();
    if want.is_empty() {
        return Ok(true);
    }
    let have = measure_fingerprints_with(s, false, &st.exclude)?;
    // each carried body against the nearest body of the part
    let mut picked = Vec::new();
    for w in &want {
        let v = num(&w["volume"]);
        let c = crate::geom::v3(&w["centroid"]).unwrap_or([0.0; 3]);
        let best = have
            .iter()
            .map(|(_, fp)| {
                let dv = (num(&fp["volume"]) - v).abs() / v.abs().max(1e-9);
                let dc = crate::geom::norm(crate::geom::sub(crate::geom::v3(&fp["centroid"]).unwrap_or([0.0; 3]), c));
                (fp.clone(), dv * 1e3 + dc)
            })
            .min_by(|a, b| a.1.partial_cmp(&b.1).unwrap());
        match best {
            Some((fp, _)) => picked.push(fp),
            None => return Ok(false),
        }
    }
    let rep = ncx::fingerprint_compare(&json!({"bodies": want}), &json!({"bodies": picked}), "exchange")?;
    let result = rep.get("result").or_else(|| rep.get("status")).and_then(Value::as_str).unwrap_or("");
    Ok(result != "mismatch")
}

pub(crate) fn num(v: &Value) -> f64 {
    v.as_f64().unwrap_or(0.0)
}

/// Measures the design's solids as NCX fingerprints. `with_moments`: also each body's principal moments
/// (SolidCraft reports inertia per placement, not per body: with several bodies each is measured alone,
/// between a checkpoint and its restore).
pub(crate) fn measure_fingerprints_with(
    s: &mut Session,
    with_moments: bool,
    exclude: &std::collections::HashSet<String>,
) -> Result<Vec<(String, Value)>> {
    let m = s.tool("measure", json!({}))?;
    let bodies: Vec<Value> = m["bodies"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|b| !exclude.contains(b["name"].as_str().unwrap_or("")))
        .cloned()
        .collect();
    // other parts' bodies in the design: moments only where this part is alone
    let with_moments = with_moments && exclude.is_empty();
    let solids: Vec<&Value> = bodies.iter().filter(|b| b["closed"].as_bool().unwrap_or(true)).collect();
    let names: Vec<String> = bodies.iter().filter_map(|b| b["name"].as_str().map(String::from)).collect();
    let mut out = Vec::new();
    for b in &solids {
        let name = b["name"].as_str().unwrap_or("").to_string();
        let moments = if !with_moments {
            None
        } else if names.len() == 1 {
            body_moments(s, b).ok()
        } else {
            let others: Vec<&String> = names.iter().filter(|n| **n != name).collect();
            let cp = s.exec("edit.checkpoint", json!({}))?;
            let id = cp["checkpoint"].clone();
            let r = s.exec("solid.remove", json!({"bodies": others})).and_then(|_| body_moments(s, b));
            s.exec("edit.restore_checkpoint", json!({"checkpoint": id}))?;
            r.ok()
        };
        out.push((name, crate::geom::fingerprint_of(b, moments)));
    }
    Ok(out)
}

pub(crate) fn measure_fingerprints(s: &mut Session, st: &PartState) -> Result<Vec<(String, Value)>> {
    measure_fingerprints_with(s, true, &st.exclude)
}

/// The names of the bodies in the design now.
pub(crate) fn body_names(s: &mut Session) -> Result<std::collections::HashSet<String>> {
    let m = s.tool("measure", json!({"names_only": true}))?;
    Ok(m["bodies"].as_array().into_iter().flatten().filter_map(|b| b["name"].as_str().map(String::from)).collect())
}

/// Principal moments of volume (mm^5) of the only body left in the design, from its mass properties.
fn body_moments(s: &mut Session, body: &Value) -> Result<crate::geom::V3> {
    let mp = s.exec("assembly.mass_properties", json!({}))?;
    let density = body["density_g_cm3"].as_f64().filter(|d| *d > 0.0).ok_or_else(|| anyhow!("no density"))?;
    let group = mp["groups"]
        .as_array()
        .and_then(|g| g.iter().find(|x| !x["mass"].is_null()))
        .ok_or_else(|| anyhow!("no mass group"))?;
    let t = &group["mass"]["inertia_world_kg_mm2"];
    let mut m = [[0.0; 3]; 3];
    for (i, row) in m.iter_mut().enumerate() {
        for (j, cell) in row.iter_mut().enumerate() {
            *cell = t[i][j].as_f64().ok_or_else(|| anyhow!("no inertia"))?;
        }
    }
    // kg mm^2 -> mm^5 at the body's density (g/cm^3 = 1e-6 kg/mm^3)
    let per = 1.0 / (density * 1e-6);
    let ev = crate::geom::principal(m);
    Ok([ev[0] * per, ev[1] * per, ev[2] * per])
}

fn verify_part(s: &mut Session, st: &mut PartState, doc: &Value, report: &mut Report) -> Result<()> {
    let expected: Vec<Value> = doc["bodies"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|b| b.get("fingerprint").is_some() && !b["sheet"].as_bool().unwrap_or(false))
        .map(|b| {
            let mut fp = b["fingerprint"].clone();
            fp["name"] = b["name"].clone();
            fp
        })
        .collect();
    let actual = measure_fingerprints(s, st)?;
    if expected.is_empty() {
        report.notes.push(format!("{}: the file gives no solid fingerprints to check against", st.doc_name));
        return Ok(());
    }
    if actual.len() != expected.len() {
        bail!("the rebuild made {} solid bodies where the file has {}", actual.len(), expected.len());
    }
    let profile = if st.used_results { "exchange" } else { "native" };
    let exp = json!({"bodies": expected});
    let act = json!({"bodies": actual.iter().map(|(_, f)| f.clone()).collect::<Vec<_>>()});
    let rep = ncx::fingerprint_compare(&exp, &act, profile)?;
    let result = rep.get("result").or_else(|| rep.get("status")).and_then(Value::as_str).unwrap_or("");
    if result == "mismatch" {
        let diffs: Vec<String> = rep["differences"]
            .as_array()
            .into_iter()
            .flatten()
            .filter(|d| d["severity"] != "info")
            .take(4)
            .map(|d| format!("{} {} vs {}", d["quantity"].as_str().unwrap_or("?"), short(&d["actual"]), short(&d["expected"])))
            .collect();
        bail!("the rebuilt shape differs from the file's ({})", diffs.join(", "));
    }
    if result == "count_only" {
        report.add(&st.doc_name, &st.doc_name, "check", Status::Note, "same shape; faces or edges split differently");
    } else {
        report.add(&st.doc_name, &st.doc_name, "check", Status::Rebuilt, format!("shape matches the file ({profile} tolerance)"));
    }
    // Give the bodies the source's names, paired as the check paired them.
    if let Some(pairs) = rep["pairs"].as_array() {
        for p in pairs {
            let (Some(i), Some(j)) = (p[0].as_u64(), p[1].as_u64()) else { continue };
            let want = expected[i as usize]["name"].as_str().unwrap_or("");
            let have = &actual[j as usize].0;
            if !want.is_empty() && want != have {
                let _ = s.exec("body.rename", json!({"body": have, "name": want}));
            }
        }
    }
    Ok(())
}

fn short(v: &Value) -> String {
    match v.as_f64() {
        Some(x) => format!("{x:.4}"),
        None => v.to_string(),
    }
}

/// SolidCraft is Z-up. A Y-up model's component is turned a quarter turn about X so it stands upright; its
/// features keep the file's coordinates inside that component. The component was made ungrounded (a
/// grounded placement can't move) and is grounded afterwards.
/// A part's material and colours, with its component active (failures are reported, not fatal).
pub(crate) fn apply_looks(s: &mut Session, doc: &Value, component: &Value, made: &mut looks::Made, report: &mut Report) -> Result<()> {
    s.exec("component.activate", json!({"component": component}))?;
    let r = looks::apply(s, doc, component, made, report);
    s.exec("component.activate", json!({"component": 0}))?;
    match r {
        Err(e) if sc::is_cancelled(&e) => Err(e),
        Err(e) => {
            report.add(doc["name"].as_str().unwrap_or("?"), "material and colours", "appearance", Status::Failed, format!("{e:#}"));
            Ok(())
        }
        Ok(()) => Ok(()),
    }
}

pub(crate) fn stand_upright(s: &mut Session, doc_name: &str, occurrence: &Value, up: &str, report: &mut Report) -> Result<()> {
    let _ = s.exec("component.activate", json!({"component": 0}));
    let moved = if up == "y" {
        Some(s.exec("occurrence.move", json!({"occurrence": occurrence, "axis": [1, 0, 0], "angle": "90 deg", "origin": [0, 0, 0]})))
    } else {
        None
    };
    let _ = s.exec("occurrence.ground", json!({"occurrence": occurrence, "grounded": true}));
    let Some(moved) = moved else { return Ok(()) };
    match moved {
        Ok(_) => report.notes.push(format!("{doc_name} was made Y-up; it is turned a quarter turn about X so it stands upright in SolidCraft.")),
        Err(e) => report.notes.push(format!("{doc_name} could not be stood upright ({e}); it lies as the file's axes put it.")),
    }
    Ok(())
}
