//! Geometric references (FORMAT-SPEC §5): the edges, faces and bodies a feature picked, described by their
//! geometry in the state the feature saw (the timeline rolled back to just before it).
//!
//! SolidCraft's features keep their picks as points (`edges: [[x,y,z]]`, `faces: [[x,y,z]]`), snapped to the
//! nearest edge or face when built. `body_topology` lists every edge (its ends, middle, length and the faces it
//! joins) and face (area, a point on it, a plane's normal or a cylinder's axis); `inspect.classify_points` says on
//! which face a point lies. From those: lines and circular edges exactly (a circle is the one through an edge's two
//! ends and middle whose arc has the edge's length), planes and cylinders with spread-out interior samples; any
//! other edge or face as freeform with the samples SolidCraft gives.
//!
//! The export runs with the part's placement moved to identity (mod.rs), so every coordinate SolidCraft reports is
//! already in the part's own frame.

use std::collections::HashMap;

use anyhow::{anyhow, Result};
use serde_json::{json, Value};

use crate::geom::{add, cross, dot, norm, scale, sub, unit, v3, V3};
use crate::sc::Session;

/// The design at one point of its timeline, with what was measured there.
pub struct Walk<'a> {
    pub s: &'a mut Session,
    /// the part component being written (0: the design itself)
    pub comp: i64,
    /// features kept active (None: the end of the timeline)
    marker: Option<usize>,
    topo: HashMap<String, Value>,
    bodies: Option<Vec<Value>>,
}

