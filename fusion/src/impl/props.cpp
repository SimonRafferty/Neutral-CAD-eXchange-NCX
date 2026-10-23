// Replay: custom properties, materials and appearances (FORMAT-SPEC 4).
//
//  - Custom properties: Fusion components have a part number and a description, and no other custom
//    properties. Those two are set from the properties of the same name; every property (with its typed
//    value and the value it resolved to) is also kept as an attribute of the component in the group
//    "NCX.properties", so nothing is lost and an exporter can write it back.
//  - Materials: looked up by name in Fusion's material libraries (exact name first, then a library material whose
//    name holds every word of the source's name that contains a digit, e.g. "6061"); the active configuration's.
//  - Appearances: plain colours become design appearances of that colour; library appearances are looked up by
//    the words of their file name, else skipped with a note. Applied document -> body -> feature -> face.
#include "host.h"
#include "replay.h"

#include <cctype>
#include <sstream>

namespace ncxf {

namespace {

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> words(const std::string& s)
{
    std::vector<std::string> w;
    std::string cur;
    for (char c : s)
    {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!cur.empty())
        {
            w.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

bool containsAll(const std::string& name, const std::vector<std::string>& ws)
{
    std::string n = lower(name);
    for (auto& w : ws)
        if (n.find(w) == std::string::npos) return false;
    return !ws.empty();
}

Ptr<Material> findMaterial(const std::string& name, std::string& how)
{
    Ptr<MaterialLibraries> libs = Application::get()->materialLibraries();
    for (size_t i = 0; i < libs->count(); i++)
        if (Ptr<Material> m = libs->item(static_cast<int>(i))->materials()->itemByName(name))
        {
            how = "exact";
            return m;
        }
    std::vector<std::string> key;
    for (auto& w : words(name))
        if (std::any_of(w.begin(), w.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) key.push_back(w);
    if (key.empty()) return nullptr;
    for (size_t i = 0; i < libs->count(); i++)
    {
        Ptr<Materials> ms = libs->item(static_cast<int>(i))->materials();
        for (size_t k = 0; k < ms->count(); k++)
            if (containsAll(ms->item(k)->name(), key))
            {
                how = "by the words " + key[0];
                return ms->item(k);
            }
    }
    return nullptr;
}

Ptr<Appearance> findLibraryAppearance(const std::string& file)
{
    // ".../brushed aluminum.p2m" -> words of the file name
    std::string base = file.substr(file.find_last_of("/\\") + 1);
    base = base.substr(0, base.find_last_of('.'));
    std::vector<std::string> ws = words(base);
    // Fusion names say "Aluminum - Brushed": the same words in any order; "aluminium" spelled either way
    for (auto& w : ws)
        if (w == "aluminium") w = "aluminum";
    Ptr<MaterialLibraries> libs = Application::get()->materialLibraries();
    for (size_t i = 0; i < libs->count(); i++)
    {
        Ptr<Appearances> as = libs->item(static_cast<int>(i))->appearances();
        for (size_t k = 0; as && k < as->count(); k++)
            if (containsAll(as->item(static_cast<int>(k))->name(), ws)) return as->item(static_cast<int>(k));
    }
    return nullptr;
}

std::string fmtColor(const json& c)
{
    std::ostringstream o;
    o << "NCX colour " << static_cast<int>(c[0].get<double>() * 255 + 0.5) << "," << static_cast<int>(c[1].get<double>() * 255 + 0.5) << ","
      << static_cast<int>(c[2].get<double>() * 255 + 0.5);
    return o.str();
}

} // namespace

void Replayer::replayProperties(const json& doc)
{
    std::string active;
    for (auto& c : doc.value("configurations", json::array()))
        if (c.value("id", "") == doc.value("activeConfiguration", "")) active = c.value("name", "");
    // ------------------------------------------------------------------ custom properties
    const json props = doc.value("customProperties", json::array());
    int kept = 0;
    for (auto& p : props)
    {
        std::string name = p.value("name", ""), cfg = p.value("configuration", "");
        if (!cfg.empty() && cfg != active) continue;   // another configuration's value
        std::string value = p.value("resolved", p.value("value", ""));
        std::string ln = lower(name);
        if (ln == "partno" || ln == "part number" || ln == "partnumber") comp_->partNumber(value);
        if (ln == "description") comp_->description(value);
        json a{{"name", name}, {"type", p.value("type", "text")}, {"value", p.value("value", "")}, {"resolved", p.value("resolved", "")}};
        if (!cfg.empty()) a["configuration"] = cfg;
        if (comp_->attributes()->add("NCX.properties", name, a.dump())) kept++;
    }
    if (!props.empty())
        report_.add(docName_, "custom properties", "approximate",
                    std::to_string(kept) + " kept as attributes of the component (group NCX.properties); part number and description set; "
                    "Fusion has no other custom properties");
    // ------------------------------------------------------------------ material
    for (auto& m : doc.value("materials", json::array()))
    {
        std::string cfg = m.value("configuration", "");
        if (!cfg.empty() && cfg != active) continue;
        std::string how;
        Ptr<Material> mat = findMaterial(m.value("name", ""), how);
        if (!mat)
        {
            report_.add(docName_, "material " + m.value("name", ""), "approximate", "not found in Fusion's material libraries; the part keeps its default material");
            continue;
        }
        comp_->material(mat);
        for (auto& b : bodies()) b->material(mat);
        if (how == "exact") report_.add(docName_, "material " + m.value("name", ""), "rebuilt", "");
        else report_.add(docName_, "material " + m.value("name", ""), "approximate", "matched to Fusion's \"" + mat->name() + "\" " + how);
    }
    // ------------------------------------------------------------------ cut lists (weldments)
    // Fusion has no cut lists: each entry's name and properties are kept as attributes of its bodies
    // (group NCX.cutList), so a BOM script or an exporter can read them back.
    for (auto& cl : doc.value("cutLists", json::array()))
    {
        int tagged = 0;
        for (auto& br : cl.value("bodies", json::array()))
        {
            std::vector<Ptr<Base>> hit;
            resolveRef(bodies(), br, hit);
            for (auto& h : hit)
            {
                Ptr<BRepBody> b = h;
                if (!b) continue;
                b->attributes()->add("NCX.cutList", "name", cl.value("name", ""));
                b->attributes()->add("NCX.cutList", "properties", cl.value("properties", json::array()).dump());
                tagged++;
            }
        }
        report_.add(docName_, "cut list " + cl.value("name", ""), "approximate",
                    std::to_string(tagged) + " body(ies) tagged with its name and properties as attributes (Fusion has no cut lists)");
    }
    // ------------------------------------------------------------------ appearances
    const json apps = doc.value("appearances", json::array());
    if (apps.empty()) return;
    Ptr<Appearances> designApps = design_->appearances();
    // document first, then bodies, features, faces (the more specific wins, as in the source)
    for (const char* stage : {"document", "body", "feature", "face"})
        for (auto& a : apps)
        {
            const json& t = a["target"];
            std::string kind = t.value("kind", "");
            if (kind != stage) continue;
            std::string item = "appearance on " + kind;
            Ptr<Appearance> app;
            std::string note;
            if (a.value("kind", "") == "appearance" && a.contains("file"))
            {
                Ptr<Appearance> lib = findLibraryAppearance(a["file"].get<std::string>());
                if (!lib)
                {
                    report_.add(docName_, item, "approximate", "library appearance " + a["file"].get<std::string>() + " not found in Fusion's library: skipped");
                    continue;
                }
                app = designApps->itemByName(lib->name());
                if (!app) app = designApps->addByCopy(lib, lib->name());
                note = "matched to Fusion's \"" + lib->name() + "\" (texture mapping not carried)";
            }
            else if (a.contains("color"))
            {
                std::string nm = fmtColor(a["color"]);
                app = designApps->itemByName(nm);
                if (!app)
                {
                    app = designApps->add(nm);
                    if (app)
                    {
                        const json& c = a["color"];
                        double tr = a.contains("optical") ? a["optical"].value("transparency", 0.0) : 0.0;
                        app->color(Color::create(static_cast<short>(c[0].get<double>() * 255 + 0.5), static_cast<short>(c[1].get<double>() * 255 + 0.5),
                                                 static_cast<short>(c[2].get<double>() * 255 + 0.5), static_cast<short>((1 - tr) * 255 + 0.5)));
                    }
                }
            }
            if (!app)
            {
                report_.add(docName_, item, "failed", "could not make the appearance: " + lastError());
                continue;
            }
            bool ok = true;
            if (kind == "document")
            {
                for (auto& b : bodies()) ok = b->appearance(app) && ok;
            }
            else if (kind == "body")
            {
                std::vector<Ptr<Base>> hit;
                json r = resolveRef(bodies(), t, hit);
                if (hit.empty()) ok = false;
                for (auto& h : hit) ok = Ptr<BRepBody>(h)->appearance(app) && ok;
            }
            else if (kind == "feature")
            {
                auto it = features.find(t.value("id", ""));
                Ptr<Feature> f = it == features.end() ? nullptr : Ptr<Feature>(it->second);
                Ptr<BRepFaces> fs = f ? f->faces() : nullptr;
                if (!fs) ok = false;
                for (size_t i = 0; fs && i < fs->count(); i++) ok = fs->item(i)->appearance(app) && ok;
            }
            else if (kind == "face")
            {
                std::vector<Ptr<Base>> hit;
                json r = resolveRef(bodies(), t, hit);
                if (hit.empty()) ok = false;
                for (auto& h : hit) ok = Ptr<BRepFace>(h)->appearance(app) && ok;
            }
            if (!ok) report_.add(docName_, item, "failed", "its target was not found or refused the appearance: " + lastError());
            else report_.add(docName_, item, note.empty() ? "rebuilt" : "approximate", note);
        }
}

} // namespace ncxf
