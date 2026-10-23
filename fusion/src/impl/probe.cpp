// Developer experiments run through the job interface ({"cmd": "probe", "what": ...}).
// Kept because FINDINGS.md quotes their results.
#include "common.h"
#include "host.h"

namespace ncxf {

namespace {

json bodyInfo(const Ptr<BRepBody>& b)
{
    json j{{"name", b->name()}, {"solid", b->isSolid()}, {"volume_mm3", b->volume() * 1000.0},
           {"area_mm2", b->area() * 100.0}, {"faces", b->faces()->count()}, {"edges", b->edges()->count()}};
    if (Ptr<BoundingBox3D> bb = b->boundingBox())
        j["bbox"] = {cm2mm(bb->minPoint()->x()), cm2mm(bb->minPoint()->y()), cm2mm(bb->minPoint()->z()),
                     cm2mm(bb->maxPoint()->x()), cm2mm(bb->maxPoint()->y()), cm2mm(bb->maxPoint()->z())};
    return j;
}

json walk(const Ptr<Component>& c)
{
    json j{{"component", c->name()}, {"bodies", json::array()}, {"occurrences", json::array()}};
    for (size_t i = 0; i < c->bRepBodies()->count(); i++) j["bodies"].push_back(bodyInfo(c->bRepBodies()->item(i)));
    for (size_t i = 0; i < c->occurrences()->count(); i++)
    {
        Ptr<Occurrence> o = c->occurrences()->item(i);
        json oj = walk(o->component());
        oj["occurrence"] = o->name();
        Ptr<Matrix3D> m = o->transform2();
        std::vector<double> a = m->asArray();
        oj["transform"] = a;
        j["occurrences"].push_back(oj);
    }
    return j;
}

} // namespace

json probe(const json& req)
{
    std::string what = req.value("what", "");
    Ptr<Application> app = Application::get();
    if (what == "stepdoc")
    {
        std::string file = req.at("file");
        Ptr<ImportManager> im = app->importManager();
        Ptr<STEPImportOptions> opt = im->createSTEPImportOptions(file);
        if (!opt) return json{{"ok", false}, {"error", "createSTEPImportOptions: " + lastError()}};
        Ptr<Document> doc = im->importToNewDocument(opt);
        if (!doc) return json{{"ok", false}, {"error", "importToNewDocument: " + lastError()}};
        trackDocument(doc);
        json res{{"ok", true}, {"docName", doc->name()}};
        if (req.value("fp", false))
        {
            Ptr<Design> d0 = doc->products()->itemByProductType("DesignProductType");
            json fps = json::array();
            for (size_t i = 0; i < d0->rootComponent()->bRepBodies()->count(); i++)
            {
                Ptr<BRepBody> b = d0->rootComponent()->bRepBodies()->item(i);
                json f = bodyFingerprint(b);
                Ptr<PhysicalProperties> pp = b->getPhysicalProperties(VeryHighCalculationAccuracy);
                double xx, yy, zz, xy, yz, xz;
                pp->getXYZMomentsOfInertia(xx, yy, zz, xy, yz, xz);
                f["raw"] = {{"density", pp->density()}, {"mass", pp->mass()}, {"xyz", {xx, yy, zz, xy, yz, xz}},
                            {"com", {pp->centerOfMass()->x(), pp->centerOfMass()->y(), pp->centerOfMass()->z()}}};
                fps.push_back(f);
            }
            res["fingerprints"] = fps;
        }
        Ptr<Design> des = doc->products()->itemByProductType("DesignProductType");
        if (des)
        {
            res["designType"] = static_cast<int>(des->designType());
            res["tree"] = walk(des->rootComponent());
            res["timeline"] = des->timeline() ? static_cast<int>(des->timeline()->count()) : -1;
        }
        if (req.value("close", true)) res["closed"] = doc->close(false);
        return res;
    }
    if (what == "timeline")
    {
        Ptr<Design> des = app->activeProduct();
        if (!des) return json{{"ok", false}, {"error", "no active design"}};
        json items = json::array();
        Ptr<Timeline> tl = des->timeline();
        for (size_t i = 0; tl && i < tl->count(); i++)
        {
            Ptr<TimelineObject> t = tl->item(i);
            Ptr<Base> e = t->entity();
            json j{{"name", t->name()}, {"type", e ? e->objectType() : "?"}, {"suppressed", t->isSuppressed()},
                   {"health", static_cast<int>(t->healthState())}, {"message", t->errorOrWarningMessage()}};
            if (Ptr<RectangularPatternFeature> rp = e)
            {
                json els = json::array();
                Ptr<PatternElements> pe = rp->patternElements();
                for (size_t k = 0; pe && k < pe->count(); k++)
                {
                    std::vector<double> a = pe->item(k)->transform()->asArray();
                    els.push_back(json{{"id", pe->item(k)->id()}, {"name", pe->item(k)->name()}, {"suppressed", pe->item(k)->isSuppressed()},
                                       {"t", {a[3] * 10, a[7] * 10, a[11] * 10}}});
                }
                j["elements"] = els;
            }
            items.push_back(j);
        }
        return json{{"ok", true}, {"timeline", items}};
    }
    if (what == "patterntest")
    {
        Ptr<Document> doc = app->documents()->add(FusionDesignDocumentType);
        trackDocument(doc);
        Ptr<Design> des = doc->products()->itemByProductType("DesignProductType");
        Ptr<Component> c = des->rootComponent();
        Ptr<Sketch> sk = c->sketches()->add(c->xYConstructionPlane());
        sk->sketchCurves()->sketchCircles()->addByCenterRadius(Point3D::create(8, 1.5, 0), 0.5);
        Ptr<ExtrudeFeatureInput> ei = c->features()->extrudeFeatures()->createInput(sk->profiles()->item(0), NewBodyFeatureOperation);
        ei->setDistanceExtent(false, ValueInput::createByReal(2));
        Ptr<ExtrudeFeature> ex = c->features()->extrudeFeatures()->add(ei);
        json res{{"ok", true}};
        res["bodiesAfterExtrude"] = c->bRepBodies()->count();
        Ptr<ObjectCollection> oc = ObjectCollection::create();
        oc->add(c->bRepBodies()->item(0));
        Ptr<RectangularPatternFeatureInput> in = c->features()->rectangularPatternFeatures()->createInput(
            oc, c->zConstructionAxis(), ValueInput::createByReal(4), ValueInput::createByReal(-1.5), SpacingPatternDistanceType);
        in->patternComputeOption(req.value("adjust", true) ? AdjustPatternCompute : IdenticalPatternCompute);
        if (req.value("q2", false)) in->quantityTwo(ValueInput::createByReal(1));
        res["quantityTwoDefault"] = in->quantityTwo() ? in->quantityTwo()->realValue() : -1;
        Ptr<RectangularPatternFeature> f = c->features()->rectangularPatternFeatures()->add(in);
        res["made"] = (bool)f;
        res["bodiesAfterPattern"] = c->bRepBodies()->count();
        res["elements"] = f ? f->patternElements()->count() : 0;
        if (f && req.value("suppress", true)) res["suppressRet"] = f->patternElements()->item(2)->isSuppressed(true), res["suppressErr"] = lastError();
        res["bodiesAfterSuppress"] = c->bRepBodies()->count();
        res["elem2Suppressed"] = f ? f->patternElements()->item(2)->isSuppressed() : false;
        des->computeAll();
        res["bodiesAfterCompute"] = c->bRepBodies()->count();
        if (f && req.value("viaTimeline", false))
        {
            f->timelineObject()->rollTo(true);
            int which = req.value("which", 1);
            res["rollRet"] = f->patternElements()->item(which)->isSuppressed(true);
            res["rollErr"] = lastError();
            des->timeline()->moveToEnd();
            res["bodiesAfterRollSuppress"] = c->bRepBodies()->count();
        }
        res["elementsAfter"] = f ? f->patternElements()->count() : 0;
        if (req.value("close", true)) doc->close(false);
        return res;
    }
    if (what == "movetest")
    {
        Ptr<Document> doc = app->documents()->add(FusionDesignDocumentType);
        trackDocument(doc);
        Ptr<Design> des = doc->products()->itemByProductType("DesignProductType");
        Ptr<Component> root = des->rootComponent();
        json res{{"ok", true}};
        Ptr<Occurrence> o = root->occurrences()->addNewComponent(Matrix3D::create());
        Ptr<Component> c = o->component();
        Ptr<Sketch> sk = c->sketches()->add(c->xYConstructionPlane());
        sk->sketchCurves()->sketchCircles()->addByCenterRadius(Point3D::create(0, 0, 0), 0.5);
        Ptr<ExtrudeFeatureInput> ei = c->features()->extrudeFeatures()->createInput(sk->profiles()->item(0), NewBodyFeatureOperation);
        ei->setDistanceExtent(false, ValueInput::createByReal(2));
        c->features()->extrudeFeatures()->add(ei);
        Ptr<Matrix3D> m = Matrix3D::create();
        m->translation(Vector3D::create(0, 0, 6));
        std::string mode = req.value("mode", "transform2");
        bool ok = false;
        if (mode == "transform2") ok = o->transform2(m);
        else if (mode == "transform") ok = o->transform(m);
        else if (mode == "initial") ok = o->initialTransform(m);
        res["setOk"] = ok;
        res["err"] = lastError();
        res["pending"] = des->snapshots()->hasPendingSnapshot();
        res["zAfterSet"] = o->transform2()->asArray()[11];
        if (req.value("snap", true) && des->snapshots()->hasPendingSnapshot()) res["snap"] = (bool)des->snapshots()->add();
        res["zAfterSnap"] = o->transform2()->asArray()[11];
        Ptr<Matrix3D> m2 = Matrix3D::create();
        m2->translation(Vector3D::create(0, 0, 9));
        Ptr<Occurrence> o2 = root->occurrences()->addExistingComponent(c, m2);
        res["zAfterAdd"] = o->transform2()->asArray()[11];
        res["o2z"] = o2->transform2()->asArray()[11];
        Ptr<Occurrence> o3 = root->occurrences()->addNewComponent(Matrix3D::create());
        res["o2zAfterNew"] = o2->transform2()->asArray()[11];
        if (req.value("delFirst", false)) res["del"] = o->deleteMe();
        des->computeAll();
        if (!req.value("delFirst", false)) res["zAfterCompute"] = o->transform2()->asArray()[11];
        res["o2zAfterCompute"] = o2->transform2()->asArray()[11];
        res["o2bodies"] = o2->bRepBodies()->count();
        doc->close(false);
        return res;
    }
    if (what == "sketches")
    {
        Ptr<Design> des = app->activeProduct();
        if (!des) return json{{"ok", false}, {"error", "no active design"}};
        json out = json::array();
        Ptr<Component> root = des->rootComponent();
        std::vector<Ptr<Component>> comps{root};
        for (size_t i = 0; i < root->allOccurrences()->count(); i++) comps.push_back(root->allOccurrences()->item(i)->component());
        for (auto& c : comps)
            for (size_t i = 0; i < c->sketches()->count(); i++)
            {
                Ptr<Sketch> sk = c->sketches()->item(i);
                json sj{{"component", c->name()}, {"name", sk->name()}, {"origin", ptMm(sk->origin())}, {"x", vecOf(sk->xDirection())},
                        {"y", vecOf(sk->yDirection())}, {"profiles", sk->profiles()->count()}, {"curves", json::array()}};
                for (size_t k = 0; k < sk->sketchCurves()->count(); k++)
                {
                    Ptr<SketchCurve> cv = sk->sketchCurves()->item(k);
                    json cj{{"type", cv->objectType()}, {"ref", cv->isReference()}, {"constr", cv->isConstruction()}};
                    if (Ptr<SketchCircle> ci = cv) cj["center"] = ptMm(ci->centerSketchPoint()->worldGeometry());
                    if (Ptr<SketchLine> l = cv) cj["a"] = ptMm(l->startSketchPoint()->worldGeometry()), cj["b"] = ptMm(l->endSketchPoint()->worldGeometry());
                    sj["curves"].push_back(cj);
                }
                out.push_back(sj);
            }
        return json{{"ok", true}, {"sketches", out}};
    }
    if (what == "params")
    {
        // every parameter of the active design: name, expression, value (cm / rad), and what it belongs to
        Ptr<Design> des = app->activeProduct();
        if (!des) return json{{"ok", false}, {"error", "no active design"}};
        json out = json::array();
        Ptr<ParameterList> ps = des->allParameters();
        for (size_t i = 0; ps && i < ps->count(); i++)
        {
            Ptr<Parameter> p = ps->item(i);
            json pj{{"name", p->name()}, {"expression", p->expression()}, {"value", p->value()}};
            if (Ptr<ModelParameter> mp = p) pj["role"] = mp->role();
            out.push_back(pj);
        }
        return json{{"ok", true}, {"params", out}};
    }
    if (what == "ui")
    {
        // where the add-in's commands are: the File menu and the Insert panel
        Ptr<UserInterface> ui = app->userInterface();
        json r{{"ok", true}};
        Ptr<Toolbar> qat = ui->toolbars()->itemById("QAT");
        Ptr<DropDownControl> file = qat ? Ptr<DropDownControl>(qat->controls()->itemById("FileSubMenuCommand")) : nullptr;
        r["fileMenuImport"] = file && file->controls()->itemById("NCX_Import");
        r["fileMenuExport"] = file && file->controls()->itemById("NCX_Export");
        Ptr<ToolbarPanel> panel = ui->allToolbarPanels()->itemById("InsertPanel");
        r["insertPanelImport"] = panel && panel->controls()->itemById("NCX_Import");
        return r;
    }
    if (what == "tempfile")
    {
        std::string file = req.at("file");
        Ptr<BRepBodies> bodies = TemporaryBRepManager::get()->createFromFile(file);
        if (!bodies) return json{{"ok", false}, {"error", "createFromFile: " + lastError()}};
        json res{{"ok", true}, {"bodies", json::array()}};
        for (size_t i = 0; i < bodies->count(); i++) res["bodies"].push_back(bodyInfo(bodies->item(i)));
        return res;
    }
    return json{{"ok", false}, {"error", "unknown probe " + what}};
}


// {"cmd": "screenshot", "file": "...png", "view": "iso|front|top|right", "w": 1200, "h": 900}
json screenshot(const json& req)
{
    Ptr<Application> app = Application::get();
    Ptr<Viewport> vp = app->activeViewport();
    if (!vp) return json{{"ok", false}, {"error", "no active viewport"}};
    Ptr<Camera> cam = vp->camera();
    std::string v = req.value("view", "iso");
    ViewOrientations o = IsoTopRightViewOrientation;
    if (v == "front") o = FrontViewOrientation;
    else if (v == "top") o = TopViewOrientation;
    else if (v == "right") o = RightViewOrientation;
    else if (v == "back") o = BackViewOrientation;
    else if (v == "bottom") o = BottomViewOrientation;
    else if (v == "isoBack") o = IsoBottomLeftViewOrientation;
    cam->viewOrientation(o);
    cam->isFitView(true);
    cam->isSmoothTransition(false);
    vp->camera(cam);
    vp->refresh();
    std::string f = req.at("file");
    bool ok = vp->saveAsImageFile(f, req.value("w", 1200), req.value("h", 900));
    return json{{"ok", ok}, {"file", f}};
}

// {"cmd": "closeDocs"}: close (unsaved) the documents this implementation created and left open; never a
// document it did not create, never a saved one.
json closeDocs(const json& req)
{
    Ptr<Application> app = Application::get();
    json closed = json::array();
    for (auto& d : trackedDocuments())
    {
        if (!d || !d->isValid() || d->isSaved()) continue;
        closed.push_back(d->name());
        d->close(false);
    }
    // documents an earlier generation of this implementation made (its registry is lost on reload): an unsaved
    // design is recognised as ours only by its contents, a component named after a document of a given NCX file
    // (or Fusion's name for a STEP root product, "ncx_<guid>")
    if (req.contains("names"))
    {
        std::set<std::string> names;
        for (auto& n : req["names"]) names.insert(n.get<std::string>());
        for (int pass = 0; pass < 100; pass++)
        {
            Ptr<Document> victim;
            Ptr<Documents> ds = app->documents();
            for (size_t i = 0; i < ds->count() && !victim; i++)
            {
                Ptr<Document> d = ds->item(i);
                if (!d || d->isSaved()) continue;
                Ptr<Design> des = d->products()->itemByProductType("DesignProductType");
                if (!des) continue;
                Ptr<Component> r = des->rootComponent();
                bool ours = names.count(r->name()) > 0;
                for (size_t k = 0; !ours && k < r->allOccurrences()->count(); k++)
                {
                    std::string cn = r->allOccurrences()->item(k)->component()->name();
                    if (names.count(cn) || cn.rfind("ncx_", 0) == 0) ours = true;
                }
                if (ours) victim = d;
            }
            if (!victim) break;
            closed.push_back(victim->name());
            victim->close(false);
        }
    }
    // or by the exact timeline a probe makes (e.g. the "movetest" probe's document, left by a failed run)
    if (req.contains("timeline"))
    {
        std::vector<std::string> want;
        for (auto& n : req["timeline"]) want.push_back(n.get<std::string>());
        Ptr<Documents> ds = app->documents();
        std::vector<Ptr<Document>> victims;
        for (size_t i = 0; i < ds->count(); i++)
        {
            Ptr<Document> d = ds->item(i);
            if (!d || d->isSaved()) continue;
            Ptr<Design> des = d->products()->itemByProductType("DesignProductType");
            if (!des || !des->timeline()) continue;
            std::vector<std::string> have;
            for (size_t k = 0; k < des->timeline()->count(); k++) have.push_back(des->timeline()->item(k)->name());
            if (have == want) victims.push_back(d);
        }
        for (auto& d : victims)
        {
            closed.push_back(d->name());
            d->close(false);
        }
    }
    return json{{"ok", true}, {"closed", closed}};
}

} // namespace ncxf
