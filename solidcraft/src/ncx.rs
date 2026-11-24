//! Safe wrappers round the NCX core's C ABI (`core/include/ncx_core.h`). Everything crosses as UTF-8;
//! structured data as JSON. Every string the core returns is released with `ncx_free`.

use std::ffi::{c_char, CStr, CString};
use std::ptr;

use anyhow::{anyhow, Result};
use serde_json::Value;

#[allow(non_camel_case_types)]
type ncx_status = i32;

extern "C" {
    fn ncx_version() -> *const c_char;
    fn ncx_schema_version() -> *const c_char;
    fn ncx_payload_write(
        step: *const c_char,
        step_len: usize,
        model: *const c_char,
        model_len: usize,
        options: *const c_char,
        out_step: *mut *mut c_char,
        out_step_len: *mut usize,
        out_report: *mut *mut c_char,
    ) -> ncx_status;
    fn ncx_payload_read(
        step: *const c_char,
        step_len: usize,
        out_json: *mut *mut c_char,
        out_json_len: *mut usize,
        out_report: *mut *mut c_char,
    ) -> ncx_status;
    fn ncx_model_validate(model: *const c_char, model_len: usize, out_report: *mut *mut c_char) -> ncx_status;
    fn ncx_fingerprint_compare_ex(
        expected: *const c_char,
        actual: *const c_char,
        options: *const c_char,
        out_report: *mut *mut c_char,
    ) -> ncx_status;
    fn ncx_free(p: *mut std::ffi::c_void);
}

/// Status words as the core names them (ncx_status).
fn status_name(s: ncx_status) -> &'static str {
    match s {
        0 => "OK",
        1 => "NO_PAYLOAD",
        2 => "HEADER_UNREADABLE",
        3 => "CHUNK_MISSING",
        4 => "DIGEST_MISMATCH",
        5 => "SCHEMA_TOO_NEW",
        6 => "INVALID_JSON",
        7 => "INVALID_MODEL",
        8 => "STEP_UNPARSEABLE",
        9 => "BAD_ARGUMENT",
        _ => "INTERNAL",
    }
}

/// Takes ownership of a core string: copies it out and frees it.
unsafe fn take(p: *mut c_char, len: Option<usize>) -> Vec<u8> {
    if p.is_null() {
        return Vec::new();
    }
    let bytes = match len {
        Some(n) => std::slice::from_raw_parts(p as *const u8, n).to_vec(),
        None => CStr::from_ptr(p).to_bytes().to_vec(),
    };
    ncx_free(p as *mut _);
    bytes
}

unsafe fn take_json(p: *mut c_char) -> Value {
    let b = take(p, None);
    serde_json::from_slice(&b).unwrap_or(Value::Null)
}

fn cstring(s: &str) -> Result<CString> {
    CString::new(s).map_err(|_| anyhow!("text with a NUL byte cannot be passed to the NCX core"))
}

pub fn version() -> String {
    unsafe { CStr::from_ptr(ncx_version()).to_string_lossy().into_owned() }
}

/// The payload schema this core writes, e.g. "0.1".
pub fn schema_version() -> String {
    unsafe { CStr::from_ptr(ncx_schema_version()).to_string_lossy().into_owned() }
}

/// Outcome of reading a file's payload.
pub enum Payload {
    /// The model JSON, and the header fields the core parsed (gen, notice, noticebox, ...).
    Model { model: Value, header: Value },
    /// A plain STEP file without NCX data.
    None,
}

/// Reads the NCX payload from STEP file text.
pub fn payload_read(step: &[u8]) -> Result<Payload> {
    let mut out: *mut c_char = ptr::null_mut();
    let mut out_len = 0usize;
    let mut rep: *mut c_char = ptr::null_mut();
    let st = unsafe { ncx_payload_read(step.as_ptr() as *const c_char, step.len(), &mut out, &mut out_len, &mut rep) };
    let report = unsafe { take_json(rep) };
    let json = unsafe { take(out, Some(out_len)) };
    match st {
        0 => {
            let model: Value = serde_json::from_slice(&json)?;
            let header = report.get("fields").cloned().unwrap_or(Value::Null);
            Ok(Payload::Model { model, header })
        }
        1 => Ok(Payload::None),
        _ => Err(anyhow!(
            "the file's NCX data cannot be read ({}): {}",
            status_name(st),
            report.get("message").and_then(Value::as_str).unwrap_or("")
        )),
    }
}

/// Embeds a model into STEP text (and adds the notice body). Returns the new file text.
pub fn payload_write(step: &[u8], model: &Value, generator: &str) -> Result<Vec<u8>> {
    let model_text = serde_json::to_string(model)?;
    let options = cstring(&serde_json::json!({ "generator": generator }).to_string())?;
    let mut out: *mut c_char = ptr::null_mut();
    let mut out_len = 0usize;
    let mut rep: *mut c_char = ptr::null_mut();
    let st = unsafe {
        ncx_payload_write(
            step.as_ptr() as *const c_char,
            step.len(),
            model_text.as_ptr() as *const c_char,
            model_text.len(),
            options.as_ptr(),
            &mut out,
            &mut out_len,
            &mut rep,
        )
    };
    let report = unsafe { take_json(rep) };
    let bytes = unsafe { take(out, Some(out_len)) };
    if st != 0 {
        return Err(anyhow!(
            "the NCX data could not be written ({}): {}",
            status_name(st),
            report.get("message").and_then(Value::as_str).unwrap_or("")
        ));
    }
    Ok(bytes)
}

/// Structural validation of a model. Returns the core's report.
pub fn validate(model: &Value) -> Result<Value> {
    let text = serde_json::to_string(model)?;
    let mut rep: *mut c_char = ptr::null_mut();
    let _st = unsafe { ncx_model_validate(text.as_ptr() as *const c_char, text.len(), &mut rep) };
    Ok(unsafe { take_json(rep) })
}

/// Compares an expected fingerprint ({"bodies": [...]}) with a measured one.
/// `profile` is "native" or "exchange" (spec section 6).
pub fn fingerprint_compare(expected: &Value, actual: &Value, profile: &str) -> Result<Value> {
    // SolidCraft measures inertia from the bodies' tessellation: curved bodies' moments come out a little
    // (measured up to 1.4e-3) away from the exact ones, while volume, area and centroid are exact. Every other
    // quantity keeps the profile's tolerance.
    fingerprint_compare_opts(expected, actual, &serde_json::json!({ "profile": profile, "moment_rel": 5e-3 }))
}

pub fn fingerprint_compare_opts(expected: &Value, actual: &Value, options: &Value) -> Result<Value> {
    let e = cstring(&expected.to_string())?;
    let a = cstring(&actual.to_string())?;
    let o = cstring(&options.to_string())?;
    let mut rep: *mut c_char = ptr::null_mut();
    let st = unsafe { ncx_fingerprint_compare_ex(e.as_ptr(), a.as_ptr(), o.as_ptr(), &mut rep) };
    let report = unsafe { take_json(rep) };
    if st != 0 {
        return Err(anyhow!("fingerprint compare failed ({})", status_name(st)));
    }
    Ok(report)
}
