#include "validate.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>

#include "version.hpp"

namespace ncx {

using nlohmann::json;

namespace {

const std::set<std::string> kStatus = {"mapped", "approximate", "opaque"};

// Feature types the schema defines; others are allowed (forward compatibility) but warned about.
const std::set<std::string> kKnownFeatures = {
    "sketch", "plane", "axis", "point", "coordinateSystem", "extrude", "cut", "revolve", "revolveCut", "fillet",
    "chamfer", "hole", "linearPattern", "circularPattern", "sketchPattern", "mirror", "shell", "draft", "sweep",
    "loft", "insertPart", "combine", "split", "deleteBody", "moveBody", "thicken", "sheetMetal", "baseFlange", "edgeFlange", "flatPattern", "weldment", "structuralMember", "sweepCut", "loftCut", "mirrorBody", "weldmentTrim", "endCap",
    "loftedBend", "extrudedSurface", "revolvedSurface", "rib", "componentPattern", "hem", "breakCorner", "sketchedBend", "jog", "mirrorPart", "mirrorComponents", "cosmeticThread", "gusset", "closedCorner", "miterFlange",
    "dome", "planarSurface", "knit", "wrap", "unfold", "fold", "sketch3d", "convertToSheetMetal", "offsetFaces",
    "importedBody", "opaque"};

struct Ctx {
    json errors = json::array();
    json warnings = json::array();
    void err(const std::string& path, const std::string& msg) {
        if (errors.size() < 200) errors.push_back({{"path", path}, {"message", msg}});
    }
    void warn(const std::string& path, const std::string& msg) {
        if (warnings.size() < 200) warnings.push_back({{"path", path}, {"message", msg}});
    }
};

bool is_str(const json& j, const char* k) { return j.is_object() && j.contains(k) && j[k].is_string(); }

int major_of(const std::string& v) {
    try { return std::stoi(v.substr(0, v.find('.'))); } catch (...) { return -1; }
}

void check_sketch(const json& def, const std::string& path, Ctx& c) {
    std::set<std::string> ents;
    if (def.contains("entities") && def["entities"].is_array()) {
        for (size_t i = 0; i < def["entities"].size(); ++i) {
            const json& e = def["entities"][i];
            std::string p = path + "/entities/" + std::to_string(i);
            if (!is_str(e, "id") || !is_str(e, "type")) { c.err(p, "sketch entity needs id and type"); continue; }
            if (!ents.insert(e["id"].get<std::string>()).second) c.err(p, "duplicate sketch entity id " + e["id"].get<std::string>());
        }
    }
    for (const char* list : {"constraints", "dimensions"}) {
        if (!def.contains(list) || !def[list].is_array()) continue;
        for (size_t i = 0; i < def[list].size(); ++i) {
            const json& k = def[list][i];
            std::string p = path + "/" + list + "/" + std::to_string(i);
            if (!is_str(k, "type") && !is_str(k, "kind")) c.err(p, "needs type (constraint) or kind (dimension)");
            if (k.contains("entities") && k["entities"].is_array())
                for (const auto& r : k["entities"])
                    // a plain string is a local sketch entity id; objects are external references
                    if (r.is_string() && !ents.count(r.get<std::string>()))
                        c.err(p, "unknown sketch entity " + r.get<std::string>());
        }
    }
}

}  // namespace

ncx_status validate_model(const json& m, json& report) {
    Ctx c;
    if (!m.is_object()) {
        c.err("", "model is not a JSON object");
    } else {
        if (!is_str(m, "format") || m["format"] != "ncx") c.err("/format", "must be \"ncx\"");
        if (!is_str(m, "schema")) c.err("/schema", "missing schema version");
        else if (major_of(m["schema"]) > major_of(kSchemaVersion)) {
            report = {{"errors", json::array({{{"path", "/schema"}, {"message", "schema " + m["schema"].get<std::string>() +
                                                " is newer than supported " + kSchemaVersion}}})},
                      {"warnings", json::array()}};
            return NCX_SCHEMA_TOO_NEW;
        }
        if (m.contains("units")) {
            const json& u = m["units"];
            if (is_str(u, "length") && u["length"] != "mm") c.err("/units/length", "must be mm");
            if (is_str(u, "angle") && u["angle"] != "deg") c.err("/units/angle", "must be deg");
        }
        std::map<std::string, std::string> doc_kind;  // id -> kind
        std::map<std::string, std::set<std::string>> deps;  // doc -> docs it uses
        if (!m.contains("documents") || !m["documents"].is_array() || m["documents"].empty()) {
            c.err("/documents", "must be a non-empty array");
        } else {
            const json& docs = m["documents"];
            for (size_t i = 0; i < docs.size(); ++i) {
                const json& d = docs[i];
                std::string p = "/documents/" + std::to_string(i);
                if (!is_str(d, "id")) { c.err(p, "document needs id"); continue; }
                std::string id = d["id"];
                std::string kind = is_str(d, "kind") ? d["kind"].get<std::string>() : "";
                if (kind != "part" && kind != "assembly") c.err(p + "/kind", "must be part or assembly");
                if (!doc_kind.emplace(id, kind).second) c.err(p + "/id", "duplicate document id " + id);
            }
            for (size_t i = 0; i < docs.size(); ++i) {
                const json& d = docs[i];
                if (!is_str(d, "id")) continue;
                std::string id = d["id"], p = "/documents/" + std::to_string(i);
                std::set<std::string> fids;
                if (d.contains("features") && d["features"].is_array()) {
                    for (size_t k = 0; k < d["features"].size(); ++k) {
                        const json& f = d["features"][k];
                        std::string fp = p + "/features/" + std::to_string(k);
                        if (!is_str(f, "id") || !is_str(f, "type")) { c.err(fp, "feature needs id and type"); continue; }
                        if (!fids.insert(f["id"].get<std::string>()).second) c.err(fp + "/id", "duplicate feature id");
                        std::string type = f["type"];
                        if (!kKnownFeatures.count(type)) c.warn(fp + "/type", "unknown feature type " + type);
                        std::string st = is_str(f, "status") ? f["status"].get<std::string>() : "";
                        if (!kStatus.count(st)) c.err(fp + "/status", "must be mapped, approximate or opaque");
                        if (st == "opaque" && !is_str(f, "reason")) c.err(fp, "opaque feature needs a reason");
                        const json def = f.contains("def") ? f["def"] : json::object();
                        if (type == "sketch") check_sketch(def, fp + "/def", c);
                        if (type == "insertPart") {
                            if (!is_str(def, "source")) c.err(fp + "/def/source", "insertPart needs a source document");
                            else if (!doc_kind.count(def["source"])) c.err(fp + "/def/source", "unknown document " + def["source"].get<std::string>());
                            else if (doc_kind[def["source"]] != "part") c.err(fp + "/def/source", "source must be a part");
                            else deps[id].insert(def["source"].get<std::string>());
                        }
                    }
                }
                if (d.contains("components") && d["components"].is_array()) {
                    if (doc_kind[id] != "assembly") c.err(p + "/components", "only assemblies have components");
                    std::set<std::string> cids;
                    for (size_t k = 0; k < d["components"].size(); ++k) {
                        const json& cp = d["components"][k];
                        std::string cpth = p + "/components/" + std::to_string(k);
                        if (!is_str(cp, "id") || !is_str(cp, "document")) { c.err(cpth, "component needs id and document"); continue; }
                        if (!cids.insert(cp["id"].get<std::string>()).second) c.err(cpth + "/id", "duplicate component id");
                        if (!doc_kind.count(cp["document"])) c.err(cpth + "/document", "unknown document " + cp["document"].get<std::string>());
                        else deps[id].insert(cp["document"].get<std::string>());
                        if (cp.contains("transform") && !(cp["transform"].is_array() && cp["transform"].size() == 16))
                            c.err(cpth + "/transform", "must be 16 numbers (row-major 4x4)");
                    }
                }
            }
            if (!is_str(m, "root") || !doc_kind.count(m["root"])) c.err("/root", "must name a document");
            // cycles in the document dependency graph (derived parts, sub-assemblies)
            std::map<std::string, int> state;  // 0 new, 1 on stack, 2 done
            std::function<bool(const std::string&)> cyclic = [&](const std::string& d) {
                if (state[d] == 1) return true;
                if (state[d] == 2) return false;
                state[d] = 1;
                for (const auto& n : deps[d])
                    if (cyclic(n)) return true;
                state[d] = 2;
                return false;
            };
            for (const auto& kv : doc_kind)
                if (cyclic(kv.first)) { c.err("/documents", "document dependency cycle through " + kv.first); break; }
        }
    }
    report = {{"errors", c.errors}, {"warnings", c.warnings}};
    return c.errors.empty() ? NCX_OK : NCX_INVALID_MODEL;
}

}  // namespace ncx