impl<'a> Walk<'a> {
    pub fn new(s: &'a mut Session, comp: i64) -> Walk<'a> {
        Walk { s, comp, marker: None, topo: HashMap::new(), bodies: None }
    }

    /// Roll the timeline so that `k` features are active (None: all of them).
    pub fn at(&mut self, k: Option<usize>) -> Result<()> {
        if self.marker != k {
            let pos = match k {
                Some(n) => json!(n),
                None => json!("end"),
            };
            self.s.exec("timeline.rollback", json!({"position": pos}))?;
            self.marker = k;
            self.topo.clear();
            self.bodies = None;
        }
        Ok(())
    }

    /// The part's bodies now, as `component.properties` reports them (volume, area, centre of mass, box, inertia).
    pub fn bodies(&mut self) -> Result<Vec<Value>> {
        if self.bodies.is_none() {
            let r = self.s.exec("component.properties", json!({"component": self.comp, "descendants": false}))?;
            self.bodies = Some(r["bodies"].as_array().cloned().unwrap_or_default());
        }
        Ok(self.bodies.clone().unwrap_or_default())
    }

    pub fn body_names(&mut self) -> Result<Vec<String>> {
        Ok(self.bodies()?.iter().filter_map(|b| b["name"].as_str().map(String::from)).collect())
    }

    fn topology(&mut self, body: &str) -> Result<Value> {
        if !self.topo.contains_key(body) {
            let t = self.s.tool("body_topology", json!({"body": body}))?;
            self.topo.insert(body.to_string(), t);
        }
        Ok(self.topo[body].clone())
    }

    /// An edge descriptor for a pick point: the nearest edge of the part's bodies (as SolidCraft snapped it).
    pub fn edge(&mut self, pick: V3, only_body: Option<&str>) -> Result<Value> {
        let mut best: Option<(f64, String, Value)> = None;
        for b in self.body_names()? {
            if only_body.map(|o| o != b).unwrap_or(false) {
                continue;
            }
            let t = self.topology(&b)?;
            for e in t["edges"].as_array().into_iter().flatten() {
                let g = edge_geo(e, &t);
                let d = g.distance(pick);
                if best.as_ref().map(|x| d < x.0).unwrap_or(true) {
                    best = Some((d, b.clone(), e.clone()));
                }
            }
        }
        let (_, body, e) = best.ok_or_else(|| anyhow!("no edge near {pick:?}"))?;
        let t = self.topology(&body)?;
        Ok(json!({"kind": "edge", "by": "geom", "geom": edge_desc(&e, &t)}))
    }

    /// The face a point lies on (body name, face record), or None.
    pub fn face_at(&mut self, pick: V3, only_body: Option<&str>) -> Result<Option<(String, Value)>> {
        let mut near: Option<(f64, String, Value)> = None;
        for b in self.body_names()? {
            if only_body.map(|o| o != b).unwrap_or(false) {
                continue;
            }
            let r = self.s.exec("inspect.classify_points", json!({"body": b, "points": [pick]}))?;
            let hit = &r["results"][0];
            let t = self.topology(&b)?;
            if hit["class"] == "on" {
                if let Some(name) = hit["face"].as_str() {
                    if let Some(f) = t["faces"].as_array().into_iter().flatten().find(|f| f["name"] == name) {
                        return Ok(Some((b, f.clone())));
                    }
                }
            }
            // not exactly on one: the face whose own point or plane is nearest (SolidCraft snaps picks)
            for f in t["faces"].as_array().into_iter().flatten() {
                let d = face_distance(f, pick);
                if near.as_ref().map(|x| d < x.0).unwrap_or(true) {
                    near = Some((d, b.clone(), f.clone()));
                }
            }
        }
        Ok(near.filter(|n| n.0 < 0.5).map(|n| (n.1, n.2)))
    }

    /// A face descriptor for a pick point.
    pub fn face(&mut self, pick: V3, only_body: Option<&str>) -> Result<Value> {
        let (body, f) = self.face_at(pick, only_body)?.ok_or_else(|| anyhow!("no face at {pick:?}"))?;
        let t = self.topology(&body)?;
        let geom = self.face_desc(&body, &f, &t)?;
        Ok(json!({"kind": "face", "by": "geom", "geom": geom}))
    }

    /// A face descriptor for a face named as SolidCraft names it (`F4:side:c1`), with its outward normal at its
    /// first sample (a plane's normal faces out of the material).
    pub fn face_by_name(&mut self, name: &str) -> Result<Option<(Value, Option<V3>)>> {
        for b in self.body_names()? {
            let t = self.topology(&b)?;
            let Some(f) = t["faces"].as_array().into_iter().flatten().find(|f| f["name"] == name).cloned() else { continue };
            let geom = self.face_desc(&b, &f, &t)?;
            let at = geom["samples"][0].clone();
            let r = self.s.exec("inspect.classify_points", json!({"body": b, "points": [at]}))?;
            let n = v3(&r["results"][0]["normal"]);
            return Ok(Some((json!({"kind": "face", "by": "geom", "geom": geom}), n)));
        }
        Ok(None)
    }

    /// A planar face of the part's bodies lying in the plane through `o` with normal `n`, as a descriptor.
    pub fn planar_face_in(&mut self, o: V3, n: V3) -> Result<Option<Value>> {
        for b in self.body_names()? {
            let t = self.topology(&b)?;
            let hit = t["faces"].as_array().into_iter().flatten().find(|f| {
                let (Some(fn_), Some(p)) = (v3(&f["plane_normal"]).filter(|v| norm(*v) > 0.5), v3(&f["point"])) else { return false };
                norm(cross(unit(fn_), n)) < 1e-9 && dot(sub(p, o), n).abs() < 1e-6
            }).cloned();
            if let Some(f) = hit {
                let geom = self.face_desc(&b, &f, &t)?;
                return Ok(Some(json!({"kind": "face", "by": "geom", "geom": geom})));
            }
        }
        Ok(None)
    }

    /// A body reference (`{volume, centroid, bbox}`) by name.
    pub fn body_ref(&mut self, name: &str) -> Result<Value> {
        let b = self.bodies()?.into_iter().find(|b| b["name"] == name).ok_or_else(|| anyhow!("no body {name}"))?;
        let bb = &b["bbox"];
        let (mn, mx) = (v3(&bb["min"]).unwrap_or([0.0; 3]), v3(&bb["max"]).unwrap_or([0.0; 3]));
        Ok(json!({"kind": "body", "by": "geom", "geom": {
            "volume": b["volume_mm3"], "centroid": b["center_of_mass"],
            "bbox": [mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]]}}))
    }

