#include "verify.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "matcher.hpp"

namespace ncx {

using nlohmann::json;

namespace {

double num(const json& j, const char* k, double dflt) {
    return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : dflt;
}

// areas agree within the relative tolerance, or within the absolute one for tiny faces
bool area_ok(double expected, double actual, double rel, double abs_tol) {
    double d = std::fabs(expected - actual);
    return d <= abs_tol || d <= rel * std::max(std::fabs(expected), std::fabs(actual));
}

}  // namespace

json face_compare(const json& req) {
    const double tol = num(req, "tolerance", 1e-3);
    // a face that went through STEP keeps its area to ~1e-5 relative (Stage 3), so 1e-3 leaves room for
    // freeform drift; the absolute floor (mm^2) keeps tiny faces from failing on rounding alone
    const double rel = num(req, "area_rel", 1e-3);
    const double abs_tol = num(req, "area_abs", 1e-2);

    std::map<std::string, double> target_area;
    std::vector<std::string> target_order;
    for (const auto& t : req.value("targets", json::array())) {
        std::string id = t.value("id", "");
        target_area[id] = num(t, "area", -1);
        target_order.push_back(id);
    }

    json unmatched = json::array(), split = json::array(), area = json::array();
    std::map<std::string, std::vector<std::string>> claimed;   // target face -> reference faces covering it
    std::map<std::string, std::vector<std::string>> claims;    // reference face -> target faces
    size_t one_to_one = 0, refs = 0;

    for (const auto& f : req.value("faces", json::array())) {
        ++refs;
        std::string id = f.value("id", "");
        json mreq = {{"ref", f.value("ref", json::object())},
                     {"tolerance", tol},
                     {"candidates", f.value("candidates", json::array())}};
        json res = match_reference(mreq);
        if (res.value("status", "") != "resolved") {
            unmatched.push_back({{"ref", id}, {"result", res.value("status", "")}, {"notes", res.value("notes", json::array())}});
            continue;
        }
        std::vector<std::string> ids;
        for (const auto& x : res["ids"]) ids.push_back(x.get<std::string>());
        claims[id] = ids;
        for (const auto& t : ids) claimed[t].push_back(id);
        if (ids.size() > 1) split.push_back({{"ref", id}, {"targets", ids}});
    }

    // merged: one rebuilt face carries the geometry of several reference faces
    json merged = json::array();
    for (const auto& [t, rs] : claimed)
        if (rs.size() > 1) merged.push_back({{"target", t}, {"refs", rs}});

    // areas, per connected group of reference and rebuilt faces (a split or merge is compared as a whole)
    std::set<std::string> done;
    for (const auto& [rid, tids] : claims) {
        if (done.count(rid)) continue;
        std::set<std::string> rset{rid}, tset;
        std::vector<std::string> queue{rid};
        while (!queue.empty()) {
            std::string r = queue.back();
            queue.pop_back();
            for (const auto& t : claims[r]) {
                if (!tset.insert(t).second) continue;
                for (const auto& r2 : claimed[t])
                    if (rset.insert(r2).second) queue.push_back(r2);
            }
        }
        double ea = 0, aa = 0;
        bool known = true;
        for (const auto& f : req["faces"]) {
            std::string id = f.value("id", "");
            if (!rset.count(id)) continue;
            double a = num(f["ref"].value("geom", json::object()), "area", -1);
            if (a < 0) known = false;
            ea += a;
        }
        for (const auto& t : tset) {
            auto it = target_area.find(t);
            if (it == target_area.end() || it->second < 0) known = false;
            else aa += it->second;
        }
        done.insert(rset.begin(), rset.end());
        if (rset.size() == 1 && tset.size() == 1) ++one_to_one;
        if (known && !area_ok(ea, aa, rel, abs_tol))
            area.push_back({{"refs", std::vector<std::string>(rset.begin(), rset.end())},
                            {"targets", std::vector<std::string>(tset.begin(), tset.end())},
                            {"expected", ea}, {"actual", aa}});
    }

    json extra = json::array();
    for (const auto& t : target_order)
        if (!claimed.count(t)) extra.push_back(t);

    // match: the same faces, one for one, with the same areas. topology: the same surfaces and areas, but
    // the faces are cut differently (e.g. a periodic face split in two by one kernel). Anything else differs.
    std::string result = "match";
    if (!unmatched.empty() || !extra.empty() || !area.empty()) result = "mismatch";
    else if (!split.empty() || !merged.empty()) result = "topology";

    return {{"result", result},  {"faces", refs},  {"targets", target_order.size()},
            {"one_to_one", one_to_one}, {"unmatched", unmatched}, {"extra", extra},
            {"split", split},    {"merged", merged}, {"area", area},
            {"tolerance", tol}};
}

}  // namespace ncx
