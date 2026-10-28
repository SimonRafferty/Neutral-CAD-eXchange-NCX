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

#include <cstring>
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
        // what it is a copy of (FORMAT-SPEC 4 `transferred`), for the export: written back as the transferred item of
        // this insertPart, not as a sketch of its own
        if (sk)
            sk->attributes()->add("NCX", "transferred",
                                  json{{"document", srcId}, {"feature", s}, {"name", item.value("name", s)}, {"stock", nf.value("name", "")}}.dump());
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
// Rigid or flexible sub-assemblies (FORMAT-SPEC 5, components): a component entry that places a sub-assembly may say
// `flexible: true`. Fusion keeps a sub-assembly's joints and its parts' Ground to Parent in the sub-assembly's
// component, which every occurrence of it shares, so a sub-assembly placed both rigid and flexible (L12: the same arm
// twice, the second flexible) is built as two components: the rigid one under the document's id, the flexible one
// under "<id>#flex" (named "<name> (flexible)" when both exist).
const char* const kFlexSuffix = "#flex";

bool placesFlexible(const json& model, const json& comp)
{
    if (!comp.value("flexible", false)) return false;
    const json* d = findDoc(model, comp.value("document", ""));
    return d && d->value("kind", "") == "assembly";
}

std::string buildKeyOf(const json& model, const json& comp)
{
    return comp.value("document", "") + (placesFlexible(model, comp) ? kFlexSuffix : "");
}

