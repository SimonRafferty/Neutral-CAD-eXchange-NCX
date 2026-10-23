#include "matcher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ncx {

using nlohmann::json;

namespace {

using V3 = std::array<double, 3>;

enum class Compat { Exact, Compatible, Incompatible };

// Surface/curve types that may legitimately represent each other across kernels
// (an analytic face exported or rebuilt as a B-spline, and vice versa).
Compat compat(const std::string& a, const std::string& b) {
    if (a == b) return Compat::Exact;
    static const std::set<std::string> freeform = {"bspline", "nurbs", "spline", "bezier", "offset", "other",
                                                   "surfaceOfRevolution", "surfaceOfExtrusion"};
    if (freeform.count(a) || freeform.count(b)) return Compat::Compatible;
    if ((a == "circle" && b == "ellipse") || (a == "ellipse" && b == "circle")) return Compat::Compatible;
    return Compat::Incompatible;
}

double num(const json& j, const char* k, double dflt = -1) {
    return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : dflt;
}

std::optional<V3> v3(const json& j, const char* k) {
    if (!j.is_object() || !j.contains(k) || !j[k].is_array() || j[k].size() != 3) return std::nullopt;
    V3 v;
    for (int i = 0; i < 3; ++i) {
        if (!j[k][i].is_number()) return std::nullopt;
        v[i] = j[k][i].get<double>();
    }
    return v;
}

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const V3& a) { return std::sqrt(dot(a, a)); }
V3 unit(const V3& a) {
    double n = norm(a);
    return n > 1e-300 ? V3{a[0] / n, a[1] / n, a[2] / n} : a;
}
// axial and radial components of p relative to a line (origin o, unit direction u)
void axial(const V3& p, const V3& o, const V3& u, double& h, double& rho) {
    V3 v = sub(p, o);
    h = dot(v, u);
    rho = std::sqrt(std::max(0.0, dot(v, v) - h * h));
}

// Distance from a point to the reference's own (unbounded) analytic geometry, if the reference
// type is analytic and its parameters are present. This is what lets the core check, without any
// kernel, that a target entity really lies on the source entity (the backward check).
std::optional<double> analytic_distance(const std::string& type, const json& p, const V3& x) {
    if (type == "plane") {
        auto n = v3(p, "normal"), o = v3(p, "point");
        if (n && o) return std::fabs(dot(sub(x, *o), unit(*n)));
    } else if (type == "cylinder") {
        auto u = v3(p, "axis"), o = v3(p, "origin");
        double r = num(p, "radius");
        if (u && o && r > 0) {
            double h, rho;
            axial(x, *o, unit(*u), h, rho);
            return std::fabs(rho - r);
        }
    } else if (type == "cone") {
        auto u = v3(p, "axis"), a = v3(p, "apex");
        double al = num(p, "semiAngle");
        if (u && a && al >= 0) {
            double h, rho;
            axial(x, *a, unit(*u), h, rho);
            // distance to either nappe of the double cone in the (h, rho) half plane
            return std::min(std::fabs(rho * std::cos(al) - h * std::sin(al)),
                            std::fabs(rho * std::cos(al) + h * std::sin(al)));
        }
    } else if (type == "sphere") {
        auto c = v3(p, "center");
        double r = num(p, "radius");
        if (c && r > 0) return std::fabs(norm(sub(x, *c)) - r);
    } else if (type == "torus") {
        auto u = v3(p, "axis"), c = v3(p, "center");
        double R = num(p, "majorRadius"), r = num(p, "minorRadius");
        if (u && c && R > 0 && r > 0) {
            double h, rho;
            axial(x, *c, unit(*u), h, rho);
            return std::fabs(std::hypot(rho - R, h) - r);
        }
    } else if (type == "line") {
        auto d = v3(p, "direction"), o = v3(p, "point");
        if (d && o) {
            double h, rho;
            axial(x, *o, unit(*d), h, rho);
            return rho;
        }
    } else if (type == "circle") {
        auto u = v3(p, "axis"), c = v3(p, "center");
        double r = num(p, "radius");
        if (u && c && r > 0) {
            double h, rho;
            axial(x, *c, unit(*u), h, rho);
            return std::hypot(rho - r, h);
        }
    }
    return std::nullopt;
}

