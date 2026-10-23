// The single-file container: the NCX payload inside a STEP file (FORMAT-SPEC.md section 2).
#pragma once

#include <map>
#include <string>

#include "ncx_core.h"
#include "nlohmann/json.hpp"

namespace ncx {

constexpr const char* kContainerMarker = "NCX/1";  // header prefix; bump the digit on an incompatible layout change
constexpr size_t kMaxChunkChars = 8000;            // OCC rejects any STEP string > 16,380 chars (Stage 1)
constexpr size_t kMaxHeaderChars = 1000;           // SolidWorks 2018 drops property values > ~1,023 chars

struct WriteOptions {
    std::string generator;
    size_t chunk_chars = kMaxChunkChars;
    bool notice = true;       // add the notice body (section 2.6)
    std::string notice_text;  // its name; empty = the default text
};

struct Result {
    ncx_status status = NCX_OK;
    nlohmann::json report = nlohmann::json::object();
    std::string out;  // new STEP text (write) or model JSON (read)
};

Result write_payload(std::string step_text, const std::string& model_json, const WriteOptions& opt);
Result read_payload(std::string step_text);

// "k=v;k=v" header fields after the marker. Throws nothing; malformed pairs are skipped.
std::map<std::string, std::string> parse_header(const std::string& header);

}  // namespace ncx
