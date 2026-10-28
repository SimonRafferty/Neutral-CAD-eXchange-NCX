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

// The design a probe works on: the unsaved document whose root component is named `root` (never a saved one), else
// the active design.
Ptr<Design> probeDesign(const json& req)
{
    Ptr<Application> app = Application::get();
    std::string root = req.value("root", "");
    if (!root.empty())
        for (size_t i = 0; i < app->documents()->count(); i++)
        {
            Ptr<Document> d = app->documents()->item(i);
            if (!d || d->isSaved()) continue;
            Ptr<Design> des = d->products()->itemByProductType("DesignProductType");
            if (des && (des->rootComponent()->name() == root || d->name() == root)) return des;
        }
    return root.empty() ? Ptr<Design>(app->activeProduct()) : nullptr;
}

std::string jointTypeName(int t)
{
    switch (t)
    {
    case RigidJointType: return "rigid";
    case RevoluteJointType: return "revolute";
    case SliderJointType: return "slider";
    case CylindricalJointType: return "cylindrical";
    case PinSlotJointType: return "pinSlot";
    case PlanarJointType: return "planar";
    case BallJointType: return "ball";
    default: return "?";
    }
}

std::string healthName(int h)
{
    switch (h)
    {
    case HealthyFeatureHealthState: return "healthy";
    case WarningFeatureHealthState: return "warning";
    case ErrorFeatureHealthState: return "error";
    case SuppressedFeatureHealthState: return "suppressed";
    case RolledBackFeatureHealthState: return "rolledBack";
    default: return "unknown";
    }
}

std::string occPath(const Ptr<Occurrence>& o) { return o ? o->fullPathName() : std::string("(none)"); }

// world transforms of every occurrence (root context), by path
std::map<std::string, std::vector<double>> worldTransforms(const Ptr<Component>& root)
{
    std::map<std::string, std::vector<double>> m;
    Ptr<OccurrenceList> occs = root->allOccurrences();
    for (size_t i = 0; occs && i < occs->count(); i++) m[occs->item(i)->fullPathName()] = occs->item(i)->transform2()->asArray();
    return m;
}