    fn face_desc(&mut self, body: &str, f: &Value, t: &Value) -> Result<Value> {
        let name = f["name"].as_str().unwrap_or("");
        let point = v3(&f["point"]).unwrap_or([0.0; 3]);
        let area = f["area"].as_f64().unwrap_or(0.0);
        // the face's boundary, as points along each of its edges
        let mut boundary: Vec<Vec<V3>> = Vec::new();
        let mut bbox = BBox::default();
        for e in t["edges"].as_array().into_iter().flatten() {
            let en = e["name"].as_str().unwrap_or("");
            if !split_names(en).iter().any(|n| n == name) {
                continue;
            }
            let g = edge_geo(e, t);
            g.add_bbox(&mut bbox);
            boundary.push(g.polyline());
        }
        let (kind, params, cands): (&str, Value, Vec<V3>) = if let Some(n) = v3(&f["plane_normal"]).filter(|n| norm(*n) > 0.5) {
            let n = unit(n);
            ("plane", json!({"normal": n, "point": point}), plane_grid(point, n, &boundary))
        } else if let Some(c) = f.get("cylinder").filter(|c| c.is_object()) {
            let a = unit(v3(&c["axis"]).unwrap_or([0.0, 0.0, 1.0]));
            let o = v3(&c["origin"]).unwrap_or([0.0; 3]);
            let r = c["radius"].as_f64().unwrap_or(0.0);
            ("cylinder", json!({"axis": a, "origin": o, "radius": r}), cylinder_grid(point, a, o, r, &boundary))
        } else {
            ("bspline", json!({}), Vec::new())
        };
        // which candidates lie on this face
        let mut on: Vec<V3> = vec![point];
        for chunk in cands.chunks(255) {
            let r = self.s.exec("inspect.classify_points", json!({"body": body, "points": chunk}))?;
            for (p, hit) in chunk.iter().zip(r["results"].as_array().into_iter().flatten()) {
                if hit["class"] == "on" && hit["face"] == name {
                    on.push(*p);
                }
            }
        }
        let segs: Vec<(V3, V3)> = boundary.iter().flat_map(|pl| pl.windows(2).map(|w| (w[0], w[1]))).collect();
        let clear = |p: V3| segs.iter().map(|(a, b)| seg_distance(p, *a, *b)).fold(f64::INFINITY, f64::min);
        let mut scored: Vec<(V3, f64)> = on.iter().map(|p| (*p, clear(*p))).collect();
        scored.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
        let mut samples: Vec<(V3, f64)> = Vec::new();
        if let Some(first) = scored.first() {
            samples.push(*first);
            let floor = (first.1 * 0.3).max(0.01).min(first.1);
            while samples.len() < 5 {
                let next = scored
                    .iter()
                    .filter(|(p, c)| *c >= floor && samples.iter().all(|(q, _)| norm(sub(*p, *q)) > 1e-6))
                    .max_by(|a, b| {
                        let da = samples.iter().map(|(q, _)| norm(sub(a.0, *q))).fold(f64::INFINITY, f64::min);
                        let db = samples.iter().map(|(q, _)| norm(sub(b.0, *q))).fold(f64::INFINITY, f64::min);
                        da.partial_cmp(&db).unwrap_or(std::cmp::Ordering::Equal)
                    })
                    .copied();
                match next {
                    Some(n) => samples.push(n),
                    None => break,
                }
            }
        }
        for (p, _) in &samples {
            bbox.add(*p);
        }
        if bbox.empty() {
            bbox.add(point);
        }
        let clearance = samples.iter().map(|s| s.1).fold(f64::INFINITY, f64::min);
        let clearance = if clearance.is_finite() { clearance } else { 0.0 };
        let mut g = json!({
            "type": kind, "params": params,
            "samples": samples.iter().map(|s| s.0).collect::<Vec<_>>(),
            "area": area, "bbox": bbox.json(), "clearance": clearance,
        });
        if clearance < 0.01 {
            g["weak"] = json!(true);
        }
        Ok(g)
    }
}

