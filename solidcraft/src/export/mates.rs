//! SolidCraft joints -> NCX mates (FORMAT-SPEC §5, Mates).
//!
//! A joint made as an assembly constraint keeps its rule (`constraint`: mate, flush, concentric, distance, angle,
//! parallel, perpendicular, tangent) and, for each end, the placement path (occurrence names from the top) and the
//! faces it was made on, by SolidCraft's persistent names. Each mated face is described in its part's own frame
//! while that part is written (`refs::Walk::face_by_name`); the mate goes into the assembly holding both ends,
//! with the component (and the nested path) below it. Rigid joints become `lock` mates. Faces that meet
//! (outward normals opposite) are `antiAligned`, faces that look the same way `aligned`.

use std::collections::{HashMap, HashSet};

use serde_json::{json, Value};

use crate::geom::{dot, v3, V3};

use super::Comp;

/// One end of a joint.
#[derive(Clone)]
pub struct End {
    pub comp: i64,
    /// occurrence names from the top of the design down to the part
    pub path: Vec<String>,
    /// the face the joint was made on (SolidCraft's name), or an origin plane of the part
    pub face: Option<String>,
    pub origin_plane: Option<&'static str>,
}

pub struct Joint {
    pub name: String,
    pub kind: String,
    pub a: End,
    pub b: End,
    pub offset: f64,
    pub angle: f64,
    pub suppressed: bool,
    /// the joint's frame (side a) in the model's frame
    pub frame: Option<Value>,
    /// SolidCraft's joint id (motion links name joints by it)
    pub id: i64,
    /// a motion link's NCX params (gear, screw, rackPinion)
    pub params: Value,
}

/// The joints `joint.inspect` lists, as far as NCX can carry them; the others with the reason.
pub fn read(joints: &Value, links: &Value) -> (Vec<Joint>, Vec<(String, String)>) {
    let mut out = Vec::new();
    let mut skipped = Vec::new();
    for j in joints["joints"].as_array().into_iter().flatten() {
        let name = j["name"].as_str().unwrap_or("Joint").to_string();
        let kind = match j["constraint"].as_str() {
            Some(c) => c.to_string(),
            None if j["type"] == "rigid" => "rigid".to_string(),
            None => {
                skipped.push((name, format!("a {} joint has no NCX mate in this version (the parts are placed, not joined)", j["type"].as_str().unwrap_or("?"))));
                continue;
            }
        };
        let end = |e: &Value| -> End {
            let snap = &e["snap"];
            let face = snap["face"].as_str().map(String::from).or_else(|| {
                e["references"].as_array().into_iter().flatten().find(|r| r["role"] == "face").and_then(|r| r["name"].as_str().map(String::from))
            });
            let origin_plane = match snap["plane"].as_str() {
                Some("XY") => Some("front"),
                Some("XZ") => Some("top"),
                Some("YZ") => Some("right"),
                _ => None,
            };
            End {
                comp: e["component"].as_i64().unwrap_or(-1),
                path: e["path"].as_str().unwrap_or("").split('/').filter(|s| !s.is_empty()).map(String::from).collect(),
                face: if origin_plane.is_some() { None } else { face },
                origin_plane,
            }
        };
        out.push(Joint {
            name,
            kind,
            a: end(&j["a"]),
            b: end(&j["b"]),
            offset: j["offset"].as_f64().unwrap_or(0.0),
            angle: j["angle"].as_f64().unwrap_or(0.0),
            suppressed: j["suppressed"].as_bool() == Some(true),
            frame: j["a"].get("frame").filter(|f| f.is_object()).cloned(),
            id: j["id"].as_i64().unwrap_or(-1),
            params: Value::Null,
        });
    }
    // motion links (gear, screw, rack and pinion) between the joints: NCX mates that couple motion, on the round
    // or flat faces those joints were made on (the moving part's end of each)
    let mut coupled = Vec::new();
    for (i, l) in links.as_array().into_iter().flatten().enumerate() {
        let c = &l["coupling"];
        let ty = c["type"].as_str().unwrap_or("");
        let (Some(ja), Some(jb)) = (out.iter().find(|j| Some(j.id) == l["a"].as_i64()), out.iter().find(|j| Some(j.id) == l["b"].as_i64())) else {
            skipped.push((format!("{ty} link {}", i + 1), "its joints are not carried".to_string()));
            continue;
        };
        // the end of a joint that is not on the other joint's parts: the part that turns or slides
        let moving = |x: &Joint, other: &Joint| -> End {
            let shared = |e: &End| e.comp == other.a.comp || e.comp == other.b.comp;
            if shared(&x.b) && !shared(&x.a) { x.a.clone() } else { x.b.clone() }
        };
        let mm = |v: &Value| v.as_f64().or_else(|| v.as_str().and_then(|s| s.trim().trim_end_matches("mm").trim().parse::<f64>().ok())).unwrap_or(0.0);
        let reverse = c["reverse"].as_bool().unwrap_or(false);
        let (kind, a, b, params) = match ty {
            "gear" => ("gear", moving(ja, jb), moving(jb, ja),
                       json!({"ratio": [mm(&c["ra"]), mm(&c["rb"])], "reverse": c["internal"].as_bool().unwrap_or(false) ^ reverse})),
            "screw" if ja.id == jb.id => ("screw", ja.a.clone(), ja.b.clone(), json!({"pitch": mm(&c["pitch"]), "reverse": reverse})),
            "rack_pinion" => {
                let r = mm(&c["radius"]);
                // NCX: the rack first, then the pinion
                ("rackPinion", moving(jb, ja), moving(ja, jb), json!({"pitchDiameter": 2.0 * r, "travelPerTurn": 2.0 * std::f64::consts::PI * r, "byTravel": false, "reverse": reverse}))
            }
            other => {
                skipped.push((format!("{other} link {}", i + 1), "this motion link has no NCX mate".to_string()));
                continue;
            }
        };
        coupled.push(Joint { name: format!("{}{}", match kind { "gear" => "Gear", "screw" => "Screw", _ => "RackPinion" }, i + 1), kind: kind.into(),
                             a, b, offset: 0.0, angle: 0.0, suppressed: false, frame: None, id: -1, params });
    }
    out.extend(coupled);
    (out, skipped)
}

