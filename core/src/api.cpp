// The C ABI (ncx_core.h). Thin: argument checks, JSON in/out, no exceptions across the boundary.
#include <cstdlib>
#include <cstring>
#include <string>

#include "container.hpp"
#include "fingerprint.hpp"
#include "matcher.hpp"
#include "ncx_core.h"
#include "validate.hpp"
#include "verify.hpp"
#include "version.hpp"

using nlohmann::json;

namespace {

const char* status_name(ncx_status s) {
    switch (s) {
        case NCX_OK: return "OK";
        case NCX_NO_PAYLOAD: return "NO_PAYLOAD";
        case NCX_HEADER_UNREADABLE: return "HEADER_UNREADABLE";
        case NCX_CHUNK_MISSING: return "CHUNK_MISSING";
        case NCX_DIGEST_MISMATCH: return "DIGEST_MISMATCH";
        case NCX_SCHEMA_TOO_NEW: return "SCHEMA_TOO_NEW";
        case NCX_INVALID_JSON: return "INVALID_JSON";
        case NCX_INVALID_MODEL: return "INVALID_MODEL";
        case NCX_STEP_UNPARSEABLE: return "STEP_UNPARSEABLE";
        case NCX_BAD_ARGUMENT: return "BAD_ARGUMENT";
        default: return "INTERNAL";
    }
}

char* dup(const std::string& s, size_t* len = nullptr) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    if (len) *len = s.size();
    return p;
}

void put_report(char** out, ncx_status s, json report) {
    if (!out) return;
    if (!report.is_object()) report = json::object();
    report["status"] = status_name(s);
    *out = dup(report.dump(-1, ' ', false, json::error_handler_t::replace));
}

json parse_or(const char* text, size_t len, bool* ok) {
    try {
        *ok = true;
        return json::parse(text, text + len);
    } catch (...) {
        *ok = false;
        return json();
    }
}

}  // namespace