/// The names of the faces an edge joins: `A|B`, where a name may itself hold `|` inside parentheses.
pub fn split_names(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut depth = 0i32;
    let mut cur = String::new();
    for c in s.chars() {
        match c {
            '(' => depth += 1,
            ')' => depth -= 1,
            '|' if depth == 0 => {
                out.push(std::mem::take(&mut cur));
                continue;
            }
            _ => {}
        }
        cur.push(c);
    }
    out.push(cur);
    out
}

/// SolidCraft measures a curved edge's length on its tessellation: a 5 mm circle comes out 0.16 % short (31.365
/// for 31.416), a 6 mm hole in a face that came from STEP 2.5 % short (36.74 for 37.70). A circle through an
/// edge's ends and middle is taken as the edge's curve when a cylinder the edge bounds has its radius and axis, or
/// else when its arc is this close to the reported length.
const ARC_LEN_TOL: f64 = 0.05;

/// Whether a face the edge bounds is a cylinder of radius `r` about an axis through `c` along `n`.
fn on_cylinder(e: &Value, topo: &Value, c: V3, n: V3, r: f64) -> bool {
    let names = split_names(e["name"].as_str().unwrap_or(""));
    topo["faces"].as_array().into_iter().flatten().any(|f| {
        if !names.iter().any(|x| f["name"] == x.as_str()) {
            return false;
        }
        let Some(cy) = f.get("cylinder").filter(|c| c.is_object()) else { return false };
        let (Some(a), Some(o), Some(cr)) = (v3(&cy["axis"]), v3(&cy["origin"]), cy["radius"].as_f64()) else { return false };
        let a = unit(a);
        let d = sub(c, o);
        let off = norm(sub(d, scale(a, dot(d, a))));
        (cr - r).abs() <= 1e-6 * r.max(1.0) && off <= 1e-6 * r.max(1.0) && norm(cross(a, n)) <= 1e-6
    })
}

/// An edge's curve as far as the topology tells it.
enum EdgeGeo {
    Line { a: V3, b: V3 },
    /// centre, unit normal (start -> end turns about it), radius, start direction, sweep (rad)
    Arc { c: V3, n: V3, r: f64, u: V3, sweep: f64 },
    Free { pts: Vec<V3> },
}

fn edge_geo(e: &Value, topo: &Value) -> EdgeGeo {
    let s = v3(&e["start"]).unwrap_or([0.0; 3]);
    let en = v3(&e["end"]).unwrap_or([0.0; 3]);
    let m = v3(&e["mid"]).unwrap_or([0.0; 3]);
    let len = e["length"].as_f64().unwrap_or(0.0);
    let tol = 1e-6 * len.max(1.0);
    let chord = norm(sub(en, s));
    if (chord - len).abs() <= tol {
        return EdgeGeo::Line { a: s, b: en };
    }
    if chord <= tol {
        // closed: a full circle has its middle opposite its start
        let c = scale(add(s, m), 0.5);
        let r = norm(sub(m, s)) * 0.5;
        if r > 0.0 {
            if let Some(n) = circle_normal(e, topo, c, s) {
                if on_cylinder(e, topo, c, n, r) || (2.0 * std::f64::consts::PI * r - len).abs() <= ARC_LEN_TOL * len.max(1.0) {
                    return EdgeGeo::Arc { c, n, r, u: unit(sub(s, c)), sweep: 2.0 * std::f64::consts::PI };
                }
            }
        }
        return EdgeGeo::Free { pts: vec![s, m, en] };
    }
    if let Some((c, n, r)) = circumcircle(s, m, en) {
        let u = unit(sub(s, c));
        let v = cross(n, u);
        let ang = |p: V3| {
            let d = sub(p, c);
            let a = dot(d, v).atan2(dot(d, u));
            if a < 0.0 {
                a + 2.0 * std::f64::consts::PI
            } else {
                a
            }
        };
        let sweep = ang(en);
        if on_cylinder(e, topo, c, n, r) || (r * sweep - len).abs() <= ARC_LEN_TOL * len.max(1.0) {
            return EdgeGeo::Arc { c, n, r, u, sweep };
        }
    }
    EdgeGeo::Free { pts: vec![s, m, en] }
}

