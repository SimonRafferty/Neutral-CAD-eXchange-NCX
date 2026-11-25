//! A part's SolidCraft features -> NCX features (FORMAT-SPEC §4), in timeline order.
//!
//! Written natively: sketches, extrudes and cuts, revolves, fillets, chamfers, boxes and cylinders (as a sketch and
//! an extrude: no other system has SolidCraft's primitives), holes, rectangular and circular patterns, mirrors,
//! shells and offset planes. Every other feature is written `opaque` with its carried result (the part's bodies
//! just after it, as STEP), so an importer still rebuilds the rest of the history. Features whose sense the file
//! cannot pin down the same way in every system (partial revolves, patterns, holes, shells, angled chamfers)
//! carry their result too, for the importer to check against.

use std::collections::{HashMap, HashSet};

use anyhow::{anyhow, bail, Result};
use serde_json::{json, Value};

use crate::geom::{cross, dot, norm, principal, sub, unit, v3, V3};
use crate::report::{Report, Status};

use super::refs::Walk;
use super::{as_ap214, expr, sketch};

/// One timeline feature as the export sees it.
pub struct Item {
    /// its index in the timeline (features before it: `k`)
    pub k: usize,
    pub id: i64,
    pub name: String,
    pub type_name: String,
    pub suppressed: bool,
    pub error: Option<String>,
    pub warning: Option<String>,
    /// `timeline.inspect`
    pub info: Value,
}

pub struct Part<'w, 'a> {
    pub w: &'w mut Walk<'a>,
    pub doc: String,
    pub params: HashSet<String>,
    /// every parameter's value (user and model), for the numbers behind expressions
    pub values: HashMap<String, f64>,
    pub features: Vec<Value>,
    pub equations: Vec<Value>,
    /// SolidCraft feature id -> the NCX feature that stands for it
    ids: HashMap<i64, String>,
    sketches: HashMap<i64, Value>,
    names: HashSet<String>,
    /// SolidCraft's sheet-metal rules (`sheet.rules`), read once; the rule the part's `sheetMetal` was written for
    rules: Option<Value>,
    sheet_rule_name: Option<String>,
    sheet_written: bool,
    /// the next feature of this part (timeline index, type, timeline.inspect), set by the caller before each write
    pub next: Option<(usize, String, Value)>,
    /// a face move written together with the angled edge flange after it: its index and definition
    face_move: Option<(usize, Value)>,
    /// the last unfold written: its NCX id and bends (a refold after it is that unfold reversed)
    last_unfold: Option<(String, Vec<Value>)>,
    /// the weldment feature NCX wants before the first structural member has been written
    weldment_written: bool,
    pub opaque: usize,
    pub mapped: usize,
}

impl<'w, 'a> Part<'w, 'a> {
    pub fn new(w: &'w mut Walk<'a>, doc: &str, params: HashSet<String>, values: HashMap<String, f64>) -> Self {
        Part {
            w,
            doc: doc.to_string(),
            params,
            values,
            features: Vec::new(),
            equations: Vec::new(),
            ids: HashMap::new(),
            sketches: HashMap::new(),
            names: HashSet::new(),
            rules: None,
            sheet_rule_name: None,
            sheet_written: false,
            next: None,
            face_move: None,
            last_unfold: None,
            weldment_written: false,
            opaque: 0,
            mapped: 0,
        }
    }

    pub fn write(&mut self, it: &Item, report: &mut Report) -> Result<()> {
        if it.suppressed {
            report.add(&self.doc, &it.name, "feature", Status::Note, "suppressed in SolidCraft: left out");
            return Ok(());
        }
        if let Some(e) = &it.error {
            report.add(&self.doc, &it.name, "feature", Status::Note, format!("it fails in SolidCraft ({e}): left out"));
            return Ok(());
        }
        self.names.insert(it.name.clone());
        let kind = it.info["type"].as_str().unwrap_or("").to_string();
        // a face moved back right before an angled edge flange placed "inside" is how SolidCraft makes the source's
        // "material inside" at that angle (the NCX import does it so): written with the flange, as that one flange
        if it.type_name.starts_with("OffsetFace") {
            if let Some((nk, nt, ni)) = &self.next {
                let nd = &ni["definition"];
                let pos = nd["position"].as_str().unwrap_or("inside");
                let angle_input = ni["inputs"].as_array().into_iter().flatten().find(|i| i["label"] == "Angle")
                    .and_then(|i| i["param"].as_str()).and_then(|p| self.values.get(p).copied()).unwrap_or(90.0);
                if *nk == it.k + 1 && nt == "EdgeFlangeFeature" && pos == "inside" && (angle_input - 90.0).abs() > 1e-6 {
                    self.face_move = Some((it.k, it.info["definition"].clone()));
                    return Ok(());
                }
            }
        }
        // a feature SolidCraft warns about that leaves the bodies as they were (a cut that misses) adds nothing:
        // written so (noGeometry), an importer skips it rather than making it cut somewhere
        if it.warning.is_some() && kind != "sketch" && kind != "construction_plane" && self.unchanged(it)? {
            self.no_geometry(it, &format!("it changes no body in SolidCraft ({})", it.warning.as_deref().unwrap_or("")));
            report.add(&self.doc, &it.name, &kind, Status::Note, "it changes no body in SolidCraft: written as adding nothing");
            return Ok(());
        }
        let r = match it.type_name.as_str() {
            "BaseFlangeFeature" => self.sheet_base(it),
            "ContourFlangeFeature" => self.sheet_contour(it),
            "ConvertToSheetMetalFeature" => self.sheet_convert(it),
            "EdgeFlangeFeature" => self.sheet_edge_flange(it),
            "UnfoldFeature" => self.sheet_unfold(it, false),
            "RefoldFeature" => self.sheet_unfold(it, true),
            _ => Err(anyhow!("-")),
        };
        let r = if r.as_ref().err().map(|e| e.to_string() == "-").unwrap_or(false) { match kind.as_str() {
            "sketch" => self.sketch(it),
            "extrude" => self.extrude(it),
            "revolve" => self.revolve(it),
            "fillet" => self.fillet(it),
            "chamfer" => self.chamfer(it),
            "cylinder" => self.cylinder(it),
            "box" => self.boxed(it),
            "hole" => self.hole(it),
            "pattern" => self.pattern(it),
            "mirror" => self.mirror(it),
            "shell" => self.shell(it),
            "construction_plane" => self.plane(it),
            "construction_axis" => self.axis(it),
            "frame_member" => self.frame_member(it),
            "draft" => self.draft(it),
            "sweep" => self.sweep(it),
            "combine" => self.combine(it),
            "remove" => self.remove(it),
            "move" => self.move_bodies(it),
            "rib" => self.rib(it),
            "split_body" | "split" => self.split(it),
            _ => Err(anyhow!("SolidCraft's {} has no NCX counterpart in this version", it.type_name)),
        } } else { r };
        match r {
            Ok(Some(note)) => {
                self.mapped += 1;
                report.add(&self.doc, &it.name, &kind, Status::Approximate, note);
            }
            Ok(None) => self.mapped += 1,
            Err(e) => {
                let why = e.to_string();
                self.opaque(it, &why)?;
                report.add(&self.doc, &it.name, &kind, Status::Result, format!("written as its finished shape: {why}"));
            }
        }
        Ok(())
    }

    // ------------------------------------------------------------------------------------------------ helpers

