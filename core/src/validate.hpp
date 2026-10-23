// Structural validation of a payload model: the rules a JSON Schema can't express
// (unique ids, cross-references, cycles) plus the version gate.
#pragma once

#include "ncx_core.h"
#include "nlohmann/json.hpp"

namespace ncx {

// Fills report with {"errors": [{path, message}], "warnings": [...]}; returns NCX_OK,
// NCX_INVALID_MODEL or NCX_SCHEMA_TOO_NEW.
ncx_status validate_model(const nlohmann::json& model, nlohmann::json& report);

}  // namespace ncx
