//! Sketches (FORMAT-SPEC §4 "Sketch").
//!
//! The sketch is made on a plane at the payload's own `frame` (origin, x, z), so the file's sketch
//! coordinates are SolidCraft's sketch coordinates and `horizontal`/`vertical` mean the same in both.
//! Every entity is drawn at its solved (seed) position; points two curves share are passed on by reference,
//! never merged by position (two payload points at one place stay two points). Then the relations and the
//! dimensions are added, each on its own, so one SolidCraft refuses costs only that item.

use std::collections::HashMap;

use anyhow::{anyhow, Result};
use serde_json::{json, Value};

use super::{features::translate_expression, num, PartState};
use crate::geom::{v2, Frame};
use crate::sc::{self, Session};

/// A payload sketch entity as built in SolidCraft.
#[derive(Clone, Debug)]
pub struct Ent {
    /// point, line, arc, circle, ellipse, spline
    pub kind: String,
    /// SolidCraft id: a curve id (l3, a1, c2...) or a point reference (l3.end, p4, c2.center)
    pub sc: String,
    /// circles and arcs: the centre point's reference, position and radius
    pub center: Option<String>,
    pub center_xy: Option<[f64; 2]>,
    pub radius: Option<f64>,
}

#[derive(Clone, Debug)]
pub struct Built {
    pub name: String,
    pub frame: Frame,
    pub ents: HashMap<String, Ent>,
}

/// What a sketch could not carry (each becomes part of the feature's "approximate" reason).
pub struct Outcome {
    pub built: Built,
    pub missing: Vec<String>,
    pub less_defined: Option<String>,
}

fn pt(v: &Value) -> Option<[f64; 2]> {
    v2(v)
}

pub fn build(s: &mut Session, st: &mut PartState, f: &Value) -> Result<Outcome> {
    let def = &f["def"];
    let name = st.unique_name(f["name"].as_str().unwrap_or("Sketch"));
    let fid = f["id"].as_str().unwrap_or("").to_string();
    let frame = Frame::from_def(&def["frame"]).ok_or_else(|| anyhow!("the sketch has no frame"))?;
    s.exec(
        "sketch.create",
        json!({"plane": {"origin": frame.origin, "x_dir": frame.x, "y_dir": frame.y}, "name": name}),
    )?;
    let r = (|| -> Result<Outcome> {
        let mut out = Outcome {
            built: Built { name: name.clone(), frame, ents: HashMap::new() },
            missing: Vec::new(),
            less_defined: None,
        };
        draw_entities(s, def, &mut out)?;
        add_constraints(s, def, &mut out)?;
        add_dimensions(s, st, def, &fid, &mut out)?;
        let solve = s.exec("sketch.solve", json!({}))?;
        let dof = solve["dof"].as_i64().unwrap_or(-1);
        if def["constrained"] == "full" && dof > 0 {
            out.less_defined = Some(format!("{dof} degrees of freedom left where the source sketch was fully defined"));
        }
        Ok(out)
    })();
    match r {
        Ok(o) => {
            s.exec("sketch.finish", json!({}))?;
            if def["hidden"].as_bool() == Some(true) {
                let _ = s.exec("browser.visibility", json!({"visible": false, "items": [name]}));
            }
            Ok(o)
        }
        Err(e) => {
            if !sc::is_cancelled(&e) {
                let _ = s.exec("sketch.cancel", json!({}));
            }
            Err(e)
        }
    }
}

/// The points each curve uses, so standalone points can be told apart.
fn used_points(def: &Value) -> std::collections::HashSet<String> {
    let mut set = std::collections::HashSet::new();
    for e in def["entities"].as_array().into_iter().flatten() {
        for k in ["startPoint", "endPoint", "centerPoint", "majorPoint", "minorPoint"] {
            if let Some(p) = e[k].as_str() {
                set.insert(p.to_string());
            }
        }
        for p in e["pointIds"].as_array().into_iter().flatten() {
            if let Some(p) = p.as_str() {
                set.insert(p.to_string());
            }
        }
    }
    set
}