    fn def<'v>(&self, it: &'v Item) -> &'v Value {
        &it.info["definition"]
    }

    /// The value behind a feature input (by its label, e.g. "Distance"), and its expression.
    fn input(&self, it: &Item, label: &str) -> Option<(f64, String)> {
        let i = it.info["inputs"].as_array()?.iter().find(|i| i["label"] == label)?;
        let ex = i["expression"].as_str().unwrap_or("").to_string();
        let v = i["param"].as_str().and_then(|p| self.values.get(p).copied()).or_else(|| plain_number(&ex))?;
        Some((v, ex))
    }

    /// An equation for a feature's own dimension when its expression names a parameter (SolidWorks numbers a
    /// feature's dimensions D1, D2...; `field` is where the value sits in the definition).
    fn feature_equation(&mut self, fid: &str, name: &str, dim: &str, field: &str, ex: &str) {
        if let Some((n, true)) = expr::neutral(ex, &self.params) {
            self.equations.push(json!({"target": format!("{dim}@{name}"), "expression": n, "native": ex, "status": "mapped",
                                       "targetRef": {"kind": "featureField", "feature": fid, "field": field}}));
        }
    }

    fn fid(&self, it: &Item) -> String {
        format!("f:{}", it.id)
    }

    fn push(&mut self, it: &Item, id: &str, kind: &str, name: &str, def: Value, note: Option<&str>) {
        let mut f = json!({"id": id, "type": kind, "name": name, "native": {"app": "SolidCraft", "type": it.type_name},
                           "status": if note.is_some() { "approximate" } else { "mapped" }, "def": def});
        if let Some(n) = note {
            f["note"] = json!(n);
        }
        self.features.push(f);
    }

    /// A name no other feature of the part has (for features the export adds, such as a primitive's sketch).
    fn fresh_name(&mut self, base: &str) -> String {
        let mut n = base.to_string();
        let mut i = 2;
        while self.names.contains(&n) {
            n = format!("{base} {i}");
            i += 1;
        }
        self.names.insert(n.clone());
        n
    }

    /// The part's bodies now, as NCX fingerprints (name, fingerprint, sheet).
    pub fn fingerprints(&mut self) -> Result<Vec<(String, Value, bool)>> {
        self.fingerprints_with(false)
    }

    /// With `precise`, each solid's moments from its fine mesh (`mesh_moments`): for fingerprints an importer
    /// compares (the part's bodies, a carried result's bodies); without, SolidCraft's own (enough to tell whether a
    /// feature changed anything, and for `replaces`, which importers match by volume and centre).
    pub fn fingerprints_with(&mut self, precise: bool) -> Result<Vec<(String, Value, bool)>> {
        let bodies = self.w.bodies()?;
        let names: Vec<String> = bodies.iter().filter_map(|b| b["name"].as_str().map(String::from)).collect();
        let counts = if names.is_empty() {
            Value::Null
        } else {
            self.w.s.exec("inspect.measure", json!({"bodies": names}))?
        };
        let mut out = Vec::new();
        for b in &bodies {
            let name = b["name"].as_str().unwrap_or("").to_string();
            let m = counts["bodies"].as_array().into_iter().flatten().find(|x| x["name"] == name.as_str()).cloned().unwrap_or(Value::Null);
            let closed = b["closed"].as_bool().unwrap_or(true) && b["volume_mm3"].is_number();
            let mut fp = fingerprint(b, &m, closed);
            if closed && precise {
                if let Some(mo) = mesh_moments(self.w.s, &name, b["volume_mm3"].as_f64().unwrap_or(0.0)) {
                    fp["moments"] = json!(mo);
                }
            }
            out.push((name, fp, !closed));
        }
        Ok(out)
    }

    /// The carried result of the feature at `k`: the part's bodies before it (`replaces`) and after it (STEP).
    fn result(&mut self, k: usize) -> Result<Value> {
        self.result_span(k, k + 1)
    }

    /// The carried result of the features from `k0` to just before `k1` (one NCX feature SolidCraft makes in two).
    fn result_span(&mut self, k0: usize, k1: usize) -> Result<Value> {
        let k = k0;
        self.w.at(Some(k0))?;
        let before: Vec<Value> = self.fingerprints()?.into_iter().map(|(_, fp, _)| fp).collect();
        self.w.at(Some(k1))?;
        let after = self.fingerprints_with(true)?;
        let names: Vec<&String> = after.iter().map(|(n, _, _)| n).collect();
        let data = if names.is_empty() {
            String::new()
        } else {
            let path = crate::import::scratch_dir().join(format!("result-{}-{k}.step", std::process::id()));
            self.w.s.exec("file.export_step", json!({"path": path.to_string_lossy(), "bodies": names}))?;
            let bytes = std::fs::read(&path).map_err(|e| anyhow!("SolidCraft wrote no STEP for the result: {e}"))?;
            let _ = std::fs::remove_file(&path);
            String::from_utf8_lossy(&as_ap214(&bytes)).into_owned()
        };
        Ok(json!({"format": "step", "replaces": before,
                  "bodies": after.iter().map(|(n, fp, sheet)| json!({"name": n, "sheet": sheet, "fingerprint": fp})).collect::<Vec<_>>(),
                  "data": data}))
    }

    fn attach_result(&mut self, it: &Item) -> Result<()> {
        let r = self.result(it.k)?;
        if let Some(f) = self.features.last_mut() {
            f["result"] = r;
        }
        Ok(())
    }

    fn opaque(&mut self, it: &Item, why: &str) -> Result<()> {
        self.opaque += 1;
        let fid = self.fid(it);
        let mut f = json!({"id": fid, "type": "opaque", "name": it.name,
                           "native": {"app": "SolidCraft", "type": it.type_name}, "status": "opaque", "reason": why, "def": {}});
        // a feature that changes no body (a construction plane or axis) has no result
        self.w.at(Some(it.k))?;
        let before = self.fingerprints()?;
        self.w.at(Some(it.k + 1))?;
        let after = self.fingerprints()?;
        let same = before.len() == after.len()
            && before.iter().zip(after.iter()).all(|(a, b)| a.0 == b.0 && a.1["volume"] == b.1["volume"] && a.1["area"] == b.1["area"]);
        if same {
            f["noGeometry"] = json!(true);
        } else {
            f["result"] = self.result(it.k)?;
        }
        self.ids.insert(it.id, fid);
        self.features.push(f);
        Ok(())
    }

    /// Whether the part's bodies are the same just before and just after the feature.
    fn unchanged(&mut self, it: &Item) -> Result<bool> {
        self.w.at(Some(it.k))?;
        let before = self.fingerprints()?;
        self.w.at(Some(it.k + 1))?;
        let after = self.fingerprints()?;
        Ok(before.len() == after.len()
            && before.iter().zip(after.iter()).all(|(a, b)| a.0 == b.0 && a.1["volume"] == b.1["volume"] && a.1["area"] == b.1["area"]))
    }

    fn no_geometry(&mut self, it: &Item, why: &str) {
        let fid = self.fid(it);
        self.features.push(json!({"id": fid, "type": "opaque", "name": it.name, "native": {"app": "SolidCraft", "type": it.type_name},
                                  "status": "opaque", "reason": why, "noGeometry": true, "def": {}}));
        self.ids.insert(it.id, fid);
    }

    fn feature_ref(&self, sc_id: i64) -> Result<Value> {
        let id = self.ids.get(&sc_id).ok_or_else(|| anyhow!("it uses feature {sc_id}, which is not written natively"))?;
        Ok(json!({"kind": "feature", "by": "id", "id": id}))
    }

    /// The NCX plane for a SolidCraft plane reference: an origin plane, a written plane feature, a face, or
    /// none (the caller then places a sketch by its frame).
    fn plane_ref(&mut self, it: &Item, pl: &Value) -> Result<Option<Value>> {
        Ok(match pl["type"].as_str().unwrap_or("") {
            "origin" => origin_plane(pl["name"].as_str().unwrap_or("")).map(|p| json!({"kind": "originPlane", "by": "id", "plane": p})),
            "construction" => pl["feature"].as_i64().and_then(|f| self.ids.get(&f)).map(|id| json!({"kind": "feature", "by": "id", "id": id})),
            "face" => match v3(&pl["at"]) {
                Some(at) => {
                    self.w.at(Some(it.k))?;
                    self.w.face(at, None).ok()
                }
                None => None,
            },
            "custom" => {
                let q = &pl["plane"];
                match (v3(&q["origin"]), v3(&q["x_dir"]), v3(&q["y_dir"])) {
                    (Some(o), Some(x), Some(y)) => self.plane_at(it, o, unit(cross(x, y)))?,
                    _ => None,
                }
            }
            _ => None,
        })
    }

    /// The plane through `o` with normal `n` as a reference: a planar face of the part lying in it (in the state
    /// the feature saw), else an origin plane when it is one; None when neither (the caller places by the frame).
    fn plane_at(&mut self, it: &Item, o: V3, n: V3) -> Result<Option<Value>> {
        self.w.at(Some(it.k))?;
        if let Some(face) = self.w.planar_face_in(o, n)? {
            return Ok(Some(face));
        }
        if let Some((ax, _)) = axis_word(n) {
            if dot(o, n).abs() < 1e-9 {
                let plane = match ax {
                    "z" => "front",
                    "y" => "top",
                    _ => "right",
                };
                return Ok(Some(json!({"kind": "originPlane", "by": "id", "plane": plane})));
            }
        }
        Ok(None)
    }

    // ----------------------------------------------------------------------------------------------- features

    fn sketch(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        // SolidCraft describes a sketch only where the timeline has evaluated it
        self.w.at(None)?;
        let sk = self.w.s.exec("sketch.inspect", json!({"sketch": it.id}))?;
        let fid = self.fid(it);
        let pl = &d["plane"];
        let mut notes: Vec<String> = Vec::new();
        let plane = if pl["type"] == "offset" {
            // a plane offset from another: written as a plane feature of its own, the sketch on it
            match self.offset_plane(it, &format!("{fid}:plane"), &format!("{} Plane", it.name), pl)? {
                Some(id) => json!({"kind": "feature", "by": "id", "id": id}),
                None => json!({"kind": "frame"}),
            }
        } else {
            match self.plane_ref(it, pl)? {
                Some(p) => p,
                None => {
                    if pl["type"] != "custom" {
                        notes.push("placed by its plane's position (its plane is not written natively)".into());
                    }
                    json!({"kind": "frame"})
                }
            }
        };
        let out = sketch::convert(&sk, &fid, &it.name, plane, &self.params);
        notes.extend(out.notes);
        self.equations.extend(out.equations);
        let note = if out.approximate { Some(notes.join("; ")) } else { None };
        self.push(it, &fid, "sketch", &it.name, out.def, note.as_deref());
        self.sketches.insert(it.id, sk);
        self.ids.insert(it.id, fid);
        Ok(note)
    }

    /// A plane feature offset from an origin plane, a written plane or a face; returns its id.
    fn offset_plane(&mut self, it: &Item, id: &str, name: &str, pl: &Value) -> Result<Option<String>> {
        let base = &pl["base"];
        let base_ref = match self.plane_ref(it, base)? {
            Some(r) => r,
            None => return Ok(None),
        };
        let ex = pl["distance"].as_str().map(String::from).unwrap_or_else(|| pl["distance"].to_string());
        let dist = self.input(it, "Offset").map(|x| x.0).or_else(|| plain_number(&ex)).unwrap_or(0.0);
        let (o, n) = base_frame(base, &base_ref);
        let origin = [o[0] + n[0] * dist, o[1] + n[1] * dist, o[2] + n[2] * dist];
        let def = json!({"references": [{"ref": base_ref, "constraint": "distance", "value": dist.abs(), "flip": dist < 0.0}],
                         "origin": origin, "normal": n});
        let pname = if name == it.name { it.name.clone() } else { self.fresh_name(name) };
        self.push(it, id, "plane", &pname, def, None);
        self.feature_equation(id, &pname, "D1", "references.0.value", &ex);
        Ok(Some(id.to_string()))
    }

    fn plane(&mut self, it: &Item) -> Result<Option<String>> {
        let pl = self.def(it)["plane"].clone();
        if pl["type"] != "offset" {
            bail!("a plane made by {} is not written natively", pl["type"].as_str().unwrap_or("?"));
        }
        let fid = self.fid(it);
        match self.offset_plane(it, &fid, &it.name.clone(), &pl)? {
            Some(id) => {
                self.ids.insert(it.id, id);
                Ok(None)
            }
            None => bail!("its base plane is not written natively"),
        }
    }

    /// A frame member: NCX `structuralMember` (FORMAT-SPEC 4) on the path sketch's lines, its profile by the ISO
    /// names SolidWorks' library uses (`iso` / `square tube` / `20 x 20 x 2`); a weldment feature before the first.
    fn frame_member(&mut self, it: &Item) -> Result<Option<String>> {
        let d0 = self.def(it).clone();
        let d = if d0["path_sketch"].is_null() && d0["def"].is_object() { d0["def"].clone() } else { d0 };
        let path_sk = d["path_sketch"].as_i64().ok_or_else(|| anyhow!("its path is not a sketch (a 3D path or point pairs)"))?;
        let skref = self.feature_ref(path_sk)?;
        let skid = skref["id"].as_str().unwrap_or("").to_string();
        let lines: Vec<String> = d["lines"].as_array().into_iter().flatten().filter_map(|l| l.as_str().map(String::from)).collect();
        if lines.is_empty() {
            bail!("no member lines");
        }
        let pr = &d["profile"]["profile"];
        let num = |v: &Value| -> f64 { v.as_f64().or_else(|| v.as_str().and_then(|s| s.trim().trim_end_matches("mm").trim().parse().ok())).unwrap_or(0.0) };
        let fmt = |v: f64| if (v - v.round()).abs() < 1e-9 { format!("{}", v.round() as i64) } else { format!("{v}") };
        let (ty, size) = match pr["kind"].as_str().unwrap_or("") {
            "rect_tube" => {
                let (w, h, t) = (num(&pr["width"]), num(&pr["height"]), num(&pr["wall"]));
                (if (w - h).abs() < 1e-9 { "square tube" } else { "rectangular tube" }, format!("{} x {} x {}", fmt(w), fmt(h), fmt(t)))
            }
            "round_tube" => ("pipe", format!("{} x {}", fmt(num(&pr["diameter"])), fmt(num(&pr["wall"])))),
            "angle" => ("angle iron", format!("{} x {} x {}", fmt(num(&pr["width"])), fmt(num(&pr["height"])), fmt(num(&pr["thickness"])))),
            "bar" => ("rectangular bar", format!("{} x {}", fmt(num(&pr["width"])), fmt(num(&pr["height"])))),
            "round_bar" => ("round bar", fmt(num(&pr["diameter"]))),
            other => bail!("a {other} profile is not written natively"),
        };
        let corner = match d["corner"].as_str().unwrap_or("none") {
            "mitre" | "miter" => "miter",
            "butt" if d["flip"].as_bool() == Some(true) => "butt2",
            "butt" => "butt1",
            _ => "none",
        };
        let angle = self.input(it, "Rotation").map(|x| x.0).unwrap_or(0.0);
        if !self.weldment_written {
            self.features.push(json!({"id": "f:weldment", "type": "weldment", "name": "Weldment", "status": "mapped",
                                      "native": {"app": "SolidCraft", "type": "frame (written for its members)"}}));
            self.weldment_written = true;
        }
        let fid = self.fid(it);
        let segments: Vec<Value> = lines.iter().map(|l| json!({"kind": "sketchEntity", "by": "id", "feature": skid, "id": super::sketch::id_of(l)})).collect();
        let gap = d["gap"].as_str().map(|g| num(&json!(g))).unwrap_or(0.0);
        let def = json!({"profile": {"standard": "iso", "type": ty, "size": size},
                         "groups": [{"segments": segments, "angle": angle, "cornerTreatment": corner, "gapWithinGroup": gap, "gapForOtherGroups": 0,
                                     "mergeArcSegmentBodies": false, "miterMergeCondition": false}],
                         "connectedSegments": if d["coped"].as_bool() == Some(true) { "copedCut" } else { "simpleCut" }, "allowProtrusion": true});
        let note = "profile by its ISO name; checked by the carried result";
        self.push(it, &fid, "structuralMember", &it.name, def, Some(note));
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(Some(note.into()))
    }

    /// A construction axis through a round face: NCX `axis` (cylindricalFace), its line along the face's extent.
    fn axis(&mut self, it: &Item) -> Result<Option<String>> {
        let d0 = self.def(it).clone();
        let d = if d0["def"].is_object() { d0["def"].clone() } else { d0 };
        if d["type"] != "through_face" {
            bail!("an axis made by {} is not written natively", d["type"].as_str().unwrap_or("?"));
        }
        let at = v3(&d["face"]["at"]).ok_or_else(|| anyhow!("its face has no position"))?;
        let fid = self.fid(it);
        self.w.at(Some(it.k))?;
        let face = self.w.face(at, None)?;
        let g = &face["geom"];
        if g["type"] != "cylinder" && g["type"] != "cone" {
            bail!("an axis through a {} face is not written natively", g["type"].as_str().unwrap_or("?"));
        }
        let (Some(o), Some(ax)) = (v3(&g["params"]["origin"]), v3(&g["params"]["axis"])) else { bail!("its face has no axis") };
        let ax = unit(ax);
        // the line spans the face: its box corners measured along the axis
        let bb: Vec<f64> = g["bbox"].as_array().into_iter().flatten().filter_map(Value::as_f64).collect();
        let (mut lo, mut hi) = (f64::INFINITY, f64::NEG_INFINITY);
        if bb.len() == 6 {
            for i in 0..8 {
                let p = [bb[if i & 1 == 0 { 0 } else { 3 }], bb[if i & 2 == 0 { 1 } else { 4 }], bb[if i & 4 == 0 { 2 } else { 5 }]];
                let t = dot(sub(p, o), ax);
                lo = lo.min(t);
                hi = hi.max(t);
            }
        }
        if !lo.is_finite() {
            lo = 0.0;
            hi = 1.0;
        }
        let line = json!({"start": crate::geom::add(o, crate::geom::scale(ax, lo)), "end": crate::geom::add(o, crate::geom::scale(ax, hi))});
        self.push(it, &fid, "axis", &it.name, json!({"axisType": "cylindricalFace", "references": [face], "line": line}), None);
        self.ids.insert(it.id, fid);
        Ok(None)
    }

    fn extrude(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let sk_id = d["sketch"].as_i64().ok_or_else(|| anyhow!("no sketch"))?;
        let profile = self.feature_ref(sk_id)?;
        let op = d["operation"].as_str().unwrap_or("new_body");
        let cut = op == "cut";
        if op == "intersect" {
            bail!("an intersecting extrude is not written natively");
        }
        let mut def = json!({"profile": profile});
        if let Some(c) = self.contours(sk_id, &d["profiles"])? {
            def["contours"] = c;
        }
        let ext = &d["extent"];
        let dir = ext["direction"].as_str().unwrap_or("positive");
        let (dist, ex) = self.input(it, "Distance").unwrap_or((0.0, String::new()));
        let through = ext["through_all"].as_bool() == Some(true);
        let mut notes: Vec<String> = Vec::new();
        if ext.get("to").map(|v| !v.is_null()).unwrap_or(false) {
            bail!("an extrude up to a face is not written natively yet");
        }
        // NCX: an extrude runs along the sketch normal, a cut against it; SolidCraft's "positive" is along it
        def["reverse"] = json!(match dir {
            "negative" => !cut,
            _ => cut,
        });
        def["end"] = match (dir, through) {
            ("symmetric", true) => json!({"type": "throughAllBoth"}),
            (_, true) => json!({"type": "throughAll"}),
            ("symmetric", false) => json!({"type": "midPlane", "depth": dist}),
            _ => json!({"type": "blind", "depth": dist}),
        };
        if let Some((d2, _)) = self.input(it, "Distance 2").filter(|x| x.0 > 0.0) {
            def["end2"] = json!({"type": "blind", "depth": d2});
        }
        // through all both ways: an extrude's two directions (throughAllBoth is a hole's word only, spec 4)
        if def["end"]["type"] == "throughAllBoth" {
            def["end"] = json!({"type": "throughAll"});
            def["end2"] = json!({"type": "throughAll"});
        }
        if let Some((so, _)) = self.input(it, "Start offset").filter(|x| x.0 != 0.0) {
            def["start"] = json!({"type": "offset", "distance": so.abs(), "reverse": so < 0.0});
        }
        let mut carry = false;
        if let Some((t, _)) = self.input(it, "Taper").filter(|x| x.0 != 0.0) {
            def["draft"] = json!({"angle": t.abs(), "outward": t > 0.0});
            notes.push("taper: its direction is checked by the carried result".into());
            carry = true;
        }
        // a thin wall (NCX `thin`): SolidCraft's side words; which side SolidWorks calls "one direction" is checked
        // by the carried result
        if let Some(th) = d.get("thin").filter(|v| v.is_object()) {
            let (t, _) = self.input(it, "Thickness").ok_or_else(|| anyhow!("its wall has no thickness"))?;
            let ty = match th["side"].as_str().unwrap_or("outside") {
                "inside" => "oppositeDirection",
                "center" => "midPlane",
                "two_sides" => "twoDirection",
                _ => "oneDirection",
            };
            let t2 = self.input(it, "Thickness 2").map(|x| x.0).unwrap_or(0.0);
            def["thin"] = json!({"type": ty, "thickness": t, "thickness2": t2, "capEnds": false, "capThickness": 0});
            notes.push("thin wall: its side is checked by the carried result".into());
            carry = true;
        } else if d.get("thin").map(|v| v.as_bool() == Some(true)).unwrap_or(false) {
            bail!("a thin-walled extrude is not written natively yet");
        }
        if !cut {
            def["merge"] = json!(op != "new_body");
        }
        let note = if notes.is_empty() { None } else { Some(notes.join("; ")) };
        self.push(it, &fid, if cut { "cut" } else { "extrude" }, &it.name, def, note.as_deref());
        if !through {
            self.feature_equation(&fid, &it.name, "D1", "end.depth", &ex);
        }
        if carry {
            self.attach_result(it)?;
        }
        self.ids.insert(it.id, fid);
        Ok(note)
    }

    /// Picked profiles as contours (each the curve ids around it); None for the whole sketch.
    fn contours(&self, sk_id: i64, profiles: &Value) -> Result<Option<Value>> {
        if profiles.is_null() || profiles["type"] == "all" {
            return Ok(None);
        }
        let sk = self.sketches.get(&sk_id).ok_or_else(|| anyhow!("its sketch was not written"))?;
        let all = sk["profiles"].as_array().cloned().unwrap_or_default();
        // loops of curves: each a contour, when it runs along whole curves (no curve twice)
        if profiles["type"] == "curves" {
            let mut out = Vec::new();
            for l in profiles["loops"].as_array().into_iter().flatten() {
                let ids: Vec<String> = l.as_array().into_iter().flatten().filter_map(|c| c.as_str().map(sketch::id_of)).collect();
                let distinct: std::collections::HashSet<&String> = ids.iter().collect();
                if ids.is_empty() || distinct.len() != ids.len() || !whole_curves(self.sketches.get(&sk_id), l) {
                    bail!("its profile is a region bounded by parts of curves, not written natively yet");
                }
                // SolidCraft's loop is the region inside it, without the closed loops within (its sketch profile's
                // holes); NCX lists those as contours of their own (a contour inside another is a hole in it)
                let want: std::collections::BTreeSet<String> = ids.iter().cloned().collect();
                let holes: Vec<Vec<String>> = self.sketches.get(&sk_id).and_then(|s| s["profiles"].as_array()).into_iter().flatten()
                    .find(|p| p["curves"].as_array().map(|c| c.iter().filter_map(|x| x.as_str().map(sketch::id_of)).collect::<std::collections::BTreeSet<_>>() == want).unwrap_or(false))
                    .map(|p| p["holes"].as_array().into_iter().flatten()
                        .map(|h| h.as_array().into_iter().flatten().filter_map(|x| x.as_str().map(sketch::id_of)).collect())
                        .collect())
                    .unwrap_or_default();
                out.push(json!(ids));
                for h in holes {
                    out.push(json!(h));
                }
            }
            return Ok(Some(json!(out)));
        }
        let picked: Vec<usize> = match profiles["type"].as_str() {
            Some("indices") | Some("index") => profiles["indices"].as_array().into_iter().flatten().filter_map(|i| i.as_u64().map(|i| i as usize)).collect(),
            _ => bail!("its profile selection ({}) is not written natively", profiles["type"].as_str().unwrap_or("?")),
        };
        let mut out = Vec::new();
        for i in picked {
            let p = all.get(i).ok_or_else(|| anyhow!("profile {i} not found"))?;
            out.push(json!(p["curves"].as_array().into_iter().flatten().filter_map(|c| c.as_str().map(sketch::id_of)).collect::<Vec<_>>()));
            for h in p["holes"].as_array().into_iter().flatten() {
                let ids: Vec<String> = h.as_array().into_iter().flatten().filter_map(|c| c.as_str().map(sketch::id_of)).collect();
                if !ids.is_empty() {
                    out.push(json!(ids));
                }
            }
        }
        Ok(Some(json!(out)))
    }

    fn revolve(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let sk_id = d["sketch"].as_i64().ok_or_else(|| anyhow!("no sketch"))?;
        let profile = self.feature_ref(sk_id)?;
        let op = d["operation"].as_str().unwrap_or("new_body");
        if op == "intersect" {
            bail!("an intersecting revolve is not written natively");
        }
        let cut = op == "cut";
        let ax = &d["axis"];
        let axis = match ax["type"].as_str().unwrap_or("") {
            "world" => json!({"kind": "originAxis", "by": "id", "axis": ax["axis"].as_str().unwrap_or("Z").to_lowercase()}),
            _ if ax.get("curve").is_some() => {
                let sk = ax["sketch"].as_i64().unwrap_or(sk_id);
                let feat = self.ids.get(&sk).cloned().ok_or_else(|| anyhow!("its axis sketch is not written"))?;
                json!({"kind": "sketchEntity", "by": "id", "feature": feat, "id": sketch::id_of(ax["curve"].as_str().unwrap_or(""))})
            }
            _ => bail!("its axis ({}) is not written natively", ax),
        };
        let (angle, ex) = self.input(it, "Angle").unwrap_or((360.0, String::new()));
        let mut def = json!({"profile": profile, "axis": axis, "angle": angle.abs()});
        if let Some(c) = self.contours(sk_id, &d["profiles"])? {
            def["contours"] = c;
        }
        let symmetric = d["direction"] == "symmetric";
        def["mode"] = json!(if symmetric { "midPlane" } else { "oneDirection" });
        // NCX turns clockwise about the axis unreversed; SolidCraft turns counter-clockwise for a positive angle
        def["reverse"] = json!(angle > 0.0);
        if !cut {
            def["merge"] = json!(op != "new_body");
        }
        let partial = (angle.abs() - 360.0).abs() > 1e-9;
        let note = if partial && !symmetric { Some("its turning sense is checked by the carried result") } else { None };
        self.push(it, &fid, if cut { "revolveCut" } else { "revolve" }, &it.name, def, note);
        self.feature_equation(&fid, &it.name, "D1", "angle", &ex);
        if note.is_some() {
            self.attach_result(it)?;
        }
        self.ids.insert(it.id, fid);
        Ok(note.map(String::from))
    }

    fn edges(&mut self, it: &Item, d: &Value) -> Result<Vec<Value>> {
        self.w.at(Some(it.k))?;
        let body = d["body"].as_str().map(String::from);
        let mut out = Vec::new();
        for p in d["edges"].as_array().into_iter().flatten() {
            let p = v3(p).ok_or_else(|| anyhow!("an edge pick is not a point"))?;
            let e = self.w.edge(p, body.as_deref())?;
            if e["geom"]["seam"] == true || e["geom"]["degenerate"] == true {
                bail!("it picks a seam edge");
            }
            out.push(e);
        }
        if out.is_empty() {
            bail!("no edges");
        }
        Ok(out)
    }

    fn fillet(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        if d.get("radii").map(|r| r.is_array()).unwrap_or(false) || d.get("variable").is_some() {
            bail!("a variable fillet is not written natively");
        }
        let fid = self.fid(it);
        let edges = self.edges(it, &d)?;
        let (r, ex) = self.input(it, "Radius").ok_or_else(|| anyhow!("no radius"))?;
        self.push(it, &fid, "fillet", &it.name, json!({"filletType": "constant", "radius": r, "edges": edges, "propagate": true}), None);
        self.feature_equation(&fid, &it.name, "D1", "radius", &ex);
        self.ids.insert(it.id, fid);
        Ok(None)
    }

    fn chamfer(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let edges = self.edges(it, &d)?;
        let (dist, ex) = self.input(it, "Distance").ok_or_else(|| anyhow!("no distance"))?;
        let flipped = vec![false; edges.len()];
        let (def, note) = if let Some((a, _)) = self.input(it, "Angle").filter(|a| (a.0 - 45.0).abs() > 1e-9) {
            (json!({"chamferType": "angleDistance", "distance": dist, "angle": a, "edges": edges, "flipped": flipped, "propagate": true}),
             Some("which face takes the distance is checked by the carried result"))
        } else if let Some((d2, _)) = self.input(it, "Distance 2").filter(|x| (x.0 - dist).abs() > 1e-9) {
            (json!({"chamferType": "distanceDistance", "distance": dist, "distance2": d2, "edges": edges, "flipped": flipped, "propagate": true}),
             Some("which face takes which distance is checked by the carried result"))
        } else {
            // equal distances: written as SolidWorks writes them (45 degrees from the distance), which every importer
            // reads (SolidWorks' importer made an equalDistance chamfer of zero size: no second distance)
            (json!({"chamferType": "angleDistance", "distance": dist, "angle": 45.0, "edges": edges, "flipped": flipped, "propagate": true}), None)
        };
        self.push(it, &fid, "chamfer", &it.name, def, note);
        self.feature_equation(&fid, &it.name, "D1", "distance", &ex);
        if note.is_some() {
            self.attach_result(it)?;
        }
        self.ids.insert(it.id, fid);
        Ok(note.map(String::from))
    }

    /// A primitive as a sketch placed by its frame and an extrude.
    #[allow(clippy::too_many_arguments)]
    fn primitive(&mut self, it: &Item, frame: (V3, V3, V3), entities: Vec<Value>, constraints: Vec<Value>, dims: Vec<Value>,
                 end: Value, op: &str, dim_exprs: Vec<(String, String)>, depth_ex: &str) -> Result<Option<String>> {
        let fid = self.fid(it);
        let skid = format!("{fid}:sketch");
        let skname = self.fresh_name(&format!("{} Sketch", it.name));
        let (o, x, z) = frame;
        let plane = self.plane_at(it, o, z)?.unwrap_or_else(|| json!({"kind": "frame"}));
        let def = json!({"constrained": "under", "hidden": true, "plane": plane,
                         "frame": {"origin": o, "x": x, "z": z}, "entities": entities, "constraints": constraints,
                         "dimensions": dims});
        self.push(it, &skid, "sketch", &skname, def, None);
        for (dim, ex) in dim_exprs {
            self.equations.push(json!({"target": format!("{dim}@{skname}"), "expression": ex, "status": "mapped",
                                       "targetRef": {"kind": "dimension", "feature": skid, "id": dim}}));
        }
        let cut = op == "cut";
        if op == "intersect" {
            bail!("an intersecting primitive is not written natively");
        }
        let mut def = json!({"profile": {"kind": "feature", "by": "id", "id": skid}, "reverse": cut, "end": end});
        if !cut {
            def["merge"] = json!(op != "new" && op != "new_body");
        }
        self.push(it, &fid, if cut { "cut" } else { "extrude" }, &it.name, def, None);
        self.feature_equation(&fid, &it.name, "D1", "end.depth", depth_ex);
        self.ids.insert(it.id, fid);
        Ok(None)
    }

    fn cylinder(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let z = unit(v3(&d["axis"]).unwrap_or([0.0, 0.0, 1.0]));
        let base = v3(&d["base"]).ok_or_else(|| anyhow!("its base is not a point (an expression)"))?;
        let x = perpendicular(z);
        let (r, rex) = self.input(it, "Radius").ok_or_else(|| anyhow!("no radius"))?;
        let (h, hex) = self.input(it, "Height").ok_or_else(|| anyhow!("no height"))?;
        if d.get("targets").map(|t| t.is_array()).unwrap_or(false) {
            bail!("a cylinder limited to some bodies is not written natively");
        }
        let entities = vec![
            json!({"id": "p1", "type": "point", "p": [0.0, 0.0]}),
            json!({"id": "c1", "type": "circle", "center": [0.0, 0.0], "centerPoint": "p1", "radius": r}),
        ];
        // placed by its frame: the centre sits at the frame's origin (not the model's), held by the frame alone
        let constraints = Vec::new();
        let dims = vec![json!({"id": "D1", "name": "D1", "kind": "diameter", "entities": ["c1"], "value": 2.0 * r,
                               "driving": true, "textPosition": [r * 0.8, r * 0.8]})];
        let mut dim_exprs = Vec::new();
        if let Some((n, true)) = expr::neutral(&rex, &self.params) {
            dim_exprs.push(("D1".to_string(), format!("2 * ({n})")));
        }
        let op = d["operation"].as_str().unwrap_or("join").to_string();
        let end = if h < 0.0 { bail!("a cylinder of negative height") } else { json!({"type": "blind", "depth": h}) };
        self.primitive(it, (base, x, z), entities, constraints, dims, end, &op, dim_exprs, &hex)
    }

    fn boxed(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let corner = v3(&d["corner"]).ok_or_else(|| anyhow!("its position is not a point (an expression)"))?;
        let (l, lex) = self.input(it, "Length").ok_or_else(|| anyhow!("no length"))?;
        let (w, wex) = self.input(it, "Width").ok_or_else(|| anyhow!("no width"))?;
        let (h, hex) = self.input(it, "Height").ok_or_else(|| anyhow!("no height"))?;
        if d.get("targets").map(|t| t.is_array()).unwrap_or(false) {
            bail!("a box limited to some bodies is not written natively");
        }
        let centred = d["centred"].as_bool() == Some(true);
        // the sketch at the box's middle (centred) or its lower corner, in the XY directions
        let (o, end) = if centred {
            (corner, json!({"type": "midPlane", "depth": h}))
        } else {
            ([corner[0] + l / 2.0, corner[1] + w / 2.0, corner[2]], json!({"type": "blind", "depth": h}))
        };
        let (a, b) = (l / 2.0, w / 2.0);
        let pts = [[-a, -b], [a, -b], [a, b], [-a, b]];
        let mut entities: Vec<Value> = pts.iter().enumerate().map(|(i, p)| json!({"id": format!("p{}", i + 1), "type": "point", "p": p})).collect();
        for i in 0..4 {
            let j = (i + 1) % 4;
            entities.push(json!({"id": format!("l{}", i + 1), "type": "line", "start": pts[i], "end": pts[j],
                                 "startPoint": format!("p{}", i + 1), "endPoint": format!("p{}", j + 1)}));
        }
        let constraints = vec![
            json!({"type": "horizontal", "entities": ["l1"]}),
            json!({"type": "vertical", "entities": ["l2"]}),
            json!({"type": "horizontal", "entities": ["l3"]}),
            json!({"type": "vertical", "entities": ["l4"]}),
        ];
        let dims = vec![
            json!({"id": "D1", "name": "D1", "kind": "horizontal", "entities": ["p1", "p2"], "value": l, "driving": true, "textPosition": [0.0, -b - 8.0]}),
            json!({"id": "D2", "name": "D2", "kind": "vertical", "entities": ["p2", "p3"], "value": w, "driving": true, "textPosition": [a + 8.0, 0.0]}),
        ];
        let mut dim_exprs = Vec::new();
        for (dim, ex) in [("D1", &lex), ("D2", &wex)] {
            if let Some((n, true)) = expr::neutral(ex, &self.params) {
                dim_exprs.push((dim.to_string(), n));
            }
        }
        let op = d["operation"].as_str().unwrap_or("join").to_string();
        self.primitive(it, (o, [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]), entities, constraints, dims, end, &op, dim_exprs, &hex)
    }

    fn hole(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let pos = v3(&d["position"]).ok_or_else(|| anyhow!("its position is an expression"))?;
        // the centres: the sketch's points, else its one position
        let mut points: Vec<V3> = Vec::new();
        let mut position_sketch = Value::Null;
        if let Some(sk) = d["points"]["sketch"].as_i64() {
            let s = match self.sketches.get(&sk) {
                Some(s) => s.clone(),
                None => {
                    self.w.at(None)?;
                    self.w.s.exec("sketch.inspect", json!({"sketch": sk}))?
                }
            };
            let pl = &s["plane"];
            let (o, x, y) = (v3(&pl["origin"]).unwrap_or([0.0; 3]), v3(&pl["x_dir"]).unwrap_or([1.0, 0.0, 0.0]), v3(&pl["y_dir"]).unwrap_or([0.0, 1.0, 0.0]));
            for id in d["points"]["ids"].as_array().into_iter().flatten() {
                if let Some(p) = s["points"].as_array().into_iter().flatten().find(|p| p["id"] == *id) {
                    let a = crate::geom::v2(&p["at"]).unwrap_or([0.0; 2]);
                    points.push([o[0] + a[0] * x[0] + a[1] * y[0], o[1] + a[0] * x[1] + a[1] * y[1], o[2] + a[0] * x[2] + a[1] * y[2]]);
                }
            }
            if let Some(r) = self.ids.get(&sk) {
                position_sketch = json!({"kind": "feature", "by": "id", "id": r});
            }
        }
        if points.is_empty() {
            points.push(pos);
        }
        self.w.at(Some(it.k))?;
        let face = self.w.face(pos, None)?;
        let h = &d["hole"];
        let kind = h["type"].as_str().unwrap_or("simple");
        let num = |label: &str| self.input(it, label).map(|x| x.0).unwrap_or(0.0);
        let dia = num("Diameter");
        let mut sizes = json!({"holeDiameter": dia, "measuredDiameters": [dia]});
        let hole_type = match kind {
            "counterbore" => {
                let (cd, cdep) = (num("Counterbore diameter"), num("Counterbore depth"));
                sizes["counterBoreDiameter"] = json!(cd);
                sizes["counterBoreDepth"] = json!(cdep);
                sizes["measuredDiameters"] = json!([dia, cd]);
                "counterbore"
            }
            "countersink" => {
                let (cd, ca) = (num("Countersink diameter"), num("Countersink angle"));
                sizes["counterSinkDiameter"] = json!(cd);
                sizes["counterSinkAngle"] = json!(ca);
                sizes["measuredDiameters"] = json!([dia]);
                "countersink"
            }
            "simple" | "drilled" => "simple",
            other => bail!("a {other} hole is not written natively"),
        };
        let depth = self.input(it, "Depth").map(|x| x.0).filter(|v| *v > 0.0);
        let end = match depth {
            Some(dp) => {
                sizes["holeDepth"] = json!(dp);
                json!({"type": "blind", "depth": dp})
            }
            None => json!({"type": "throughAll", "depth": 0}),
        };
        let mut def = json!({"face": face, "points": points, "holeType": hole_type, "end": end, "sizes": sizes});
        if !position_sketch.is_null() {
            def["positionSketch"] = position_sketch;
        }
        let note = "carried with its result (hole conventions differ between systems)";
        self.push(it, &fid, "hole", &it.name, def, None);
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        let _ = note;
        Ok(None)
    }

    fn seeds(&self, d: &Value) -> Result<Vec<Value>> {
        if d.get("bodies").map(|b| b.is_array() && !b.as_array().unwrap().is_empty()).unwrap_or(false) {
            bail!("a pattern or mirror of bodies is not written natively");
        }
        let mut out = Vec::new();
        for f in d["features"].as_array().into_iter().flatten() {
            out.push(self.feature_ref(f.as_i64().ok_or_else(|| anyhow!("a seed is not a feature id"))?)?);
        }
        if out.is_empty() {
            bail!("no seed features");
        }
        Ok(out)
    }

    fn pattern(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let seeds = self.seeds(&d)?;
        let p = &d["pattern"];
        let num = |label: &str| self.input(it, label);
        let def = match p["type"].as_str().unwrap_or("") {
            "rectangular" => {
                let mut def = json!({"seeds": seeds, "geometryPattern": false, "seedOnly": false});
                for (k, dir, cl, sl) in [("dir1", "dir1", "Quantity", "Spacing"), ("dir2", "dir2", "Quantity 2", "Spacing 2")] {
                    let Some(v) = v3(&p[dir]) else { continue };
                    let count = num(cl).map(|x| x.0).unwrap_or(1.0).round() as i64;
                    if count < 2 && k == "dir2" {
                        continue;
                    }
                    let (ax, rev) = axis_word(v).ok_or_else(|| anyhow!("its direction {v:?} is not along an origin axis"))?;
                    let spacing = num(sl).map(|x| x.0).unwrap_or(0.0);
                    def[k] = json!({"ref": {"kind": "originAxis", "by": "id", "axis": ax}, "reverse": rev, "spacing": spacing, "count": count});
                }
                (def, "linearPattern")
            }
            "circular" => {
                let o = v3(&p["origin"]).unwrap_or([0.0; 3]);
                let a = v3(&p["axis"]).unwrap_or([0.0, 0.0, 1.0]);
                if norm(o) > 1e-9 && norm(cross(o, unit(a))) > 1e-9 {
                    bail!("its axis does not pass through the origin");
                }
                let (ax, rev) = axis_word(a).ok_or_else(|| anyhow!("its axis {a:?} is not an origin axis"))?;
                let count = num("Quantity").map(|x| x.0).unwrap_or(2.0).round() as i64;
                let angle = num("Total angle").map(|x| x.0).unwrap_or(360.0);
                (json!({"seeds": seeds, "axis": {"kind": "originAxis", "by": "id", "axis": ax}, "count": count, "angle": angle,
                        "equalSpacing": true, "reverse": rev, "geometryPattern": false}), "circularPattern")
            }
            other => bail!("a {other} pattern is not written natively"),
        };
        self.push(it, &fid, def.1, &it.name, def.0, None);
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(None)
    }

    fn mirror(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let plane = self.plane_ref(it, &d["plane"])?.ok_or_else(|| anyhow!("its mirror plane is not written natively"))?;
        if let Some(bodies) = d["bodies"].as_array().filter(|b| !b.is_empty()) {
            self.w.at(Some(it.k))?;
            let mut refs = Vec::new();
            for b in bodies {
                refs.push(self.w.body_ref(b.as_str().unwrap_or(""))?);
            }
            let merge = d["combine"].as_bool() == Some(true);
            self.push(it, &fid, "mirrorBody", &it.name, json!({"bodies": refs, "plane": plane, "merge": merge, "knit": false}), None);
            self.attach_result(it)?;
        } else {
            let seeds = self.seeds(&d)?;
            self.push(it, &fid, "mirror", &it.name, json!({"seeds": seeds, "plane": plane, "geometryPattern": false}), None);
            // carried too: a system whose seeds came in as carried shapes cannot mirror them
            self.attach_result(it)?;
        }
        self.ids.insert(it.id, fid);
        Ok(None)
    }

    fn shell(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        self.w.at(Some(it.k))?;
        let body = d["body"].as_str().map(String::from);
        let mut faces = Vec::new();
        for p in d["faces"].as_array().into_iter().flatten() {
            let p = v3(p).ok_or_else(|| anyhow!("a face pick is a name, not a point"))?;
            faces.push(self.w.face(p, body.as_deref())?);
        }
        let (t, ex) = self.input(it, "Thickness").ok_or_else(|| anyhow!("no thickness"))?;
        let outward = d["direction"] == "outward" || d["outward"].as_bool() == Some(true);
        self.push(it, &fid, "shell", &it.name, json!({"thickness": t, "outward": if outward { 1 } else { 0 }, "faces": faces}), None);
        self.feature_equation(&fid, &it.name, "D1", "thickness", &ex);
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(None)
    }
}

