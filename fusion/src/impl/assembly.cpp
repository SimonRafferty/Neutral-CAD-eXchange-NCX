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
        // shown as the inserting part shows it (the item's `hidden`); without it hidden: transferred sketches are
        // reference geometry (ISSUES 36)
        if (sk) sk->isLightBulbOn(!item.value("hidden", true));
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
// How a placed component is held (Fusion 2706, see FINDINGS 13): at the top level, a fixed component is pinned
// (Occurrence.isGrounded, the browser's Pin); inside a sub-assembly, a fixed component is grounded to its parent
// (Occurrence.isGroundToParent, the anchor icon), so the sub-assembly moves as one when any of it is dragged, as a
// SolidWorks sub-assembly does; everything else is explicitly neither (Fusion had anchored some top-level parts).
// Whether a sub-assembly is solved as flexible: a component entry of it says `flexible: true` (ISSUES 35); the
// payload does not carry it yet, so sub-assemblies are rigid, SolidWorks' default.
bool flexibleDoc(const json& model, const std::string& docId)
{
    for (auto& d : model["documents"])
        for (auto& c : d.value("components", json::array()))
            if (c.value("document", "") == docId && c.value("flexible", false)) return true;
    return false;
}

void holdOccurrence(const Ptr<Occurrence>& o, bool fixed, bool topLevel)
{
    if (!o) return;
    if (topLevel)
    {
        o->isGroundToParent(false);
        o->isGrounded(fixed);
    }
    else
    {
        o->isGrounded(false);
        o->isGroundToParent(fixed);
    }
}

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

// The faces a face-by-face check did not pair, briefly: how many, and the first few reference faces by type, area
// and box (which surface of the part differs).
json facesBrief(const json& fc, const json& refBodies)
{
    std::vector<const json*> refs;
    for (auto& b : refBodies)
        if (b.contains("faces"))
            for (auto& f : b["faces"]) refs.push_back(&f);
    json out{{"unmatched", fc.value("unmatched", json::array()).size()}, {"extra", fc.value("extra", json::array()).size()},
             {"area", fc.value("area", json::array()).size()}};
    json first = json::array();
    for (auto& u : fc.value("unmatched", json::array()))
    {
        if (first.size() >= 6) break;
        std::string id = u.value("ref", "");
        size_t k = id.rfind(':') == std::string::npos ? SIZE_MAX : std::stoul(id.substr(id.rfind(':') + 1));
        if (k >= refs.size()) continue;
        const json& g = (*refs[k])["geom"];
        first.push_back(json{{"type", g.value("type", "")}, {"area", g.value("area", 0.0)}, {"bbox", g.value("bbox", json::array())},
                             {"result", u.value("result", "")}});
    }
    out["firstUnmatched"] = first;
    if (!fc.value("area", json::array()).empty()) out["firstArea"] = fc["area"][0];
    return out;
}