struct Cand {
    std::string id;
    std::string type;
    std::vector<double> d;     // distance per reference sample (mm)
    std::vector<bool> cover;   // d <= tol
    size_t covered = 0;
    Compat compat = Compat::Incompatible;
    double size = -1;          // length or area if given
    double param_dev = 0;      // parameter disagreement (dimensionless, 0 = identical)
    int on_ref = -1;           // backward check: 1 lies on the reference geometry, 0 does not, -1 unknown
    std::vector<std::string> neighbors;
};

// 1 - |cos| between two directions (axes and normals may be reported with either sense)
double dir_dev(const std::optional<V3>& a, const std::optional<V3>& b) {
    if (!a || !b || norm(*a) < 1e-12 || norm(*b) < 1e-12) return 0;
    return 1.0 - std::fabs(dot(unit(*a), unit(*b)));
}

double param_deviation(const json& rp, const json& cp) {
    double dev = 0;
    for (const char* k : {"radius", "majorRadius", "minorRadius", "semiAngle"}) {
        double a = num(rp, k), b = num(cp, k);
        if (a > 0 && b > 0) dev += std::fabs(a - b) / std::max(a, b);
    }
    for (const char* k : {"axis", "normal", "direction"}) dev += dir_dev(v3(rp, k), v3(cp, k));
    return dev;
}

// per-sample min distance over a set of candidates
void set_stats(const std::vector<const Cand*>& set, size_t m, double& rms, double& mx) {
    double s = 0;
    mx = 0;
    for (size_t i = 0; i < m; ++i) {
        double best = 1e300;
        for (const Cand* c : set) best = std::min(best, c->d[i]);
        s += best * best;
        mx = std::max(mx, best);
    }
    rms = m ? std::sqrt(s / m) : 0;
}

json describe(const std::vector<const Cand*>& set, size_t m) {
    double rms, mx;
    set_stats(set, m, rms, mx);
    json ids = json::array();
    for (const Cand* c : set) ids.push_back(c->id);
    return {{"ids", ids}, {"rms", rms}, {"max", mx}};
}

// smallest sets of usable candidates that together cover every sample
std::vector<std::vector<const Cand*>> minimal_covers(std::vector<const Cand*> pool, size_t m, const MatchOptions& opt) {
    std::sort(pool.begin(), pool.end(), [](const Cand* a, const Cand* b) { return a->covered > b->covered; });
    if (pool.size() > opt.max_candidates) pool.resize(opt.max_candidates);
    std::vector<std::vector<const Cand*>> covers;
    for (size_t k = 2; k <= std::min({opt.max_set, pool.size(), m}) && covers.empty(); ++k) {
        std::vector<size_t> idx(k);
        for (size_t i = 0; i < k; ++i) idx[i] = i;
        for (;;) {
            std::vector<bool> cov(m, false);
            for (size_t i : idx)
                for (size_t s = 0; s < m; ++s) cov[s] = cov[s] || pool[i]->cover[s];
            if (std::all_of(cov.begin(), cov.end(), [](bool b) { return b; })) {
                std::vector<const Cand*> set;
                for (size_t i : idx) set.push_back(pool[i]);
                covers.push_back(set);
            }
            size_t i = k;
            while (i > 0 && idx[i - 1] == pool.size() - k + i - 1) --i;
            if (i == 0) break;
            ++idx[i - 1];
            for (size_t j = i; j < k; ++j) idx[j] = idx[j - 1] + 1;
        }
    }
    return covers;
}

}  // namespace