/// A full circle's normal: from a plane or cylinder it bounds.
fn circle_normal(e: &Value, topo: &Value, c: V3, s: V3) -> Option<V3> {
    let names = split_names(e["name"].as_str().unwrap_or(""));
    for f in topo["faces"].as_array().into_iter().flatten() {
        if !names.iter().any(|n| f["name"] == n.as_str()) {
            continue;
        }
        if let Some(n) = v3(&f["plane_normal"]).filter(|n| norm(*n) > 0.5) {
            if dot(unit(n), sub(s, c)).abs() < 1e-6 * norm(sub(s, c)).max(1.0) {
                return Some(unit(n));
            }
        }
        if let Some(a) = f.get("cylinder").and_then(|c| v3(&c["axis"])) {
            return Some(unit(a));
        }
    }
    None
}

/// The circle through three points: centre, unit normal (a -> b -> c turns positively about it), radius.
fn circumcircle(a: V3, b: V3, c: V3) -> Option<(V3, V3, f64)> {
    let ab = sub(b, a);
    let ac = sub(c, a);
    let n = cross(ab, ac);
    let nn = dot(n, n);
    if nn < 1e-18 {
        return None;
    }
    let t1 = scale(cross(n, ab), dot(ac, ac));
    let t2 = scale(cross(ac, n), dot(ab, ab));
    let center = add(a, scale(add(t1, t2), 0.5 / nn));
    Some((center, unit(n), norm(sub(a, center))))
}

impl EdgeGeo {
    fn point_at(&self, f: f64) -> V3 {
        match self {
            EdgeGeo::Line { a, b } => add(*a, scale(sub(*b, *a), f)),
            EdgeGeo::Arc { c, n, r, u, sweep } => {
                let v = cross(*n, *u);
                let t = f * sweep;
                add(*c, add(scale(*u, r * t.cos()), scale(v, r * t.sin())))
            }
            EdgeGeo::Free { pts } => pts[1],
        }
    }

    fn polyline(&self) -> Vec<V3> {
        match self {
            EdgeGeo::Line { a, b } => (0..=8).map(|i| add(*a, scale(sub(*b, *a), i as f64 / 8.0))).collect(),
            EdgeGeo::Arc { .. } => (0..=32).map(|i| self.point_at(i as f64 / 32.0)).collect(),
            EdgeGeo::Free { pts } => pts.clone(),
        }
    }

    fn distance(&self, p: V3) -> f64 {
        match self {
            EdgeGeo::Line { a, b } => seg_distance(p, *a, *b),
            EdgeGeo::Arc { c, n, r, u, sweep } => {
                let d = sub(p, *c);
                let h = dot(d, *n);
                let inplane = sub(d, scale(*n, h));
                let rho = norm(inplane);
                let v = cross(*n, *u);
                let mut ang = dot(inplane, v).atan2(dot(inplane, *u));
                if ang < 0.0 {
                    ang += 2.0 * std::f64::consts::PI;
                }
                if ang <= *sweep + 1e-12 {
                    (h * h + (rho - r) * (rho - r)).sqrt()
                } else {
                    norm(sub(p, self.point_at(0.0))).min(norm(sub(p, self.point_at(1.0))))
                }
            }
            EdgeGeo::Free { pts } => pts.windows(2).map(|w| seg_distance(p, w[0], w[1])).fold(f64::INFINITY, f64::min),
        }
    }

