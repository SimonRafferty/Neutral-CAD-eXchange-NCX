//! SolidCraft design -> NCX file.
//!
//! This version writes each design's shape and parameters: the STEP geometry comes from SolidCraft's own
//! STEP writer, the payload describes the part as `plain` (FORMAT-SPEC §4: geometry only, nothing to
//! rebuild, fingerprints marked plain), and the NCX core embeds it with the notice body. The feature
//! history is written by a later version.

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

fn export_from(s: &mut Session, job: &Job, sc_version: &str, report: &mut Report) -> Result<()> {
    set_progress(&job.progress, "Opening the design", "", -1.0);
    s.tool("open", json!({"path": job.input.to_string_lossy()})).context("SolidCraft could not open the design")?;
    let design = s.tool("inspect_design", json!({"measure": false, "sketches": false, "timeline": "summary"}))?;
    let name = design["name"].as_str().filter(|n| !n.is_empty()).map(String::from).unwrap_or_else(|| {
        job.input.file_stem().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "Design".into())
    });

    set_progress(&job.progress, "Measuring", &name, -1.0);
    let m = s.tool("measure", json!({}))?;
    let bodies: Vec<Value> = m["bodies"].as_array().cloned().unwrap_or_default();
    let solids: Vec<&Value> = bodies.iter().filter(|b| b["closed"].as_bool().unwrap_or(true)).collect();
    if solids.is_empty() {
        bail!("the design has no solid bodies to write");
    }
    // fingerprints with each body's principal moments (spec §6 requires them)
    let measured = crate::import::measure_fingerprints_with(s, true)?;

    set_progress(&job.progress, "Writing the geometry", &name, -1.0);
    let step_path = scratch_dir().join(format!("export-{}.step", std::process::id()));
    s.exec("file.export_step", json!({"path": step_path.to_string_lossy()}))?;
    let step = std::fs::read(&step_path).context("SolidCraft wrote no STEP file")?;
    let _ = std::fs::remove_file(&step_path);
    let step = as_ap214(&step);

    // The part: plain (geometry only), with the design's own parameters for whoever receives it.
    let doc_id = format!("doc:{name}");
    let mut fps = Vec::new();
    let mut doc_bodies = Vec::new();
    for (i, (body_name, fp)) in measured.iter().enumerate() {
        if fp.get("moments").is_none() {
            bail!("the inertia of body {body_name} could not be measured");
        }
        let mut fp = fp.clone();
        fp["plain"] = json!(true);
        fps.push(fp.clone());
        doc_bodies.push(json!({"id": format!("body:{}", i + 1), "name": body_name, "fingerprint": fp}));
    }
    let params: Vec<Value> = design["params"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|p| p["model"].as_bool() != Some(true))
        .map(|p| json!({"name": p["name"], "value": p["value"], "unit": "none", "expression": p["expression"]}))
        .collect();
    let model = json!({
        "format": "ncx",
        "schema": ncx::schema_version(),
        "generator": {"app": "SolidCraft", "version": sc_version.trim_start_matches("solidcraft-cli ").to_string(),
                      "exporter": format!("ncx-solidcraft {}", env!("CARGO_PKG_VERSION"))},
        "units": {"length": "mm", "angle": "deg", "mass": "kg"},
        "orientation": {"up": "z"},
        "root": doc_id,
        "documents": [{
            "id": doc_id, "kind": "part", "name": name,
            "sourceFile": job.input.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(),
            "plain": true,
            "configurations": [{"id": "cfg:Default", "name": "Default", "values": {}}],
            "activeConfiguration": "cfg:Default",
            "parameters": params, "equations": [], "features": [], "bodies": doc_bodies
        }],
        "fingerprint": {"bodies": fps}
    });
    let v = ncx::validate(&model)?;
    if v["status"].as_str().map(|s| s != "OK").unwrap_or(false) {
        bail!("the NCX data failed its own check: {}", v["message"].as_str().unwrap_or(""));
    }
    let out = ncx::payload_write(&step, &model, &format!("ncx-solidcraft {}", env!("CARGO_PKG_VERSION")))?;
    std::fs::write(&job.output, out).with_context(|| format!("cannot write {}", job.output.display()))?;
    report.add(&name, &name, "part", Status::Note, "written as its shape (this version does not write the feature history yet)");
    report.notes.push(format!("{} solid bodies written.", solids.len()));
    Ok(())
}

/// SolidCraft's STEP writer labels its files AP242. NCX files must declare AP214 (FORMAT-SPEC §1: SolidWorks
/// 2018 never returns from importing an AP242 assembly). The entities SolidCraft writes (B-rep geometry and
/// product structure) are AP214's too, so only the labels change: the schema, the application context and
/// protocol, written as SolidWorks writes them.
fn as_ap214(step: &[u8]) -> Vec<u8> {
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
