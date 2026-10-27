// Import NCX: read the payload, check it against the file's geometry, rebuild, verify, report.
//
//   job {"cmd": "import", "file": "...ncx.step", "close": bool (close the result unsaved, for tests),
//        "faces": bool (face-by-face check, default true)}
//   -> {"ok", "status": "rebuilt" | "fallback" | "failed", "report": [...], "verification": [...], ...}
#include "host.h"
#include "reference.h"
#include "replay.h"

#include <shlobj.h>

#include <chrono>
#include <ctime>
#include <fstream>

namespace ncxf {

const json* findDoc(const json& model, const std::string& id)
{
    for (auto& d : model["documents"])
        if (d.value("id", "") == id) return &d;
    return nullptr;
}

json expectedFingerprint(const json& doc)
{
    json a = json::array();
    if (doc.contains("bodies"))
        for (auto& b : doc["bodies"])
            if (b.contains("fingerprint")) a.push_back(b["fingerprint"]);
    return json{{"bodies", a}};
}

Ptr<Design> newDesign(Ptr<Document>& doc)
{
    Ptr<Application> app = Application::get();
    doc = app->documents()->add(FusionDesignDocumentType);
    if (!doc) return nullptr;
    trackDocument(doc);
    Ptr<Design> des = doc->products()->itemByProductType("DesignProductType");
    if (des) des->designType(ParametricDesignType);
    return des;
}

std::vector<Ptr<BRepBody>> componentBodies(const Ptr<Component>& c, bool solidOnly)
{
    std::vector<Ptr<BRepBody>> v;
    for (size_t i = 0; i < c->bRepBodies()->count(); i++)
        if (!solidOnly || c->bRepBodies()->item(i)->isSolid()) v.push_back(c->bRepBodies()->item(i));
    return v;
}

// Give the rebuilt bodies the payload's body names, matched by fingerprint (Fusion names bodies Body1...).
void nameBodies(const Ptr<Component>& c, const json& doc)
{
    if (!doc.contains("bodies")) return;
    std::vector<Ptr<BRepBody>> bs = componentBodies(c, false);
    std::vector<json> fps;
    for (auto& b : bs) fps.push_back(bodyFingerprint(b));
    for (auto& pb : doc["bodies"])
    {
        if (!pb.contains("fingerprint") || !pb.contains("name")) continue;
        const json& f = pb["fingerprint"];
        for (size_t i = 0; i < bs.size(); i++)
        {
            double v = fps[i]["volume"].get<double>(), w = f.value("volume", 0.0);
            if (std::fabs(v - w) <= 1e-4 * std::max(std::fabs(w), 1e-9) && dist(v3(fps[i]["centroid"]), v3(f["centroid"])) < 0.01)
            {
                bs[i]->name(pb["name"].get<std::string>());
                break;
            }
        }
    }
}

std::string differencesText(const json& cmp)
{
    std::set<std::string> q;
    if (cmp.contains("differences"))
        for (auto& d : cmp["differences"])
            if (d.value("severity", "") == "mismatch") q.insert(d.value("quantity", "?"));
    std::string s;
    for (auto& x : q) s += (s.empty() ? "" : ", ") + x;
    return s.empty() ? "body count" : s;
}

// Parameter flex (FORMAT-SPEC 6.4): each probe changes one parameter of the rebuild, every part is measured
// against the probe's fingerprints (parts it does not list must not move), and the value is put back.
json flexCheck(const json& model, std::map<std::string, Replayer*>& rebuilt, const std::set<std::string>& skipDocs)
{
    json results = json::array();
    std::map<std::string, json> baseline;
    for (auto& kv : rebuilt) baseline[kv.first] = fingerprintOf(componentBodies(kv.second->component(), true));
    for (auto& d : model["documents"])
    {
        if (!d.contains("flex")) continue;
        auto rit = rebuilt.find(d.value("id", ""));
        for (auto& probe : d["flex"])
        {
            std::string pname = probe.value("parameter", "");
            json r{{"document", d.value("name", "")}, {"parameter", pname}, {"value", probe.value("value", 0.0)}};
            std::set<std::string> moved;
            for (auto& kv : probe["fingerprints"].items()) moved.insert(kv.key());
            std::string skipWhy;
            for (auto& m : moved)
                if (skipDocs.count(m)) skipWhy = "a part it moves carries fixed geometry or fell back";
            if (rit == rebuilt.end()) skipWhy = "its document was not rebuilt";
            if (!skipWhy.empty())
            {
                r["result"] = "skipped";
                r["reason"] = skipWhy;
                results.push_back(r);
                continue;
            }
            progressStep(d.value("name", "") + ": flexing " + pname + " (parameter check)");
            Ptr<Parameter> p = rit->second->parameterFor(pname);
            if (!p)
            {
                r["result"] = "failed";
                r["reason"] = "no Fusion parameter for " + pname;
                results.push_back(r);
                continue;
            }
            std::string saved = p->expression();
            std::string kind = probe.value("kind", "variable");
            double v = probe.value("value", 0.0);
            bool set;
            if (kind == "length") set = p->value(mm2cm(v));
            else if (kind == "angle") set = p->value(deg2rad(v));
            else
            {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.12g", v);
                set = p->expression(buf);
            }
            if (!set)
            {
                r["result"] = "failed";
                r["reason"] = "Fusion refused the value: " + lastError();
                results.push_back(r);
                continue;
            }
            json parts = json::array();
            bool ok = true;
            for (auto& kv : rebuilt)
            {
                if (skipDocs.count(kv.first)) continue;
                progressTick(d.value("name", "") + ": flexing " + pname + ", measuring " + kv.second->documentName());
                json actual = fingerprintOf(componentBodies(kv.second->component(), true));
                json expected = moved.count(kv.first) ? probe["fingerprints"][kv.first] : baseline[kv.first];
                json cmp = coreCompare(expected, actual, "native");
                std::string res = cmp.value("result", "?");
                if (res == "mismatch") ok = false;
                parts.push_back(json{{"document", kv.second->documentName()}, {"result", res}, {"moved", moved.count(kv.first) > 0}});
            }
            p->expression(saved);
            r["result"] = ok ? "match" : "mismatch";
            r["parts"] = parts;
            results.push_back(r);
        }
    }
    return results;
}

json importFileInner(const fs::path& file, const json& opts, Ptr<Document>& createdDoc);

std::string rootDocName(const json& model)
{
    const json* d = findDoc(model, model.value("root", ""));
    return d ? d->value("name", "") : std::string();
}

// The import with progress and cancel: a cancelled import closes every document it created, unsaved, so
// nothing half-built is left behind (a defined state).
json importFile(const fs::path& file, const json& opts)
{
    if (opts.value("cancelAfter", -1) >= 0) progressCancelAfter(opts["cancelAfter"].get<int>());
    // the dialog shows for job-driven imports too (someone may be watching Fusion); "progress": false hides it
    progressBegin("Import NCX: " + file.filename().u8string(), 100, opts.value("progress", true));
    size_t docsBefore = trackedDocuments().size();
    Ptr<Document> created;
    json res;
    try
    {
        res = importFileInner(file, opts, created);
    }
    catch (const Cancelled& c)
    {
        // every document this import made (the design, the scratch STEP import of the reference) is closed unsaved
        std::vector<Ptr<Document>> docs = trackedDocuments();
        for (size_t i = docsBefore; i < docs.size(); i++)
            if (docs[i] && docs[i]->isValid()) docs[i]->close(false);
        if (created && created->isValid()) created->close(false);
        res = json{{"ok", true}, {"status", "cancelled"}, {"cancelledAt", c.where},
                   {"report", json::array({json{{"document", file.filename().u8string()}, {"item", "import"}, {"result", "cancelled"},
                                                {"reason", "cancelled by the user at " + c.where + "; everything this import made was closed"}}})}};
    }
    if (res.is_object()) res["progress"] = progressTimeline();
    progressEnd();
    return res;
}

json importFileInner(const fs::path& file, const json& opts, Ptr<Document>& createdDoc)
{
    auto t0 = std::chrono::steady_clock::now();
    Report report;
    json out{{"ok", true}, {"file", file.u8string()}};
    progressPhase("read payload");
    std::string stepText = readFile(file);
    if (stepText.empty()) return json{{"ok", false}, {"error", "cannot read " + file.u8string()}};
    CoreResult pr = corePayloadRead(stepText);
    out["payloadStatus"] = pr.statusName;
    NoticeId noticeName = NoticeId::fromHeader(pr.report.contains("fields") ? pr.report["fields"] : json::object());
    json model;
    std::string plainReason;
    if (pr.status != NCX_OK)
        plainReason = pr.statusName == "NO_PAYLOAD" ? "the file carries no NCX payload" : "the NCX payload is unreadable (" + pr.statusName + ")";
    else
    {
        try
        {
            model = json::parse(pr.data);
        }
        catch (const std::exception& e)
        {
            plainReason = std::string("the payload is not valid JSON: ") + e.what();
        }
    }
    // the progress total: a step per feature, component and mate, and a few per document (reference, checks)
    if (model.is_object() && model.contains("documents"))
    {
        int n = 4;
        for (auto& d : model["documents"])
            n += 4 + static_cast<int>(d.value("features", json::array()).size() + d.value("components", json::array()).size() +
                                      d.value("mates", json::array()).size() + d.value("flex", json::array()).size());
        progressSetTotal(n);
    }
    // the reference: the file's own geometry
    Reference ref;
    if (plainReason.empty())
    {
        progressPhase("reference");
        std::set<std::string> only;   // developer: "only": [part names] rebuilds only those parts (and their sources)
        for (auto& n : opts.value("only", json::array())) only.insert(n.get<std::string>());
        ref = loadReference(file, model, noticeName, opts.value("faces", true), only);
        if (!ref.ok) return json{{"ok", false}, {"error", "reference geometry: " + ref.error}};
        out["notice"] = ref.noticeFound.empty() ? "not found" : ref.noticeFound;
        json stale = coreCompare(model["fingerprint"], ref.fingerprint, "exchange");
        out["staleCheck"] = stale.value("result", "?");
        if (stale.value("result", "") != "match") out["staleDetail"] = stale;   // which bodies, which quantities
        // Fusion's STEP reader measures some freeform bodies of its own files differently (a GrabCAD caster's modelled
        // thread, read back: 2e-3 in volume, 5e-2 in moments, while OpenCASCADE and SolidWorks read the same file within
        // tolerance). A mismatch only there, every body present and each volume within 1%, is reported and the rebuild
        // goes on: each part is still checked against the payload once rebuilt, so an edit there still shows
        bool readerOnly = stale.value("result", "") == "mismatch";
        std::set<long long> oddBodies;
        if (readerOnly)
            for (auto& d : stale.value("differences", json::array()))
            {
                if (d.value("severity", "") != "mismatch") continue;
                if (!d.contains("body") || !d["body"].is_number_integer()) { readerOnly = false; break; }
                long long bi = d["body"].get<long long>();
                const json& eb = model["fingerprint"]["bodies"];
                bool ff = bi >= 0 && bi < static_cast<long long>(eb.size()) && eb[bi].value("freeform", false);
                bool smallVol = d.value("quantity", "") != "volume" ||
                                std::fabs(d.value("actual", 0.0) - d.value("expected", 0.0)) <= 0.01 * std::fabs(d.value("expected", 1.0));
                if (!ff || !smallVol) { readerOnly = false; break; }
                oddBodies.insert(bi);
            }
        if (readerOnly)
            report.add(file.filename().u8string(), "file", "note",
                       std::to_string(oddBodies.size()) + " freeform bod" + (oddBodies.size() == 1 ? "y" : "ies") +
                           " of the file measure a little differently in Fusion's STEP reader (" + differencesText(stale) +
                           "); the rest matches its payload, so the model is rebuilt and each part checked");
        else if (stale.value("result", "") == "mismatch")
            plainReason = "the file's geometry no longer matches its payload (" + differencesText(stale) +
                          "): it was changed after export";
    }
    progressStep("building the design");
    Ptr<Document> doc;
    Ptr<Design> des = newDesign(doc);
    createdDoc = doc;
    if (!des) return json{{"ok", false}, {"error", "cannot create a design: " + lastError()}};
    // upright in Fusion's own frame (FORMAT-SPEC 4 `orientation`): the model goes into one component under one
    // rotation when the file's up axis is not Fusion's; inside it everything stays in the file's frame
    const std::string fileUp = fileUpAxis(model);
    const bool zUp = fusionIsZUp();
    out["fileUp"] = fileUp;
    out["fusionUp"] = zUp ? "z" : "y";
    auto makeWrapper = [&](const Ptr<Design>& d) -> Ptr<Component> {
        modelWrapper() = nullptr;
        d->rootComponent()->attributes()->add("NCX", "orientation", fileUp);
        Ptr<Matrix3D> r = uprightRotation(fileUp, zUp);
        if (!r) return d->rootComponent();
        Ptr<Occurrence> w = d->rootComponent()->occurrences()->addNewComponent(r);
        if (!w) return d->rootComponent();
        const json* rd = findDoc(model, model.is_object() ? model.value("root", "") : "");
        w->component()->name(rd ? rd->value("name", "") : file.stem().u8string());
        w->component()->attributes()->add("NCX", "wrapper", fileUp);
        w->isGrounded(true);
        modelWrapper() = w;
        return w->component();
    };
    struct WrapperReset
    {
        ~WrapperReset() { modelWrapper() = nullptr; }
    } wrapperReset;
    Ptr<Component> root = makeWrapper(des);
    json verification = json::array();
    if (!plainReason.empty())
    {
        progressPhase("fallback");
        std::string w = importPlain(file, root, model.is_object() ? model : json{{"documents", json::array()}}, noticeName);
        report.add(file.filename().u8string(), "file", "fallback", plainReason + "; imported as plain geometry" + (w.empty() ? "" : " (" + w + ")"));
        out["status"] = "fallback";
    }
    else
    {
        const json* rootDoc = findDoc(model, model.value("root", ""));
        if (!rootDoc) return json{{"ok", false}, {"error", "payload root document missing"}};
        bool single = model["documents"].size() == 1 && rootDoc->value("kind", "") == "part";
        if (!single)
        {
            std::string st;
            importAssembly(model, ref, des, report, opts, file, verification, st);
            out["status"] = st;
        }
        else if (rootDoc->value("plain", false))
        {
            // a plain part (FORMAT-SPEC 4: no history in the source, imported geometry only): the file's geometry as
            // it is, with its properties; nothing rebuilt or compared, a note rather than a fallback
            progressPhase("plain part");
            root->name(rootDoc->value("name", ""));
            std::string w = importPlain(file, root, model, noticeName);
            Ptr<Component> plainComp = root;
            if (root->bRepBodies()->count() == 0 && root->occurrences()->count() > 0) plainComp = root->occurrences()->item(0)->component();
            Replayer rp(report, model, file);
            rp.replayPropertiesOn(*rootDoc, plainComp);
            report.add(rootDoc->value("name", ""), "part", "note",
                       "no history in the source (imported geometry): its bodies as the file has them" + (w.empty() ? "" : " (" + w + ")"));
            verification.push_back(json{{"document", rootDoc->value("name", "")}, {"mass", "plain"}});
            out["status"] = "plain";
        }
        else
        {
            root->name(rootDoc->value("name", ""));
            setMeasureContext(modelWrapper());
            // sheet-metal features build in the active component: the wrapper's, when there is one
            // (only then: a feature made while an occurrence is active keeps proxies of its inputs)
            if (modelWrapper() && hasSheetMetal(*rootDoc)) modelWrapper()->activate();
            Replayer rp(report, model, file);
            rp.occurrence = modelWrapper();
            rp.trace = opts.value("trace", false);
            if (ref.components.count("")) rp.refBodies = &ref.components[""].bodies;
            progressPhase("replay");
            rp.replayPart(*rootDoc, root);
            if (modelWrapper()) des->activateRootComponent();
            setMeasureContext(modelWrapper());
            progressPhase("verify mass");
            progressStep(rootDoc->value("name", "") + ": checking mass properties");
            nameBodies(root, *rootDoc);
            // mass properties: the rebuilt part against the payload (native; exchange where STEP was involved)
            json expected = expectedFingerprint(*rootDoc);
            json actual = fingerprintOf(componentBodies(root, true));
            // splines without their NURBS (files before core 0.2.0): Fusion fits a spline through the fit points a
            // little differently from the source, so the part is compared at exchange tolerance. Freeform bodies
            // (FORMAT-SPEC 6): Fusion's own loft or sweep surface is another kernel's fit of the same intent, so
            // these too are compared under exchange, where the freeform tolerances apply to them only.
            bool splines = false, freeform = false;
            for (auto& nf : (*rootDoc)["features"])
                if (nf.contains("def") && nf["def"].contains("entities"))
                    for (auto& e : nf["def"]["entities"])
                        if (e.value("type", "") == "spline" && !e.contains("nurbs")) splines = true;
            for (auto& b : expected["bodies"])
                if (b.value("freeform", false)) freeform = true;
            for (auto& b : actual["bodies"])
                if (b.value("freeform", false)) freeform = true;
            const char* profile = (rp.resultsApplied || splines || freeform || !rp.looseReason.empty()) ? "exchange" : "native";
            json cmp = coreCompare(expected, actual, profile);
            if (!rp.looseReason.empty() && !rp.resultsApplied && cmp.value("result", "") != "mismatch" &&
                coreCompare(expected, actual, "native").value("result", "") == "mismatch")
                report.add(rootDoc->value("name", ""), "tolerance", "note", rp.looseReason + "; the part matches at exchange tolerance");
            if ((splines || freeform) && !rp.resultsApplied && cmp.value("result", "") != "mismatch" &&
                coreCompare(expected, actual, "native").value("result", "") == "mismatch")
                report.add(rootDoc->value("name", ""), splines ? "splines" : "freeform bodies", "note",
                           splines ? "Fusion fits splines through the payload's fit points slightly differently from the source; the part matches at exchange tolerance"
                                   : "freeform (spline) surfaces are Fusion's own fit; the part matches at the exchange profile's freeform tolerance (FORMAT-SPEC 6)");
            json v{{"document", rootDoc->value("name", "")}, {"mass", cmp.value("result", "?")}, {"tolerance", profile}};
            if (cmp.value("result", "") != "match") v["massDetail"] = cmp;
            if (cmp.value("result", "") == "mismatch")
            {
                // geometry wins: a wrong parametric model is worse than an honest plain one
                std::string why = "rebuilt geometry differs in " + differencesText(cmp);
                report.add(rootDoc->value("name", ""), "part", "fallback", why + "; replaced by the file's own geometry");
                out["status"] = "fallback";
                if (!opts.value("keepPartial", false))
                {
                    doc->close(false);
                    des = newDesign(doc);
                    createdDoc = doc;
                    root = makeWrapper(des);
                }
                else
                {
                    Ptr<Occurrence> o = root->occurrences()->addNewComponent(Matrix3D::create());
                    root = o->component();
                }
                progressPhase("fallback");
                importPlain(file, root, model, noticeName);
                // the plain geometry keeps the document's material and appearances (its bodies are in the
                // component the STEP import made, or in root)
                Ptr<Component> plainComp = root;
                if (root->bRepBodies()->count() == 0 && root->occurrences()->count() > 0) plainComp = root->occurrences()->item(0)->component();
                rp.replayPropertiesOn(*rootDoc, plainComp);
                recordDocument(plainComp, *rootDoc);   // provenance (host.h): the export can give the source's history back
            }
            else
            {
                out["status"] = "rebuilt";
                recordDocument(root, *rootDoc);   // provenance (host.h)
                // structure: every mapped feature present under its name
                json missing = json::array();
                for (auto& nf : (*rootDoc)["features"])
                {
                    if (nf.value("status", "") == "opaque" || rp.markers.count(nf.value("id", ""))) continue;
                    auto it = rp.features.find(nf.value("id", ""));
                    if (it == rp.features.end() || !it->second) missing.push_back(nf.value("name", ""));
                    else if (featureName(it->second) != nf.value("name", "") &&
                             featureName(it->second) != nf.value("name", "") + " (result)")
                        missing.push_back(nf.value("name", "") + " (named " + featureName(it->second) + ")");
                }
                v["structure"] = missing.empty() ? "match" : "mismatch";
                if (!missing.empty()) v["structureMissing"] = missing;
                // face by face, against the file's own B-rep
                if (opts.value("faces", true) && ref.components.count(""))
                {
                    progressPhase("verify faces");
                    progressStep(rootDoc->value("name", "") + ": checking faces against the file");
                    json fc = faceCompare(ref.components[""].bodies, componentBodies(root, true), 0.001, rootDoc->value("name", "") + ": checking faces");
                    v["faces"] = fc.value("result", "?");
                    if (fc.value("result", "") == "mismatch")
                    {
                        json brief{{"unmatched", fc.value("unmatched", json::array()).size()},
                                   {"extra", fc.value("extra", json::array()).size()},
                                   {"area", fc.value("area", json::array()).size()}};
                        v["facesDetail"] = brief;
                    }
                }
            }
            if (out.value("status", "") == "rebuilt" && rootDoc->contains("flex"))
            {
                std::map<std::string, Replayer*> rebuilt{{rootDoc->value("id", ""), &rp}};
                progressPhase("flex");
                std::set<std::string> skip;
                if (rp.resultsApplied) skip.insert(rootDoc->value("id", ""));
                json fx = flexCheck(model, rebuilt, skip);
                std::string agg = "match";
                for (auto& x : fx)
                    if (x.value("result", "") != "match") agg = x.value("result", "");
                v["flex"] = agg;
                v["flexDetail"] = fx;
            }
            verification.push_back(v);
        }
    }
    if (modelWrapper() && model.is_object())
        report.add(rootDocName(model), "orientation", "note",
                   "placed upright: the file's up axis is " + fileUp + " and Fusion is " + (zUp ? "Z" : "Y") +
                       "-up, so the model is in one component (" + modelWrapper()->component()->name() +
                       ") turned to Fusion's up axis; inside it the model keeps the file's coordinates");
    setMeasureContext(nullptr);
    out["report"] = report.entries;
    out["verification"] = verification;
    out["jointFrames"] = jointFrames();
    jointFrames() = json::array();
    // counts for the summary line
    std::map<std::string, int> counts;
    for (auto& e : report.entries) counts[e["result"].get<std::string>()]++;
    out["counts"] = counts;
    out["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (opts.value("close", false) && doc) out["closed"] = doc->close(false);
    return out;
}

json importJob(const json& req)
{
    if (!req.contains("file")) return json{{"ok", false}, {"error", "import needs a file"}};
    return importFile(fs::u8path(req["file"].get<std::string>()), req);
}

namespace {

// "A, B, C and 4 more"
std::string nameList(const std::vector<std::string>& names, size_t most = 5)
{
    std::string s;
    for (size_t i = 0; i < names.size() && i < most; i++) s += (i ? ", " : "") + names[i];
    if (names.size() > most) s += " and " + std::to_string(names.size() - most) + " more";
    return s;
}

} // namespace

// The import report box in plain words. Most people who open an NCX file are not CAD specialists: a list of every
// approximated feature reads as a list of errors. So the box says the model came in, names only the parts worth
// looking at, and counts the rest; the saved report file keeps every entry.
//   -> {"text", "clean" (nothing to say beyond "it worked"), "check" (a part's shape may differ: a warning)}
json importSummary(const json& res)
{
    const json& report = res.contains("report") ? res["report"] : json::array();
    // parts replaced by the file's own geometry: right shape, no feature history (their features don't count)
    std::set<std::string> fixedSet;
    bool wholeFilePlain = false;
    for (auto& e : report)
        if (e.value("result", "") == "fallback")
        {
            if (e.value("item", "") == "file") wholeFilePlain = true;
            else fixedSet.insert(e.value("document", ""));
        }
    // parts whose mass properties (volume, area, centroid, inertia) did not match and that were not replaced: the
    // one thing worth a warning. A face-by-face difference with matching mass properties is the same surfaces cut
    // into faces differently (the folding step's Cheek Opposite: a few small faces split another way), which the
    // report file lists but the box does not
    std::vector<std::string> check;
    for (auto& v : res.value("verification", json::array()))
    {
        std::string d = v.value("document", "");
        if (fixedSet.count(d)) continue;
        if (v.contains("mass") && v["mass"].is_string() && v["mass"] == "mismatch") check.push_back(d);
    }
    // features built another way (approximate) or left out (failed) in parts that kept their history; notes are
    // information only and are not mentioned
    int different = 0;
    for (auto& e : report)
    {
        std::string r = e.value("result", "");
        if (r != "rebuilt" && r != "note" && r != "fallback" && !fixedSet.count(e.value("document", ""))) different++;
    }
    std::vector<std::string> fixed(fixedSet.begin(), fixedSet.end());

    json out{{"check", !check.empty()}};
    if (wholeFilePlain)
    {
        out["text"] = "Your model has been imported into Fusion.\n\n"
                      "This is a short report on how the import went. It is not an error message.\n\n"
                      "- The model has the right shape, but it came in as fixed shapes without its editable feature "
                      "history (the NCX information in the file could not be used).";
        return out;
    }
    if (check.empty() && fixed.empty() && different == 0)
    {
        out["clean"] = true;
        out["text"] = "Your model has been imported into Fusion, and everything matches the original.";
        return out;
    }
    std::string t = "Your model has been imported into Fusion.\n\n"
                    "This is a short report on how the import went. It is not an error message.\n\n";
    if (check.empty())
        t += "- Every part was checked against the original, and its shape matches.\n";
    else
        t += "- Please look over " + std::string(check.size() == 1 ? "this part" : "these parts") +
             ": the check found the shape may differ from the original: " + nameList(check) + ".\n";
    if (!fixed.empty())
        t += "- " + nameList(fixed) + (fixed.size() == 1 ? " has" : " have") +
             " the right shape, but came in without editable feature history (Fusion could not rebuild "
             + (fixed.size() == 1 ? "it" : "them") + " step by step).\n";
    if (different > 0)
        t += "- " + std::to_string(different) + (different == 1 ? " item was" : " items were") +
             " carried over slightly differently, as Fusion has no exact match" + (check.empty() ? ". This does not change the shape.\n" : ".\n");
    if (!t.empty() && t.back() == '\n') t.pop_back();
    out["text"] = t;
    return out;
}

// The full report as a text file in Documents\NCX reports (one per imported file, replaced on the next import of the
// same file name), for whoever helps the user. Returns the path, or "" when it could not be written.
std::string writeImportReport(const fs::path& source, const json& res, const std::string& summary)
{
    PWSTR p = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) dir = fs::path(p) / L"NCX reports";
    CoTaskMemFree(p);
    if (dir.empty()) return "";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file = dir / (source.stem().wstring() + L" - import report.txt");
    std::ofstream f(file, std::ios::binary);
    if (!f) return "";
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", std::localtime(&now));
    f << "NCX import report\r\nFile: " << source.u8string() << "\r\nImported: " << ts << "\r\n\r\n";
    std::string s = summary;   // the box's text, with Windows line ends
    for (size_t i = 0; (i = s.find('\n', i)) != std::string::npos; i += 2) s.replace(i, 1, "\r\n");
    f << s << "\r\n\r\n";
    f << "Shape checks against the original (per part)\r\n";
    for (auto& v : res.value("verification", json::array()))
    {
        f << "  " << v.value("document", "");
        for (const char* k : {"mass", "faces", "structure", "flex"})
            if (v.contains(k) && v[k].is_string()) f << "   " << k << ": " << v[k].get<std::string>();
        f << "\r\n";
    }
    f << "\r\nEvery item (part | item | result | reason)\r\n";
    for (auto& e : res.value("report", json::array()))
        f << "  " << e.value("document", "") << " | " << e.value("item", "") << " | " << e.value("result", "")
          << (e.value("reason", "").empty() ? "" : " | " + e.value("reason", "")) << "\r\n";
    if (!f) return "";
    return file.u8string();
}

json uiImport(const json& req)
{
    Ptr<Application> app = Application::get();
    Ptr<UserInterface> ui = app->userInterface();
    std::string path = req.value("file", "");   // developer: a test passes the file instead of the dialog
    if (path.empty())
    {
        Ptr<FileDialog> dlg = ui->createFileDialog();
        dlg->title("Import NCX");
        dlg->filter("NCX / STEP files (*.step;*.stp);;All files (*.*)");
        dlg->isMultiSelectEnabled(false);
        if (dlg->showOpen() != DialogOK) return json{{"ok", true}, {"cancelled", true}};
        path = dlg->filename();
    }
    // No face-by-face check from the menu: the mass check decides everything the user is told (and a fallback),
    // while the face check took over half of a large import (the folding step: 370 s of 680 on a fast PC, its
    // thousands of distance queries). Test jobs ({"cmd": "import"}) keep it; a developer can ask for it here.
    json o{{"faces", req.value("faces", false)}, {"ui", true}};
    if (req.contains("cancelAfter")) o["cancelAfter"] = req["cancelAfter"];
    json res = importFile(fs::u8path(path), o);
    if (res.value("status", "") == "cancelled")
    {
        if (!req.value("quiet", false)) ui->messageBox("NCX import cancelled; nothing was left open.", "NCX");
        return res;
    }
    if (!res.value("ok", false))
    {
        if (!req.value("quiet", false))
            ui->messageBox("NCX could not import this file.\n\n" + res.value("error", "?"), "NCX import", OKButtonType, CriticalIconType);
        return res;
    }
    json summary = importSummary(res);
    std::string reportFile = writeImportReport(fs::u8path(path), res, summary.value("text", ""));
    std::string box = summary.value("text", "");
    if (!summary.value("clean", false))
        box += "\n\nClick OK to carry on." + (reportFile.empty() ? "" : "\n\nFull details, for anyone helping you, are saved in:\n" + reportFile);
    res["summary"] = box;
    if (!reportFile.empty()) res["reportFile"] = reportFile;
    if (req.value("quiet", false)) return res;   // developer test: no modal box
    ui->messageBox(box, "NCX import report", OKButtonType, summary.value("check", false) ? WarningIconType : InformationIconType);
    return res;
}



} // namespace ncxf