/// The faces each component's mates need described.
pub fn faces_wanted(joints: &[Joint]) -> HashMap<i64, HashSet<String>> {
    let mut m: HashMap<i64, HashSet<String>> = HashMap::new();
    for j in joints {
        for e in [&j.a, &j.b] {
            if let Some(f) = &e.face {
                m.entry(e.comp).or_default().insert(f.clone());
            }
        }
    }
    m
}

/// The mates of the assembly `asm`: the joints whose ends meet first in it. `strip` is the number of leading
/// path names above the model (1 when the model sits in an upright placement), `faces` the descriptors measured
/// per (component, face name) with their outward normals in the part's frame.
pub fn for_assembly(
    asm: i64,
    root: i64,
    strip: usize,
    comps: &HashMap<i64, Comp>,
    joints: &[Joint],
    faces: &HashMap<(i64, String), (Value, Option<V3>)>,
    notes: &mut Vec<(String, String)>,
) -> Vec<Value> {
    let mut mates = Vec::new();
    for j in joints {
        let pa: Vec<&String> = j.a.path.iter().skip(strip).collect();
        let pb: Vec<&String> = j.b.path.iter().skip(strip).collect();
        let mut c = 0;
        while c < pa.len() && c < pb.len() && pa[c] == pb[c] {
            c += 1;
        }
        if c >= pa.len() || c >= pb.len() {
            continue; // both ends in one part: nothing to mate
        }
        // the assembly the two ends meet in
        let Some(holder) = walk(root, &pa[..c], comps) else { continue };
        if holder != asm {
            continue;
        }
        let side = |e: &End, p: &[&String]| -> Result<(Value, Option<V3>), String> {
            let r = if let Some(pl) = e.origin_plane {
                (json!({"kind": "originPlane", "by": "id", "plane": pl}), None)
            } else if let Some(f) = &e.face {
                match faces.get(&(e.comp, f.clone())) {
                    Some((d, n)) => (d.clone(), n.map(|n| to_assembly(n, &p[c..], walk(root, &p[..c], comps).unwrap_or(root), comps))),
                    None => return Err(format!("its face {f} could not be described")),
                }
            } else if j.kind == "rigid" {
                (json!({"kind": "originPlane", "by": "id", "plane": "front"}), None)
            } else {
                return Err("it was made on something other than a face or an origin plane".into());
            };
            let mut refobj = json!({"component": format!("c:{}", p[c]), "ref": r.0});
            if p.len() > c + 1 {
                refobj["path"] = json!(p[c + 1..].iter().map(|n| format!("c:{n}")).collect::<Vec<_>>());
            }
            Ok((refobj, r.1))
        };
        let (ra, rb) = match (side(&j.a, &pa), side(&j.b, &pb)) {
            (Ok(a), Ok(b)) => (a, b),
            (Err(e), _) | (_, Err(e)) => {
                notes.push((j.name.clone(), format!("not written: {e} (the parts are placed, not joined)")));
                continue;
            }
        };
        let (kind, params): (&str, Value) = match j.kind.as_str() {
            "mate" | "flush" => ("coincident", Value::Null),
            "concentric" => ("concentric", Value::Null),
            "distance" => ("distance", json!({"distance": j.offset.abs()})),
            "angle" => ("angle", json!({"angle": j.angle})),
            "parallel" => ("parallel", Value::Null),
            "perpendicular" => ("perpendicular", Value::Null),
            "tangent" => ("tangent", Value::Null),
            "rigid" => ("lock", Value::Null),
            "gear" | "screw" | "rackPinion" => (j.kind.as_str(), j.params.clone()),
            other => {
                notes.push((j.name.clone(), format!("a {other} constraint has no NCX mate in this version (the parts are placed, not joined)")));
                continue;
            }
        };
        let alignment = match (ra.1, rb.1) {
            (Some(na), Some(nb)) if kind != "concentric" => {
                if dot(na, nb) < 0.0 {
                    "antiAligned"
                } else {
                    "aligned"
                }
            }
            _ => match j.kind.as_str() {
                "mate" => "antiAligned",
                _ => "aligned",
            },
        };
        let (mut ra, mut rb) = (ra, rb);
        if kind == "rackPinion" {
            // FORMAT-SPEC: the rack's reference and the pinion's, by role
            ra.0["role"] = json!("rack");
            rb.0["role"] = json!("pinion");
        }
        let mut m = json!({"id": format!("m:{}", j.name), "name": j.name, "type": kind, "refs": [ra.0, rb.0],
                           "alignment": alignment, "status": "mapped"});
        if !params.is_null() {
            m["params"] = params;
        }
        if j.suppressed {
            m["suppressed"] = json!({"cfg:Default": true});
        }
        if asm == root {
            if let Some(f) = &j.frame {
                if let (Some(o), Some(x), Some(z)) = (v3(&f["origin"]), v3(&f["x"]), v3(&f["z"])) {
                    m["frame"] = json!({"origin": o, "x": x, "z": z});
                }
            }
        }
        mates.push(m);
    }
    mates
}