    fn add_bbox(&self, b: &mut BBox) {
        match self {
            EdgeGeo::Line { a, b: e } => {
                b.add(*a);
                b.add(*e);
            }
            EdgeGeo::Arc { c, n, r, u, sweep } => {
                b.add(self.point_at(0.0));
                b.add(self.point_at(1.0));
                // where the circle reaches furthest along each axis, if the arc gets there
                let v = cross(*n, *u);
                for k in 0..3 {
                    let mut ek = [0.0; 3];
                    ek[k] = 1.0;
                    let (cu, cv) = (dot(ek, *u), dot(ek, v));
                    if cu.hypot(cv) < 1e-12 {
                        continue;
                    }
                    for t0 in [cv.atan2(cu), cv.atan2(cu) + std::f64::consts::PI] {
                        let mut t = t0.rem_euclid(2.0 * std::f64::consts::PI);
                        if t > *sweep + 1e-12 {
                            t = f64::NAN;
                        }
                        if t.is_finite() {
                            b.add(add(*c, add(scale(*u, r * t.cos()), scale(v, r * t.sin()))));
                        }
                    }
                }
            }
            EdgeGeo::Free { pts } => {
                for p in pts {
                    b.add(*p);
                }
            }
        }
    }
}

/// The descriptor of one topology edge.
fn edge_desc(e: &Value, topo: &Value) -> Value {
    let g = edge_geo(e, topo);
    // a circular edge's exact length (SolidCraft's is measured on its tessellation)
    let len = match &g {
        EdgeGeo::Arc { r, sweep, .. } => r * sweep,
        _ => e["length"].as_f64().unwrap_or(0.0),
    };
    let mut bb = BBox::default();
    g.add_bbox(&mut bb);
    let names = split_names(e["name"].as_str().unwrap_or(""));
    let mut d = match &g {
        EdgeGeo::Line { a, b } => json!({"type": "line", "params": {"direction": unit(sub(*b, *a)), "point": a}}),
        EdgeGeo::Arc { c, n, r, .. } => json!({"type": "circle", "params": {"axis": n, "center": c, "radius": r}}),
        EdgeGeo::Free { .. } => json!({"type": "bspline", "params": {}}),
    };
    let samples: Vec<V3> = match &g {
        EdgeGeo::Free { pts } => vec![pts[1]],
        _ => [0.1, 0.3, 0.5, 0.7, 0.9].iter().map(|f| g.point_at(*f)).collect(),
    };
    d["samples"] = json!(samples);
    d["length"] = json!(len);
    d["bbox"] = bb.json();
    if names.len() < 2 || names.windows(2).any(|w| w[0] == w[1]) {
        d["seam"] = json!(true);
    }
    if len < 1e-9 {
        d["degenerate"] = json!(true);
    }
    d
}

fn face_distance(f: &Value, p: V3) -> f64 {
    let q = v3(&f["point"]).unwrap_or([1e9; 3]);
    if let Some(n) = v3(&f["plane_normal"]).filter(|n| norm(*n) > 0.5) {
        // off its plane, else the distance to its sample point as a weak tie-break
        dot(sub(p, q), unit(n)).abs() + 1e-3 * norm(sub(p, q))
    } else if let Some(c) = f.get("cylinder").filter(|c| c.is_object()) {
        let a = unit(v3(&c["axis"]).unwrap_or([0.0, 0.0, 1.0]));
        let o = v3(&c["origin"]).unwrap_or([0.0; 3]);
        let d = sub(p, o);
        let rho = norm(sub(d, scale(a, dot(d, a))));
        (rho - c["radius"].as_f64().unwrap_or(0.0)).abs() + 1e-3 * norm(sub(p, q))
    } else {
        norm(sub(p, q))
    }
}

fn seg_distance(p: V3, a: V3, b: V3) -> f64 {
    let ab = sub(b, a);
    let l2 = dot(ab, ab);
    let t = if l2 > 0.0 { (dot(sub(p, a), ab) / l2).clamp(0.0, 1.0) } else { 0.0 };
    norm(sub(p, add(a, scale(ab, t))))
}

fn perpendicular(n: V3) -> V3 {
    let t = if n[0].abs() < 0.9 { [1.0, 0.0, 0.0] } else { [0.0, 1.0, 0.0] };
    unit(cross(n, t))
}