std::string baseDocId(const std::string& key)
{
    size_t n = std::strlen(kFlexSuffix);
    return key.size() > n && key.compare(key.size() - n, n, kFlexSuffix) == 0 ? key.substr(0, key.size() - n) : key;
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
        // In the upright wrapper (common.h) a native body's mass properties come where its occurrence puts it
        // (measured: a part placed only in the rotated wrapper read its centroid rotated), so the bodies are measured
        // as proxies in the root's context and taken back through the wrapper's placement
        Ptr<Occurrence> w = modelWrapper();
        Ptr<Occurrence> po = w ? o->createForAssemblyContext(w) : nullptr;
        if (po)
        {
            Ptr<Matrix3D> inv = w->transform2()->copy();
            inv->invert();
            Ptr<BRepBodies> pb = po->bRepBodies();
            for (size_t k = 0; pb && k < pb->count(); k++)
                if (pb->item(k)->isSolid() && pb->item(k)->isLightBulbOn())
                {
                    out.push_back(pb->item(k));
                    xfs.push_back(inv);
                }
            continue;
        }
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

// A component holding the file's own bodies of a part: the fallback for a part that did not rebuild, or a plain
// part (no history in the source).
Ptr<Occurrence> plainComponent(const Ptr<Component>& root, const RefComponent& rc, const std::string& name, bool fallback = true)
{
    progressStep(name + (fallback ? ": the file's own geometry (fallback)" : ": its geometry (no history in the source)"));
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
    // the model's component: the root, or the upright wrapper (common.h); occurrences made in it are measured and
    // activated through the wrapper (MeasureManager measures only in the root's context)
    Ptr<Occurrence> wrapper = modelWrapper();
    Ptr<Component> root = wrapper ? wrapper->component() : des->rootComponent();
    // Parts and sub-assemblies are built inside the wrapper too, and measured and activated through it. (Built at
    // the design's root instead, an instance placed in the wrapper from a component whose build occurrence is at the
    // root did not get the placement asked for: L12's hanger came out in the file's frame, L03's pins turned.)
    // Their cuts cannot reach each other: the parts built before are hidden while one is rebuilt (below).
    Ptr<Component> buildRoot = root;
    auto inRoot = [&](const Ptr<Occurrence>& o) -> Ptr<Occurrence> {
        if (!o || !wrapper) return o;
        Ptr<Occurrence> p = o->createForAssemblyContext(wrapper);
        return p ? p : o;
    };
    // developer: where the model's top-level occurrences are at each phase (trace)
    auto xfDebug = [&](const std::string& when) {
        if (!opts.value("trace", false) || !wrapper) return;
        std::string t;
        for (size_t k = 0; k < root->occurrences()->count(); k++)
        {
            Ptr<Occurrence> o = root->occurrences()->item(k);
            Ptr<Matrix3D> m = o->transform2();
            char b[160];
            std::snprintf(b, sizeof b, "%s [%.2f %.2f %.2f / %.2f %.2f %.2f]; ", o->name().c_str(), m->getCell(1, 1), m->getCell(1, 2), m->getCell(2, 1),
                          m->getCell(0, 3), m->getCell(1, 3), m->getCell(2, 3));
            t += b;
        }
        report.add("debug", "placement " + when, "note", t);
    };
    std::map<std::string, std::unique_ptr<Replayer>> reps;
    std::map<std::string, Replayer*> registry;
    std::map<std::string, Ptr<Occurrence>> buildOcc;   // document id -> the occurrence it was built under
    std::map<std::string, Ptr<Component>> compOf;      // document id -> component its instances use
    std::vector<std::pair<Ptr<Component>, std::string>> cutCopies;   // copies made for assembly-level cuts, with their document
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
        // a plain part (FORMAT-SPEC 4: no history in the source, imported geometry only): the file's own bodies, as
        // they are, nothing rebuilt or compared; a note, not a fallback
        if (d.value("plain", false))
        {
            auto rit = ref.components.find(name);
            if (rit == ref.components.end() || rit->second.temp.empty())
            {
                report.add(name, "part", "failed", "a plain part (no history in the source), but the file has no geometry for it");
                continue;
            }
            progressPhase("plain parts");
            Ptr<Occurrence> plain = plainComponent(buildRoot, rit->second, name, false);
            auto rp = std::make_unique<Replayer>(report, model, file);
            rp->registry = &registry;
            if (plain) rp->replayPropertiesOn(d, plain->component());
            registry[id] = rp.get();
            buildOcc[id] = plain;
            compOf[id] = plain ? plain->component() : nullptr;
            skipFlex.insert(id);
            report.add(name, "part", "note", "no history in the source (imported geometry): its bodies as the file has them");
            verification.push_back(json{{"document", name}, {"mass", "plain"}});
            reps[id] = std::move(rp);
            continue;
        }
        Ptr<Occurrence> occ = buildRoot->occurrences()->addNewComponent(Matrix3D::create());
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
        rp->occurrence = inRoot(occ);
        // Fusion's sheet-metal features put their bodies into the ACTIVE component, not the one whose
        // features() made them (measured, L11: the base flange's body landed in the root): the part's
        // component is activated while it is rebuilt
        // Every part is rebuilt with its occurrence active: Fusion's sheet-metal features build in the active
        // component, and a cut made without naming its bodies then reaches only the active component's (measured on
        // the folding step: with no part active, such cuts also cut the other parts built at the same origin).
        // Features made so keep proxies of their inputs; the exporter reads them as native objects.
        inRoot(occ)->activate();
        progressPhase("replay: " + name);
        // the parts built before are hidden while this one is rebuilt: every part is built at the origin, and a cut
        // made without naming its bodies (sheet metal refuses named ones) cut the visible parts it met (measured:
        // the Cheek Opposite's Cut-Extrude7 reached the Top Hanger Cheek built before it, and the other way round)
        std::vector<Ptr<Occurrence>> hidden;
        for (auto& kv : buildOcc)
            if (kv.second && kv.second->isValid() && kv.second->isLightBulbOn())
            {
                kv.second->isLightBulbOn(false);
                hidden.push_back(kv.second);
            }
        rp->replayPart(d, comp);
        for (auto& h : hidden)
            if (h && h->isValid()) h->isLightBulbOn(true);
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
        // a mismatch: is the rebuild what Fusion measures on the file's own B-rep? (evidence only: two kernels
        // measure some analytic bodies differently too; the spec allows that escape for freeform bodies only)
        if (cmp.value("result", "") == "mismatch" && rit != ref.components.end() && !rit->second.bodies.empty())
        {
            json refFps = json::array();
            for (auto& b : rit->second.bodies)
                if (b.contains("fingerprint")) refFps.push_back(b["fingerprint"]);
            json rc = coreCompare(json{{"bodies", refFps}}, actual, "native");
            v["vsFileGeometry"] = rc.value("result", "?");
            if (rc.value("result", "") != "match") v["vsFileGeometryDetail"] = rc;
            // Spec 6.1's rule for freeform bodies, applied to any body (ISSUES 39, measured on the folding step's nut:
            // every face matches, the rebuild equals Fusion's measurement of the file's B-rep at native, and differs
            // from the payload by 5.6e-5 in volume): the rebuild is kept, reported approximate with the reason
            json ex = coreCompare(expected, actual, "exchange");
            if (rc.value("result", "") == "match" && ex.value("result", "") != "mismatch")
            {
                cmp = ex;
                v["mass"] = ex.value("result", "match");
                v["massNote"] = "matches Fusion's own measurement of the file's B-rep at native and the payload at exchange";
                report.add(name, "mass check", "approximate",
                           "the rebuild equals Fusion's measurement of the file's own geometry (native tolerance) and the payload's "
                           "fingerprint at exchange tolerance only: the two kernels measure this body differently (" +
                               differencesText(v["massDetail"]) + "); kept");
            }
        }
        if (rp->resultsApplied) skipFlex.insert(id);
        if (cmp.value("result", "") == "mismatch" && opts.value("faces", true) && rit != ref.components.end() &&
            !rit->second.bodies.empty() && rit->second.bodies[0].contains("faces"))
        {
            // which faces differ, before the part falls back (for the report)
            progressStep(name + ": checking faces against the file");
            setMeasureContext(inRoot(occ));
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
                comp->attributes()->add("NCX", "partialRebuild", name);   // kept for diagnosis; never exported
                occ->isLightBulbOn(false);
                partialRebuilds.push_back(occ);
                Ptr<Occurrence> plain = plainComponent(buildRoot, rit->second, name);
                if (plain) rp->replayPropertiesOn(d, plain->component());
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
                setMeasureContext(inRoot(occ));
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
    // how each sub-assembly is placed: rigid and/or flexible (each its own Fusion component, see placesFlexible)
    std::set<std::string> placedRigid, placedFlex;
    for (auto& d : model["documents"])
        for (auto& c : d.value("components", json::array()))
            (placesFlexible(model, c) ? placedFlex : placedRigid).insert(c.value("document", ""));
    for (auto& d0 : model["documents"])
    {
        if (d0.value("kind", "") != "assembly" || (!only.empty() && !onlyAsm.count(d0.value("name", "")))) continue;
        std::string docId = d0.value("id", "");
        std::vector<bool> variants;   // false: rigid (or the root), true: flexible
        if (docId == rootId || placedRigid.count(docId) || !placedFlex.count(docId)) variants.push_back(false);
        if (docId != rootId && placedFlex.count(docId)) variants.push_back(true);
        for (bool flex : variants)
        {
        // the flexible build of a sub-assembly is keyed "<id>#flex" (its occurrences, its joints)
        json dv = d0;
        if (flex) dv["id"] = docId + kFlexSuffix;
        const json& d = dv;
        std::string id = d.value("id", ""), name = d.value("name", "");
        std::string compName = name + (flex && variants.size() > 1 ? " (flexible)" : "");
        Ptr<Component> comp;
        if (id == rootId)
        {
            comp = root;
            comp->name(name);
        }
        else
        {
            Ptr<Occurrence> o = buildRoot->occurrences()->addNewComponent(Matrix3D::create());
            comp = o->component();
            comp->name(compName);
            buildOcc[id] = o;
        }
        compOf[id] = comp;
        progressPhase("placement");
        // the components an assembly-level cut changes (FORMAT-SPEC 4: the cut changes those instances in this
        // assembly, not their parts): Fusion's cut in an assembly changes the component, so every instance of it, so
        // each of these instances is an independent copy of its component (marked, with the source's attributes)
        std::set<std::string> cutComps;
        for (auto& af : d.value("features", json::array()))
            if (af.value("type", "") == "cut" && af.contains("def") && af["def"].contains("components"))
                for (auto& cc : af["def"]["components"]) cutComps.insert(cc.get<std::string>());
        size_t nComp = d.value("components", json::array()).size(), kComp = 0;
        for (auto& c : d.value("components", json::array()))
        {
            progressStep(name + ": placing " + c.value("name", c.value("id", "")) + " (" + std::to_string(++kComp) + " of " +
                         std::to_string(nComp) + ")");
            std::string cd = buildKeyOf(model, c);
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
            // In the upright wrapper, an identity placement is not taken as given: Fusion put such an instance where
            // the component's build occurrence is in the world (measured on L12: the hanger came out with the
            // inverse of the wrapper's rotation, lying in the file's frame). A translation of 1e-9 mm (below every
            // tolerance) makes Fusion take it as a placement in the wrapper.
            if (wrapper && xf->isEqualTo(Matrix3D::create()))
            {
                Ptr<Vector3D> tr = xf->translation();
                tr->x(tr->x() + 1e-10);
                xf->translation(tr);
            }
            bool cutCopy = cutComps.count(c.value("id", "")) > 0;
            Ptr<Occurrence> o = cutCopy ? comp->occurrences()->addNewComponentCopy(cit->second, xf)
                                        : comp->occurrences()->addExistingComponent(cit->second, xf);
            if (o && cutCopy)
            {
                Ptr<Component> cc = o->component();
                cc->attributes()->add("NCX", "assemblyCutCopy", cd);   // the document it copies (recorded below)
                cutCopies.push_back({cc, cd});
                cc->name(cit->second->name() + " (" + c.value("name", c.value("id", "")) + ", assembly cut)");
                report.add(name, c.value("name", c.value("id", "")), "approximate",
                           "an independent copy of " + cit->second->name() + " (Fusion's cut in an assembly changes the component itself; this instance takes the assembly's cut, the part does not)");
            }
            if (!o)
            {
                report.add(name, c.value("name", c.value("id", "")), "failed", "could not place the component: " + lastError());
                continue;
            }
            // A rigid sub-assembly's components are grounded to it before its joints are made (made after, Fusion
            // refused it on a part already joined). In a flexible one only its fixed components are: its mates hold
            // the rest, and those move with the parent's motion (FORMAT-SPEC 5).
            // in the upright wrapper the top level is held to the wrapper (pinned itself), as a sub-assembly's parts are
            holdOccurrence(o, c.value("fixed", false) || (comp != root && !flex), comp == root && !wrapper);
            occOf[id + "|" + c.value("id", "")] = o;
        }
        if (id == rootId) xfDebug("after placing");
        // assembly-level features (planes, sketches, cuts; patterns are placed instances) and mates (joints.cpp)
        if (!d.value("features", json::array()).empty() || !d.value("parameters", json::array()).empty())
        {
            auto arp = std::make_unique<Replayer>(report, model, file);
            arp->asmOccs = &occOf;
            arp->occurrence = id == rootId ? wrapper : inRoot(buildOcc[id]);
            progressPhase("assembly features");
            arp->replayPart(d, comp);
            reps[id] = std::move(arp);
        }
        progressPhase("joints");
        progressStep(name + ": joints from the mates");
        buildJoints(model, d, comp, occOf, report);
        if (id == rootId) xfDebug("after joints");
        // A sub-assembly's own mates are joints in it (they hold its parts). A rigid one (SolidWorks' default) also
        // has every component grounded to it (Ground to Parent), so it moves as one piece when the top level moves;
        // the joints are made at the file's placement, so they agree with the grounding. A flexible one keeps only
        // its joints (and its fixed components grounded to it).
        if (id != rootId && !flex)
        {
            std::string refused;
            for (size_t k = 0; k < comp->occurrences()->count(); k++)
                if (!comp->occurrences()->item(k)->isGroundToParent()) refused += (refused.empty() ? "" : ", ") + comp->occurrences()->item(k)->name();
            report.add(name, "rigid sub-assembly", refused.empty() ? "note" : "approximate",
                       "solved rigid (the file does not say flexible): its mates are joints in it and its components are grounded to it" +
                           (refused.empty() ? std::string() : " (Fusion did not ground " + refused + ": it moves as its joints allow)"));
        }
        else if (flex)
            report.add(name, "flexible sub-assembly", "note",
                       "solved flexible (the file says so): its mates are joints in " + compName +
                           ", only its fixed components are grounded to it, and the parent's mates join its parts, so they move with the parent's motion");
        }
    }
    // the build occurrences no instance took over: sub-assemblies' and nested parts' are deleted, unused parts
    // (layouts) stay, hidden
    progressPhase("placement");
    std::set<std::string> instanced;
    for (auto& d : model["documents"])
        for (auto& c : d.value("components", json::array())) instanced.insert(buildKeyOf(model, c));
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
        if (wrapper) aside->isGroundToParent(true);   // pinning a nested occurrence does not tie it to its parent
        else aside->isGrounded(true);
    }
    xfDebug("after tidying");
    // joints hidden in every assembly and sub-assembly, as Fusion shows a new design: the user turns them on when needed
    for (size_t i = 0; i < des->allComponents()->count(); i++)
        if (Ptr<Component> c = des->allComponents()->item(i)) c->isJointsFolderLightBulbOn(false);
    // Provenance of whole documents (exporter.cpp, document passthrough): every document's component keeps the source
    // document and its state as imported (recordDocument); every placed occurrence its component entry (NCX/component).
    // An export writes a document Fusion left as it was back as the source's own, so SolidWorks -> Fusion -> SolidWorks
    // gives the source's history, mates and limits back.
    progressStep("recording where each document came from");
    for (auto& kv : compOf)
        if (const json* sd = findDoc(model, baseDocId(kv.first)))
            if (kv.second && kv.second->isValid()) recordDocument(kv.second, *sd);
    // a copy for an assembly-level cut is its part's document too (the cut belongs to the assembly)
    for (auto& cc : cutCopies)
        if (const json* sd = findDoc(model, baseDocId(cc.second)))
            if (cc.first && cc.first->isValid()) recordDocument(cc.first, *sd);
    for (auto& kv : occOf)
    {
        size_t bar = kv.first.find('|');
        if (kv.second && kv.second->isValid())
            kv.second->attributes()->add("NCX", "component", json{{"document", baseDocId(kv.first.substr(0, bar))}, {"id", kv.first.substr(bar + 1)}}.dump());
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
