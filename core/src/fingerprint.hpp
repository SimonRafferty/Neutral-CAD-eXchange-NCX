// Geometry fingerprint: hashing for the header and tolerant comparison (DESIGN.md section 5).
#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace ncx {

// The defaults ("native") are for a model against itself rebuilt in the same kernel. B-rep that has been
// through STEP ("exchange") differs more: freeform faces re-imported by SolidWorks were measured up to
// 3.4e-5 relative in volume (lofts, sweeps; Stage 3), so exchange allows 2e-4. A real edit of 0.1 mm on a
// 100 mm part moves volume by ~1e-3, still well outside.
struct FingerprintTolerances {
    double volume_rel = 1e-5;
    double area_rel = 1e-5;
    double centroid_abs = 1e-3;    // mm
    double moment_rel = 1e-4;      // relative to the body's largest principal moment
    double bbox_abs = 0.5;         // mm; kernels' boxes are often not tight, so only a warning

    static FingerprintTolerances exchange() {
        FingerprintTolerances t;
        t.volume_rel = 2e-4;
        t.area_rel = 2e-4;
        t.centroid_abs = 0.01;
        t.moment_rel = 5e-4;
        return t;
    }
};

// Tolerances from options {"profile": "native"|"exchange", "volume_rel"?, "area_rel"?, "centroid_abs"?,
// "moment_rel"?, "bbox_abs"?}: the profile first, then any explicit value. Throws on an unknown profile.
FingerprintTolerances fingerprint_tolerances(const nlohmann::json& options);

// 16 hex digits identifying a fingerprint (informational header field fp=).
std::string fingerprint_hash(const nlohmann::json& fingerprint);

// {"status": "match"|"mismatch"|"count_only", "differences": [...], "pairs": [...]}
nlohmann::json fingerprint_compare(const nlohmann::json& expected, const nlohmann::json& actual,
                                   const FingerprintTolerances& tol = {});

}  // namespace ncx