namespace {

json match_once(const json& req, double tolerance) {
    json out = {{"status", "unresolved"}, {"ids", json::array()}, {"alternatives", json::array()},
                {"notes", json::array()}};
    MatchOptions opt;
    opt.tolerance = tolerance;
    const json ref = req.value("ref", json::object());
    const json geom = ref.value("geom", json::object());
    const std::string rtype = geom.value("type", "");
    const json rparams = geom.value("params", json::object());
    size_t m = geom.contains("samples") && geom["samples"].is_array() ? geom["samples"].size() : 0;
    if (m == 0) {
        out["notes"].push_back("reference has no sample points");
        return out;
    }
    double rsize = num(geom, "length", num(geom, "area"));
    // A weak descriptor's samples sit (almost) on the entity's boundary, so they also touch its
    // neighbours: only an unambiguous single exact-type cover is trusted, never a split.
    const bool weak = geom.value("weak", false);
    if (weak) out["notes"].push_back("weak descriptor: samples have no clearance from the entity boundary");
    // The reference's own extent bounds the backward check: the analytic geometry is unbounded (a whole
    // circle, an infinite plane), so without it a neighbouring arc or coplanar face would pass.
    std::optional<std::array<double, 6>> rbox;
    if (geom.contains("bbox") && geom["bbox"].is_array() && geom["bbox"].size() == 6) {
        std::array<double, 6> b;
        for (int i = 0; i < 6; ++i) b[i] = geom["bbox"][i].is_number() ? geom["bbox"][i].get<double>() : 0.0;
        rbox = b;
    }
    auto inside_box = [&](const V3& x) {
        if (!rbox) return true;
        const auto& b = *rbox;
        double t = opt.tolerance;
        return x[0] >= b[0] - t && x[1] >= b[1] - t && x[2] >= b[2] - t && x[0] <= b[3] + t && x[1] <= b[4] + t &&
               x[2] <= b[5] + t;
    };

    std::vector<Cand> cands;
    for (const auto& c : req.value("candidates", json::array())) {
        Cand k;
        k.id = c.value("id", "");
        k.type = c.value("type", "");
        if (!c.contains("distances") || !c["distances"].is_array() || c["distances"].size() != m) {
            out["notes"].push_back("candidate " + k.id + " ignored: needs " + std::to_string(m) + " distances");
            continue;
        }
        for (const auto& x : c["distances"]) k.d.push_back(x.is_number() ? std::fabs(x.get<double>()) : 1e300);
        k.cover.resize(m);
        for (size_t i = 0; i < m; ++i)
            if ((k.cover[i] = k.d[i] <= opt.tolerance)) ++k.covered;
        k.compat = compat(rtype, k.type);
        k.size = num(c, "length", num(c, "area"));
        k.param_dev = param_deviation(rparams, c.value("params", json::object()));
        if (c.contains("samples") && c["samples"].is_array() && !c["samples"].empty()) {
            bool all = true, known = true;
            for (const auto& s : c["samples"]) {
                auto x = v3(json{{"p", s}}, "p");
                auto dd = x ? analytic_distance(rtype, rparams, *x) : std::nullopt;
                if (!dd) { known = false; break; }
                if (*dd > opt.tolerance || !inside_box(*x)) all = false;
            }
            if (known) k.on_ref = all ? 1 : 0;
        }
        if (c.contains("neighbors") && c["neighbors"].is_array())
            for (const auto& n : c["neighbors"])
                if (n.is_string()) k.neighbors.push_back(n.get<std::string>());
        cands.push_back(std::move(k));
    }
    std::map<std::string, const Cand*> by_id;
    for (const auto& k : cands) by_id[k.id] = &k;

    // usable: covers at least one sample, has a compatible type, and (when checkable) lies on the reference
    std::vector<const Cand*> usable;
    for (const auto& k : cands) {
        if (!k.covered) continue;
        if (k.compat == Compat::Incompatible) {
            if (k.covered == m)
                out["notes"].push_back("candidate " + k.id + " (" + k.type + ") covers every sample but its type is incompatible with " + rtype);
            continue;
        }
        if (k.on_ref == 0) continue;  // touches the samples but leaves the reference surface/curve
        usable.push_back(&k);
    }

    std::vector<const Cand*> chosen;
    std::vector<const Cand*> full;
    for (const Cand* k : usable)
        if (k->covered == m) full.push_back(k);
    auto rank = [&](const Cand* a, const Cand* b) {  // better first: exact type, params, then distance
        if ((a->compat == Compat::Exact) != (b->compat == Compat::Exact)) return a->compat == Compat::Exact;
        if (std::fabs(a->param_dev - b->param_dev) > 1e-6) return a->param_dev < b->param_dev;
        double ra, ma, rb, mb;
        set_stats({a}, m, ra, ma);
        set_stats({b}, m, rb, mb);
        return ra < rb;
    };
    if (!full.empty()) {
        // 1) single candidates covering every sample
        std::sort(full.begin(), full.end(), rank);
        if (weak && (full.size() > 1 || full[0]->compat != Compat::Exact)) {
            out["status"] = "ambiguous";
            out["notes"].push_back("weak descriptor covered by " + std::to_string(full.size()) + " candidate(s); not trusted");
        } else if (full.size() == 1 ||
            (full[0]->compat == Compat::Exact && full[1]->compat != Compat::Exact) ||
            (full[1]->param_dev - full[0]->param_dev > 1e-3)) {
            chosen = {full[0]};
            if (full.size() > 1) out["notes"].push_back("tie broken by type/parameter agreement");
        } else {
            out["status"] = "ambiguous";
            out["notes"].push_back(std::to_string(full.size()) + " candidates each cover every sample");
        }
        for (size_t i = chosen.empty() ? 0 : 1; i < full.size() && out["alternatives"].size() < 3; ++i)
            out["alternatives"].push_back(describe({full[i]}, m));
    } else if (!weak && [&] {
                   // 2a) with backward checks: every usable candidate of one type that lies on the reference
                   // geometry (inside its extent) and touches a sample is provably a piece of the split.
                   // Prefer the reference's exact type; otherwise the type covering most samples.
                   std::map<std::string, std::vector<const Cand*>> by_type;
                   for (const Cand* k : usable)
                       if (k->on_ref == 1) by_type[k->type].push_back(k);
                   std::vector<std::string> order;
                   if (by_type.count(rtype)) order.push_back(rtype);
                   for (const auto& kv : by_type)
                       if (kv.first != rtype) order.push_back(kv.first);
                   for (const auto& t : order) {
                       const auto& group = by_type[t];
                       std::vector<bool> cov(m, false);
                       for (const Cand* k : group)
                           for (size_t s = 0; s < m; ++s) cov[s] = cov[s] || k->cover[s];
                       if (group.size() > 1 && std::all_of(cov.begin(), cov.end(), [](bool b) { return b; })) {
                           chosen = group;
                           return true;
                       }
                   }
                   return false;
               }()) {
        out["notes"].push_back("split: one source entity matches " + std::to_string(chosen.size()) +
                               " target entities (backward-checked)");
    } else {
        // 2b) a split entity: the union of the smallest candidate sets that together cover every sample.
        // Several minimal sets normally differ only by a piece whose end touches a sample (a sample
        // on the junction of two pieces); every such piece belongs to the split, so take the union -
        // provided the pieces agree on type parameters (radius, axis, normal).
        auto covers = weak ? decltype(minimal_covers(usable, m, opt)){} : minimal_covers(usable, m, opt);
        if (weak) {
            out["status"] = "ambiguous";
            out["notes"].push_back("weak descriptor needs a split; not trusted");
        } else if (!covers.empty()) {
            std::set<const Cand*> uni;
            for (const auto& c : covers) uni.insert(c.begin(), c.end());
            // pieces of one split share the type and its parameters (radius, axis, normal)
            const std::string& t0 = (*uni.begin())->type;
            bool consistent = std::all_of(uni.begin(), uni.end(), [&](const Cand* c) {
                return c->param_dev <= 1e-3 && c->type == t0;
            });
            if (consistent) {
                chosen.assign(uni.begin(), uni.end());
                out["notes"].push_back("split: one source entity matches " + std::to_string(chosen.size()) + " target entities" +
                                       (covers.size() > 1 ? " (union of " + std::to_string(covers.size()) + " minimal covers)" : ""));
            } else {
                out["status"] = "ambiguous";
                out["notes"].push_back(std::to_string(covers.size()) + " different minimal sets cover every sample");
                for (size_t i = 0; i < covers.size() && i < 3; ++i) out["alternatives"].push_back(describe(covers[i], m));
            }
        }
    }

    if (!chosen.empty()) {
        // 3) completion: pieces of a split that no sample happened to fall on. Walk neighbours of the
        // matched set, adding compatible pieces that lie on the reference geometry (backward check),
        // while the matched size is short of the reference size.
        double tot = 0;
        bool sizes = rsize > 0;
        for (const Cand* c : chosen) {
            if (c->size < 0) sizes = false;
            tot += c->size;
        }
        if (sizes && tot < 0.99 * rsize) {
            std::set<const Cand*> in(chosen.begin(), chosen.end());
            std::deque<const Cand*> q(chosen.begin(), chosen.end());
            size_t added = 0;
            while (!q.empty() && tot < 0.99 * rsize) {
                const Cand* c = q.front();
                q.pop_front();
                for (const auto& nid : c->neighbors) {
                    auto it = by_id.find(nid);
                    if (it == by_id.end() || in.count(it->second)) continue;
                    const Cand* n = it->second;
                    if (n->on_ref != 1 || n->compat == Compat::Incompatible || n->size < 0 ||
                        n->type != chosen.front()->type)
                        continue;
                    if (tot + n->size > 1.01 * rsize) continue;  // would overshoot: not part of this entity
                    in.insert(n);
                    q.push_back(n);
                    chosen.push_back(n);
                    tot += n->size;
                    ++added;
                }
            }
            if (added) out["notes"].push_back("completed split with " + std::to_string(added) + " connected piece(s) lying on the reference geometry");
        }
        json d = describe(chosen, m);
        out["status"] = "resolved";
        out["ids"] = d["ids"];
        out["rms"] = d["rms"];
        out["max"] = d["max"];
        if (sizes) {
            out["size_ratio"] = tot / rsize;
            if (tot < 0.99 * rsize || tot > 1.01 * rsize)
                out["notes"].push_back("matched size differs from the reference by " +
                                       std::to_string(static_cast<int>(std::round((tot / rsize - 1) * 100))) + " %");
            // Short of the reference: some pieces of a split were not found. Using the result would
            // apply a feature to part of the entity, so it is reported as partial, never as resolved.
            // (Larger is accepted: the target kernel merged the entity with a smooth neighbour.)
            if (tot < 0.99 * rsize) out["status"] = "partial";
        }
    } else if (out["status"] == "unresolved") {
        // 4) nothing covers it: report the nearest miss so the failure can be explained
        const Cand* best = nullptr;
        double best_rms = 0;
        for (const auto& k : cands) {
            double r, mx;
            set_stats({&k}, m, r, mx);
            if (!best || k.covered > best->covered || (k.covered == best->covered && r < best_rms)) {
                best = &k;
                best_rms = r;
            }
        }
        if (best) {
            json d = describe({best}, m);
            d["covered"] = best->covered;
            d["type"] = best->type;
            out["nearest"] = d;
        }
        out["notes"].push_back("no candidate set covers every sample within " + std::to_string(opt.tolerance) + " mm");
    }
    return out;
}

}  // namespace

json match_reference(const json& req) {
    // Strict first, so near-coincident small features are told apart; only if nothing resolves,
    // retry once at 10x. Stage 2 measured SolidWorks-vs-OCC drift up to ~6e-4 mm, and zero wrong
    // matches at 1e-2 mm across the corpus (results/stage2/FINDINGS.md).
    MatchOptions opt;
    double tol = req.contains("tolerance") && req["tolerance"].is_number() ? req["tolerance"].get<double>() : opt.tolerance;
    bool relax = req.value("relax", true);
    json out = match_once(req, tol);
    out["tolerance"] = tol;
    if (relax && out["status"] == "unresolved") {
        json again = match_once(req, tol * 10);
        if (again["status"] != "unresolved") {
            again["notes"].push_back("resolved only at the relaxed tolerance " + std::to_string(tol * 10) + " mm");
            again["tolerance"] = tol * 10;
            return again;
        }
    }
    return out;
}

}  // namespace ncx