fn draw_entities(s: &mut Session, def: &Value, out: &mut Outcome) -> Result<()> {
    let ents = def["entities"].as_array().cloned().unwrap_or_default();
    let used = used_points(def);
    // payload point id -> SolidCraft point reference, filled as curves create their points
    let mut points: HashMap<String, String> = HashMap::new();
    let pos = |id: &str| -> Option<[f64; 2]> {
        ents.iter().find(|e| e["id"] == id).and_then(|e| pt(&e["p"]))
    };

    for e in &ents {
        let id = e["id"].as_str().unwrap_or("").to_string();
        let kind = e["type"].as_str().unwrap_or("");
        let construction = e["construction"].as_bool().unwrap_or(false);
        // a point argument: the shared point's reference if it exists, else the coordinates
        let arg = |pid: Option<&str>, xy: Option<[f64; 2]>, points: &HashMap<String, String>| -> Value {
            if let Some(r) = pid.and_then(|p| points.get(p)) {
                json!(r)
            } else {
                json!(xy.unwrap_or([0.0, 0.0]))
            }
        };
        let r: Result<()> = (|| {
            match kind {
                "line" => {
                    let a = arg(e["startPoint"].as_str(), pt(&e["start"]), &points);
                    let b = arg(e["endPoint"].as_str(), pt(&e["end"]), &points);
                    let rep = s.exec("sketch.line", json!({"points": [a, b], "connect": false, "construction": construction}))?;
                    let cid = first(&rep["created"]["curves"]).ok_or_else(|| anyhow!("no line made"))?;
                    remember(&mut points, e["startPoint"].as_str(), format!("{cid}.start"));
                    remember(&mut points, e["endPoint"].as_str(), format!("{cid}.end"));
                    out.built.ents.insert(id.clone(), Ent { kind: "line".into(), sc: cid, center: None, center_xy: None, radius: None });
                }
                "circle" => {
                    let c = arg(e["centerPoint"].as_str(), pt(&e["center"]), &points);
                    let rep = s.exec(
                        "sketch.circle.center",
                        json!({"center": c, "radius": num(&e["radius"]), "connect": false, "construction": construction}),
                    )?;
                    let cid = first(&rep["created"]["curves"]).ok_or_else(|| anyhow!("no circle made"))?;
                    let cref = format!("{cid}.center");
                    remember(&mut points, e["centerPoint"].as_str(), cref.clone());
                    out.built.ents.insert(
                        id.clone(),
                        Ent {
                            kind: "circle".into(),
                            sc: cid,
                            center: Some(points.get(e["centerPoint"].as_str().unwrap_or("")).cloned().unwrap_or(cref)),
                            center_xy: pt(&e["center"]),
                            radius: e["radius"].as_f64(),
                        },
                    );
                }
                "arc" => {
                    let c = arg(e["centerPoint"].as_str(), pt(&e["center"]), &points);
                    let a = arg(e["startPoint"].as_str(), pt(&e["start"]), &points);
                    let b = arg(e["endPoint"].as_str(), pt(&e["end"]), &points);
                    let ccw = e["ccw"].as_bool().unwrap_or(true);
                    let mut p = json!({"center": c, "start": a, "end": b, "connect": false, "construction": construction});
                    if !ccw {
                        p["clockwise"] = json!(true);
                    }
                    let rep = s.exec("sketch.arc.center_point", p)?;
                    let cid = first(&rep["created"]["curves"]).ok_or_else(|| anyhow!("no arc made"))?;
                    remember(&mut points, e["centerPoint"].as_str(), format!("{cid}.center"));
                    remember(&mut points, e["startPoint"].as_str(), format!("{cid}.start"));
                    remember(&mut points, e["endPoint"].as_str(), format!("{cid}.end"));
                    out.built.ents.insert(
                        id.clone(),
                        Ent {
                            kind: "arc".into(),
                            sc: cid,
                            center: e["centerPoint"].as_str().and_then(|p| points.get(p).cloned()),
                            center_xy: pt(&e["center"]),
                            radius: e["radius"].as_f64(),
                        },
                    );
                }
                "ellipse" => {
                    let c = arg(e["centerPoint"].as_str(), pt(&e["center"]), &points);
                    let major = pt(&e["major"]).ok_or_else(|| anyhow!("ellipse without its major point"))?;
                    let minor = pt(&e["minor"]).ok_or_else(|| anyhow!("ellipse without its minor point"))?;
                    let rep = s.exec("sketch.ellipse", json!({"center": c, "major": major, "minor": minor}))?;
                    let cid = first(&rep["created"]["curves"]).ok_or_else(|| anyhow!("no ellipse made"))?;
                    remember(&mut points, e["centerPoint"].as_str(), format!("{cid}.center"));
                    out.built.ents.insert(id.clone(), Ent { kind: "ellipse".into(), sc: cid, center: None, center_xy: pt(&e["center"]), radius: None });
                }
                "spline" => {
                    let rep = draw_spline(s, e)?;
                    let cid = first(&rep["created"]["curves"]).ok_or_else(|| anyhow!("no spline made"))?;
                    let created: Vec<String> = strings(&rep["created"]["points"]);
                    let ids: Vec<String> = strings(&e["pointIds"]);
                    if let (Some(first_id), Some(last_id)) = (ids.first(), ids.last()) {
                        if let Some(p) = created.iter().find(|p| p.ends_with(".start")).or(created.first()) {
                            points.entry(first_id.clone()).or_insert_with(|| p.clone());
                        }
                        if let Some(p) = created.iter().find(|p| p.ends_with(".end")).or(created.last()) {
                            points.entry(last_id.clone()).or_insert_with(|| p.clone());
                        }
                    }
                    out.built.ents.insert(id.clone(), Ent { kind: "spline".into(), sc: cid, center: None, center_xy: None, radius: None });
                }
                _ => {}
            }
            Ok(())
        })();
        if let Err(err) = r {
            if sc::is_cancelled(&err) {
                return Err(err);
            }
            out.missing.push(format!("{kind} {id} not drawn ({err})"));
        }
    }

    // Standalone points (no curve uses them), and the points curves use, recorded.
    for e in &ents {
        if e["type"] != "point" {
            continue;
        }
        let id = e["id"].as_str().unwrap_or("").to_string();
        if let Some(r) = points.get(&id) {
            out.built.ents.insert(id, Ent { kind: "point".into(), sc: r.clone(), center: None, center_xy: pt(&e["p"]), radius: None });
            continue;
        }
        if used.contains(&id) {
            // a curve that should have made it was not drawn
            continue;
        }
        let xy = pos(&id).unwrap_or([0.0, 0.0]);
        match s.exec("sketch.point", json!({"point": xy, "connect": false})) {
            Ok(rep) => {
                let pid = rep["point"].as_str().map(String::from).or_else(|| first(&rep["created"]["points"]));
                if let Some(pid) = pid {
                    out.built.ents.insert(id, Ent { kind: "point".into(), sc: pid, center: None, center_xy: Some(xy), radius: None });
                }
            }
            Err(err) if sc::is_cancelled(&err) => return Err(err),
            Err(err) => out.missing.push(format!("point {id} not drawn ({err})")),
        }
    }
    Ok(())
}