/// Candidate points on a plane, over its boundary's extent.
fn plane_grid(p0: V3, n: V3, boundary: &[Vec<V3>]) -> Vec<V3> {
    let u = perpendicular(n);
    let v = cross(n, u);
    let (mut a0, mut a1, mut b0, mut b1) = (f64::INFINITY, f64::NEG_INFINITY, f64::INFINITY, f64::NEG_INFINITY);
    for p in boundary.iter().flatten() {
        let d = sub(*p, p0);
        let (x, y) = (dot(d, u), dot(d, v));
        a0 = a0.min(x);
        a1 = a1.max(x);
        b0 = b0.min(y);
        b1 = b1.max(y);
    }
    if !a0.is_finite() {
        return Vec::new();
    }
    let k = 15;
    let mut out = Vec::new();
    for i in 0..k {
        for j in 0..k {
            let x = a0 + (a1 - a0) * (i as f64 + 0.5) / k as f64;
            let y = b0 + (b1 - b0) * (j as f64 + 0.5) / k as f64;
            out.push(add(p0, add(scale(u, x), scale(v, y))));
        }
    }
    out
}

/// Candidate points on a cylinder, over its boundary's angle and height.
fn cylinder_grid(p0: V3, a: V3, o: V3, r: f64, boundary: &[Vec<V3>]) -> Vec<V3> {
    let d0 = sub(p0, o);
    let u = unit(sub(d0, scale(a, dot(d0, a))));
    if norm(u) < 0.5 {
        return Vec::new();
    }
    let v = cross(a, u);
    let (mut t0, mut t1, mut h0, mut h1) = (f64::INFINITY, f64::NEG_INFINITY, f64::INFINITY, f64::NEG_INFINITY);
    for p in boundary.iter().flatten() {
        let d = sub(*p, o);
        let t = dot(d, v).atan2(dot(d, u));
        let h = dot(d, a);
        t0 = t0.min(t);
        t1 = t1.max(t);
        h0 = h0.min(h);
        h1 = h1.max(h);
    }
    if !t0.is_finite() {
        return Vec::new();
    }
    let k = 15;
    let mut out = Vec::new();
    for i in 0..k {
        for j in 0..k {
            let t = t0 + (t1 - t0) * (i as f64 + 0.5) / k as f64;
            let h = h0 + (h1 - h0) * (j as f64 + 0.5) / k as f64;
            out.push(add(o, add(scale(a, h), add(scale(u, r * t.cos()), scale(v, r * t.sin())))));
        }
    }
    out
}

#[derive(Default)]
struct BBox {
    min: Option<V3>,
    max: Option<V3>,
}

impl BBox {
    fn add(&mut self, p: V3) {
        let (mn, mx) = (self.min.get_or_insert(p), self.max.get_or_insert(p));
        for k in 0..3 {
            mn[k] = mn[k].min(p[k]);
            mx[k] = mx[k].max(p[k]);
        }
    }
    fn empty(&self) -> bool {
        self.min.is_none()
    }
    fn json(&self) -> Value {
        let (a, b) = (self.min.unwrap_or([0.0; 3]), self.max.unwrap_or([0.0; 3]));
        json!([a[0], a[1], a[2], b[0], b[1], b[2]])
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn names_split_at_the_top_level() {
        assert_eq!(split_names("F2:side:l2|F2:top"), vec!["F2:side:l2", "F2:top"]);
        assert_eq!(split_names("F11:blend(F2:side:l1|F2:top)|F2:top"), vec!["F11:blend(F2:side:l1|F2:top)", "F2:top"]);
    }
    #[test]
    fn a_quarter_arc_is_a_circle() {
        let e = json!({"start": [10.0, 0.0, 0.0], "end": [0.0, 10.0, 0.0],
                       "mid": [10.0 * 0.5f64.sqrt(), 10.0 * 0.5f64.sqrt(), 0.0],
                       "length": 10.0 * std::f64::consts::FRAC_PI_2, "name": "a|b"});
        let d = edge_desc(&e, &json!({"faces": []}));
        assert_eq!(d["type"], "circle");
        assert!((d["params"]["radius"].as_f64().unwrap() - 10.0).abs() < 1e-9);
        let bb = d["bbox"].as_array().unwrap();
        assert!((bb[3].as_f64().unwrap() - 10.0).abs() < 1e-9 && (bb[4].as_f64().unwrap() - 10.0).abs() < 1e-9);
    }
}