extern "C" {

NCX_API const char* ncx_version(void) { return ncx::kVersion; }
NCX_API const char* ncx_schema_version(void) { return ncx::kSchemaVersion; }

NCX_API ncx_status ncx_payload_write(const char* step_text, size_t step_len, const char* model_json, size_t model_len,
                                     const char* options_json, char** out_step, size_t* out_step_len,
                                     char** out_report) {
    if (out_step) *out_step = nullptr;
    if (!step_text || !model_json || !out_step) {
        put_report(out_report, NCX_BAD_ARGUMENT, {{"message", "NULL argument"}});
        return NCX_BAD_ARGUMENT;
    }
    try {
        ncx::WriteOptions opt;
        if (options_json) {
            bool ok;
            json o = parse_or(options_json, std::strlen(options_json), &ok);
            if (!ok) {
                put_report(out_report, NCX_INVALID_JSON, {{"message", "options_json is not JSON"}});
                return NCX_INVALID_JSON;
            }
            opt.generator = o.value("generator", "");
            opt.chunk_chars = o.value("chunk_chars", ncx::kMaxChunkChars);
        // "notice": false (none) | "text" (the notice body's name); default: the standard notice
        if (o.contains("notice") && o["notice"].is_boolean()) opt.notice = o["notice"].get<bool>();
        if (o.contains("notice") && o["notice"].is_string()) opt.notice_text = o["notice"].get<std::string>();
        }
        auto r = ncx::write_payload(std::string(step_text, step_len), std::string(model_json, model_len), opt);
        if (r.status == NCX_OK) *out_step = dup(r.out, out_step_len);
        put_report(out_report, r.status, r.report);
        return r.status;
    } catch (const std::exception& e) {
        put_report(out_report, NCX_INTERNAL, {{"message", e.what()}});
        return NCX_INTERNAL;
    }
}

NCX_API ncx_status ncx_payload_read(const char* step_text, size_t step_len, char** out_json, size_t* out_json_len,
                                    char** out_report) {
    if (out_json) *out_json = nullptr;
    if (!step_text || !out_json) {
        put_report(out_report, NCX_BAD_ARGUMENT, {{"message", "NULL argument"}});
        return NCX_BAD_ARGUMENT;
    }
    try {
        auto r = ncx::read_payload(std::string(step_text, step_len));
        if (!r.out.empty()) *out_json = dup(r.out, out_json_len);
        put_report(out_report, r.status, r.report);
        return r.status;
    } catch (const std::exception& e) {
        put_report(out_report, NCX_INTERNAL, {{"message", e.what()}});
        return NCX_INTERNAL;
    }
}

NCX_API ncx_status ncx_model_validate(const char* model_json, size_t model_len, char** out_report) {
    if (!model_json) {
        put_report(out_report, NCX_BAD_ARGUMENT, {{"message", "NULL argument"}});
        return NCX_BAD_ARGUMENT;
    }
    try {
        bool ok;
        json m = parse_or(model_json, model_len, &ok);
        if (!ok) {
            put_report(out_report, NCX_INVALID_JSON, {{"message", "model is not JSON"}});
            return NCX_INVALID_JSON;
        }
        json rep;
        ncx_status s = ncx::validate_model(m, rep);
        put_report(out_report, s, rep);
        return s;
    } catch (const std::exception& e) {
        put_report(out_report, NCX_INTERNAL, {{"message", e.what()}});
        return NCX_INTERNAL;
    }
}

NCX_API ncx_status ncx_fingerprint_compare(const char* expected_json, const char* actual_json, char** out_report) {
    return ncx_fingerprint_compare_ex(expected_json, actual_json, nullptr, out_report);
}

NCX_API ncx_status ncx_fingerprint_compare_ex(const char* expected_json, const char* actual_json, const char* options_json,
                                              char** out_report) {
    if (!expected_json || !actual_json) {
        put_report(out_report, NCX_BAD_ARGUMENT, {{"message", "NULL argument"}});
        return NCX_BAD_ARGUMENT;
    }
    try {
        bool ok1, ok2, ok3 = true;
        json e = parse_or(expected_json, std::strlen(expected_json), &ok1);
        json a = parse_or(actual_json, std::strlen(actual_json), &ok2);
        json o = options_json ? parse_or(options_json, std::strlen(options_json), &ok3) : json::object();
        if (!ok1 || !ok2 || !ok3) {
            put_report(out_report, NCX_INVALID_JSON, {{"message", "fingerprint or options is not JSON"}});
            return NCX_INVALID_JSON;
        }
        ncx::FingerprintTolerances tol;
        try {
            tol = ncx::fingerprint_tolerances(o);
        } catch (const std::invalid_argument& ex) {
            put_report(out_report, NCX_BAD_ARGUMENT, {{"message", ex.what()}});
            return NCX_BAD_ARGUMENT;
        }
        json rep = ncx::fingerprint_compare(e, a, tol);
        rep["result"] = rep["status"];  // put_report overwrites "status" with the call status
        put_report(out_report, NCX_OK, rep);
        return NCX_OK;
    } catch (const std::exception& ex) {
        put_report(out_report, NCX_INTERNAL, {{"message", ex.what()}});
        return NCX_INTERNAL;
    }
}

NCX_API ncx_status ncx_fingerprint_hash(const char* fingerprint_json, char** out_hex) {
    if (out_hex) *out_hex = nullptr;
    if (!fingerprint_json || !out_hex) return NCX_BAD_ARGUMENT;
    bool ok;
    json f = parse_or(fingerprint_json, std::strlen(fingerprint_json), &ok);
    if (!ok) return NCX_INVALID_JSON;
    try {
        *out_hex = dup(ncx::fingerprint_hash(f));
        return NCX_OK;
    } catch (...) {
        return NCX_INTERNAL;
    }
}

NCX_API ncx_status ncx_reference_match(const char* request_json, char** out_result) {
    if (out_result) *out_result = nullptr;
    if (!request_json || !out_result) return NCX_BAD_ARGUMENT;
    bool ok;
    json req = parse_or(request_json, std::strlen(request_json), &ok);
    if (!ok) {
        put_report(out_result, NCX_INVALID_JSON, {{"message", "request is not JSON"}});
        return NCX_INVALID_JSON;
    }
    try {
        json res = ncx::match_reference(req);
        res["result"] = res["status"];
        put_report(out_result, NCX_OK, res);
        return NCX_OK;
    } catch (const std::exception& e) {
        put_report(out_result, NCX_INTERNAL, {{"message", e.what()}});
        return NCX_INTERNAL;
    }
}

NCX_API ncx_status ncx_face_compare(const char* request_json, char** out_result) {
    if (out_result) *out_result = nullptr;
    if (!request_json || !out_result) return NCX_BAD_ARGUMENT;
    bool ok;
    json req = parse_or(request_json, std::strlen(request_json), &ok);
    if (!ok) {
        put_report(out_result, NCX_INVALID_JSON, {{"message", "request is not JSON"}});
        return NCX_INVALID_JSON;
    }
    try {
        put_report(out_result, NCX_OK, ncx::face_compare(req));
        return NCX_OK;
    } catch (const std::exception& e) {
        put_report(out_result, NCX_INTERNAL, {{"message", e.what()}});
        return NCX_INTERNAL;
    }
}

NCX_API void ncx_free(void* p) { std::free(p); }

}  // extern "C"