fn draw_spline(s: &mut Session, e: &Value) -> Result<Value> {
    let n = &e["nurbs"];
    if let (Some(deg), Some(knots), Some(cps)) = (n["degree"].as_u64(), n["knots"].as_array(), n["controlPoints"].as_array()) {
        let weights = n["weights"].as_array();
        let rational = weights.map(|w| w.iter().any(|x| (x.as_f64().unwrap_or(1.0) - 1.0).abs() > 1e-12)).unwrap_or(false);
        if !rational || deg == 2 {
            let pts: Vec<[f64; 2]> = cps.iter().filter_map(pt).collect();
            let mut p = json!({"points": pts, "degree": deg, "knots": knots});
            if rational {
                p["weights"] = json!(weights);
            }
            if let Ok(r) = s.exec("sketch.spline.control_knots", p) {
                return Ok(r);
            }
        }
    }
    let pts: Vec<[f64; 2]> = e["points"].as_array().into_iter().flatten().filter_map(pt).collect();
    s.exec("sketch.spline.fit_point", json!({"points": pts}))
}

fn first(v: &Value) -> Option<String> {
    v.as_array()?.first()?.as_str().map(String::from)
}

fn strings(v: &Value) -> Vec<String> {
    v.as_array().into_iter().flatten().filter_map(|x| x.as_str().map(String::from)).collect()
}