json movedBetween(const std::map<std::string, std::vector<double>>& a, const std::map<std::string, std::vector<double>>& b)
{
    json moved = json::array();
    for (auto& kv : b)
    {
        auto it = a.find(kv.first);
        if (it == a.end()) continue;
        double d = 0;
        for (size_t k = 0; k < 16; k++) d = std::max(d, std::fabs(kv.second[k] - it->second[k]) * (k == 3 || k == 7 || k == 11 ? 10.0 : 1.0));
        if (d > 1e-6) moved.push_back(json{{"occurrence", kv.first}, {"change", d}});
    }
    return moved;
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
    if (what == "activate")
    {
        // developer: make the open design whose model (root or upright wrapper) is named `name` the active document
        std::string want = req.value("name", "");
        Ptr<Documents> ds = app->documents();
        for (size_t i = 0; i < ds->count(); i++)
        {
            Ptr<Design> d = ds->item(i)->products()->itemByProductType("DesignProductType");
            if (!d) continue;
            Ptr<Occurrence> w = findModelWrapper(d);
            std::string n = w ? w->component()->name() : d->rootComponent()->name();
            if (n == want) return json{{"ok", ds->item(i)->activate()}};
        }
        return json{{"ok", false}, {"error", "no open design named " + want}};
    }
    if (what == "centroids")
    {
        // developer: each occurrence's first body, its centre of mass read natively and through the occurrence
        Ptr<Design> des = app->activeProduct();
        json a = json::array();
        Ptr<OccurrenceList> os = des->rootComponent()->allOccurrences();
        for (size_t i = 0; os && i < os->count(); i++)
        {
            Ptr<Occurrence> o = os->item(i);
            if (o->component()->bRepBodies()->count() == 0) continue;
            Ptr<BRepBody> nb = o->component()->bRepBodies()->item(0);
            Ptr<BRepBody> pb = o->bRepBodies()->item(0);
            auto c = [](const Ptr<BRepBody>& b) {
                Ptr<PhysicalProperties> pp = b->getPhysicalProperties(VeryHighCalculationAccuracy);
                return json::array({cm2mm(pp->centerOfMass()->x()), cm2mm(pp->centerOfMass()->y()), cm2mm(pp->centerOfMass()->z())});
            };
            a.push_back(json{{"path", o->fullPathName()}, {"native", c(nb)}, {"proxy", c(pb)}, {"occCount", o->component()->occurrences()->count()},
                             {"g2p", o->isGroundToParent()}});
        }
        return json{{"ok", true}, {"items", a}};
    }
    if (what == "tlctx")
    {
        // developer: which timeline entities come as proxies (assembly context) and in which component
        Ptr<Design> des = app->activeProduct();
        json a = json::array();
        for (size_t i = 0; des && i < des->timeline()->count(); i++)
        {
            Ptr<Base> e = des->timeline()->item(i)->entity();
            json j{{"name", des->timeline()->item(i)->name()}, {"type", e ? e->objectType() : "null"}};
            if (Ptr<Feature> f = e)
            {
                j["proxy"] = (bool)f->assemblyContext();
                j["component"] = f->parentComponent()->name();
                Ptr<Feature> nf = f->parentComponent()->features()->itemByName(f->name());
                j["byName"] = (bool)nf;
                if (nf) j["byNameProxy"] = (bool)nf->assemblyContext();
                if (Ptr<ExtrudeFeature> x = e)
                    if (Ptr<Profile> p = x->profile()) j["profileSketchProxy"] = (bool)p->parentSketch()->assemblyContext();
            }
            if (Ptr<Sketch> s = e) j["proxy"] = (bool)s->assemblyContext();
            a.push_back(j);
        }
        return json{{"ok", true}, {"items", a}};
    }
    if (what == "computeAll")
    {
        // developer: recompute the whole design (as Fusion does on some edits), then the tree
        Ptr<Design> des = app->activeProduct();
        if (!des) return json{{"ok", false}, {"error", "no active design"}};
        bool ok = des->computeAll();
        return json{{"ok", ok}, {"error", ok ? "" : lastError()}, {"tree", walk(des->rootComponent())}};
    }
    if (what == "tree")
    {
        // the active design's components, bodies (volume, box in mm) and occurrence transforms
        Ptr<Design> des = app->activeProduct();
        if (!des) return json{{"ok", false}, {"error", "no active design"}};
        return json{{"ok", true}, {"tree", walk(des->rootComponent())}};
    }
    if (what == "threads")
    {
        // developer: what Fusion gives for each thread feature of the root (its data, its faces)
        Ptr<Design> des = app->activeProduct();
        json out = json::array();
        Ptr<ThreadFeatures> tfs = des ? des->rootComponent()->features()->threadFeatures() : nullptr;
        for (size_t i = 0; tfs && i < tfs->count(); i++)
        {
            Ptr<ThreadFeature> th = tfs->item(i);
            json row{{"name", th->name()}, {"modelled", th->isModeled()}};
            for (int state = 0; state < 2; state++)
            {
                Ptr<TimelineObject> tto = th->timelineObject();
                if (tto) tto->rollTo(state == 0);   // 0: just before it, 1: after it
                Ptr<ThreadFeature> t2 = tfs->item(i);
                Ptr<ThreadInfo> ti = t2 ? t2->threadInfo() : nullptr;
                std::string e1 = lastError();
                Ptr<BRepFaces> fs = t2 ? t2->inputCylindricalFaces() : nullptr;
                std::string e2 = lastError();
                Ptr<BRepFaces> own = t2 ? t2->faces() : nullptr;
                row[state == 0 ? "before" : "after"] = json{{"info", (bool)ti}, {"infoErr", e1}, {"faces", fs ? (int)fs->count() : -1}, {"facesErr", e2},
                                                            {"ownFaces", own ? (int)own->count() : -1}};
            }
            des->timeline()->moveToEnd();
            out.push_back(row);
        }
        return json{{"ok", true}, {"threads", out}};
    }
    if (what == "farmeasure")
    {
        // a body's mass properties as Fusion gives them where it is, against a temporary copy moved to the origin
        // (developer: far from the origin, do the centroid and moments lose precision?) {"bodies": [indices]}
        Ptr<Design> des = app->activeProduct();
        if (!des) return json{{"ok", false}, {"error", "no active design"}};
        Ptr<Component> root = des->rootComponent();
        Ptr<TemporaryBRepManager> tm = TemporaryBRepManager::get();
        json out = json::array();
        for (auto& jb : req.value("bodies", json::array()))
        {
            size_t i = jb.get<size_t>();
            if (i >= root->bRepBodies()->count()) continue;
            Ptr<BRepBody> b = root->bRepBodies()->item(i);
            json here = bodyFingerprint(b);
            std::array<double, 6> bx = here["bbox"].get<std::array<double, 6>>();
            V3 mid{(bx[0] + bx[3]) / 2, (bx[1] + bx[4]) / 2, (bx[2] + bx[5]) / 2};
            Ptr<BRepBody> t = tm->copy(b);
            Ptr<Matrix3D> m = Matrix3D::create();
            m->translation(Vector3D::create(-mm2cm(mid[0]), -mm2cm(mid[1]), -mm2cm(mid[2])));
            tm->transform(t, m);
            // a temporary body has no material: a real body made from it in a scratch component is measured
            Ptr<Occurrence> o = root->occurrences()->addNewComponent(Matrix3D::create());
            Ptr<BaseFeature> bf = o->component()->features()->baseFeatures()->add();
            json moved;
            if (bf && bf->startEdit())
            {
                Ptr<BRepBody> nb = o->component()->bRepBodies()->add(t, bf);
                bf->finishEdit();
                if (o->component()->bRepBodies()->count() > 0)
                {
                    moved = bodyFingerprint(o->component()->bRepBodies()->item(0));
                    for (int k = 0; k < 3; k++) moved["centroid"][k] = moved["centroid"][k].get<double>() + mid[k];
                }
            }
            o->deleteMe();
            out.push_back(json{{"i", i}, {"here", here}, {"moved", moved}});
        }
        return json{{"ok", true}, {"bodies", out}};
    }
    if (what == "volumes")
    {
        // the solid volume (mm^3) and body count after each timeline item, to find where a rebuild departs from
        // its source (developer)
        Ptr<Design> des = app->activeProduct();
        if (!des) return json{{"ok", false}, {"error", "no active design"}};
        Ptr<Timeline> tl = des->timeline();
        json items = json::array();
        for (size_t i = 0; tl && i < tl->count(); i++)
        {
            Ptr<TimelineObject> t = tl->item(i);
            t->rollTo(false);
            double v = 0;
            int n = 0;
            Ptr<Components> cs = des->allComponents();
            for (size_t c = 0; cs && c < cs->count(); c++)
                for (size_t b = 0; b < cs->item(c)->bRepBodies()->count(); b++)
                {
                    Ptr<BRepBody> body = cs->item(c)->bRepBodies()->item(b);
                    if (!body->isSolid()) continue;
                    v += body->volume() * 1000;
                    n++;
                }
            Ptr<Base> e = t->entity();
            items.push_back(json{{"i", i}, {"name", t->name()}, {"type", e ? e->objectType() : "?"}, {"volume", v}, {"bodies", n},
                                 {"health", static_cast<int>(t->healthState())}});
        }
        if (tl) tl->moveToEnd();
        return json{{"ok", true}, {"volumes", items}};
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
    if (what == "joints")
    {
        // every joint and as-built joint of the design, by the component that holds it, and every occurrence
        Ptr<Design> des = probeDesign(req);
        if (!des) return json{{"ok", false}, {"error", "no such design"}};
        json res{{"ok", true}, {"design", des->parentDocument()->name()}, {"joints", json::array()}, {"occurrences", json::array()}};
        try
        {
        // the root and every component under it (Design.allComponents returned nothing here)
        std::vector<Ptr<Component>> comps{des->rootComponent()};
        {
            std::set<std::string> seen{des->rootComponent()->entityToken()};
            Ptr<OccurrenceList> all = des->rootComponent()->allOccurrences();
            for (size_t i = 0; all && i < all->count(); i++)
                if (seen.insert(all->item(i)->component()->entityToken()).second) comps.push_back(all->item(i)->component());
        }
        for (size_t c = 0; c < comps.size(); c++)
        {
            Ptr<Component> comp = comps[c];
            res["trace"] = "component " + std::to_string(c) + (comp ? " " + comp->name() : std::string(" null"));
            res["jointsFolderLightBulb"][comp->name()] = comp->isJointsFolderLightBulbOn();
            auto one =[&](const std::string& kind, const std::string& name, const Ptr<JointMotion>& jm, const Ptr<Occurrence>& o1,
                           const Ptr<Occurrence>& o2, const Ptr<TimelineObject>& to, bool supp, bool bulb) {
                json j{{"kind", kind}, {"name", name}, {"in", comp->name()}, {"type", jm ? jointTypeName(jm->jointType()) : "?"},
                       {"one", occPath(o1)}, {"two", occPath(o2)}, {"suppressed", supp}, {"lightBulb", bulb},
                       {"oneIsProxy", o1 && o1->assemblyContext() != nullptr}, {"twoIsProxy", o2 && o2->assemblyContext() != nullptr}};
                if (to) j["health"] = healthName(to->healthState()), j["message"] = to->errorOrWarningMessage();
                res["joints"].push_back(j);
            };
            for (size_t i = 0; i < comp->joints()->count(); i++)
            {
                Ptr<Joint> j = comp->joints()->item(i);
                res["trace"] = comp->name() + " joint " + std::to_string(i);
                one("joint", j->name(), j->jointMotion(), j->occurrenceOne(), j->occurrenceTwo(), j->timelineObject(), j->isSuppressed(), j->isLightBulbOn());
            }
            for (size_t i = 0; i < comp->asBuiltJoints()->count(); i++)
            {
                Ptr<AsBuiltJoint> j = comp->asBuiltJoints()->item(i);
                // a sub-assembly's joint is read through its (first) top-level occurrence
                if (comp != des->rootComponent())
                {
                    Ptr<OccurrenceList> oc = des->rootComponent()->allOccurrencesByComponent(comp);
                    if (oc && oc->count() > 0)
                        if (Ptr<AsBuiltJoint> pj = j->createForAssemblyContext(oc->item(0))) j = pj;
                }
                res["trace"] = comp->name() + " asBuilt " + std::to_string(i) + " " + j->name();
                one("asBuilt", j->name(), j->jointMotion(), j->occurrenceOne(), j->occurrenceTwo(), j->timelineObject(), j->isSuppressed(), j->isLightBulbOn());
                // its frame in the root: the joint geometry is in the component's frame; a sub-assembly's joint is
                // placed by that sub-assembly's (first) top-level occurrence
                if (Ptr<JointGeometry> g = j->geometry())
                {
                    Ptr<Matrix3D> xf = Matrix3D::create();
                    if (comp != des->rootComponent() && !j->assemblyContext())
                    {
                        Ptr<OccurrenceList> oc = des->rootComponent()->allOccurrencesByComponent(comp);
                        if (oc && oc->count() > 0) xf = oc->item(0)->transform2();
                    }
                    Ptr<Point3D> o = g->origin()->copy();
                    o->transformBy(xf);
                    Ptr<Vector3D> z = g->primaryAxisVector()->copy();   // the joint's Z: the axis of its motion
                    z->transformBy(xf);
                    Ptr<Vector3D> px = g->primaryAxisVector()->copy();
                    px->transformBy(xf);
                    json& last = res["joints"].back();
                    last["origin"] = json::array({cm2mm(o->x()), cm2mm(o->y()), cm2mm(o->z())});
                    last["axis"] = json::array({z->x(), z->y(), z->z()});
                    last["geometryType"] = static_cast<int>(g->geometryType());
                    last["xAxis"] = json::array({px->x(), px->y(), px->z()});
                    if (Ptr<BRepFace> f = g->entityOne()) last["entityOneIsProxy"] = f->assemblyContext() != nullptr,
                                                          last["entityOneContext"] = f->assemblyContext() ? f->assemblyContext()->fullPathName() : "";
                }
            }
        }
        Ptr<OccurrenceList> occs = des->rootComponent()->allOccurrences();
        for (size_t i = 0; occs && i < occs->count(); i++)
        {
            Ptr<Occurrence> o = occs->item(i);
            res["occurrences"].push_back(json{{"path", o->fullPathName()}, {"component", o->component()->name()}, {"grounded", o->isGrounded()}, {"groundToParent", o->isGroundToParent()}, {"name", o->name()},
                                              {"proxy", o->assemblyContext() != nullptr}, {"visible", o->isLightBulbOn()}});
        }
        }
        catch (...)
        {
            res["ok"] = false;
            res["error"] = "exception at " + res.value("trace", std::string("?")) + ": " + lastError();
        }
        return res;
    }
    if (what == "sketchvis")
    {
        // every sketch of every component: its light bulb (the source's `hidden` is a light bulb off)
        Ptr<Design> des = probeDesign(req);
        if (!des) return json{{"ok", false}, {"error", "no such design"}};
        std::vector<Ptr<Component>> comps{des->rootComponent()};
        std::set<std::string> seen{des->rootComponent()->entityToken()};
        Ptr<OccurrenceList> all = des->rootComponent()->allOccurrences();
        for (size_t i = 0; all && i < all->count(); i++)
            if (seen.insert(all->item(i)->component()->entityToken()).second) comps.push_back(all->item(i)->component());
        json res{{"ok", true}, {"sketches", json::array()}};
        for (auto& c : comps)
            for (size_t i = 0; i < c->sketches()->count(); i++)
                res["sketches"].push_back(json{{"component", c->name()}, {"sketch", c->sketches()->item(i)->name()},
                                               {"lightBulb", c->sketches()->item(i)->isLightBulbOn()}});
        return res;
    }
    if (what == "jointmove")
    {
        // drive one joint's motion value (revolute/cylindrical: rotation by `deg`; slider: `mm`) and list the
        // occurrences that moved in the root context; then put it back
        Ptr<Design> des = probeDesign(req);
        if (!des) return json{{"ok", false}, {"error", "no such design"}};
        std::string name = req.value("name", "");
        Ptr<JointMotion> jm;
        std::vector<Ptr<Component>> comps{des->rootComponent()};
        {
            Ptr<OccurrenceList> all = des->rootComponent()->allOccurrences();
            for (size_t i = 0; all && i < all->count(); i++) comps.push_back(all->item(i)->component());
        }
        for (size_t c = 0; c < comps.size() && !jm; c++)
        {
            Ptr<Component> comp = comps[c];
            for (size_t i = 0; i < comp->joints()->count() && !jm; i++)
                if (comp->joints()->item(i)->name() == name) jm = comp->joints()->item(i)->jointMotion();
            for (size_t i = 0; i < comp->asBuiltJoints()->count() && !jm; i++)
                if (comp->asBuiltJoints()->item(i)->name() == name) jm = comp->asBuiltJoints()->item(i)->jointMotion();
        }
        if (!jm) return json{{"ok", false}, {"error", "no joint " + name}};
        // joints to suppress for the test (each restored after)
        std::vector<Ptr<AsBuiltJoint>> off;
        for (auto& n : req.value("suppress", json::array()))
            for (size_t c = 0; c < comps.size(); c++)
                for (size_t i = 0; i < comps[c]->asBuiltJoints()->count(); i++)
                    if (comps[c]->asBuiltJoints()->item(i)->name() == n.get<std::string>() && !comps[c]->asBuiltJoints()->item(i)->isSuppressed())
                    {
                        comps[c]->asBuiltJoints()->item(i)->isSuppressed(true);
                        off.push_back(comps[c]->asBuiltJoints()->item(i));
                    }
        auto before = worldTransforms(des->rootComponent());
        json res{{"ok", true}, {"joint", name}, {"type", jointTypeName(jm->jointType())}};
        bool set = false;
        double deg = req.value("deg", 10.0), mm = req.value("mm", 5.0);
        if (Ptr<RevoluteJointMotion> r = jm)
        {
            res["was"] = rad2deg(r->rotationValue());
            set = r->rotationValue(r->rotationValue() + deg2rad(deg));
        }
        else if (Ptr<CylindricalJointMotion> cy = jm)
        {
            res["was"] = rad2deg(cy->rotationValue());
            set = cy->rotationValue(cy->rotationValue() + deg2rad(deg));
        }
        else if (Ptr<SliderJointMotion> sl = jm)
        {
            res["was"] = cm2mm(sl->slideValue());
            set = sl->slideValue(sl->slideValue() + mm2cm(mm));
        }
        res["set"] = set;
        if (!set) res["error"] = lastError();
        if (req.value("compute", false)) des->computeAll();   // reverts positions not captured (measured)
        auto after = worldTransforms(des->rootComponent());
        res["moved"] = movedBetween(before, after);
        if (req.value("restore", true))
        {
            if (Ptr<RevoluteJointMotion> r = jm) r->rotationValue(r->rotationValue() - deg2rad(deg));
            else if (Ptr<CylindricalJointMotion> cy = jm) cy->rotationValue(cy->rotationValue() - deg2rad(deg));
            else if (Ptr<SliderJointMotion> sl = jm) sl->slideValue(sl->slideValue() - mm2cm(mm));
            for (auto& j : off) j->isSuppressed(false);
        }
        if (Ptr<RevoluteJointMotion> r = jm) res["now"] = rad2deg(r->rotationValue());
        return res;
    }
    if (what == "drivetest")
    {
        // Does driving a joint's value move a component? Two boxes in two components, one grounded, joined by an
        // as-built revolute joint (or, with "regular": true, a joint); optionally a position snapshot first.
        Ptr<Document> doc = app->documents()->add(FusionDesignDocumentType);
        trackDocument(doc);
        Ptr<Design> des = doc->products()->itemByProductType("DesignProductType");
        des->designType(ParametricDesignType);
        Ptr<Component> root = des->rootComponent();
        auto box = [&](double x) {
            Ptr<Occurrence> o = root->occurrences()->addNewComponent(Matrix3D::create());
            Ptr<Component> c = o->component();
            Ptr<Sketch> sk = c->sketches()->add(c->xYConstructionPlane());
            sk->sketchCurves()->sketchLines()->addTwoPointRectangle(Point3D::create(x, -0.5, 0), Point3D::create(x + 4, 0.5, 0));
            Ptr<ExtrudeFeatureInput> ei = c->features()->extrudeFeatures()->createInput(sk->profiles()->item(0), NewBodyFeatureOperation);
            ei->setDistanceExtent(false, ValueInput::createByReal(1));
            c->features()->extrudeFeatures()->add(ei);
            return o;
        };
        Ptr<Occurrence> a1 = box(0), a2 = box(4);
        a1->isGrounded(true);
        json res{{"ok", true}};
        Ptr<ConstructionPoint> pt = a2->component()->originConstructionPoint()->createForAssemblyContext(a2);
        Ptr<JointGeometry> g = JointGeometry::createByPoint(pt);
        Ptr<JointMotion> jm;
        if (req.value("nojoint", false)) {}
        else if (req.value("regular", false))
        {
            Ptr<JointGeometry> g1 = JointGeometry::createByPoint(a1->component()->originConstructionPoint()->createForAssemblyContext(a1));
            Ptr<JointInput> ji = root->joints()->createInput(g, g1);
            ji->setAsRevoluteJointMotion(ZAxisJointDirection);
            Ptr<Joint> j = root->joints()->add(ji);
            res["made"] = (bool)j;
            if (j) jm = j->jointMotion();
        }
        else
        {
            Ptr<AsBuiltJointInput> in = root->asBuiltJoints()->createInput(a2, a1, g);
            in->setAsRevoluteJointMotion(ZAxisJointDirection);
            Ptr<AsBuiltJoint> j = root->asBuiltJoints()->add(in);
            res["made"] = (bool)j;
            if (j) jm = j->jointMotion();
        }
        if (req.value("snapshot", false) && des->snapshots()->hasPendingSnapshot()) des->snapshots()->add();
        auto boxOfOcc = [](const Ptr<Occurrence>& o) {
            Ptr<BoundingBox3D> bb = o->bRepBodies()->item(0)->boundingBox();
            return json::array({cm2mm(bb->minPoint()->x()), cm2mm(bb->minPoint()->y()), cm2mm(bb->maxPoint()->x()), cm2mm(bb->maxPoint()->y())});
        };
        res["box0"] = boxOfOcc(a2);
        res["tx0"] = a2->transform2()->asArray();
        auto before = worldTransforms(root);
        Ptr<RevoluteJointMotion> r = jm;
        res["set"] = r ? r->rotationValue(deg2rad(30)) : false;
        res["value"] = r ? rad2deg(r->rotationValue()) : -1;
        if (req.value("compute", false)) des->computeAll();   // reverts positions not captured (measured)
        if (req.value("events", false)) adsk::doEvents();
        res["moved"] = movedBetween(before, worldTransforms(root));
        res["box1"] = boxOfOcc(a2);
        res["tx1"] = a2->transform2()->asArray();
        // and moving the occurrence itself (what a drag does to the occurrence)
        Ptr<Matrix3D> m = a2->transform2();
        m->translation(Vector3D::create(m->translation()->x(), m->translation()->y() + 1, m->translation()->z()));
        auto b2 = worldTransforms(root);
        res["transformSet"] = a2->transform2(m);
        if (req.value("compute", false)) des->computeAll();   // reverts positions not captured (measured)
        if (req.value("events", false)) adsk::doEvents();
        res["movedByTransform"] = movedBetween(b2, worldTransforms(root));
        res["nOcc"] = b2.size();
        for (auto& kv : worldTransforms(root)) res["after"][kv.first] = kv.second;
        for (auto& kv : before) res["before"][kv.first] = kv.second;
        if (req.value("close", true)) doc->close(false);
        return res;
    }
    if (what == "occdrag")
    {
        // what a drag does: rotate one top-level occurrence (`occurrence`, its full path) by `deg` about the axis of
        // the as-built joint `axisJoint`, then list what moved (Fusion solves the joints when an occurrence is moved;
        // no computeAll, which reverts positions not captured); `restore` puts it back
        Ptr<Design> des = probeDesign(req);
        if (!des) return json{{"ok", false}, {"error", "no such design"}};
        Ptr<Component> root = des->rootComponent();
        Ptr<Occurrence> occ;
        Ptr<OccurrenceList> all = root->allOccurrences();
        for (size_t i = 0; all && i < all->count(); i++)
            if (all->item(i)->fullPathName() == req.value("occurrence", "")) occ = all->item(i);
        if (!occ) return json{{"ok", false}, {"error", "no occurrence " + req.value("occurrence", "")}};
        Ptr<AsBuiltJoint> aj;
        for (size_t i = 0; i < root->asBuiltJoints()->count(); i++)
            if (root->asBuiltJoints()->item(i)->name() == req.value("axisJoint", "")) aj = root->asBuiltJoints()->item(i);
        if (!aj || !aj->geometry()) return json{{"ok", false}, {"error", "no as-built joint " + req.value("axisJoint", "")}};
        Ptr<Point3D> o = aj->geometry()->origin();
        Ptr<Vector3D> ax = aj->geometry()->primaryAxisVector();
        Ptr<Matrix3D> rot = Matrix3D::create();
        rot->setToRotation(deg2rad(req.value("deg", 5.0)), ax, o);
        auto before = worldTransforms(root);
        Ptr<Matrix3D> was = occ->transform2();
        Ptr<Matrix3D> m = was->copy();
        m->transformBy(rot);
        json res{{"ok", true}, {"set", occ->transform2(m)}};
        auto after = worldTransforms(root);
        res["moved"] = movedBetween(before, after);
        if (req.value("restore", true)) occ->transform2(was);
        return res;
    }
    if (what == "offsettest")
    {
        // Fusion's start offset of an extrude: a 10 mm cube on z 0..10, a 2 mm square sketched on XY (z 0) cut
        // through all with start offset +-3 mm in each direction; the removed volume tells where the cut ran
        json res{{"ok", true}, {"runs", json::array()}};
        for (int dir = 0; dir < 2; dir++)
            for (int sg = -1; sg <= 1; sg += 2)
            {
                Ptr<Document> doc = app->documents()->add(FusionDesignDocumentType);
                trackDocument(doc);
                Ptr<Design> des = doc->products()->itemByProductType("DesignProductType");
                des->designType(ParametricDesignType);
                Ptr<Component> c = des->rootComponent();
                Ptr<Sketch> sk = c->sketches()->add(c->xYConstructionPlane());
                sk->sketchCurves()->sketchLines()->addTwoPointRectangle(Point3D::create(-0.5, -0.5, 0), Point3D::create(0.5, 0.5, 0));
                Ptr<ExtrudeFeatureInput> ei = c->features()->extrudeFeatures()->createInput(sk->profiles()->item(0), NewBodyFeatureOperation);
                ei->setDistanceExtent(false, ValueInput::createByReal(1));
                c->features()->extrudeFeatures()->add(ei);
                Ptr<Sketch> s2 = c->sketches()->add(c->xYConstructionPlane());
                s2->sketchCurves()->sketchLines()->addTwoPointRectangle(Point3D::create(-0.1, -0.1, 0), Point3D::create(0.1, 0.1, 0));
                Ptr<ExtrudeFeatureInput> ci = c->features()->extrudeFeatures()->createInput(s2->profiles()->item(0), CutFeatureOperation);
                ci->setOneSideExtent(req.value("blind", false) ? Ptr<ExtentDefinition>(DistanceExtentDefinition::create(ValueInput::createByReal(0.4)))
                                                                : Ptr<ExtentDefinition>(ThroughAllExtentDefinition::create()),
                                     dir ? NegativeExtentDirection : PositiveExtentDirection);
                ci->startExtent(OffsetStartDefinition::create(ValueInput::createByReal(0.3 * sg)));
                Ptr<ExtrudeFeature> f = c->features()->extrudeFeatures()->add(ci);
                json r{{"direction", dir ? "negative" : "positive"}, {"offset_mm", 3 * sg}, {"made", (bool)f}, {"error", f ? "" : lastError()}};
                if (c->bRepBodies()->count())
                {
                    Ptr<BRepBody> b = c->bRepBodies()->item(0);
                    r["removed_mm3"] = 1000.0 - b->volume() * 1000.0;
                }
                if (f && f->timelineObject()) r["health"] = f->timelineObject()->errorOrWarningMessage();
                res["runs"].push_back(r);
                doc->close(false);
            }
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
                        {"y", vecOf(sk->yDirection())}, {"profiles", sk->profiles()->count()}, {"full", sk->isFullyConstrained()}, {"curves", json::array()}};
                for (size_t k = 0; k < sk->sketchCurves()->count(); k++)
                {
                    Ptr<SketchCurve> cv = sk->sketchCurves()->item(k);
                    json cj{{"type", cv->objectType()}, {"ref", cv->isReference()}, {"constr", cv->isConstruction()}, {"full", cv->isFullyConstrained()}};
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

// {"cmd": "openArchive", "file": "...f3d"}: open a Fusion archive as a new, unsaved design (tracked, so closeDocs
// closes it): a user's own Fusion model to export in a test.
json openArchive(const json& req)
{
    Ptr<Application> app = Application::get();
    Ptr<ImportManager> im = app->importManager();
    Ptr<FusionArchiveImportOptions> o = im ? im->createFusionArchiveImportOptions(req.value("file", "")) : nullptr;
    if (!o) return json{{"ok", false}, {"error", "could not read the archive"}};
    Ptr<Document> d = im->importToNewDocument(o);
    if (!d) return json{{"ok", false}, {"error", "the archive did not open: " + lastError()}};
    trackDocument(d);
    d->activate();
    return json{{"ok", true}, {"name", d->name()}};
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