/// The component reached from `from` through the placements named.
fn walk(from: i64, names: &[&String], comps: &HashMap<i64, Comp>) -> Option<i64> {
    let mut cur = from;
    for n in names {
        cur = comps.get(&cur)?.children.iter().find(|ch| &&ch.2 == n)?.1;
    }
    Some(cur)
}

/// A direction in a part's frame, in the frame of the assembly `asm` that places it through the names given.
fn to_assembly(v: V3, names: &[&String], asm: i64, comps: &HashMap<i64, Comp>) -> V3 {
    // the placements from the assembly down to the part, then applied from the part up
    let mut chain: Vec<&Value> = Vec::new();
    let mut cur = asm;
    for n in names {
        let Some(ch) = comps.get(&cur).and_then(|c| c.children.iter().find(|ch| &&ch.2 == n)) else { break };
        chain.push(&ch.3);
        cur = ch.1;
    }
    let mut out = v;
    for t in chain.iter().rev() {
        // SolidCraft's rows are the images of the part's axes: v' = v · R
        let r = |i: usize, j: usize| t[i][j].as_f64().unwrap_or(if i == j { 1.0 } else { 0.0 });
        out = [
            out[0] * r(0, 0) + out[1] * r(1, 0) + out[2] * r(2, 0),
            out[0] * r(0, 1) + out[1] * r(1, 1) + out[2] * r(2, 1),
            out[0] * r(0, 2) + out[1] * r(1, 2) + out[2] * r(2, 2),
        ];
    }
    out
}