fn remember(points: &mut HashMap<String, String>, pid: Option<&str>, r: String) {
    if let Some(p) = pid {
        points.entry(p.to_string()).or_insert(r);
    }
}

/// A payload entity reference inside this sketch, as SolidCraft names it.
fn ref_of(out: &Outcome, v: &Value) -> Option<String> {
    if let Some(id) = v.as_str() {
        return out.built.ents.get(id).map(|e| e.sc.clone());
    }
    match v["kind"].as_str() {
        Some("origin") => Some("origin".into()),
        _ => None,
    }
}

fn add_constraints(s: &mut Session, def: &Value, out: &mut Outcome) -> Result<()> {
    for c in def["constraints"].as_array().into_iter().flatten() {
        let ty = c["type"].as_str().unwrap_or("");
        let ents = c["entities"].as_array().cloned().unwrap_or_default();
        let refs: Vec<Option<String>> = ents.iter().map(|e| ref_of(out, e)).collect();
        if refs.iter().any(Option::is_none) {
            let what = ents.iter().filter_map(|e| e["kind"].as_str()).collect::<Vec<_>>().join(", ");
            out.missing.push(format!("{ty} relation to {} not carried", if what.is_empty() { "an entity not drawn" } else { &what }));
            continue;
        }
        let r: Vec<String> = refs.into_iter().flatten().collect();
        let call: Option<(&str, Value)> = match (ty, r.len()) {
            ("coincident", 2) => Some(("sketch.constraint.coincident", json!({"a": r[0], "b": r[1]}))),
            ("horizontal" | "vertical", 1) => Some(("sketch.constraint.horizontal_vertical", json!({"line": r[0], "mode": ty}))),
            ("horizontal" | "vertical", 2) => Some(("sketch.constraint.horizontal_vertical", json!({"points": [r[0], r[1]], "mode": ty}))),
            ("parallel", 2) => Some(("sketch.constraint.parallel", json!({"a": r[0], "b": r[1]}))),
            ("perpendicular", 2) => Some(("sketch.constraint.perpendicular", json!({"a": r[0], "b": r[1]}))),
            ("tangent", 2) => Some(("sketch.constraint.tangent", json!({"a": r[0], "b": r[1]}))),
            ("equal", n) if n >= 2 => Some(("sketch.constraint.equal", json!({"curves": r}))),
            ("patterned", n) if n >= 2 => Some(("sketch.constraint.equal", json!({"curves": r}))),
            ("concentric", 2) => Some(("sketch.constraint.concentric", json!({"a": r[0], "b": r[1]}))),
            ("collinear", 2) => Some(("sketch.constraint.collinear", json!({"a": r[0], "b": r[1]}))),
            ("midpoint", 2) => Some(("sketch.constraint.midpoint", json!({"a": r[0], "b": r[1]}))),
            ("symmetric", 3) => Some(("sketch.constraint.symmetry", json!({"a": r[0], "b": r[1], "line": r[2]}))),
            ("fix", 1) => Some(("sketch.constraint.fix", json!({"entity": r[0], "fixed": true}))),
            _ => None,
        };
        match call {
            Some((cmd, p)) => match s.exec(cmd, p) {
                Ok(_) => {}
                Err(e) if sc::is_cancelled(&e) => return Err(e),
                Err(e) => out.missing.push(format!("{ty} relation not added ({})", one_line(&e.to_string()))),
            },
            None => out.missing.push(format!("{ty} relation not carried (no counterpart)")),
        }
    }
    Ok(())
}

fn one_line(s: &str) -> String {
    let mut t: String = s.lines().next().unwrap_or("").to_string();
    t.truncate(160);
    t
}