impl<'w, 'a> Part<'w, 'a> {
    fn body_refs(&mut self, it: &Item, names: &[Value]) -> Result<Vec<Value>> {
        self.w.at(Some(it.k))?;
        let mut out = Vec::new();
        for b in names {
            out.push(self.w.body_ref(b.as_str().ok_or_else(|| anyhow!("a body is not named"))?)?);
        }
        Ok(out)
    }

    fn draft(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        // the neutral plane: an origin plane or a written plane
        let pl = &d["neutral"];
        let neutral = match pl["type"].as_str() {
            Some("custom") => {
                let o = v3(&pl["plane"]["origin"]).unwrap_or([0.0; 3]);
                let n = unit(cross(v3(&pl["plane"]["x_dir"]).unwrap_or([1.0, 0.0, 0.0]), v3(&pl["plane"]["y_dir"]).unwrap_or([0.0, 1.0, 0.0])));
                match (axis_word(n), norm(o) < 1e-9) {
                    (Some((ax, _)), true) => json!({"kind": "originPlane", "by": "id",
                                                    "plane": match ax { "z" => "front", "y" => "top", _ => "right" }}),
                    _ => bail!("its neutral plane is not an origin plane"),
                }
            }
            _ => self.plane_ref(it, pl)?.ok_or_else(|| anyhow!("its neutral plane is not written natively"))?,
        };
        self.w.at(Some(it.k))?;
        let mut faces = Vec::new();
        for p in d["faces"].as_array().into_iter().flatten() {
            faces.push(self.w.face(v3(p).ok_or_else(|| anyhow!("a face pick is not a point"))?, None)?);
        }
        let (a, ex) = self.input(it, "Angle").ok_or_else(|| anyhow!("no angle"))?;
        let note = "its pull direction is checked by the carried result";
        self.push(it, &fid, "draft", &it.name, json!({"neutral": neutral, "faces": faces, "angle": a.abs(), "reverse": a < 0.0,
                                                      "propagation": 0}), Some(note));
        self.feature_equation(&fid, &it.name, "D1", "angle", &ex);
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(Some(note.into()))
    }

