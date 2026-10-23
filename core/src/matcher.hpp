// Reference matching (the topological-naming solver), DESIGN.md section 4.
//
// The host has measured, for each candidate entity of the target model, the distance from each
// descriptor sample point to that entity. Because replay rebuilds the geometry in the same
// absolute frame, a correct candidate is (to modelling tolerance) at distance ~0 from the samples
// of the entity it corresponds to. Matching is therefore a small set-cover problem over the
// samples, with type/parameter agreement and size as tie-breakers.
#pragma once

#include "nlohmann/json.hpp"

namespace ncx {

struct MatchOptions {
    double tolerance = 1e-3;      // mm: a sample is covered by a candidate within this distance
    size_t max_set = 9;           // largest split searched by sample cover (no point exceeding the sample count)
    size_t max_candidates = 16;   // candidates considered for multi-entity covers (by coverage)
};

// request: {"ref": {...}, "tolerance"?: mm, "candidates": [...]}  ->  result JSON (see ncx_core.h)
nlohmann::json match_reference(const nlohmann::json& request);

}  // namespace ncx
