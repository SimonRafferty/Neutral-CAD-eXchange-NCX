/*
 * ncx_core - the shared core of the neutral CAD translator.
 *
 * Plain C ABI so the same library serves a C# add-in (P/Invoke), a C++ add-in (direct link)
 * and Python (ctypes). Everything crosses the boundary as UTF-8 bytes; structured data is JSON.
 * The core never calls a CAD API: hosts compute geometry facts and pass them in.
 *
 * Memory: every char** output is allocated by the core and must be released with ncx_free().
 * Outputs are NUL-terminated; the *_len outputs give the length without the NUL.
 * Every function returns an ncx_status; *out_report (when given) is a JSON object
 * {"status": "...", "message": "...", ...details} for both success and failure.
 */
#ifndef NCX_CORE_H
#define NCX_CORE_H

#include <stddef.h>

#ifdef _WIN32
#  ifdef NCX_CORE_BUILD
#    define NCX_API __declspec(dllexport)
#  else
#    define NCX_API __declspec(dllimport)
#  endif
#else
#  define NCX_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ncx_status {
    NCX_OK = 0,
    NCX_NO_PAYLOAD = 1,         /* the STEP file carries no NCX property */
    NCX_HEADER_UNREADABLE = 2,  /* NCX property found but its header item is malformed */
    NCX_CHUNK_MISSING = 3,      /* fewer chunk items than the header announces */
    NCX_DIGEST_MISMATCH = 4,    /* reassembled payload fails its SHA-256 or length check */
    NCX_SCHEMA_TOO_NEW = 5,     /* payload schema major version newer than this core reads */
    NCX_INVALID_JSON = 6,       /* an input that must be JSON is not */
    NCX_INVALID_MODEL = 7,      /* JSON is well formed but violates the model's structural rules */
    NCX_STEP_UNPARSEABLE = 8,   /* not an ISO 10303-21 file this core can patch */
    NCX_BAD_ARGUMENT = 9,       /* NULL pointer or similar caller error */
    NCX_INTERNAL = 99
} ncx_status;

/* Library version, e.g. "0.1.0". Static string: do not free. */
NCX_API const char* ncx_version(void);

/* Highest payload schema this core reads/writes, e.g. "0.1". Static string: do not free. */
NCX_API const char* ncx_schema_version(void);

/*
 * Embed model_json into a STEP AP214/AP242 file text (container rules: FORMAT-SPEC.md).
 * Any existing NCX payload in step_text is removed first. options_json may be NULL or e.g.
 * {"generator": "ncx-sw 0.1", "chunk_chars": 8000}.
 * out_step receives the new file text (same line-ending style as the input).
 */
NCX_API ncx_status ncx_payload_write(const char* step_text, size_t step_len,
                                     const char* model_json, size_t model_len,
                                     const char* options_json,
                                     char** out_step, size_t* out_step_len,
                                     char** out_report);

/*
 * Extract the model JSON from STEP file text. On success out_json holds the exact JSON
 * bytes that were written; out_report includes the parsed header fields.
 */
NCX_API ncx_status ncx_payload_read(const char* step_text, size_t step_len,
                                    char** out_json, size_t* out_json_len,
                                    char** out_report);

/* Structural validation of a model (schema version, ids, cross-references). */
NCX_API ncx_status ncx_model_validate(const char* model_json, size_t model_len, char** out_report);

/*
 * Compare a payload fingerprint with one the host measured on the imported B-rep.
 * Both are {"bodies": [{volume, area, centroid[3], moments[3], bbox[6], faces?, edges?}, ...]} in mm.
 * out_report: {"status": "OK", "result": "match"|"mismatch"|"count_only", "differences": [...]}.
 * ("status" is always the call status; domain outcomes are in "result".)
 */
NCX_API ncx_status ncx_fingerprint_compare(const char* expected_json, const char* actual_json,
                                           char** out_report);

/*
 * As ncx_fingerprint_compare, with options_json (may be NULL) choosing the tolerances:
 * {"profile": "native" (default: the same kernel rebuilt the model) | "exchange" (B-rep that went through
 * STEP or another kernel), and optionally "volume_rel", "area_rel", "centroid_abs" (mm), "moment_rel",
 * "bbox_abs" (mm) overriding the profile}. An unknown profile is NCX_BAD_ARGUMENT.
 */
NCX_API ncx_status ncx_fingerprint_compare_ex(const char* expected_json, const char* actual_json,
                                              const char* options_json, char** out_report);

/* Short fingerprint hash as written to the header's fp= field (16 hex digits). */
NCX_API ncx_status ncx_fingerprint_hash(const char* fingerprint_json, char** out_hex);

/*
 * Resolve one geometric reference against host-measured candidates (FORMAT-SPEC.md section 5).
 * request_json: {"ref": {descriptor: geom.type, params, samples, area|length, bbox, weak?},
 *                "tolerance"?: mm (default 0.001), "relax"?: bool (default true: retry once at 10x),
 *                "candidates": [{id, type, params, distances: [one per ref sample],
 *                                area|length, samples?: [own points], neighbors?: [ids]}]}
 * out_result: {"status": "OK", "result": "resolved"|"partial"|"ambiguous"|"unresolved",
 *              "ids": [...], "rms", "max", "size_ratio"?, "tolerance", "alternatives", "nearest"?, "notes"}
 * Only "resolved" may be used to build a feature; the other results are reported fallbacks.
 */
NCX_API ncx_status ncx_reference_match(const char* request_json, char** out_result);

/*
 * Face-by-face geometry verification (FORMAT-SPEC.md section 6.3): every reference face is matched as by
 * ncx_reference_match, then the pairing is judged as a whole.
 * request_json: {"tolerance"?: mm, "area_rel"?: (default 1e-3), "area_abs"?: mm^2 (default 0.01),
 *                "targets": [{id, area}]  (every face of the rebuilt model),
 *                "faces": [{id, ref: descriptor, candidates: [...]}]  (every reference face)}
 * out_result: {"status": "OK", "result": "match"|"topology"|"mismatch", "faces", "targets", "one_to_one",
 *              "unmatched": [{ref, result, notes}], "extra": [target ids], "split": [{ref, targets}],
 *              "merged": [{target, refs}], "area": [{refs, targets, expected, actual}], "tolerance"}
 * "topology": the same surfaces and areas, but faces cut differently (split/merged).
 */
NCX_API ncx_status ncx_face_compare(const char* request_json, char** out_result);

/* Release memory returned by any ncx_* function. NULL is ignored. */
NCX_API void ncx_free(void* p);

#ifdef __cplusplus
}
#endif

#endif /* NCX_CORE_H */