fn add_dimensions(s: &mut Session, st: &mut PartState, def: &Value, fid: &str, out: &mut Outcome) -> Result<()> {
    for d in def["dimensions"].as_array().into_iter().flatten() {
        let kind = d["kind"].as_str().unwrap_or("distance");
        let ents = d["entities"].as_array().cloned().unwrap_or_default();
        let value = num(&d["value"]);
        let label = d["fullName"].as_str().or(d["name"].as_str()).unwrap_or("?").to_string();
        // circles and arcs in a distance are measured at their centres (spec), except two concentric ones
        let concentric = ents.len() == 2 && {
            let a = ents[0].as_str().and_then(|i| out.built.ents.get(i));
            let b = ents[1].as_str().and_then(|i| out.built.ents.get(i));
            matches!((a, b), (Some(a), Some(b)) if a.center_xy.is_some() && a.radius.is_some() && b.radius.is_some()
                && a.center_xy.zip(b.center_xy).map(|(p, q)| (p[0]-q[0]).hypot(p[1]-q[1]) < 1e-6).unwrap_or(false))
        };
        let mut refs = Vec::new();
        for e in &ents {
            let r = if let Some(id) = e.as_str() {
                out.built.ents.get(id).map(|en| {
                    if matches!(kind, "distance" | "horizontal" | "vertical") && !concentric && (en.kind == "circle" || en.kind == "arc") {
                        en.center.clone().unwrap_or_else(|| en.sc.clone())
                    } else {
                        en.sc.clone()
                    }
                })
            } else {
                ref_of(out, e)
            };
            refs.push(r);
        }
        if refs.iter().any(Option::is_none) {
            out.missing.push(format!("dimension {label} to geometry outside the sketch not carried"));
            continue;
        }
        let refs: Vec<String> = refs.into_iter().flatten().collect();
        let ty = match kind {
            "horizontal" => "horizontal",
            "vertical" => "vertical",
            "angle" => "angle",
            "radius" => "radius",
            "diameter" => "diameter",
            _ => "distance",
        };
        let unit = if kind == "angle" { "deg" } else { "mm" };
        // what drives it: an equation, a parameter, or its own value
        let eq = st
            .equations
            .get(&format!("{fid}:{}", d["id"].as_str().unwrap_or("")))
            .or_else(|| d["fullName"].as_str().and_then(|n| st.equations.get(n)))
            .cloned();
        let mut expr = format!("{value} {unit}");
        let mut note = None;
        if let Some(e) = eq {
            match translate_expression(&e, st) {
                Some(x) => expr = x,
                None => note = Some(format!("dimension {label}: its equation {e} was not carried; its value was")),
            }
        } else if let Some(p) = d["param"].as_str() {
            if let Some(name) = st.params.get(p) {
                expr = name.clone();
            }
        }
        let driving = d["driving"].as_bool().unwrap_or(true);
        let mut p = json!({"entities": refs, "type": ty, "value": expr, "scale_sketch": false});
        if let Some(t) = pt(&d["textPosition"]) {
            p["text_at"] = json!(t);
        }
        if !driving {
            p["if_redundant"] = json!("driven");
        }
        match s.exec("sketch.dimension", p.clone()) {
            Ok(rep) => {
                if let (Some(full), Some(param)) = (d["fullName"].as_str(), rep["param"].as_str()) {
                    st.dim_params.insert(full.to_string(), param.to_string());
                    // equations may name it without the document: D1@SK_Base
                    let short = full.split('@').take(2).collect::<Vec<_>>().join("@");
                    st.dim_params.entry(short).or_insert_with(|| param.to_string());
                }
                if !driving {
                    if let Some(k) = first(&rep["created"]["dimensions"]) {
                        let _ = s.exec("sketch.toggle_driven", json!({"constraint": k, "driven": true}));
                    }
                }
                if let Some(n) = note {
                    out.missing.push(n);
                }
            }
            Err(e) if sc::is_cancelled(&e) => return Err(e),
            Err(e) => {
                // a dimension the sketch already determines: keep it as a driven one
                p["if_redundant"] = json!("driven");
                match s.exec("sketch.dimension", p) {
                    Ok(_) => out.missing.push(format!("dimension {label} is driven here (the sketch already fixes it)")),
                    Err(e2) if sc::is_cancelled(&e2) => return Err(e2),
                    Err(_) => out.missing.push(format!("dimension {label} not added ({})", one_line(&e.to_string()))),
                }
            }
        }
    }
    Ok(())
}