    fn sweep(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        if !(d["profiles"].is_null() || d["profiles"]["type"] == "all") {
            bail!("a sweep of picked profiles is not written natively");
        }
        let profile = self.feature_ref(d["sketch"].as_i64().ok_or_else(|| anyhow!("no profile sketch"))?)?;
        let path_sk = d["path_sketch"].as_i64().ok_or_else(|| anyhow!("its path is not a sketch"))?;
        // NCX's path is a whole sketch: the curves picked must be all the path sketch draws
        let drawn = self.sketches.get(&path_sk).map(|s| {
            s["curves"].as_array().into_iter().flatten().filter(|c| c["construction"].as_bool() != Some(true)).count()
        });
        if drawn != Some(d["path"].as_array().map(|p| p.len()).unwrap_or(0)) {
            bail!("its path is part of a sketch");
        }
        if d.get("rails").map(|r| r.is_array()).unwrap_or(false) || d.get("twist").map(|t| !t.is_null()).unwrap_or(false) {
            bail!("a sweep with rails or a twist is not written natively");
        }
        let path = self.feature_ref(path_sk)?;
        let op = d["operation"].as_str().unwrap_or("new_body");
        let (kind, merge) = match op {
            "cut" => ("sweepCut", None),
            "intersect" => bail!("an intersecting sweep is not written natively"),
            "new" | "new_body" => ("sweep", Some(false)),
            _ => ("sweep", Some(true)),
        };
        let mut def = json!({"profile": profile, "path": path, "twistControl": 0, "pathAlignment": 0, "twistAngle": 0,
                             "tangentPropagation": false, "maintainTangency": false});
        if let Some(m) = merge {
            def["merge"] = json!(m);
        }
        self.push(it, &fid, kind, &it.name, def, None);
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(None)
    }

