// Import NCX: read the payload, check it against the file's geometry, rebuild, verify, report.
//
//   job {"cmd": "import", "file": "...ncx.step", "close": bool (close the result unsaved, for tests),
//        "faces": bool (face-by-face check, default true)}
//   -> {"ok", "status": "rebuilt" | "fallback" | "failed", "report": [...], "verification": [...], ...}
#include "host.h"
#include "reference.h"
#include "replay.h"

#include <chrono>

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
        if (stale.value("result", "") == "mismatch")
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
    json o{{"faces", true}, {"ui", true}};
    if (req.contains("cancelAfter")) o["cancelAfter"] = req["cancelAfter"];
    json res = importFile(fs::u8path(path), o);
    if (res.value("status", "") == "cancelled")
    {
        if (!req.value("quiet", false)) ui->messageBox("NCX import cancelled; nothing was left open.", "NCX");
        return res;
    }
    // a one-line status when everything carried; the report when something needs attention
    std::string lines;
    int attention = 0;
    for (auto& e : res.value("report", json::array()))
    {
        std::string r = e.value("result", "");
        if (r == "rebuilt") continue;
        attention++;
        lines += e.value("document", "") + " / " + e.value("item", "") + ": " + r + (e.value("reason", "").empty() ? "" : " - " + e.value("reason", "")) + "\n";
    }
    for (auto& v : res.value("verification", json::array()))
    {
        for (const char* k : {"mass", "faces", "structure"})
            if (v.contains(k) && v[k] != "match" && v[k] != "topology" && v[k] != "count_only")
            {
                attention++;
                lines += v.value("document", "") + ": " + k + " check " + v[k].get<std::string>() + "\n";
            }
    }
    res["summary"] = lines;
    if (req.value("quiet", false)) return res;   // developer test: no modal box
    if (!res.value("ok", false)) ui->messageBox("NCX import failed: " + res.value("error", "?"));
    else if (attention == 0) ui->messageBox("NCX import: everything rebuilt and verified.", "NCX");
    else ui->messageBox("NCX import (" + res.value("status", "") + "):\n\n" + lines, "NCX");
    return res;
}



} // namespace ncxf