// A component holding the file's own bodies of a part (the fallback for a part that did not rebuild).
Ptr<Occurrence> plainComponent(const Ptr<Component>& root, const RefComponent& rc, const std::string& name)
{
    progressStep(name + ": the file's own geometry (fallback)");
    Ptr<Occurrence> o = root->occurrences()->addNewComponent(Matrix3D::create());
    Ptr<Component> c = o->component();
    c->name(name);
    Ptr<BaseFeature> bf = c->features()->baseFeatures()->add();
    bf->startEdit();
    // no Cancelled may leave the base feature in edit: ticks here only update the message
    size_t k = 0;
    try
    {
        for (auto& t : rc.temp)
        {
            progressTick(name + ": the file's own geometry, body " + std::to_string(++k) + " of " + std::to_string(rc.temp.size()));
            c->bRepBodies()->add(TemporaryBRepManager::get()->copy(t), bf);
        }
    }
    catch (const Cancelled&)
    {
        bf->finishEdit();
        throw;
    }
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
    std::vector<Ptr<Occurrence>> partialRebuilds;   // a fallback part's partial rebuild, kept hidden
    bool anyFallback = false;
    // developer: "only": [part names] rebuilds those parts and the documents they insert, and no assembly
    std::set<std::string> only, onlyAsm;
    for (auto& n : opts.value("only", json::array())) only.insert(n.get<std::string>());
    // developer: "onlyAssemblies": [sub-assembly names] builds those sub-assemblies and the parts they use
    for (auto& n : opts.value("onlyAssemblies", json::array()))
    {
        onlyAsm.insert(n.get<std::string>());
        for (auto& d : model["documents"])
            if (d.value("name", "") == n.get<std::string>())
                for (auto& c : d.value("components", json::array()))
                    if (const json* cd = findDoc(model, c.value("document", ""))) only.insert(cd->value("name", ""));
    }
    if (!only.empty())
        for (bool grew = true; grew;)
        {
            grew = false;
            for (auto& d : model["documents"])
                if (only.count(d.value("name", "")))
                    for (auto& f : d.value("features", json::array()))
                        if (f.value("type", "") == "insertPart" && f.contains("def"))
                            if (const json* src = findDoc(model, f["def"].value("source", "")))
                                grew |= only.insert(src->value("name", "")).second;
        }
    // ------------------------------------------------------------------ parts
    for (auto& d : model["documents"])
    {
        if (d.value("kind", "") != "part") continue;
        std::string id = d.value("id", ""), name = d.value("name", "");
        if (!only.empty() && !only.count(name)) continue;
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
        // Fusion's sheet-metal features put their bodies into the ACTIVE component, not the one whose
        // features() made them (measured, L11: the base flange's body landed in the root): the part's
        // component is activated while it is rebuilt
        occ->activate();
        progressPhase("replay: " + name);
        rp->replayPart(d, comp);
        progressPhase("verify mass");
        des->activateRootComponent();
        progressStep(name + ": checking mass properties");
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
        if (cmp.value("result", "") == "mismatch" && opts.value("faces", true) && rit != ref.components.end() &&
            !rit->second.bodies.empty() && rit->second.bodies[0].contains("faces"))
        {
            // which faces differ, before the part falls back (for the report)
            progressStep(name + ": checking faces against the file");
            setMeasureContext(occ);
            json fc = faceCompare(rit->second.bodies, componentBodies(comp, true), 0.001, name + ": checking faces");
            setMeasureContext(nullptr);
            v["faces"] = fc.value("result", "?");
            if (fc.value("result", "") != "match") v["facesDetail"] = facesBrief(fc, rit->second.bodies);
        }
        if (cmp.value("result", "") == "mismatch")
        {
            std::string why = "rebuilt geometry differs in " + differencesText(cmp);
            if (rit == ref.components.end() || rit->second.temp.empty())
                report.add(name, "part", "failed", why + "; the file has no geometry of its own for it");
            else
            {
                report.add(name, "part", "fallback", why + "; replaced by the file's own geometry");
                progressPhase("fallback");
                comp->name(name + " (partial rebuild)");
                occ->isLightBulbOn(false);
                partialRebuilds.push_back(occ);
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
                progressPhase("verify faces");
                progressStep(name + ": checking faces against the file");
                setMeasureContext(occ);
                json fc = faceCompare(rit->second.bodies, componentBodies(comp, true), 0.001, name + ": checking faces");
                v["faces"] = fc.value("result", "?");
                if (fc.value("result", "") != "match") v["facesDetail"] = facesBrief(fc, rit->second.bodies);
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
        if (d.value("kind", "") != "assembly" || (!only.empty() && !onlyAsm.count(d.value("name", "")))) continue;
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
        progressPhase("placement");
        size_t nComp = d.value("components", json::array()).size(), kComp = 0;
        for (auto& c : d.value("components", json::array()))
        {
            progressStep(name + ": placing " + c.value("name", c.value("id", "")) + " (" + std::to_string(++kComp) + " of " +
                         std::to_string(nComp) + ")");
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
            // a rigid sub-assembly's components are grounded to it before its joints are made (made after, Fusion
            // refused it on a part already joined)
            holdOccurrence(o, c.value("fixed", false) || (comp != root && !flexibleDoc(model, id)), comp == root);
            occOf[id + "|" + c.value("id", "")] = o;
        }
        // assembly-level features (planes, sketches, cuts; patterns are placed instances) and mates (joints.cpp)
        if (!d.value("features", json::array()).empty() || !d.value("parameters", json::array()).empty())
        {
            auto arp = std::make_unique<Replayer>(report, model, file);
            arp->asmOccs = &occOf;
            arp->occurrence = id == rootId ? nullptr : buildOcc[id];
            progressPhase("assembly features");
            arp->replayPart(d, comp);
            reps[id] = std::move(arp);
        }
        progressPhase("joints");
        progressStep(name + ": joints from the mates");
        buildJoints(model, d, comp, occOf, report);
        // a sub-assembly is rigid unless the source solves it as flexible (ISSUES 35): its own mates are joints in it
        // (they hold its parts), and one rigid group of all its components keeps them from moving relative to each
        // other when the top level moves, as a rigid SolidWorks sub-assembly does
        // a sub-assembly is rigid unless the source solves it as flexible (ISSUES 35): its mates are joints in it,
        // and every component of it is also grounded to it (Ground to Parent), so it moves as one piece when the top
        // level moves, as a rigid SolidWorks sub-assembly does. The joints are made at the file's placement, so
        // they agree with the grounding.
        if (id != rootId && !flexibleDoc(model, id))
        {
            std::string refused;
            for (size_t k = 0; k < comp->occurrences()->count(); k++)
                if (!comp->occurrences()->item(k)->isGroundToParent()) refused += (refused.empty() ? "" : ", ") + comp->occurrences()->item(k)->name();
            report.add(name, "rigid sub-assembly", refused.empty() ? "note" : "approximate",
                       "solved rigid (SolidWorks' default): its mates are joints in it and its components are grounded to it" +
                           (refused.empty() ? std::string() : " (Fusion did not ground " + refused + ": it moves as its joints allow)"));
        }
    }
    // the build occurrences no instance took over: sub-assemblies' and nested parts' are deleted, unused parts
    // (layouts) stay, hidden
    progressPhase("placement");
    std::set<std::string> instanced;
    for (auto& d : model["documents"])
        for (auto& c : d.value("components", json::array())) instanced.insert(c.value("document", ""));
    // what stays (layouts, the parts' partial rebuilds) goes into one hidden component, so the top level holds
    // only the assembly's own components (loose copies there overlapped the assembly and could be dragged)
    Ptr<Occurrence> aside;
    auto putAside = [&](const Ptr<Occurrence>& o) {
        if (!aside)
        {
            aside = root->occurrences()->addNewComponent(Matrix3D::create());
            if (aside) aside->component()->name("NCX source parts (layouts, partial rebuilds)");
        }
        Ptr<Occurrence> moved = aside ? o->moveToComponent(aside) : nullptr;
        Ptr<Occurrence> h = moved ? moved : o;
        h->isLightBulbOn(false);
        h->isGroundToParent(true);
        return (bool)moved;
    };
    for (auto& kv : buildOcc)
    {
        progressTick("tidying the build components");
        if (usedBuild.count(kv.first) || !kv.second || !kv.second->isValid()) continue;
        if (instanced.count(kv.first) && !(onlyAsm.count(kv.second->component()->name()))) kv.second->deleteMe();
        else
        {
            std::string n = kv.second->component()->name();
            bool moved = putAside(kv.second);
            report.add(n, "placement", "note", std::string("not used by any assembly (a layout): kept hidden") +
                                                   (moved ? " in the component 'NCX source parts (layouts, partial rebuilds)'" : " in the design"));
        }
    }
    for (auto& o : partialRebuilds)
        if (o && o->isValid()) putAside(o);
    if (aside)
    {
        aside->isLightBulbOn(false);
        aside->isGrounded(true);
    }
    progressStep("capturing the positions");
    if (des->snapshots() && des->snapshots()->hasPendingSnapshot()) des->snapshots()->add();
    // ------------------------------------------------------------------ the whole model against the file
    progressPhase("whole-model check");
    progressStep("checking the whole assembly against the file");
    std::vector<Ptr<Matrix3D>> xfs;
    std::vector<Ptr<BRepBody>> all = worldBodies(root, xfs);
    json fps = json::array();
    for (size_t i = 0; i < all.size(); i++)
    {
        progressTick("checking the whole assembly against the file: body " + std::to_string(i + 1) + " of " + std::to_string(all.size()));
        fps.push_back(bodyFingerprint(all[i], xfs[i]));
    }
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
        progressPhase("flex");
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
