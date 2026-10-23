// Replay of one part document: the feature loop, the carried-result fallback.
#include "replay.h"

#include "host.h"

namespace ncxf {

Replayer::Replayer(Report& report, const json& model, const fs::path& file) : report_(report), model_(model), file_(file) {}

namespace {
Ptr<TimelineObject> timelineObjectOf(const Ptr<Base>& o)
{
    if (Ptr<Feature> f = o) return f->timelineObject();
    if (Ptr<Sketch> s = o) return s->timelineObject();
    if (Ptr<ConstructionPlane> p = o) return p->timelineObject();
    if (Ptr<ConstructionAxis> a = o) return a->timelineObject();
    if (Ptr<ConstructionPoint> p = o) return p->timelineObject();
    return nullptr;
}
} // namespace

std::vector<Ptr<BRepBody>> Replayer::bodies(bool solidOnly) const
{
    std::vector<Ptr<BRepBody>> v;
    if (!comp_) return v;
    Ptr<BRepBodies> bs = comp_->bRepBodies();
    for (size_t i = 0; i < bs->count(); i++)
    {
        Ptr<BRepBody> b = bs->item(i);
        if (!b || (solidOnly && !b->isSolid())) continue;
        v.push_back(b);
    }
    return v;
}

std::string Replayer::replayPart(const json& doc, const Ptr<Component>& comp)
{
    doc_ = doc;
    docName_ = doc.value("name", "");
    comp_ = comp;
    design_ = comp->parentDesign();
    features.clear();
    sketches_.clear();
    markers.clear();
    deferred_.clear();
    unfolds_.clear();
    resultsApplied = false;
    looseReason.clear();
    paramByNative_.clear();
    setMeasureContext(occurrence);
    replayParameters(doc);
    json feats = doc.contains("features") ? doc["features"] : json::array();
    auto snapshot = [&]() {
        if (!trace) return;
        json a = json::array();
        for (auto& b : bodies())
        {
            json fp = bodyFingerprint(b);
            a.push_back(json{{"v", fp["volume"]}, {"c", fp["centroid"]}, {"solid", b->isSolid()}});
        }
        report_.pendingTrace = a;
    };
    for (auto& nf : feats)
    {
        std::string fname = nf.value("name", "");
        progressStep(docName_ + ": " + fname);
        std::string failure;
        try
        {
            if (nf.value("status", "") == "opaque")
            {
                if (!nf.contains("result"))
                {
                    bool supp = nf.contains("suppressed") && nf["suppressed"].value(doc.value("activeConfiguration", ""), false);
                    if (supp) markers.insert(nf.value("id", ""));
                    report_.add(docName_, fname, supp ? "note" : "failed",
                                (supp ? "suppressed and not mapped by the exporter: " : "not mapped by the exporter: ") + nf.value("reason", ""));
                    continue;
                }
                failure = "not mapped by the exporter: " + nf.value("reason", "");
            }
            else
            {
                // a mapped feature that also carries its result (FORMAT-SPEC 4: e.g. a loft between sections
                // with different numbers of segments, where systems' default alignments differ): the native
                // rebuild is kept only if it gives the result's bodies; feature types that choose their variant
                // by the result check it themselves
                static const std::set<std::string> selfChecked{"edgeFlange", "hem", "sketchedBend", "breakCorner", "loftedBend", "rib",
                                                               "fillet", "structuralMember", "insertPart", "mirrorPart", "sketch",
                                                               "sketch3d", "jog", "closedCorner", "miterFlange"};
                bool checkResult = nf.contains("result") && nf["result"].contains("bodies") && !selfChecked.count(nf.value("type", ""));
                std::vector<json> before;
                if (checkResult) before = solidFingerprints();
                failure = replayFeature(nf);
                if (failure.empty() && checkResult)
                {
                    std::string detail;
                    if (!matchesCarriedResult(nf, before, &detail, "exchange"))
                    {
                        auto fit = features.find(nf.value("id", ""));
                        if (fit != features.end())
                        {
                            if (Ptr<Feature> ft = fit->second) ft->deleteMe();
                            features.erase(fit);
                        }
                        failure = "the Fusion " + nf.value("type", std::string("feature")) + " differs from the source's carried result (" + detail + ")";
                    }
                }
                if (failure == "~deferred") continue;   // reported by the feature that replays it
                if (failure.rfind("~note:", 0) == 0)
                {
                    // deliberately not built, and nothing is lost by it
                    markers.insert(nf.value("id", ""));
                    report_.add(docName_, fname, "note", failure.substr(6));
                    continue;
                }
                if (failure.empty())
                {
                    snapshot();
                    bool approx = nf.value("status", "") == "approximate";
                    report_.add(docName_, fname, approx ? "approximate" : "rebuilt", approx ? nf.value("note", "") : nf.value("info", ""));
                    continue;
                }
            }
        }
        catch (const Cancelled&)
        {
            throw;
        }
        catch (const std::exception& e)
        {
            failure = std::string("exception: ") + e.what();
        }
        // not rebuilt: its carried result keeps the rest of the history rebuildable (FORMAT-SPEC 4)
        if (!nf.contains("result"))
        {
            snapshot();
            report_.add(docName_, fname, "failed", failure);
            continue;
        }
        std::string noResult = applyResult(nf);
        snapshot();
        if (noResult.empty())
            report_.add(docName_, fname, "approximate", "carried as fixed geometry (its resulting bodies), not rebuilt: " + failure);
        else
            report_.add(docName_, fname, "failed", failure + "; its carried result could not be used either: " + noResult);
    }
    replayEquations(doc);
    replayProperties(doc);
    for (auto& kv : deferred_)
    {
        const json& sk = kv.second;
        if (features.count(sk.value("id", ""))) continue;
        markers.insert(sk.value("id", ""));
        report_.add(docName_, sk.value("name", ""), "approximate",
                    "its plane is generated by " + kv.first.substr(0, kv.first.rfind('|')) + ", which was not rebuilt (its carried result holds its effect)");
    }
    // configurations: Fusion's configured designs are not built; the active configuration is (its values
    // are the payload's solved values) and features it suppresses are suppressed
    std::string active = doc.value("activeConfiguration", "");
    for (auto& nf : feats)
    {
        if (!nf.contains("suppressed") || !nf["suppressed"].value(active, false) || markers.count(nf.value("id", ""))) continue;
        auto it = features.find(nf.value("id", ""));
        Ptr<TimelineObject> to = it == features.end() ? nullptr : timelineObjectOf(it->second);
        if (to && to->isSuppressed(true))
            report_.add(docName_, nf.value("name", ""), "note", "suppressed, as in the active configuration");
        else
            report_.add(docName_, nf.value("name", ""), "failed", "suppressed in the active configuration, but could not be suppressed here: " + lastError());
    }
    if (doc.contains("configurations") && doc["configurations"].size() > 1)
    {
        std::string names;
        for (auto& c : doc["configurations"])
            if (c.value("id", "") != active) names += (names.empty() ? "" : ", ") + c.value("name", "");
        std::string an;
        for (auto& c : doc["configurations"])
            if (c.value("id", "") == active) an = c.value("name", "");
        report_.add(docName_, "configurations", "approximate",
                    "the active configuration (" + an + ") is rebuilt; the others (" + names + ") are not created as Fusion configurations");
    }
    return "";
}

std::string Replayer::replayFeature(json& nf)
{
    std::string t = nf.value("type", "");
    json def = nf.contains("def") ? nf["def"] : json::object();
    if (t == "sketch") return replaySketch(nf, def);
    if (t == "sketch3d") return replaySketch3D(nf, def);
    if (t == "structuralMember") return replayStructuralMember(nf, def);
    if (t == "hole") return replayHole(nf, def);
    if (t == "insertPart") return replayInsertPart(nf, def);
    if (t == "coordinateSystem") return replayCoordinateSystem(nf, def);
    if (t == "extrude") return replayExtrude(nf, def, false);
    if (t == "cut") return replayExtrude(nf, def, true);
    if (t == "revolve") return replayRevolve(nf, def, false);
    if (t == "revolveCut") return replayRevolve(nf, def, true);
    if (t == "fillet") return replayFillet(nf, def);
    if (t == "chamfer") return replayChamfer(nf, def);
    if (t == "plane") return replayPlane(nf, def);
    if (t == "axis") return replayAxis(nf, def);
    if (t == "point") return replayPoint(nf, def);
    if (t == "linearPattern") return replayLinearPattern(nf, def);
    if (t == "circularPattern") return replayCircularPattern(nf, def);
    if (t == "mirror") return replayMirror(nf, def, false);
    if (t == "mirrorBody") return replayMirror(nf, def, true);
    if (t == "deleteBody") return replayDeleteBody(nf, def);
    if (t == "combine") return replayCombine(nf, def);
    if (t == "moveBody") return replayMoveBody(nf, def);
    if (t == "shell") return replayShell(nf, def);
    if (t == "draft") return replayDraft(nf, def);
    if (t == "sweep") return replaySweep(nf, def, false);
    if (t == "sweepCut") return replaySweep(nf, def, true);
    if (t == "loft") return replayLoft(nf, def, false);
    if (t == "loftCut") return replayLoft(nf, def, true);
    if (t == "extrudedSurface") return replaySurfaceExtrude(nf, def);
    if (t == "revolvedSurface") return replaySurfaceRevolve(nf, def);
    if (t == "planarSurface") return replayPlanarSurface(nf, def);
    if (t == "knit") return replayKnit(nf, def);
    if (t == "split") return replaySplit(nf, def);
    if (t == "rib") return replayRib(nf, def);
    if (t == "cosmeticThread")
        return replayMarker(nf, "cosmetic thread (" + def.value("callout", std::string("?")) + ") not shown: it changes no geometry");
    if (t == "componentPattern" || t == "mirrorComponents")
        return replayMarker(nf, "its instances are placed where the file lists them (not a live Fusion pattern)");
    if (t == "sheetMetal") return replaySheetMetal(nf, def);
    if (t == "baseFlange") return replayBaseFlange(nf, def);
    if (t == "edgeFlange") return replayEdgeFlange(nf, def);
    if (t == "hem") return replayHem(nf, def);
    if (t == "sketchedBend") return replaySketchedBend(nf, def);
    if (t == "breakCorner") return replayBreakCorner(nf, def);
    if (t == "loftedBend") return replayLoftedBend(nf, def);
    if (t == "closedCorner") return replayClosedCorner(nf, def);
    if (t == "miterFlange") return replayMiterFlange(nf, def);
    if (t == "jog") return replayJog(nf, def);
    if (t == "unfold") return replayUnfold(nf, def, false);
    if (t == "fold") return replayUnfold(nf, def, true);
    if (t == "flatPattern")
    {
        // SolidWorks keeps its flat pattern suppressed while the part is shown folded
        bool supp = nf.contains("suppressed") && nf["suppressed"].value(doc_.value("activeConfiguration", ""), false);
        if (supp) return "~note:flat pattern (suppressed in the active configuration, the part is folded): Fusion makes one on demand from the sheet-metal body";
        return replayMarker(nf, "flat pattern not made: Fusion makes it on demand from the sheet-metal body");
    }
    if (t == "weldment") return replayMarker(nf, "weldment marker: Fusion has no weldment environment; members are built as bodies");
    return "no Fusion replay for feature type " + t + " yet";
}

// ---------------------------------------------------------------------------------------- carried results
namespace {

// Import a STEP text into temporary bodies (through a scratch document, closed again).
std::vector<Ptr<BRepBody>> stepToTemporaryBodies(const std::string& stepText, std::string& why)
{
    std::vector<Ptr<BRepBody>> res;
    fs::path tmp = tempDir() / ("result_" + std::to_string(GetTickCount64()) + ".step");
    writeFile(tmp, stepText);
    Ptr<Application> app = Application::get();
    Ptr<Document> active = app->activeDocument();
    Ptr<ImportManager> im = app->importManager();
    Ptr<STEPImportOptions> opt = im->createSTEPImportOptions(tmp.u8string());
    if (!opt)
    {
        why = "STEP reader: " + lastError();
        return res;
    }
    opt->isViewFit(false);
    Ptr<Document> doc = im->importToNewDocument(opt);
    trackDocument(doc);
    std::error_code ec;
    if (!doc)
    {
        why = "STEP import failed: " + lastError();
        fs::remove(tmp, ec);
        return res;
    }
    Ptr<Design> des = doc->products()->itemByProductType("DesignProductType");
    Ptr<TemporaryBRepManager> tbm = TemporaryBRepManager::get();
    if (des)
    {
        Ptr<Component> root = des->rootComponent();
        for (size_t i = 0; i < root->bRepBodies()->count(); i++) res.push_back(tbm->copy(root->bRepBodies()->item(i)));
        Ptr<OccurrenceList> occs = root->allOccurrences();
        for (size_t i = 0; occs && i < occs->count(); i++)
        {
            Ptr<Occurrence> o = occs->item(i);
            for (size_t k = 0; k < o->component()->bRepBodies()->count(); k++)
            {
                Ptr<BRepBody> t = tbm->copy(o->component()->bRepBodies()->item(k));
                tbm->transform(t, o->transform2());
                res.push_back(t);
            }
        }
    }
    doc->close(false);
    if (active) active->activate();
    fs::remove(tmp, ec);
    return res;
}

double relDiff(double a, double b) { return std::fabs(a - b) / std::max({std::fabs(a), std::fabs(b), 1e-12}); }

} // namespace

std::vector<json> Replayer::solidFingerprints() const
{
    std::vector<json> v;
    for (auto& b : bodies(true)) v.push_back(bodyFingerprint(b));
    return v;
}

bool Replayer::matchesCarriedResult(const json& nf, const std::vector<json>& before, std::string* detail, const char* profile)
{
    if (!nf.contains("result") || !nf["result"].contains("bodies")) return false;
    const json& r = nf["result"];
    json expected = json::array();
    for (auto& b : r["bodies"])
        if (!b.value("sheet", false) && b.contains("fingerprint")) expected.push_back(b["fingerprint"]);
    // the bodies from before that the feature did not replace stay as they were
    std::set<size_t> replaced;
    for (auto& want : r.value("replaces", json::array()))
    {
        double best = 1e300;
        size_t bi = SIZE_MAX;
        for (size_t i = 0; i < before.size(); i++)
        {
            if (replaced.count(i)) continue;
            double cost = relDiff(before[i]["volume"].get<double>(), want.value("volume", 0.0)) +
                          dist(v3(before[i]["centroid"]), v3(want["centroid"])) / 10.0;
            if (cost < best)
            {
                best = cost;
                bi = i;
            }
        }
        if (bi != SIZE_MAX && best < 1e-2) replaced.insert(bi);
    }
    for (size_t i = 0; i < before.size(); i++)
        if (!replaced.count(i)) expected.push_back(before[i]);
    json actual = json::array();
    for (auto& f : solidFingerprints()) actual.push_back(f);
    json cmp = coreCompare(json{{"bodies", expected}}, json{{"bodies", actual}}, profile);
    if (detail) *detail = cmp.value("result", "?") + (cmp.value("result", "") == "mismatch" ? " (" + differencesText(cmp) + ")" : "");
    return cmp.value("result", "") != "mismatch";
}

std::string Replayer::applyResult(const json& nf)
{
    const json& r = nf["result"];
    // 1. the bodies the feature replaced, found by fingerprint in the part as rebuilt so far
    std::vector<Ptr<BRepBody>> current = bodies();
    std::vector<json> fps;
    for (auto& b : current) fps.push_back(resultFingerprint(b));   // surfaces too (a surface result replaces surfaces)
    std::vector<Ptr<BRepBody>> replaced;
    std::set<size_t> used;
    for (auto& want : r["replaces"])
    {
        double best = 1e300;
        size_t bi = SIZE_MAX;
        for (size_t i = 0; i < current.size(); i++)
        {
            if (used.count(i)) continue;
            double cost = relDiff(fps[i]["volume"].get<double>(), want.value("volume", 0.0)) +
                          relDiff(fps[i]["area"].get<double>(), want.value("area", 0.0)) +
                          dist(v3(fps[i]["centroid"]), v3(want["centroid"])) / 10.0;
            if (cost < best)
            {
                best = cost;
                bi = i;
            }
        }
        if (bi == SIZE_MAX || best > 1e-2) return "the bodies it replaces are not in the rebuilt part (earlier steps differ)";
        used.insert(bi);
        replaced.push_back(current[bi]);
    }
    // 2. its bodies, through Fusion's STEP reader
    std::vector<Ptr<BRepBody>> made;
    std::string data = r.value("data", "");
    if (!data.empty() && !r.value("bodies", json::array()).empty())
    {
        std::string why;
        made = stepToTemporaryBodies(data, why);
        if (made.empty()) return why.empty() ? "its STEP data holds no bodies" : why;
    }
    // 3. swap: remove the replaced bodies, add the new ones in a base feature
    bool sheet = false;
    for (auto& b : replaced)
        if (b->isSheetMetal()) sheet = true;
    for (auto& b : replaced)
    {
        Ptr<RemoveFeatures> rf = comp_->features()->removeFeatures();
        if (!rf->add(b)) return "could not remove a replaced body: " + lastError();
    }
    if (!made.empty())
    {
        Ptr<BaseFeature> bf = comp_->features()->baseFeatures()->add();
        if (!bf) return "base feature: " + lastError();
        bf->startEdit();
        std::vector<Ptr<BRepBody>> added;
        for (auto& t : made)
        {
            Ptr<BRepBody> nb = comp_->bRepBodies()->add(t, bf);
            if (nb) added.push_back(nb);
        }
        bf->finishEdit();
        bf->name(nf.value("name", "") + " (result)");
        // a sheet-metal body stays one: later sheet-metal features (and cuts in a sheet-metal component) only
        // find sheet-metal bodies (measured on P12: a cut after a carried edge flange found "No target body")
        if (sheet && comp_->activeSheetMetalRule())
            for (size_t i = 0; i < comp_->bRepBodies()->count(); i++)
            {
                Ptr<BRepBody> b = comp_->bRepBodies()->item(i);
                if (!b->isSolid() || b->isSheetMetal()) continue;
                Ptr<BRepFace> biggest;
                for (size_t k = 0; k < b->faces()->count(); k++)
                {
                    Ptr<BRepFace> f = b->faces()->item(k);
                    if (Ptr<Plane>(f->geometry()) && (!biggest || f->area() > biggest->area())) biggest = f;
                }
                if (biggest && !b->convertToSheetMetal(biggest, comp_->activeSheetMetalRule()))
                    logLine("convert to sheet metal failed: " + lastError());
            }
        features[nf.value("id", "")] = bf;
        // 4. names, matched by fingerprint
        if (r.contains("bodies"))
            for (auto& rb : r["bodies"])
            {
                if (!rb.contains("name")) continue;
                for (size_t i = 0; i < comp_->bRepBodies()->count(); i++)
                {
                    Ptr<BRepBody> b = comp_->bRepBodies()->item(i);
                    json fp = bodyFingerprint(b);
                    if (relDiff(fp["volume"].get<double>(), rb["fingerprint"].value("volume", 0.0)) < 1e-3 &&
                        dist(v3(fp["centroid"]), v3(rb["fingerprint"]["centroid"])) < 0.05)
                        b->name(rb["name"].get<std::string>());
                }
            }
    }
    resultsApplied = true;
    return "";
}

} // namespace ncxf
