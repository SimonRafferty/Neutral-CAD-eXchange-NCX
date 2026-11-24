//! Small vector helpers, sketch frames and the measurements the fingerprint check needs.

use serde_json::{json, Value};

pub type V3 = [f64; 3];

pub fn v3(v: &Value) -> Option<V3> {
    let a = v.as_array()?;
    if a.len() < 3 {
        return None;
    }
    Some([a[0].as_f64()?, a[1].as_f64()?, a[2].as_f64()?])
}

pub fn v2(v: &Value) -> Option<[f64; 2]> {
    let a = v.as_array()?;
    if a.len() < 2 {
        return None;
    }
    Some([a[0].as_f64()?, a[1].as_f64()?])
}

pub fn add(a: V3, b: V3) -> V3 {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}
pub fn sub(a: V3, b: V3) -> V3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
pub fn scale(a: V3, s: f64) -> V3 {
    [a[0] * s, a[1] * s, a[2] * s]
}
pub fn dot(a: V3, b: V3) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
pub fn cross(a: V3, b: V3) -> V3 {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}
pub fn norm(a: V3) -> f64 {
    dot(a, a).sqrt()
}
pub fn unit(a: V3) -> V3 {
    let n = norm(a);
    if n < 1e-15 {
        a
    } else {
        scale(a, 1.0 / n)
    }
}

/// A sketch's frame as the payload gives it: origin, x and z (normal); y = z × x.
#[derive(Clone, Copy, Debug)]
pub struct Frame {
    pub origin: V3,
    pub x: V3,
    pub y: V3,
    pub z: V3,
}

impl Frame {
    pub fn from_def(frame: &Value) -> Option<Frame> {
        let origin = v3(frame.get("origin")?)?;
        let x = unit(v3(frame.get("x")?)?);
        let z = unit(v3(frame.get("z")?)?);
        let y = unit(cross(z, x));
        Some(Frame { origin, x, y, z })
    }
    pub fn to_world(&self, p: [f64; 2]) -> V3 {
        add(self.origin, add(scale(self.x, p[0]), scale(self.y, p[1])))
    }
}

/// Principal values of a symmetric 3x3 matrix (Jacobi rotations), ascending.
pub fn principal(m: [[f64; 3]; 3]) -> V3 {
    let mut a = m;
    for _ in 0..50 {
        // largest off-diagonal element
        let (mut p, mut q, mut big) = (0, 1, a[0][1].abs());
        for (i, j) in [(0, 2), (1, 2)] {
            if a[i][j].abs() > big {
                p = i;
                q = j;
                big = a[i][j].abs();
            }
        }
        let scale_ = a[0][0].abs() + a[1][1].abs() + a[2][2].abs();
        if big <= 1e-14 * scale_.max(1e-300) {
            break;
        }
        let theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        let t = theta.signum() / (theta.abs() + (theta * theta + 1.0).sqrt());
        let t = if theta == 0.0 { 1.0 } else { t };
        let c = 1.0 / (t * t + 1.0).sqrt();
        let s = t * c;
        let mut b = a;
        for k in 0..3 {
            b[k][p] = c * a[k][p] - s * a[k][q];
            b[k][q] = s * a[k][p] + c * a[k][q];
        }
        let mut d = b;
        for k in 0..3 {
            d[p][k] = c * b[p][k] - s * b[q][k];
            d[q][k] = s * b[p][k] + c * b[q][k];
        }
        a = d;
    }
    let mut ev = [a[0][0], a[1][1], a[2][2]];
    ev.sort_by(|x, y| x.partial_cmp(y).unwrap());
    ev
}

/// One body as SolidCraft's `measure` reports it, turned into an NCX fingerprint (spec §6, mm).
/// `moments` (principal, of volume: density 1) only where the caller measured them.
pub fn fingerprint_of(body: &Value, moments: Option<V3>) -> Value {
    let bb = &body["bbox"];
    let min = v3(&bb["min"]).unwrap_or([0.0; 3]);
    let max = v3(&bb["max"]).unwrap_or([0.0; 3]);
    let mut fp = json!({
        "volume": body["volume_mm3"].as_f64().unwrap_or(0.0),
        "area": body["area_mm2"].as_f64().unwrap_or(0.0),
        "centroid": body["center_of_mass"].clone(),
        "bbox": [min[0], min[1], min[2], max[0], max[1], max[2]],
        "faces": body["faces"].clone(),
        "edges": body["edges"].clone(),
    });
    if let Some(m) = moments {
        fp["moments"] = json!(m);
    }
    fp
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn principal_of_diagonal_and_rotated() {
        let ev = principal([[3.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 2.0]]);
        assert!((ev[0] - 1.0).abs() < 1e-12 && (ev[2] - 3.0).abs() < 1e-12);
        // [[2,1,0],[1,2,0],[0,0,5]] has eigenvalues 1, 3, 5
        let ev = principal([[2.0, 1.0, 0.0], [1.0, 2.0, 0.0], [0.0, 0.0, 5.0]]);
        assert!((ev[0] - 1.0).abs() < 1e-9 && (ev[1] - 3.0).abs() < 1e-9 && (ev[2] - 5.0).abs() < 1e-9);
    }
    #[test]
    fn frame_y_is_z_cross_x() {
        let f = Frame::from_def(&json!({"origin": [0,0,0], "x": [1,0,0], "z": [0,1,0]})).unwrap();
        // XZ-style plane with normal +Y: y = z × x = (0,1,0)×(1,0,0) = (0,0,-1)
        assert_eq!(f.y, [0.0, 0.0, -1.0]);
    }
}