    fn combine(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let op = match d["operation"].as_str().unwrap_or("join") {
            "join" | "add" => "add",
            "cut" | "subtract" => "subtract",
            "intersect" | "common" => "common",
            other => bail!("a {other} combine is not written natively"),
        };
        if d["keep_tools"].as_bool() == Some(true) {
            bail!("a combine that keeps its tools is not written natively");
        }
        let main = self.body_refs(it, &[d["target"].clone()])?.remove(0);
        let tools = self.body_refs(it, d["tools"].as_array().map(|v| v.as_slice()).unwrap_or(&[]))?;
        self.push(it, &fid, "combine", &it.name, json!({"operation": op, "main": main, "bodies": tools}), None);
        self.ids.insert(it.id, fid);
        Ok(None)
    }

    fn remove(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let bodies = self.body_refs(it, d["bodies"].as_array().map(|v| v.as_slice()).unwrap_or(&[]))?;
        if bodies.is_empty() {
            bail!("no bodies");
        }
        self.push(it, &fid, "deleteBody", &it.name, json!({"bodies": bodies, "keep": false}), None);
        self.ids.insert(it.id, fid);
        Ok(None)
    }

    fn move_bodies(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        // a turn about an axis along X, Y or Z (NCX rotate: angles about the axes through `origin`, degrees); the
        // carried result checks the sense
        if let Some(ax) = v3(&d["rotate_axis"]).or_else(|| v3(&d["axis"])) {
            let ax = unit(ax);
            let Some(i) = (0..3).find(|&i| (ax[i].abs() - 1.0).abs() < 1e-9) else { bail!("a turn about an axis that is not X, Y or Z is not written natively") };
            let (a, _) = self.input(it, "Angle").ok_or_else(|| anyhow!("its angle is not a number"))?;
            let mut angles = [0.0; 3];
            angles[i] = a * ax[i].signum();
            let origin = v3(&d["origin"]).unwrap_or([0.0; 3]);
            let bodies = self.body_refs(it, d["bodies"].as_array().map(|v| v.as_slice()).unwrap_or(&[]))?;
            let copy = d["copy"].as_bool() == Some(true);
            self.push(it, &fid, "moveBody", &it.name, json!({"bodies": bodies, "copy": copy, "copies": 1, "rotate": {"origin": origin, "angles": angles}}),
                      Some("turning sense checked by the carried result"));
            self.attach_result(it)?;
            self.ids.insert(it.id, fid);
            return Ok(Some("turning sense checked by the carried result".into()));
        }
        if d.get("angle").map(|a| !a.is_null()).unwrap_or(false) {
            bail!("a turning move is not written natively");
        }
        let tr = [
            self.input(it, "X distance").map(|x| x.0).unwrap_or(0.0),
            self.input(it, "Y distance").map(|x| x.0).unwrap_or(0.0),
            self.input(it, "Z distance").map(|x| x.0).unwrap_or(0.0),
        ];
        let bodies = self.body_refs(it, d["bodies"].as_array().map(|v| v.as_slice()).unwrap_or(&[]))?;
        let copy = d["copy"].as_bool() == Some(true);
        let copies = if copy { d["copy_frames"].as_array().map(|c| c.len()).unwrap_or(1).max(1) } else { 1 };
        self.push(it, &fid, "moveBody", &it.name, json!({"bodies": bodies, "copy": copy, "copies": copies, "translate": tr}), None);
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(None)
    }

