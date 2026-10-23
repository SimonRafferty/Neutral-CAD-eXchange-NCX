// Face-by-face geometry verification (FORMAT-SPEC section 6.3): pair every face of the reference B-rep with
// the face(s) of the rebuilt model that carry the same geometry, using the reference matcher per face,
// then judge the pairing as a whole (missing, extra, split, merged faces; face areas).
#pragma once

#include "nlohmann/json.hpp"

namespace ncx {

// request: {"tolerance"?: mm, "area_rel"?: relative area tolerance, "area_abs"?: mm^2,
//           "targets": [{"id", "area"}],                      every face of the rebuilt model
//           "faces": [{"id", "ref": descriptor, "candidates": [...]}]}   every reference face, measured
//           against the rebuilt model as for ncx_reference_match
nlohmann::json face_compare(const nlohmann::json& request);

}  // namespace ncx
