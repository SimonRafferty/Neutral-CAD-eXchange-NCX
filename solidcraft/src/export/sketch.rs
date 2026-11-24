//! A SolidCraft sketch (`sketch.inspect`) -> an NCX `sketch` definition (FORMAT-SPEC §4, Sketches).
//!
//! SolidCraft lists points and curves in two arrays; relations and dimensions name their entities by index into
//! them (`p`, `q`: points; `l`, `c`, `a`, `b`: curves). Entity ids are SolidCraft's own (`l1`, `c1`, `p3`), with a
//! point of a curve (`l1.start`) written `l1_start`; the sketch origin is NCX's `origin` reference.

use std::collections::{HashMap, HashSet};

use serde_json::{json, Value};

use crate::geom::{cross, unit, v2, v3, V3};

use super::expr;

pub struct Sketch {
    pub def: Value,
    pub equations: Vec<Value>,
    pub notes: Vec<String>,
    /// entities or relations left out: the sketch is written `approximate`
    pub approximate: bool,
}

pub fn id_of(sc: &str) -> String {
    sc.replace('.', "_")
}

/// `plane` is the NCX plane reference, decided by the caller (origin plane, plane feature, face or frame).
pub fn convert(sk: &Value, feature_id: &str, sketch_name: &str, plane: Value, params: &HashSet<String>) -> Sketch {
    let mut notes = Vec::new();
    let mut approximate = false;
    let pl = &sk["plane"];
    let o = v3(&pl["origin"]).unwrap_or([0.0; 3]);
    let x = unit(v3(&pl["x_dir"]).unwrap_or([1.0, 0.0, 0.0]));
    let y = unit(v3(&pl["y_dir"]).unwrap_or([0.0, 1.0, 0.0]));
    let z: V3 = unit(cross(x, y));

    let points: Vec<Value> = sk["points"].as_array().cloned().unwrap_or_default();
    let curves: Vec<Value> = sk["curves"].as_array().cloned().unwrap_or_default();
    let at_of: HashMap<String, [f64; 2]> =
        points.iter().filter_map(|p| Some((id_of(p["id"].as_str()?), v2(&p["at"])?))).collect();
    let point_ref = |i: usize| -> Value {
        match points.get(i).and_then(|p| p["id"].as_str()) {
            Some("origin") | None => json!({"kind": "origin", "by": "id"}),
            Some(id) => json!(id_of(id)),
        }
    };
    let curve_ref = |i: usize| -> Value { json!(curves.get(i).and_then(|c| c["id"].as_str()).map(id_of).unwrap_or_default()) };

    let mut entities = Vec::new();
    let mut constraints = Vec::new();
    for p in &points {
        let id = p["id"].as_str().unwrap_or("");
        if id == "origin" {
            continue;
        }
        entities.push(json!({"id": id_of(id), "type": "point", "p": p["at"]}));
        if p["fixed"].as_bool() == Some(true) && p["link"].is_null() {
            constraints.push(json!({"type": "fix", "entities": [id_of(id)]}));
        }
    }
    let mut curve_ids: HashSet<String> = HashSet::new();
    for c in &curves {
        let id = id_of(c["id"].as_str().unwrap_or(""));
        let construction = c["construction"].as_bool() == Some(true);
        let pid = |k: &str| json!(id_of(c[k].as_str().unwrap_or("")));
        let mut e = match c["type"].as_str().unwrap_or("") {
            "line" => json!({"id": id, "type": "line", "start": c["start_at"], "end": c["end_at"],
                             "startPoint": pid("start"), "endPoint": pid("end")}),
            "circle" => json!({"id": id, "type": "circle", "center": c["center_at"], "centerPoint": pid("center"),
                               "radius": c["radius"]}),
            // SolidCraft's arcs run counter-clockwise from start to end
            "arc" => json!({"id": id, "type": "arc", "center": c["center_at"], "centerPoint": pid("center"),
                            "radius": c["radius"], "start": c["start_at"], "end": c["end_at"],
                            "startPoint": pid("start"), "endPoint": pid("end"), "ccw": true}),
            "ellipse" => {
                let ctr = v2(&c["center_at"]).unwrap_or([0.0; 2]);
                let maj = v2(&c["major_at"]).unwrap_or([ctr[0] + 1.0, ctr[1]]);
                let (dx, dy) = (maj[0] - ctr[0], maj[1] - ctr[1]);
                let l = dx.hypot(dy).max(1e-12);
                let r = c["minor_radius"].as_f64().unwrap_or(0.0);
                let minor = [ctr[0] - dy / l * r, ctr[1] + dx / l * r];
                let mut e = json!({"id": id, "type": "ellipse", "center": ctr, "major": maj, "minor": minor,
                                   "centerPoint": pid("center"), "majorPoint": pid("major")});
                // the minor axis end SolidCraft drew there, if any
                if let Some((mid, _)) = at_of.iter().find(|(_, a)| (a[0] - minor[0]).hypot(a[1] - minor[1]) < 1e-6) {
                    e["minorPoint"] = json!(mid);
                }
                e
            }
            "spline" if c["control"].as_bool() != Some(true) && c["periodic"].as_bool() != Some(true) => {
                let ids: Vec<Value> = c["points"].as_array().into_iter().flatten().map(|p| json!(id_of(p.as_str().unwrap_or("")))).collect();
                json!({"id": id, "type": "spline", "points": c["points_at"], "pointIds": ids})
            }
            other => {
                notes.push(format!("{} curve {} left out", if other.is_empty() { "an unknown" } else { other }, id));
                approximate = true;
                continue;
            }
        };
        if construction {
            e["construction"] = json!(true);
        }
        if c["fixed"].as_bool() == Some(true) {
            // a linked curve (a projected model edge or the sketch's own axis) is kept where it is
            constraints.push(json!({"type": "fix", "entities": [id.clone()]}));
        }
        curve_ids.insert(id);
        entities.push(e);
    }

    let mut dimensions = Vec::new();
    let mut equations = Vec::new();
    let idx = |c: &Value, k: &str| c[k].as_u64().map(|v| v as usize);
    let pos_of = |r: &Value| -> Option<[f64; 2]> {
        match r {
            Value::String(id) => {
                at_of.get(id).copied().or_else(|| {
                    let c = curves.iter().find(|c| c["id"].as_str().map(id_of).as_deref() == Some(id.as_str()))?;
                    match c["type"].as_str()? {
                        "line" => {
                            let (a, b) = (v2(&c["start_at"])?, v2(&c["end_at"])?);
                            Some([(a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0])
                        }
                        _ => v2(&c["center_at"]),
                    }
                })
            }
            _ => Some([0.0, 0.0]),
        }
    };
    for c in sk["constraints"].as_array().into_iter().flatten() {
        let t = c["type"].as_str().unwrap_or("");
        let p = idx(c, "p").map(point_ref);
        let q = idx(c, "q").map(point_ref);
        let l = idx(c, "l").map(curve_ref);
        let cc = idx(c, "c").map(curve_ref);
        let a = idx(c, "a").map(curve_ref);
        let b = idx(c, "b").map(curve_ref);
        if c.get("param").is_some() || c.get("value").map(|v| v.is_number()).unwrap_or(false) && t != "fix" {
            // a dimension
            let (kind, ents): (&str, Vec<Value>) = match t {
                "distance" => ("distance", vec![p.clone(), q.clone()].into_iter().flatten().collect()),
                "distance_x" | "horizontal_distance" => ("horizontal", vec![p.clone(), q.clone()].into_iter().flatten().collect()),
                "distance_y" | "vertical_distance" => ("vertical", vec![p.clone(), q.clone()].into_iter().flatten().collect()),
                "point_line_distance" => ("distance", vec![p.clone(), l.clone()].into_iter().flatten().collect()),
                "line_distance" | "parallel_distance" => ("distance", vec![a.clone(), b.clone()].into_iter().flatten().collect()),
                "length" => {
                    // a line's length: between its two ends
                    let li = idx(c, "l").and_then(|i| curves.get(i));
                    let ends = li.map(|ln| vec![json!(id_of(ln["start"].as_str().unwrap_or(""))), json!(id_of(ln["end"].as_str().unwrap_or("")))]);
                    ("distance", ends.unwrap_or_default())
                }
                "diameter" => ("diameter", cc.clone().into_iter().collect()),
                // twice a point's (or line's) distance from the revolve axis: NCX's diameter on two entities, the
                // measured one and the centreline (as SolidWorks writes its revolve diameters)
                "linear_diameter" => ("diameter", vec![p.clone().or(cc.clone()).or(a.clone()), l.clone().or(b.clone())].into_iter().flatten().collect()),
                "radius" => ("radius", cc.clone().into_iter().collect()),
                "angle" => ("angle", vec![a.clone(), b.clone()].into_iter().flatten().collect()),
                _ => ("", Vec::new()),
            };
            if kind.is_empty() || ents.is_empty() {
                notes.push(format!("dimension {} ({t}) left out", c["id"].as_str().unwrap_or("")));
                approximate = true;
                continue;
            }
            let name = c["param"].as_str().map(String::from).unwrap_or_else(|| c["id"].as_str().unwrap_or("d").to_string());
            let full = format!("{name}@{sketch_name}");
            let mut value = c["value"].as_f64().unwrap_or(0.0);
            if kind == "angle" && c["unit"] == "rad" {
                value = value.to_degrees();
            }
            // where the text goes: beside the entities' middle
            let ps: Vec<[f64; 2]> = ents.iter().filter_map(pos_of).collect();
            let n = ps.len().max(1) as f64;
            let mut tp = [ps.iter().map(|p| p[0]).sum::<f64>() / n, ps.iter().map(|p| p[1]).sum::<f64>() / n];
            match kind {
                "horizontal" => tp[1] += 8.0,
                "vertical" => tp[0] += 8.0,
                "diameter" | "radius" => {
                    let r = idx(c, "c").and_then(|i| curves.get(i)).and_then(|cv| cv["radius"].as_f64()).unwrap_or(5.0);
                    tp = [tp[0] + r * 0.8, tp[1] + r * 0.8];
                }
                _ => {
                    tp[0] += 5.0;
                    tp[1] += 5.0;
                }
            }
            let driving = c["driven"].as_bool() != Some(true);
            dimensions.push(json!({"id": name, "name": name, "fullName": full, "kind": kind, "entities": ents,
                                   "value": value, "driving": driving, "textPosition": tp}));
            if let Some(ex) = c["expression"].as_str() {
                if let Some((neutral, true)) = expr::neutral(ex, params) {
                    equations.push(json!({"target": full, "expression": neutral, "native": ex, "status": "mapped",
                                          "targetRef": {"kind": "dimension", "feature": feature_id, "id": name}}));
                }
            }
            continue;
        }
        let ents: Vec<Value> = match t {
            "coincident" => vec![p, q].into_iter().flatten().collect(),
            "point_on_curve" => vec![p, cc].into_iter().flatten().collect(),
            "midpoint" => vec![l, p].into_iter().flatten().collect(),
            "horizontal" | "vertical" => match (l.clone(), p.clone(), q.clone()) {
                (Some(l), _, _) => vec![l],
                (None, Some(p), Some(q)) => {
                    constraints.push(json!({"type": format!("{t}Points"), "entities": [p, q]}));
                    continue;
                }
                _ => Vec::new(),
            },
            "parallel" | "perpendicular" | "equal" | "tangent" | "concentric" | "collinear" => vec![a, b].into_iter().flatten().collect(),
            "symmetric" => match (p, q, a, b) {
                (Some(p), Some(q), _, _) => vec![Some(p), Some(q), l].into_iter().flatten().collect(),
                (_, _, Some(a), Some(b)) => vec![Some(a), Some(b), l].into_iter().flatten().collect(),
                _ => Vec::new(),
            },
            "fix" => vec![p, cc, l].into_iter().flatten().collect(),
            _ => Vec::new(),
        };
        let word = match t {
            "point_on_curve" => "coincident",
            other => other,
        };
        if ents.len() < if word == "fix" || word == "horizontal" || word == "vertical" { 1 } else { 2 } {
            notes.push(format!("relation {} ({t}) left out", c["id"].as_str().unwrap_or("")));
            approximate = true;
            continue;
        }
        constraints.push(json!({"type": word, "entities": ents}));
    }

    let full = sk["fully_constrained"].as_bool() == Some(true) || sk["dof"].as_i64() == Some(0);
    let def = json!({
        "constrained": if full { "full" } else { "under" },
        "hidden": sk["visible"].as_bool() != Some(true),
        "plane": plane,
        "frame": {"origin": o, "x": x, "z": z},
        "entities": entities,
        "constraints": constraints,
        "dimensions": dimensions,
    });
    Sketch { def, equations, notes, approximate }
}