    fn rib(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let sk = d["sketch"].as_i64().ok_or_else(|| anyhow!("no sketch"))?;
        let drawn = self.sketches.get(&sk).map(|s| {
            s["curves"].as_array().into_iter().flatten().filter(|c| c["construction"].as_bool() != Some(true)).count()
        });
        if drawn != Some(d["curves"].as_array().map(|c| c.len()).unwrap_or(0)) {
            bail!("its profile is part of a sketch");
        }
        let profile = self.feature_ref(sk)?;
        let (th, ex) = self.input(it, "Thickness").ok_or_else(|| anyhow!("no thickness"))?;
        let note = "which way it fills is checked by the carried result";
        self.push(it, &fid, "rib", &it.name, json!({"profile": profile, "twoSided": true, "thickness": th, "reverseThickness": false,
                                                    "flipMaterial": false, "extrusion": "parallelToSketch", "ribType": "natural"}),
                  Some(note));
        self.feature_equation(&fid, &it.name, "D1", "thickness", &ex);
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(Some(note.into()))
    }

    fn split(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let tool = self.plane_ref(it, &d["plane"])?.ok_or_else(|| anyhow!("its splitting tool is not a written plane"))?;
        let mut def = json!({"tools": [tool], "consume": false});
        if let Some(b) = d["body"].as_str() {
            def["bodies"] = json!(self.body_refs(it, &[json!(b)])?);
        }
        self.push(it, &fid, "split", &it.name, def, None);
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(None)
    }
}

/// Principal moments of volume of one body, integrated over SolidCraft's "fine" STL mesh and scaled by the
/// exact volume over the mesh's, to the power 5/3 (a mesh lies just inside a curved surface). SolidCraft's own
/// inertia comes from a coarser tessellation: up to 4.8e-4 of the largest moment off on the user's Rear AC bodies
/// against OpenCASCADE on the same B-rep (the exchange tolerance is 5e-4); this way at most 8.4e-5 ("finest":
/// 3.4e-5, at five times the time: a minute for the Fan Cowl). None when the mesh does
/// not hold exactly that body (a name two placements share) or cannot be written.
pub fn mesh_moments(s: &mut crate::sc::Session, body: &str, exact_volume: f64) -> Option<V3> {
    if exact_volume <= 0.0 {
        return None;
    }
    let path = crate::import::scratch_dir().join(format!("moments-{}.stl", std::process::id()));
    s.exec("file.export_stl", json!({"path": path.to_string_lossy(), "bodies": [body], "quality": "fine"})).ok()?;
    let data = std::fs::read(&path).ok()?;
    let _ = std::fs::remove_file(&path);
    let (v, m) = stl_moments(&data)?;
    if (v / exact_volume - 1.0).abs() > 1e-3 {
        return None;
    }
    let k = (exact_volume / v).powf(5.0 / 3.0);
    Some([m[0] * k, m[1] * k, m[2] * k])
}

/// Volume and principal moments of volume (about the centroid) of a closed binary STL mesh.
fn stl_moments(d: &[u8]) -> Option<(f64, V3)> {
    if d.len() < 84 {
        return None;
    }
    let n = u32::from_le_bytes(d[80..84].try_into().ok()?) as usize;
    if d.len() < 84 + n * 50 || n == 0 {
        return None;
    }
    let f = |o: usize| f32::from_le_bytes(d[o..o + 4].try_into().unwrap()) as f64;
    let (mut v6s, mut c) = (0.0, [0.0; 3]);
    let mut s2 = [[0.0f64; 3]; 3];
    for t in 0..n {
        let o = 84 + t * 50 + 12;
        let p: Vec<V3> = (0..3).map(|k| [f(o + k * 12), f(o + k * 12 + 4), f(o + k * 12 + 8)]).collect();
        let (a, b, cc) = (p[0], p[1], p[2]);
        let v6 = dot(a, cross(b, cc));
        v6s += v6;
        for i in 0..3 {
            c[i] += (a[i] + b[i] + cc[i]) * v6;
            for j in 0..3 {
                s2[i][j] += v6 * (2.0 * a[i] * a[j] + 2.0 * b[i] * b[j] + 2.0 * cc[i] * cc[j] + a[i] * b[j] + a[j] * b[i]
                    + a[i] * cc[j] + a[j] * cc[i] + b[i] * cc[j] + b[j] * cc[i]);
            }
        }
    }
    let v = v6s / 6.0;
    if v.abs() < 1e-12 {
        return None;
    }
    let cen = [c[0] / 24.0 / v, c[1] / 24.0 / v, c[2] / 24.0 / v];
    let mut m = [[0.0; 3]; 3];
    for i in 0..3 {
        for j in 0..3 {
            m[i][j] = s2[i][j] / 120.0 - v * cen[i] * cen[j];
        }
    }
    let tr = m[0][0] + m[1][1] + m[2][2];
    let mut inertia = [[0.0; 3]; 3];
    for i in 0..3 {
        for j in 0..3 {
            inertia[i][j] = if i == j { tr - m[i][j] } else { -m[i][j] };
        }
    }
    Some((v.abs(), principal(inertia)))
}

impl<'w, 'a> Part<'w, 'a> {
    /// The sheet-metal rule a feature uses (by name, else the part's last), written once as the part's `sheetMetal`
    /// defaults before its first sheet-metal feature. Returns thickness, bend radius and K-factor.
    fn sheet_defaults(&mut self, it: &Item, rule: Option<&str>) -> Result<(f64, f64, f64)> {
        if self.rules.is_none() {
            self.rules = Some(self.w.s.exec("sheet.rules", json!({}))?);
        }
        let rules = self.rules.clone().unwrap_or(Value::Null);
        let want = rule.map(String::from).or_else(|| self.sheet_rule_name.clone());
        let r = rules["rules"].as_array().into_iter().flatten()
            .find(|r| want.as_deref().map(|w| r["rule"]["name"] == w).unwrap_or(false))
            .or_else(|| rules["rules"].as_array().and_then(|a| a.iter().find(|r| r["rule"]["name"] == rules["active"])))
            .cloned()
            .ok_or_else(|| anyhow!("its sheet-metal rule was not found"))?;
        let v = &r["values"];
        let (t, rad, k) = (v["thickness"].as_f64().unwrap_or(0.0), v["bend_radius"].as_f64().unwrap_or(0.0), v["k_factor"].as_f64().unwrap_or(0.5));
        if t <= 0.0 {
            bail!("its sheet-metal rule has no thickness");
        }
        let name = r["rule"]["name"].as_str().unwrap_or("").to_string();
        if self.sheet_rule_name.as_deref() != Some(name.as_str()) || !self.sheet_written {
            let relief = v["relief_width"].as_f64().map(|w| w / t).unwrap_or(0.5);
            let id = format!("{}:sheet", self.fid(it));
            let fname = self.fresh_name("Sheet-Metal");
            self.features.push(json!({"id": id, "type": "sheetMetal", "name": fname,
                "native": {"app": "SolidCraft", "type": "SheetMetalRule"}, "status": "mapped",
                "def": {"thickness": t, "bendRadius": rad, "bendAllowanceType": "kFactor", "kFactor": k, "bendAllowance": 0,
                        "autoRelief": true, "reliefType": "rectangular", "reliefRatio": relief}}));
            self.sheet_rule_name = Some(name);
            self.sheet_written = true;
        }
        Ok((t, rad, k))
    }

