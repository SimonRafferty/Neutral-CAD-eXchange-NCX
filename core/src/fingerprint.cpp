#include "fingerprint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "codec.hpp"

namespace ncx {

using nlohmann::json;

namespace {

double num(const json& j, const char* key, double dflt = 0.0) {
    return j.contains(key) && j[key].is_number() ? j[key].get<double>() : dflt;
}

std::vector<double> vec(const json& j, const char* key) {
    std::vector<double> v;
    if (j.contains(key) && j[key].is_array())
        for (const auto& x : j[key]) v.push_back(x.is_number() ? x.get<double>() : 0.0);
    return v;
}

double clean(double v, double quantum) {  // round, and never print "-0"
    double r = std::round(v / quantum) * quantum;
    return std::fabs(r) < quantum / 2 ? 0.0 : r;
}

const json& bodies_of(const json& fp) {
    static const json empty = json::array();
    return fp.contains("bodies") && fp["bodies"].is_array() ? fp["bodies"] : empty;
}

}  // namespace

FingerprintTolerances fingerprint_tolerances(const json& options) {
    FingerprintTolerances t;
    if (!options.is_object()) return t;
    std::string profile = options.contains("profile") && options["profile"].is_string() ? options["profile"].get<std::string>() : "native";
    if (profile == "exchange") t = FingerprintTolerances::exchange();
    else if (profile != "native") throw std::invalid_argument("unknown tolerance profile " + profile);
    t.volume_rel = num(options, "volume_rel", t.volume_rel);
    t.area_rel = num(options, "area_rel", t.area_rel);
    t.centroid_abs = num(options, "centroid_abs", t.centroid_abs);
    t.moment_rel = num(options, "moment_rel", t.moment_rel);
    t.bbox_abs = num(options, "bbox_abs", t.bbox_abs);
    // never tighter than the analytic tolerances, whatever the overrides
    t.freeform_volume_rel = std::max(num(options, "freeform_volume_rel", t.freeform_volume_rel), t.volume_rel);
    t.freeform_area_rel = std::max(num(options, "freeform_area_rel", t.freeform_area_rel), t.area_rel);
    t.freeform_centroid_abs = std::max(num(options, "freeform_centroid_abs", t.freeform_centroid_abs), t.centroid_abs);
    t.freeform_moment_rel = std::max(num(options, "freeform_moment_rel", t.freeform_moment_rel), t.moment_rel);
    return t;
}

std::string fingerprint_hash(const json& fingerprint) {
    std::vector<std::string> lines;
    for (const auto& b : bodies_of(fingerprint)) {
        auto c = vec(b, "centroid");
        c.resize(3, 0.0);
        char buf[160];
        std::snprintf(buf, sizeof buf, "V=%.5e;A=%.5e;C=%.2f,%.2f,%.2f", num(b, "volume"), num(b, "area"),
                      clean(c[0], 0.01), clean(c[1], 0.01), clean(c[2], 0.01));
        lines.emplace_back(buf);
    }
    std::sort(lines.begin(), lines.end());
    std::string all;
    for (const auto& l : lines) all += l + "\n";
    return sha256_hex(all).substr(0, 16);
}

json fingerprint_compare(const json& expected, const json& actual, const FingerprintTolerances& tol) {
    json rep = {{"status", "match"}, {"differences", json::array()}, {"pairs", json::array()}};
    const json& E = bodies_of(expected);
    const json& A = bodies_of(actual);
    bool mismatch = false, count_only = false;
    auto diff = [&](const std::string& what, size_t i, double e, double a, double tolv, bool hard) {
        rep["differences"].push_back({{"body", i}, {"quantity", what}, {"expected", e}, {"actual", a},
                                      {"tolerance", tolv}, {"severity", hard ? "mismatch" : "warning"}});
        if (hard) mismatch = true;
    };
    if (E.size() != A.size()) {
        rep["differences"].push_back({{"quantity", "body_count"}, {"expected", E.size()}, {"actual", A.size()},
                                      {"severity", "mismatch"}});
        rep["status"] = "mismatch";
        return rep;
    }
    // pair each expected body with the closest unused actual body (volume, then centroid)
    std::vector<bool> used(A.size(), false);
    for (size_t i = 0; i < E.size(); ++i) {
        const json& e = E[i];
        auto ce = vec(e, "centroid");
        ce.resize(3, 0.0);
        double ve = num(e, "volume");
        size_t best = A.size();
        double best_cost = 0;
        for (size_t j = 0; j < A.size(); ++j) {
            if (used[j]) continue;
            auto ca = vec(A[j], "centroid");
            ca.resize(3, 0.0);
            double dv = std::fabs(num(A[j], "volume") - ve) / std::max(std::fabs(ve), 1e-9);
            double dc = std::hypot(std::hypot(ca[0] - ce[0], ca[1] - ce[1]), ca[2] - ce[2]);
            double cost = dv * 1e3 + dc;
            if (best == A.size() || cost < best_cost) { best = j; best_cost = cost; }
        }
        used[best] = true;
        rep["pairs"].push_back({i, best});
        const json& a = A[best];
        // a body of a plain document (FORMAT-SPEC 3: no history in the source, its geometry only) is paired, so the
        // others pair as they should, but not compared: it is the file's own B-rep, which two kernels measure
        // differently (a user's STEP-made turret: up to 1e-3), and nothing is rebuilt from it
        if (e.contains("plain") && e["plain"].is_boolean() && e["plain"].get<bool>()) {
            rep["plain"] = rep.value("plain", 0) + 1;
            continue;
        }
        auto is_freeform = [](const json& b) { return b.contains("freeform") && b["freeform"].is_boolean() && b["freeform"].get<bool>(); };
        const bool ff = is_freeform(e) || is_freeform(a);
        const double vol_rel = ff ? tol.freeform_volume_rel : tol.volume_rel;
        const double area_rel = ff ? tol.freeform_area_rel : tol.area_rel;
        const double cen_abs = ff ? tol.freeform_centroid_abs : tol.centroid_abs;
        const double mom_rel = ff ? tol.freeform_moment_rel : tol.moment_rel;

        double va = num(a, "volume");
        if (std::fabs(va - ve) > vol_rel * std::max(std::fabs(ve), 1e-9)) diff("volume", i, ve, va, vol_rel, true);
        double ae = num(e, "area"), aa = num(a, "area");
        if (std::fabs(aa - ae) > area_rel * std::max(std::fabs(ae), 1e-9)) diff("area", i, ae, aa, area_rel, true);
        auto ca = vec(a, "centroid");
        ca.resize(3, 0.0);
        for (int k = 0; k < 3; ++k)
            if (std::fabs(ca[k] - ce[k]) > cen_abs)
                diff(std::string("centroid.") + "xyz"[k], i, ce[k], ca[k], cen_abs, true);
        auto me = vec(e, "moments"), ma = vec(a, "moments");
        if (me.size() == 3 && ma.size() == 3) {
            std::sort(me.begin(), me.end());
            std::sort(ma.begin(), ma.end());
            double scale = std::max({std::fabs(me[0]), std::fabs(me[1]), std::fabs(me[2]), 1e-12});
            for (int k = 0; k < 3; ++k)
                if (std::fabs(ma[k] - me[k]) > mom_rel * scale)
                    diff("moment." + std::to_string(k), i, me[k], ma[k], mom_rel, true);
        }
        auto be = vec(e, "bbox"), ba = vec(a, "bbox");
        if (be.size() == 6 && ba.size() == 6)
            for (int k = 0; k < 6; ++k)
                if (std::fabs(ba[k] - be[k]) > tol.bbox_abs) diff("bbox." + std::to_string(k), i, be[k], ba[k], tol.bbox_abs, false);
        for (const char* key : {"faces", "edges"})
            if (e.contains(key) && a.contains(key) && e[key] != a[key]) {
                rep["differences"].push_back({{"body", i}, {"quantity", key}, {"expected", e[key]}, {"actual", a[key]},
                                              {"severity", "info"}});
                count_only = true;
            }
    }
    rep["status"] = mismatch ? "mismatch" : (count_only ? "count_only" : "match");
    return rep;
}

}  // namespace ncx
