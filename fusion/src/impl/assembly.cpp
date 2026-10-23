// Import of a multi-document payload (FORMAT-SPEC 4, Documents; ADDING-A-CAD-SYSTEM step 4).
//
// The whole file becomes one Fusion design:
//  - every part document is rebuilt once, in its own component, under an occurrence at the origin (so component
//    and model space coincide while it is built), and verified on its own;
//  - every assembly document is a component whose occurrences place the parts (and sub-assemblies) by the
//    payload's transforms; the root assembly is the design's root component;
//  - a part no assembly uses (a layout) keeps its occurrence, hidden;
//  - mates: every component arrives where the file places it; mates become Fusion joints where that is
//    implemented (see joints.cpp), else they are reported.
//
// Insert Part (a derived part): Fusion's Derive needs saved cloud documents, so the functional equivalent is
// built inside the design: the source's sketches are rebuilt in the derived part, and each of their driving
// dimensions is given the expression "the source's dimension", so the layout still drives the part.
#include "host.h"
#include "reference.h"
#include "replay.h"

#include <memory>

namespace ncxf {

// ---------------------------------------------------------------------------------------- insert part
std::string Replayer::replayInsertPart(json& nf, const json& def)
{
    std::string srcId = def.value("source", "");
    if (!registry || !registry->count(srcId)) return "its source document " + srcId + " was not rebuilt";
    Replayer* src = (*registry)[srcId];
    if (def.contains("transform"))
    {
        const json& t = def["transform"];
        static const double id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (int i = 0; i < 16; i++)
            if (std::fabs(t[i].get<double>() - id[i]) > 1e-9) return "Insert Part with a transform is not supported yet";
    }
    std::vector<std::string> notes;
    int sketchesDone = 0;
    for (auto& item : def.value("items", json::array()))
    {
        std::string s = item.value("source", "");
        std::string key = "T:" + srcId + "|" + s;
        if (s.rfind("plane:", 0) == 0)
        {
            std::string pl = s.substr(6);
            features[key] = pl == "front" ? Ptr<Base>(comp_->xYConstructionPlane())
                                          : pl == "top" ? Ptr<Base>(comp_->xZConstructionPlane()) : Ptr<Base>(comp_->yZConstructionPlane());
            continue;
        }
        const json* sd = src->sketchDef(s);
        if (!sd)
        {
            notes.push_back("transferred item " + item.value("name", s) + " (not a sketch) not carried");
            continue;
        }
        json copy{{"id", key}, {"name", item.value("name", s)}, {"type", "sketch"}, {"status", "mapped"}};
        std::string w = replaySketch(copy, *sd);
        if (!w.empty())
        {
            notes.push_back("transferred sketch " + item.value("name", s) + ": " + w);
            continue;
        }
        sketchesDone++;
        // its driving dimensions follow the source's
        Ptr<Sketch> sk = features[key];
        std::string skName = sk ? sk->name() : "";
        int linked = 0, total = 0;
        for (auto& d : sd->value("dimensions", json::array()))
        {
            if (!d.value("driving", true)) continue;
            total++;
            std::string srcSketch = featureName(src->features[s]);
            Ptr<Parameter> from = src->parameterFor(d.value("name", "") + "@" + srcSketch);
            Ptr<Parameter> to = parameterFor(d.value("name", "") + "@" + skName);
            if (from && to && to->expression(from->name())) linked++;
        }
        if (linked < total) notes.push_back(std::to_string(total - linked) + " dimension(s) of " + skName + " not linked to the source");
        if (copy.value("status", "") == "approximate") notes.push_back(skName + ": " + copy.value("note", ""));
    }
    if (sketchesDone == 0 && !notes.empty()) return notes[0];
    markers.insert(nf.value("id", ""));
    std::string all = "Insert Part rebuilt as copies of the source's sketches whose dimensions follow the source's parameters (Fusion's Derive needs saved documents)";
    for (auto& n : notes) all += "; " + n;
    nf["status"] = "approximate";
    nf["note"] = all;
    return "";
}

// ---------------------------------------------------------------------------------------- assembly import
namespace {

std::vector<Ptr<BRepBody>> worldBodies(const Ptr<Component>& root, std::vector<Ptr<Matrix3D>>& xfs)
{
    std::vector<Ptr<BRepBody>> out;
    for (size_t i = 0; i < root->bRepBodies()->count(); i++)
        if (root->bRepBodies()->item(i)->isSolid())
        {
            out.push_back(root->bRepBodies()->item(i));
            xfs.push_back(Matrix3D::create());
        }
    Ptr<OccurrenceList> occs = root->allOccurrences();
    for (size_t i = 0; occs && i < occs->count(); i++)
    {
        Ptr<Occurrence> o = occs->item(i);
        if (!o->isLightBulbOn()) continue;
        // hidden parents hide their children too
        bool visible = true;
        for (Ptr<Occurrence> p = o->assemblyContext(); p; p = p->assemblyContext())
            if (!p->isLightBulbOn()) visible = false;
        if (!visible) continue;
        Ptr<Component> c = o->component();
        for (size_t k = 0; k < c->bRepBodies()->count(); k++)
            if (c->bRepBodies()->item(k)->isSolid() && c->bRepBodies()->item(k)->isLightBulbOn())
            {
                out.push_back(c->bRepBodies()->item(k));
                xfs.push_back(o->transform2());
            }
    }
    return out;
}

// A component holding the file's own bodies of a part (the fallback for a part that did not rebuild).
Ptr<Occurrence> plainComponent(const Ptr<Component>& root, const RefComponent& rc, const std::string& name)
{
    Ptr<Occurrence> o = root->occurrences()->addNewComponent(Matrix3D::create());
    Ptr<Component> c = o->component();
    c->name(name);
    Ptr<BaseFeature> bf = c->features()->baseFeatures()->add();
    bf->startEdit();
    for (auto& t : rc.temp) c->bRepBodies()->add(TemporaryBRepManager::get()->copy(t), bf);
    bf->finishEdit();
    bf->name("File geometry");
    return o;
}

} // namespace

json importAssembly(const json& model, Reference& ref, const Ptr<Design>& des, Report& report, const json& opts,
                    const fs::path& file, json& verification, std::string& status)
{
    Ptr<Component> root = des->rootComponent();
    std::map<std::string, std::unique_ptr<Replayer>> reps;
    std::map<std::string, Replayer*> registry;
    std::map<std::string, Ptr<Occurrence>> buildOcc;   // document id -> the occurrence it was built under
    std::map<std::string, Ptr<Component>> compOf;      // document id -> component its instances use
    std::set<std::string> skipFlex;
    bool anyFallback = false;
    // ------------------------------------------------------------------ parts
    for (auto& d : model["documents"])
    {
        if (d.value("kind", "") != "part") continue;
        std::string id = d.value("id", ""), name = d.value("name", "");
        Ptr<Occurrence> occ = root->occurrences()->addNewComponent(Matrix3D::create());
        if (!occ)
        {
            report.add(name, "part", "failed", "could not create a component: " + lastError());
            continue;
        }
        Ptr<Component> comp = occ->component();
        comp->name(name);
        auto rp = std::make_unique<Replayer>(report, model, file);
        rp->registry = &registry;
        rp->trace = opts.value("trace", false);
        auto rit = ref.components.find(name);
        if (rit != ref.components.end()) rp->refBodies = &rit->second.bodies;
        rp->occurrence = occ;
        rp->replayPart(d, comp);
        nameBodies(comp, d);
        registry[id] = rp.get();
        buildOcc[id] = occ;
        compOf[id] = comp;
        json v{{"document", name}};
        json expected = expectedFingerprint(d);
        json actual = fingerprintOf(componentBodies(comp, true));
        const char* profile = (rp->resultsApplied || !rp->looseReason.empty()) ? "exchange" : "native";
        json cmp = coreCompare(expected, actual, profile);
        v["mass"] = cmp.value("result", "?");
        v["tolerance"] = profile;
        if (cmp.value("result", "") != "match") v["massDetail"] = cmp;
        if (rp->resultsApplied) skipFlex.insert(id);
        if (cmp.value("result", "") == "mismatch")
        {
            std::string why = "rebuilt geometry differs in " + differencesText(cmp);
            if (rit == ref.components.end() || rit->second.temp.empty())
                report.add(name, "part", "failed", why + "; the file has no geometry of its own for it");
            else
            {
                report.add(name, "part", "fallback", why + "; replaced by the file's own geometry");
                comp->name(name + " (partial rebuild)");
                occ->isLightBulbOn(false);
                Ptr<Occurrence> plain = plainComponent(root, rit->second, name);
                buildOcc[id] = plain;
                compOf[id] = plain->component();
                skipFlex.insert(id);
                anyFallback = true;
            }
        }
        else
        {
            json missing = json::array();
            for (auto& nf : d.value("features", json::array()))
            {
                if (nf.value("status", "") == "opaque" || rp->markers.count(nf.value("id", ""))) continue;
                auto it = rp->features.find(nf.value("id", ""));
                if (it == rp->features.end() || !it->second) missing.push_back(nf.value("name", ""));
            }
            v["structure"] = missing.empty() ? "match" : "mismatch";
            if (!missing.empty()) v["structureMissing"] = missing;
            // the file's B-rep holds a part as the assembly shows it: a part an assembly cut changes is not
            // compared face by face (the assembly's own check covers the cut)
            bool cutByAssembly = false;
            for (auto& ad : model["documents"])
                for (auto& af : ad.value("features", json::array()))
                    if (af.value("type", "") == "cut" && af.contains("def"))
                        for (auto& cid : af["def"].value("components", json::array()))
                            for (auto& c : ad.value("components", json::array()))
                                if (c.value("id", "") == cid.get<std::string>() && c.value("document", "") == id) cutByAssembly = true;
            if (cutByAssembly) v["faces"] = "skipped";
            if (cutByAssembly) v["facesReason"] = "an assembly cut changes this part; the assembly's check covers it";
            if (!cutByAssembly && opts.value("faces", true) && rit != ref.components.end() && !rit->second.bodies.empty())
            {
                setMeasureContext(occ);
                json fc = faceCompare(rit->second.bodies, componentBodies(comp, true));
                v["faces"] = fc.value("result", "?");
            }
        }
        verification.push_back(v);
        reps[id] = std::move(rp);
    }
    setMeasureContext(nullptr);
    // ------------------------------------------------------------------ assemblies
    std::string rootId = model.value("root", "");
    std::set<std::string> usedBuild;
    std::map<std::string, Ptr<Occurrence>> occOf;   // "<assembly id>|<component id>" -> occurrence
    for (auto& d : model["documents"])
    {
        if (d.value("kind", "") != "assembly") continue;
        std::string id = d.value("id", ""), name = d.value("name", "");
        Ptr<Component> comp;
        if (id == rootId)
        {
            comp = root;
            comp->name(name);
        }
        else
        {
            Ptr<Occurrence> o = root->occurrences()->addNewComponent(Matrix3D::create());
            comp = o->component();
            comp->name(name);
            buildOcc[id] = o;
        }
        compOf[id] = comp;
        for (auto& c : d.value("components", json::array()))
        {
            std::string cd = c.value("document", "");
            auto cit = compOf.find(cd);
            if (cit == compOf.end())
            {
                report.add(name, c.value("name", c.value("id", "")), "failed", "its document " + cd + " was not rebuilt");
                continue;
            }
            Ptr<Matrix3D> xf = matrixFromRowMajorMm(c["transform"]);
            // Every instance is a new occurrence made at its place. The build occurrence is never moved: in a
            // parametric design a moved occurrence is only a pending position, which the next occurrence added
            // reverts, and while pending it offsets every new occurrence of the same component (measured).
            Ptr<Occurrence> o = comp->occurrences()->addExistingComponent(cit->second, xf);
            if (!o)
            {
                report.add(name, c.value("name", c.value("id", "")), "failed", "could not place the component: " + lastError());
                continue;
            }
            if (c.value("fixed", false)) o->isGrounded(true);
            occOf[id + "|" + c.value("id", "")] = o;
        }
        // assembly-level features (planes, sketches, cuts; patterns are placed instances) and mates (joints.cpp)
        if (!d.value("features", json::array()).empty() || !d.value("parameters", json::array()).empty())
        {
            auto arp = std::make_unique<Replayer>(report, model, file);
            arp->asmOccs = &occOf;
            arp->occurrence = id == rootId ? nullptr : buildOcc[id];
            arp->replayPart(d, comp);
            reps[id] = std::move(arp);
        }
        buildJoints(model, d, comp, occOf, report);
    }
    // the build occurrences no instance took over: sub-assemblies' and nested parts' are deleted, unused parts
    // (layouts) stay, hidden
    std::set<std::string> instanced;
    for (auto& d : model["documents"])
        for (auto& c : d.value("components", json::array())) instanced.insert(c.value("document", ""));
    for (auto& kv : buildOcc)
    {
        if (usedBuild.count(kv.first) || !kv.second || !kv.second->isValid()) continue;
        if (instanced.count(kv.first)) kv.second->deleteMe();
        else
        {
            kv.second->isLightBulbOn(false);
            report.add(kv.second->component()->name(), "placement", "note", "not used by any assembly (a layout): kept hidden in the design");
        }
    }
    if (des->snapshots() && des->snapshots()->hasPendingSnapshot()) des->snapshots()->add();
    // ------------------------------------------------------------------ the whole model against the file
    std::vector<Ptr<Matrix3D>> xfs;
    std::vector<Ptr<BRepBody>> all = worldBodies(root, xfs);
    json fps = json::array();
    for (size_t i = 0; i < all.size(); i++) fps.push_back(bodyFingerprint(all[i], xfs[i]));
    bool exchange = anyFallback || !skipFlex.empty();
    json cmp = coreCompare(model["fingerprint"], json{{"bodies", fps}}, exchange ? "exchange" : "native");
    json av{{"document", (findDoc(model, rootId) ? findDoc(model, rootId)->value("name", "") : "")}, {"assembly", true},
            {"mass", cmp.value("result", "?")}, {"tolerance", exchange ? "exchange" : "native"}};
    if (cmp.value("result", "") != "match") av["massDetail"] = cmp;
    // parameter flex over the whole model
    std::map<std::string, Replayer*> rebuilt;
    for (auto& kv : reps) rebuilt[kv.first] = kv.second.get();
    bool anyFlex = false;
    for (auto& d : model["documents"])
        if (d.contains("flex")) anyFlex = true;
    if (anyFlex)
    {
        json fx = flexCheck(model, rebuilt, skipFlex);
        std::string agg = "match";
        for (auto& x : fx)
            if (x.value("result", "") != "match") agg = x.value("result", "");
        av["flex"] = agg;
        av["flexDetail"] = fx;
    }
    verification.push_back(av);
    status = anyFallback ? "partial" : "rebuilt";
    return json::object();
}

} // namespace ncxf