    /// Unfold (every bend) or refold: the fixed face is the planar face that stays where it was (same plane and
    /// area before and after); an unfold's bends are its inner bend faces, a refold names the unfold it reverses
    /// (FORMAT-SPEC 4 `unfold` / `fold`).
    fn sheet_unfold(&mut self, it: &Item, fold: bool) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let (_, r, _) = self.sheet_defaults(it, None)?;
        self.w.at(Some(it.k))?;
        let body = match d["body"].as_str() {
            Some(b) => b.to_string(),
            None => self.w.body_names()?.into_iter().next().ok_or_else(|| anyhow!("no body"))?,
        };
        let before = self.w.planes(&body)?;
        let bend_pts = if fold { Vec::new() } else { self.w.bend_faces(&body, r)? };
        self.w.at(Some(it.k + 1))?;
        let after = self.w.planes(&body).unwrap_or_default();
        let still = before.iter().filter(|(_, n, p, a)| {
            after.iter().any(|(_, n2, p2, a2)| norm(cross(*n, *n2)) < 1e-6 && dot(sub(*p2, *p), *n).abs() < 1e-6 && (a - a2).abs() <= 1e-6 * a.max(1.0))
        }).max_by(|x, y| x.3.partial_cmp(&y.3).unwrap_or(std::cmp::Ordering::Equal)).cloned();
        let (_, _, fixed_pt, _) = still.ok_or_else(|| anyhow!("its fixed face was not found (no face stays where it was)"))?;
        self.w.at(Some(it.k))?;
        let fixed = self.w.face(fixed_pt, Some(&body))?;
        let mut def = json!({"fixedFace": fixed});
        let note;
        if fold {
            let (uid, bends) = self.last_unfold.clone().ok_or_else(|| anyhow!("the unfold it reverses was not written natively"))?;
            def["unfold"] = json!({"kind": "feature", "by": "id", "id": uid});
            def["bends"] = json!(bends);
            note = "reverses the unfold it names; checked by the carried result";
        } else {
            if bend_pts.is_empty() {
                bail!("no bends to unfold");
            }
            let mut bends = Vec::new();
            for (i, p) in bend_pts.iter().enumerate() {
                bends.push(json!({"name": format!("Bend{}", i + 1), "face": self.w.face(*p, Some(&body))?}));
            }
            self.last_unfold = Some((fid.clone(), bends.clone()));
            def["bends"] = json!(bends);
            note = "every bend; checked by the carried result";
        }
        self.push(it, &fid, if fold { "fold" } else { "unfold" }, &it.name, def, Some(note));
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(Some(note.into()))
    }

    fn sheet_base(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let sk_id = d["sketch"].as_i64().ok_or_else(|| anyhow!("no sketch"))?;
        let profile = self.feature_ref(sk_id)?;
        let (t, r, k) = self.sheet_defaults(it, d["rule"].as_str())?;
        let mut def = json!({"profile": profile, "thickness": t, "reverseThickness": d["flip"].as_bool() == Some(true),
                             "bendRadius": r, "reverse": false, "kFactor": k});
        if let Some(c) = self.contours(sk_id, &d["profiles"])? {
            def["contours"] = c;
        }
        let note = "which side the thickness goes is checked by the carried result";
        self.push(it, &fid, "baseFlange", &it.name, def, Some(note));
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(Some(note.into()))
    }

    fn sheet_contour(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let sk_id = d["sketch"].as_i64().ok_or_else(|| anyhow!("no sketch"))?;
        // NCX's profile is the whole sketch: the chain must be every curve the sketch draws
        let drawn = self.sketches.get(&sk_id).map(|s| {
            s["curves"].as_array().into_iter().flatten().filter(|c| c["construction"].as_bool() != Some(true)).count()
        });
        if drawn != Some(d["curves"].as_array().map(|c| c.len()).unwrap_or(0)) {
            bail!("its chain is part of a sketch");
        }
        let profile = self.feature_ref(sk_id)?;
        let (t, r, k) = self.sheet_defaults(it, d["rule"].as_str())?;
        let (depth, ex) = self.input(it, "Distance").ok_or_else(|| anyhow!("no distance"))?;
        let def = json!({"profile": profile, "thickness": t, "reverseThickness": d["flip"].as_bool() == Some(true), "bendRadius": r,
                         "kFactor": k, "reverse": d["reverse"].as_bool() == Some(true), "end": {"type": "blind", "depth": depth}});
        let note = "which side the thickness goes is checked by the carried result";
        self.push(it, &fid, "baseFlange", &it.name, def, Some(note));
        self.feature_equation(&fid, &it.name, "D1", "end.depth", &ex);
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(Some(note.into()))
    }

    fn sheet_convert(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let (t, r, _) = self.sheet_defaults(it, d["rule"].as_str())?;
        let body = d["body"].as_str().map(String::from);
        let face_pick = v3(&d["face"]).ok_or_else(|| anyhow!("its fixed face is not a point"))?;
        self.w.at(Some(it.k))?;
        let fixed = self.w.face(face_pick, body.as_deref())?;
        let outside = d["direction"] == "outside";
        // the bend edges: the solid's straight edges that became bends (SolidCraft finds them itself; NCX lists them)
        let before = self.w.line_edges(body.as_deref())?;
        self.w.at(Some(it.k + 1))?;
        let bends = self.w.cylinders()?;
        let mut picks = Vec::new();
        for (p, dir, n1, n2) in before {
            let phi = dot(n1, n2).abs().clamp(-1.0, 1.0).acos();
            let half = (phi / 2.0).cos().max(1e-6);
            let hit = bends.iter().any(|(o, a, rad)| {
                if norm(cross(*a, dir)) > 1e-6 {
                    return false;
                }
                let q = sub(p, *o);
                let dd = norm(sub(q, crate::geom::scale(*a, dot(q, *a))));
                // the solid's faces are the sheet's outer faces (or inner ones, converted outward): the edge is the
                // bend's virtual sharp, (r + t) / cos(phi / 2) from its axis (r / cos(phi / 2) outward)
                let want = if outside { r } else { r + t } / half;
                (rad - if outside { r } else { r + t }).abs() < 1e-6 && (dd - want).abs() < 1e-4 * want.max(1.0)
            });
            if hit {
                picks.push(p);
            }
        }
        self.w.at(Some(it.k))?;
        let mut bend_edges = Vec::new();
        for p in picks {
            bend_edges.push(self.w.edge(p, body.as_deref())?);
        }
        let def = json!({"fixedFace": fixed, "bendEdges": bend_edges, "thickness": t, "bendRadius": r, "reverse": outside, "keepBody": false});
        let note = "its bend edges found from the bends it made; checked by the carried result";
        self.push(it, &fid, "convertToSheetMetal", &it.name, def, Some(note));
        self.attach_result(it)?;
        self.ids.insert(it.id, fid);
        Ok(Some(note.into()))
    }

    /// An edge flange with the profile sketch SolidWorks needs: a rectangle on the flange's plane, its base on the
    /// edge, as wide as the flange and as high as its web, measured on the flange SolidCraft built (its faces are
    /// named after the feature).
    fn sheet_edge_flange(&mut self, it: &Item) -> Result<Option<String>> {
        let d = self.def(it).clone();
        let fid = self.fid(it);
        let pts: Vec<V3> = d["edges"].as_array().into_iter().flatten().filter_map(|e| v3(e).or_else(|| v3(&e["point"]))).collect();
        if pts.is_empty() {
            bail!("no edges");
        }
        if d["edges"].as_array().into_iter().flatten().any(|e| e.get("height").is_some() || e.get("angle").is_some() || e.get("segments").is_some()) {
            bail!("an edge flange with its own height, angle or width per edge is not written natively yet");
        }
        let (t, r, _) = self.sheet_defaults(it, None)?;
        let (ang, _) = self.input(it, "Angle").unwrap_or((90.0, String::new()));
        let (h, ex) = self.input(it, "Height").ok_or_else(|| anyhow!("no height"))?;
        // with the face move before it (see write): the flange on the edge as it was before the move (one edge only)
        let moved = self.face_move.take().filter(|m| m.0 + 1 == it.k && pts.len() == 1);
        let k0 = moved.as_ref().map(|m| m.0).unwrap_or(it.k);
        let mut picks = pts.clone();
        if let Some((_, md)) = &moved {
            let delta = md["distance"].as_str().and_then(|x| x.trim().trim_end_matches("mm").trim().parse::<f64>().ok())
                .or_else(|| md["distance"].as_f64()).unwrap_or(0.0);
            if let Some(fp) = md["faces"].as_array().and_then(|f| f.first()).and_then(v3) {
                self.w.at(Some(k0))?;
                let mut found = None;
                for b in self.w.body_names()? {
                    let r = self.w.s.exec("inspect.classify_points", json!({"body": b, "points": [fp]}))?;
                    if r["results"][0]["class"] == "on" {
                        found = v3(&r["results"][0]["normal"]);
                        break;
                    }
                }
                let n = found.ok_or_else(|| anyhow!("the moved face was not found"))?;
                picks[0] = sub(picks[0], crate::geom::scale(unit(n), delta));
            }
        }
        // the flange's own bends (a flange on another flange's end has that one's bend right beside it)
        self.w.at(Some(it.k + 1))?;
        let mut bend_axes = self.w.cylinders_of(Some(it.id))?;
        if bend_axes.is_empty() {
            bend_axes = self.w.cylinders()?;
        }
        // the profile's height as SolidWorks draws it for that length: from the outer virtual sharp L - t tan(a/2)
        // (its own files: 25 mm at 90 degrees -> 23.5, 1.5 mm at 45 degrees -> 0.879, t 1.5), from the inner one L,
        // from the bend's tangent L + r
        let a_half = (ang.to_radians() / 2.0).tan();
        let hi = match d["height_from"].as_str().unwrap_or("outer") {
            "inner" => h,
            "tangent" => h + r,
            _ => h - t * a_half,
        };
        if hi <= 0.0 {
            bail!("its profile would have no height");
        }
        let mut edges_j = Vec::new();
        let mut profiles_j = Vec::new();
        for (i, pick) in picks.iter().enumerate() {
            self.w.at(Some(k0))?;
            let mut edge = self.w.edge(*pick, d["body"].as_str())?;
            let gp = &edge["geom"]["params"];
            let (Some(mut a0), Some(x)) = (v3(&gp["point"]), v3(&gp["direction"])) else { bail!("an edge flange on a curved edge is not written natively") };
            let x = unit(x);
            // the flange's rise, from what defines it: the sheet's end face points off the edge (u), the bend lies on
            // the side of the face the flange rises from (n), and the web leaves at the flange's angle:
            // y = cos(a) u + sin(a) n (the faces SolidCraft names after a flange are no guide to its web)
            let faces = self.w.edge_faces(*pick)?;
            let square: Vec<(V3, f64)> = faces.iter().copied().filter(|(n, _)| dot(*n, x).abs() < 1e-6).collect();
            // the sheet's end face is the one a thickness wide (area over the edge's length); by area alone a flange
            // shorter than the thickness (the Half Shell's 45-degree flange, 0.88 mm) would swap the two
            let elen = edge["geom"]["length"].as_f64().unwrap_or(1.0).max(1e-9);
            let by_width = |f: &(V3, f64)| (f.1 / elen - t).abs();
            let small = square.iter().copied().min_by(|a, b| by_width(a).partial_cmp(&by_width(b)).unwrap_or(std::cmp::Ordering::Equal));
            let big = small.and_then(|s| square.iter().copied().find(|f| f.0 != s.0));
            let (Some((n_big, _)), Some((u, _))) = (big, small) else { bail!("the faces at the flange's edge were not found") };
            let axis = bend_axes.iter().filter(|(_, a, _)| norm(cross(*a, x)) < 1e-6)
                .map(|(o, _, rad)| {
                    let q = sub(*o, a0);
                    let off = sub(q, crate::geom::scale(x, dot(q, x)));
                    (norm(off), off, *rad)
                })
                .filter(|(dd, _, rad)| *dd < (r + 3.0 * t + *rad) * 2.0)
                .min_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));
            let Some((_, off, _)) = axis else { bail!("the flange's bend was not found") };
            let n_rise = if dot(off, n_big) >= 0.0 { n_big } else { crate::geom::scale(n_big, -1.0) };
            let a_rad = ang.to_radians();
            let y = unit(crate::geom::add(crate::geom::scale(u, a_rad.cos()), crate::geom::scale(n_rise, a_rad.sin())));
            // SolidWorks builds the flange on the edge of the face it rises from: that edge, or its twin a thickness away
            if dot(n_big, n_rise) < 0.0 {
                let twin = sub(*pick, crate::geom::scale(n_big, t));
                let e2 = self.w.edge(twin, d["body"].as_str())?;
                if let (Some(p2), Some(x2)) = (v3(&e2["geom"]["params"]["point"]), v3(&e2["geom"]["params"]["direction"])) {
                    if norm(cross(unit(x2), x)) < 1e-6 {
                        edge = e2;
                        a0 = p2;
                    }
                }
            }
            let x = v3(&edge["geom"]["params"]["direction"]).map(unit).unwrap_or(x);
            // the profile spans the edge (SolidCraft's flange runs the edge's whole length)
            let (s0, s1) = (0.0, edge["geom"]["length"].as_f64().unwrap_or(elen));
            let z = unit(cross(x, y));
            let skid = if picks.len() == 1 { format!("{fid}:profile") } else { format!("{fid}:profile{}", i + 1) };
            let skname = self.fresh_name(&if picks.len() == 1 { format!("{} profile", it.name) } else { format!("{} profile {}", it.name, i + 1) });
            let ents = json!([
                {"id": "p1", "type": "point", "p": [s0, 0.0]}, {"id": "p2", "type": "point", "p": [s1, 0.0]},
                {"id": "p3", "type": "point", "p": [s1, hi]}, {"id": "p4", "type": "point", "p": [s0, hi]},
                {"id": "s1", "type": "line", "start": [s0, 0.0], "end": [s1, 0.0], "startPoint": "p1", "endPoint": "p2"},
                {"id": "s2", "type": "line", "start": [s1, 0.0], "end": [s1, hi], "startPoint": "p2", "endPoint": "p3"},
                {"id": "s3", "type": "line", "start": [s1, hi], "end": [s0, hi], "startPoint": "p3", "endPoint": "p4"},
                {"id": "s4", "type": "line", "start": [s0, hi], "end": [s0, 0.0], "startPoint": "p4", "endPoint": "p1"}]);
            let cons = json!([{"type": "horizontal", "entities": ["s1"]}, {"type": "vertical", "entities": ["s2"]},
                              {"type": "horizontal", "entities": ["s3"]}, {"type": "vertical", "entities": ["s4"]}]);
            let dims = json!([{"id": "d1", "name": "D1", "fullName": format!("D1@{skname}"), "kind": "distance", "entities": ["p1", "p4"],
                               "value": hi, "driving": true, "textPosition": [(s0 + s1) / 2.0, hi / 2.0]}]);
            self.features.push(json!({"id": skid, "type": "sketch", "name": skname, "status": "mapped",
                "native": {"app": "SolidCraft", "type": "flange profile (written for the flange)"},
                "def": {"plane": {"kind": "generated", "by": it.name}, "frame": {"origin": a0, "x": x, "z": z},
                        "entities": ents, "constraints": cons, "dimensions": dims, "constrained": "under", "hidden": true}}));
            edges_j.push(edge);
            profiles_j.push(json!({"kind": "feature", "by": "id", "id": skid}));
        }
        // SolidCraft's words: inside = the outer virtual sharp on the edge (FORMAT-SPEC bendSharp; materialInside at 90)
        let pos = match d["position"].as_str().unwrap_or("inside") {
            _ if moved.is_some() => "materialInside",
            "material_outside" => "materialOutside",
            "outside" | "bend_outside" => "bendOutside",
            "middle" => "bendCenterline",
            "tangent" => "bendTangent",
            _ if (ang - 90.0).abs() < 1e-6 => "materialInside",
            _ => "bendSharp",
        };
        let datum = match d["height_from"].as_str().unwrap_or("outer") {
            "inner" => "innerVirtualSharp",
            "tangent" => "bendTangent",
            _ => "outerVirtualSharp",
        };
        let br = self.input(it, "Radius").map(|x| x.0).unwrap_or(r);
        let mut def = json!({"edge": edges_j[0], "profile": profiles_j[0], "angle": ang,
            "useDefaultRadius": (br - r).abs() < 1e-9, "bendRadius": br, "position": pos, "useDefaultRelief": true,
            "reliefType": "rectangular", "useReliefRatio": true, "reliefRatio": 0.5, "reliefWidth": t * 0.5, "reliefDepth": t * 0.5,
            "length": {"type": "blind", "distance": h, "datum": datum, "reverse": false}});
        if edges_j.len() > 1 {
            def["edges"] = json!(edges_j);
            def["profiles"] = json!(profiles_j);
        }
        let note = "checked by the carried result (systems place a flange by different words)";
        self.push(it, &fid, "edgeFlange", &it.name, def, Some(note));
        self.feature_equation(&fid, &it.name, "D1", "length.distance", &ex);
        let res = self.result_span(k0, it.k + 1)?;
        if let Some(f) = self.features.last_mut() {
            f["result"] = res;
        }
        self.ids.insert(it.id, fid);
        Ok(Some(note.into()))
    }
}

/// Whether a loop of curve ids runs along whole curves: a closed curve (circle, ellipse) alone, or open curves
/// each ending where the next begins (by their end positions). A loop that uses part of a curve (a region where
/// curves cross) is not a contour.
fn whole_curves(sketch: Option<&Value>, ids: &Value) -> bool {
    let Some(sk) = sketch else { return false };
    let ids: Vec<&str> = ids.as_array().into_iter().flatten().filter_map(|c| c.as_str()).collect();
    let curve = |id: &str| sk["curves"].as_array().into_iter().flatten().find(|c| c["id"] == id).cloned();
    let mut ends: Vec<([f64; 2], [f64; 2])> = Vec::new();
    for id in &ids {
        let Some(c) = curve(id) else { return false };
        match c["type"].as_str().unwrap_or("") {
            "circle" | "ellipse" => return ids.len() == 1,
            _ => match (crate::geom::v2(&c["start_at"]), crate::geom::v2(&c["end_at"])) {
                (Some(a), Some(b)) => ends.push((a, b)),
                _ => return false,
            },
        }
    }
    let near = |a: [f64; 2], b: [f64; 2]| (a[0] - b[0]).hypot(a[1] - b[1]) < 1e-6;
    // each curve shares an end with the next (either way round), and the last with the first
    let n = ends.len();
    (0..n).all(|i| {
        let (a, b) = ends[i];
        let (c, d) = ends[(i + 1) % n];
        near(a, c) || near(a, d) || near(b, c) || near(b, d)
    })
}

/// A fingerprint (FORMAT-SPEC §6) from `component.properties` (volume, area, centre, box, inertia about the
/// centre) and `inspect.measure` (face and edge counts). Moments of volume: inertia / density.
pub fn fingerprint(b: &Value, counts: &Value, closed: bool) -> Value {
    let bb = &b["bbox"];
    let mn = v3(&bb["min"]).unwrap_or([0.0; 3]);
    let mx = v3(&bb["max"]).unwrap_or([0.0; 3]);
    let mut fp = json!({
        "volume": if closed { b["volume_mm3"].as_f64().unwrap_or(0.0) } else { 0.0 },
        "area": b["area_mm2"].as_f64().unwrap_or(0.0),
        "centroid": b.get("center_of_mass").filter(|c| c.is_array()).cloned()
            .unwrap_or_else(|| json!([(mn[0] + mx[0]) / 2.0, (mn[1] + mx[1]) / 2.0, (mn[2] + mx[2]) / 2.0])),
        "bbox": [mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]],
        "faces": counts["faces"].as_u64().unwrap_or(0),
        "edges": counts["edges"].as_u64().unwrap_or(0),
    });
    let mut moments = [0.0; 3];
    if closed {
        if let (Some(t), Some(dens)) = (b["inertia_g_mm2"].as_array(), b["density_g_cm3"].as_f64().filter(|d| *d > 0.0)) {
            let mut m = [[0.0; 3]; 3];
            for i in 0..3 {
                for j in 0..3 {
                    m[i][j] = t.get(i).and_then(|r| r.get(j)).and_then(|v| v.as_f64()).unwrap_or(0.0);
                }
            }
            let ev = principal(m);
            let per = 1.0 / (dens * 1e-3); // g/cm^3 -> g/mm^3
            moments = [ev[0] * per, ev[1] * per, ev[2] * per];
        }
    }
    fp["moments"] = json!(moments);
    if !closed {
        fp["sheet"] = json!(true);
    }
    fp
}

fn plain_number(s: &str) -> Option<f64> {
    let t = s.trim().trim_end_matches("mm").trim_end_matches("deg").trim();
    t.parse::<f64>().ok()
}

fn origin_plane(name: &str) -> Option<&'static str> {
    match name {
        "XY" => Some("front"),
        "XZ" => Some("top"),
        "YZ" => Some("right"),
        _ => None,
    }
}

/// An origin plane's origin and normal, or a written plane's (from its definition when known).
fn base_frame(base: &Value, r: &Value) -> (V3, V3) {
    let n = match base["name"].as_str() {
        Some("XY") => [0.0, 0.0, 1.0],
        Some("XZ") => [0.0, 1.0, 0.0],
        Some("YZ") => [1.0, 0.0, 0.0],
        _ => r["geom"]["params"]["normal"].as_array().and_then(|_| v3(&r["geom"]["params"]["normal"])).unwrap_or([0.0, 0.0, 1.0]),
    };
    let o = v3(&r["geom"]["params"]["point"]).unwrap_or([0.0; 3]);
    (o, n)
}

/// `x`, `y` or `z` for a direction along an origin axis, and whether it points the negative way.
fn axis_word(v: V3) -> Option<(&'static str, bool)> {
    let u = unit(v);
    for (k, w) in ["x", "y", "z"].iter().enumerate() {
        if (u[k].abs() - 1.0).abs() < 1e-9 {
            return Some((w, u[k] < 0.0));
        }
    }
    None
}

fn perpendicular(z: V3) -> V3 {
    // X where it is square to the axis (SolidWorks' convention for a plane's x), else Y
    if dot(z, [1.0, 0.0, 0.0]).abs() < 0.9 {
        unit(sub([1.0, 0.0, 0.0], [z[0] * z[0], z[0] * z[1], z[0] * z[2]]))
    } else {
        unit(sub([0.0, 1.0, 0.0], [z[1] * z[0], z[1] * z[1], z[1] * z[2]]))
    }
}
