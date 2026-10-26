// Export NCX (ADDING-A-CAD-SYSTEM step 6): the active Fusion design -> the neutral log + Fusion's STEP export,
// embedded with ncx_payload_write.
//
//   job {"cmd": "export", "file": "<out.ncx.step>"}   (the menu command asks for the file)
//
// Carried as mapped features: sketches (lines, arcs, circles, points, splines by fit points; the relations and
// dimensions FORMAT-SPEC names), extrude / cut, revolve / revolve cut, constant fillets, equal-distance
// chamfers, offset construction planes, user parameters and dimension expressions.
// Every other timeline feature is written opaque with its carried result (the bodies after it, as STEP), so an
// importer can still rebuild the rest of the history (FORMAT-SPEC 4, "Feature results").
// Assemblies: components (each a part document) with their transforms and grounded state; joints as the mates
// that leave the same freedom (MateWriter below).
//
// Sign conventions are the inverse of the importer's (which follow the SolidWorks exporter): a payload is read
// the same way whichever system wrote it.
#include "host.h"
#include "replay.h"

#include <functional>
#include <sstream>

namespace ncxf {

namespace {

json vj(const V3& v) { return json::array({v[0], v[1], v[2]}); }

std::string fmt(double v)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.12g", v);
    return b;
}

struct DocWriter
{
    Ptr<Design> des;
    Ptr<Component> comp;
    std::string docId, docName;
    json features = json::array();
    std::map<std::string, std::string> idOfToken;   // entityToken -> payload feature id
    std::map<std::string, std::map<std::string, std::string>> sketchIds;   // sketch feature id -> entity token -> id
    int n = 0;
    std::vector<std::string> notes;

    // a new feature id: none the source's features use (walk keeps those for the objects an NCX import made)
    std::set<std::string> reservedIds, usedIds;
    bool passthrough = true;   // feature passthrough (walk); off with the export's {"passthrough": false}
    std::string fid()
    {
        std::string id;
        do
            id = "f:" + std::to_string(++n);
        while (reservedIds.count(id) || usedIds.count(id));
        return id;
    }
    bool extrudeNegative = false;
    std::vector<std::pair<V3, V3>> holeAxes;   // the hole being written: its drilled cylinders' axes (point, direction)
    json holeDiameters = json::array();         // and the distinct diameters of its cylinders, ascending   // the extrude being written went against its profile's normal

    // where an extrude's side faces lie relative to its profile plane (the extent objects do not say:
    // isPositiveDirection reads true for a negative through-all, measured); call with the feature computed
    static bool sideFacesBelow(const Ptr<ExtrudeFeature>& f)
    {
        Ptr<Profile> p0 = f->profile();
        if (!p0)
            if (Ptr<ObjectCollection> oc = f->profile()) p0 = oc->item(0);
        Ptr<BRepFaces> sf = f->sideFaces();
        if (!p0 || !sf || !sf->count()) return false;
        Ptr<Sketch> sk = p0->parentSketch();
        if (sk->nativeObject()) sk = sk->nativeObject();   // both in the component's own frame (upright wrapper)
        V3 nrm = unit(cross(vecOf(sk->xDirection()), vecOf(sk->yDirection())));
        V3 o = ptMm(sk->origin());
        double sum = 0;
        for (size_t k = 0; k < sf->count(); k++)
        {
            Ptr<BRepFace> fk = sf->item(k);
            if (fk->nativeObject()) fk = fk->nativeObject();
            sum += dot(sub(ptMm(fk->pointOnFace()), o), nrm);
        }
        return sum < 0;
    }

    std::string idOf(const Ptr<Base>& entity)
    {
        std::string tok;
        if (Ptr<Feature> f = entity) tok = f->entityToken();
        else if (Ptr<Sketch> s = entity) tok = (s->nativeObject() ? s->nativeObject() : s)->entityToken();
        else if (Ptr<ConstructionPlane> p = entity) tok = (p->nativeObject() ? p->nativeObject() : p)->entityToken();
        auto it = idOfToken.find(tok);
        return it == idOfToken.end() ? "" : it->second;
    }

    std::map<std::string, Ptr<Feature>> featureOfId;   // mapped features written so far, by payload id

    std::vector<Ptr<BRepBody>> solids()
    {
        std::vector<Ptr<BRepBody>> v;
        for (size_t i = 0; i < comp->bRepBodies()->count(); i++)
            if (comp->bRepBodies()->item(i)->isSolid()) v.push_back(comp->bRepBodies()->item(i));
        return v;
    }

    // Every body, solid or surface: a feature's carried result lists both (FORMAT-SPEC 4 `result.bodies` with
    // `sheet`), although part and file fingerprints cover solids only.
    std::vector<Ptr<BRepBody>> allBodies()
    {
        std::vector<Ptr<BRepBody>> v;
        for (size_t i = 0; i < comp->bRepBodies()->count(); i++) v.push_back(comp->bRepBodies()->item(i));
        return v;
    }


    json planeRef(const Ptr<Base>& p)
    {
        // a proxy (the model in the upright wrapper, common.h) is read as its native entity, in its component's frame
        Ptr<Base> pn = p;
        if (Ptr<ConstructionPlane> c0 = p)
            if (c0->nativeObject()) pn = c0->nativeObject();
        if (Ptr<BRepFace> f0 = p)
            if (f0->nativeObject()) pn = f0->nativeObject();
        if (Ptr<ConstructionPlane> cp = pn)
        {
            if (cp == comp->xYConstructionPlane()) return json{{"kind", "originPlane"}, {"by", "id"}, {"plane", "front"}};
            if (cp == comp->xZConstructionPlane()) return json{{"kind", "originPlane"}, {"by", "id"}, {"plane", "top"}};
            if (cp == comp->yZConstructionPlane()) return json{{"kind", "originPlane"}, {"by", "id"}, {"plane", "right"}};
            std::string id = idOf(cp);
            if (!id.empty()) return json{{"kind", "feature"}, {"by", "id"}, {"id", id}};
            return json{{"kind", "frame"}};
        }
        if (Ptr<BRepFace> f = pn) return descriptor(f);
        notes.push_back(std::string("a sketch plane of type ") + (p ? p->objectType() : "null") + " is written as its frame only");
        return json{{"kind", "frame"}};
    }

    // a sketch-space NURBS (cm) as FORMAT-SPEC 4 `nurbs` (sketch mm; knots unitless)
    static json nurbsJson(const Ptr<NurbsCurve3D>& nc)
    {
        if (!nc) return nullptr;
        std::vector<Ptr<Point3D>> cps;
        int degree = 0;
        std::vector<double> knots, weights;
        bool rational = false, periodic = false;
        if (!nc->getData(cps, degree, knots, rational, weights, periodic)) return nullptr;
        json j{{"degree", degree}, {"knots", knots}, {"controlPoints", json::array()}, {"periodic", periodic}};
        for (auto& p : cps) j["controlPoints"].push_back(json::array({cm2mm(p->x()), cm2mm(p->y())}));
        if (rational) j["weights"] = weights;
        return j;
    }

    // ---------------------------------------------------------------------------------- 3D sketch
    // A Fusion sketch whose geometry leaves its plane (an NCX 3D sketch rebuilt, or a user's 3D sketch) goes out
    // as `sketch3d` (FORMAT-SPEC 4): entities at their component coordinates, shared points by id, lines along the
    // component axes as alongX / alongY / alongZ, and point-to-point distances.
    static bool isNonPlanar(const Ptr<Sketch>& sk)
    {
        Ptr<SketchPoints> ps = sk->sketchPoints();
        for (size_t i = 0; i < ps->count(); i++)
            if (std::fabs(ps->item(i)->geometry()->z()) > 1e-7) return true;
        return false;
    }

    json sketch3dDef(const Ptr<Sketch>& sk, const std::string& id)
    {
        auto& ids = sketchIds[id];
        json d{{"constrained", sk->isFullyConstrained() ? "full" : "under"}, {"space", "3d"}, {"entities", json::array()},
               {"constraints", json::array()}, {"dimensions", json::array()}};
        auto P = [&](const Ptr<SketchPoint>& p) { return vj(ptMm(sk->sketchToModelSpace(p->geometry()))); };
        int np = 0, ns = 0;
        auto pointId = [&](const Ptr<SketchPoint>& p) -> std::string {
            std::string tok = p->entityToken();
            auto it = ids.find(tok);
            if (it != ids.end()) return it->second;
            std::string pid = "p" + std::to_string(++np);
            ids[tok] = pid;
            d["entities"].push_back(json{{"id", pid}, {"type", "point"}, {"p", P(p)}});
            return pid;
        };
        Ptr<SketchCurves> cs = sk->sketchCurves();
        for (size_t i = 0; i < cs->count(); i++)
        {
            Ptr<SketchCurve> c = cs->item(i);
            if (c->isReference()) continue;
            json e{{"id", "s" + std::to_string(++ns)}};
            if (Ptr<SketchLine> l = c)
            {
                e["type"] = "line";
                e["startPoint"] = pointId(l->startSketchPoint());
                e["endPoint"] = pointId(l->endSketchPoint());
                e["start"] = P(l->startSketchPoint());
                e["end"] = P(l->endSketchPoint());
                V3 dir = unit(sub(v3(e["end"]), v3(e["start"])));
                const char* along = std::fabs(std::fabs(dir[0]) - 1) < 1e-9 ? "alongX" : std::fabs(std::fabs(dir[1]) - 1) < 1e-9 ? "alongY"
                                    : std::fabs(std::fabs(dir[2]) - 1) < 1e-9 ? "alongZ" : nullptr;
                if (along) d["constraints"].push_back(json{{"type", along}, {"entities", {e["id"]}}});
            }
            else if (Ptr<SketchArc> a = c)
            {
                e["type"] = "arc";
                e["centerPoint"] = pointId(a->centerSketchPoint());
                e["startPoint"] = pointId(a->startSketchPoint());
                e["endPoint"] = pointId(a->endSketchPoint());
                e["center"] = P(a->centerSketchPoint());
                e["start"] = P(a->startSketchPoint());
                e["end"] = P(a->endSketchPoint());
                e["radius"] = cm2mm(a->radius());
                // Fusion runs an arc counter-clockwise from its start to its end about its normal
                // its normal in the component's space (world geometry is turned by the upright wrapper)
                Ptr<Arc3D> g = a->geometry();
                Ptr<Vector3D> nv = g ? g->normal() : nullptr;
                if (nv) nv->transformBy((sk->nativeObject() ? sk->nativeObject() : sk)->transform());
                e["normal"] = vj(nv ? unit(vecOf(nv)) : V3{0, 0, 1});
                e["ccw"] = true;
            }
            else
            {
                notes.push_back(sk->name() + ": a " + c->objectType() + " in a 3D sketch is not exported");
                continue;
            }
            if (c->isConstruction()) e["construction"] = true;
            ids[c->entityToken()] = e["id"].get<std::string>();
            d["entities"].push_back(e);
        }
        int nd = 0;
        Ptr<SketchDimensions> dims = sk->sketchDimensions();
        for (size_t i = 0; i < dims->count(); i++)
        {
            Ptr<SketchLinearDimension> ld = dims->item(i);
            Ptr<SketchPoint> a = ld ? Ptr<SketchPoint>(ld->entityOne()) : nullptr, b = ld ? Ptr<SketchPoint>(ld->entityTwo()) : nullptr;
            if (!a || !b || ld->orientation() != AlignedDimensionOrientation)
            {
                notes.push_back(sk->name() + ": a 3D sketch dimension other than an aligned point-to-point distance is not exported");
                continue;
            }
            Ptr<ModelParameter> mp = ld->parameter();
            json dj{{"id", "d" + std::to_string(++nd)}, {"name", "D" + std::to_string(nd)}, {"kind", "distance"},
                    {"entities", {pointId(a), pointId(b)}}, {"value", dist(v3(P(a)), v3(P(b)))}, {"driving", ld->isDriving()}};
            if (mp) dj["param"] = mp->name();
            dj["fullName"] = "D" + std::to_string(nd) + "@" + sk->name();
            d["dimensions"].push_back(dj);
        }
        return d;
    }

    // ---------------------------------------------------------------------------------- sketch
    // the planar face of the part lying in this plane (point, unit normal; mm), or null
    Ptr<BRepFace> faceInPlane(const V3& o, const V3& nz)
    {
        for (auto& b : solids())
            for (size_t i = 0; i < b->faces()->count(); i++)
            {
                Ptr<BRepFace> f = b->faces()->item(i);
                Ptr<Plane> g = f->geometry();
                if (!g) continue;
                V3 nn = unit(vecOf(g->normal()));
                if (std::fabs(std::fabs(dot(nn, nz)) - 1) < 1e-9 && std::fabs(dot(sub(o, ptMm(g->origin())), nn)) < 1e-6) return f;
            }
        return nullptr;
    }

    json sketchDef(const Ptr<Sketch>& sk, const std::string& id)
    {
        auto& ids = sketchIds[id];
        Ptr<Matrix3D> m = sk->transform();   // sketch -> component space
        V3 o = ptMm(sk->origin()), x = unit(vecOf(sk->xDirection())), y = unit(vecOf(sk->yDirection()));
        json plane = planeRef(sk->referencePlane());
        if (plane.value("kind", "") == "frame")
            if (Ptr<BRepFace> f = faceInPlane(o, unit(cross(x, y))))
            {
                // Fusion gives no reference plane for some face sketches: the face lying in the sketch plane
                plane = descriptor(f);
                notes.pop_back();
            }
        json d{{"plane", plane},
               {"frame", {{"origin", vj(o)}, {"x", vj(x)}, {"z", vj(unit(cross(x, y)))}}},
               {"entities", json::array()}, {"constraints", json::array()}, {"dimensions", json::array()},
               {"constrained", sk->isFullyConstrained() ? "full" : "under"}};
        auto uv = [&](const Ptr<Point3D>& p) { return json::array({cm2mm(p->x()), cm2mm(p->y())}); };
        int np = 0, ns = 0;
        Ptr<SketchPoint> originPt = sk->originPoint();
        // Entities an NCX import made keep their payload ids (attribute NCX/id, sketch.cpp), so the source's references
        // to them (contours, region boundaries, axes, a passed-through feature's definition) stay valid; new entities
        // get ids none of those use.
        auto recorded = [](const Ptr<SketchEntity>& e) -> std::string {
            Ptr<Attribute> a = e ? e->attributes()->itemByName("NCX", "id") : nullptr;
            return a ? a->value() : "";
        };
        std::set<std::string> reserved, used;
        for (size_t i = 0; i < sk->sketchCurves()->count(); i++) reserved.insert(recorded(sk->sketchCurves()->item(i)));
        for (size_t i = 0; i < sk->sketchPoints()->count(); i++) reserved.insert(recorded(sk->sketchPoints()->item(i)));
        auto idFor = [&](const Ptr<SketchEntity>& e, const char* pre, int& n) {
            std::string r = recorded(e);
            if (r.empty() || used.count(r))
                do
                    r = pre + std::to_string(++n);
                while (reserved.count(r) || used.count(r));
            used.insert(r);
            return r;
        };
        auto pointId = [&](const Ptr<SketchPoint>& p) -> std::string {
            if (!p) return "";
            std::string tok = p->entityToken();
            auto it = ids.find(tok);
            if (it != ids.end()) return it->second;
            std::string pid = idFor(p, "p", np);
            ids[tok] = pid;
            d["entities"].push_back(json{{"id", pid}, {"type", "point"}, {"p", uv(p->geometry())}});
            return pid;
        };
        Ptr<SketchCurves> cs = sk->sketchCurves();
        for (size_t i = 0; i < cs->count(); i++)
        {
            Ptr<SketchCurve> c = cs->item(i);
            if (c->isReference()) continue;
            std::string sid = idFor(c, "s", ns);
            json e{{"id", sid}};
            if (Ptr<SketchLine> l = c)
            {
                e["type"] = "line";
                e["start"] = uv(l->startSketchPoint()->geometry());
                e["end"] = uv(l->endSketchPoint()->geometry());
                e["startPoint"] = pointId(l->startSketchPoint());
                e["endPoint"] = pointId(l->endSketchPoint());
            }
            else if (Ptr<SketchCircle> ci = c)
            {
                e["type"] = "circle";
                e["center"] = uv(ci->centerSketchPoint()->geometry());
                e["radius"] = cm2mm(ci->radius());
                e["centerPoint"] = pointId(ci->centerSketchPoint());
            }
            else if (Ptr<SketchArc> a = c)
            {
                // Fusion's arcs run counter-clockwise about the sketch normal, from start to end
                e["type"] = "arc";
                e["center"] = uv(a->centerSketchPoint()->geometry());
                e["radius"] = cm2mm(a->radius());
                e["start"] = uv(a->startSketchPoint()->geometry());
                e["end"] = uv(a->endSketchPoint()->geometry());
                e["ccw"] = true;
                e["centerPoint"] = pointId(a->centerSketchPoint());
                e["startPoint"] = pointId(a->startSketchPoint());
                e["endPoint"] = pointId(a->endSketchPoint());
            }
            else if (Ptr<SketchFittedSpline> sp = c)
            {
                e["type"] = "spline";
                e["points"] = json::array();
                e["pointIds"] = json::array();
                Ptr<SketchPointList> fp = sp->fitPoints();
                for (size_t k = 0; fp && k < fp->count(); k++)
                {
                    e["points"].push_back(uv(fp->item(k)->geometry()));
                    e["pointIds"].push_back(pointId(fp->item(k)));
                }
                e["nurbs"] = nurbsJson(sp->geometry());
            }
            else if (Ptr<SketchFixedSpline> fx = c)
            {
                // a fixed spline (Fusion's own, or an NCX import's exact curve) has no fit points: its ends stand
                // for them, and `nurbs` is the curve
                e["type"] = "spline";
                e["points"] = json::array({uv(fx->startSketchPoint()->geometry()), uv(fx->endSketchPoint()->geometry())});
                e["pointIds"] = json::array({pointId(fx->startSketchPoint()), pointId(fx->endSketchPoint())});
                e["nurbs"] = nurbsJson(fx->geometry());
            }
            else if (Ptr<SketchEllipse> el = c)
            {
                e["type"] = "ellipse";
                Ptr<Point3D> ctr = el->centerSketchPoint()->geometry();
                Ptr<Vector3D> ax = el->majorAxis();
                e["center"] = uv(ctr);
                double R = el->majorAxisRadius(), r = el->minorAxisRadius();
                V3 u = unit(vecOf(ax));
                e["major"] = json::array({cm2mm(ctr->x() + u[0] * R), cm2mm(ctr->y() + u[1] * R)});
                e["minor"] = json::array({cm2mm(ctr->x() - u[1] * r), cm2mm(ctr->y() + u[0] * r)});
                e["centerPoint"] = pointId(el->centerSketchPoint());
            }
            else
            {
                notes.push_back(sk->name() + ": a " + c->objectType() + " is not exported");
                continue;
            }
            if (c->isConstruction()) e["construction"] = true;
            ids[c->entityToken()] = sid;
            d["entities"].push_back(e);
        }
        Ptr<SketchPoints> pts = sk->sketchPoints();
        for (size_t i = 0; i < pts->count(); i++)
        {
            Ptr<SketchPoint> p = pts->item(i);
            if (p == originPt || p->isReference()) continue;
            // the end points of projected curves are Fusion's, not the user's (not marked as references themselves):
            // written as free points they would leave the re-imported sketch under-defined (L06 round trip)
            if (Ptr<SketchEntityList> conn = p->connectedEntities())
            {
                bool onlyRefs = conn->count() > 0;
                for (size_t k = 0; k < conn->count(); k++)
                    if (!conn->item(k)->isReference()) onlyRefs = false;
                if (onlyRefs) continue;
            }
            pointId(p);
        }
        // geometry projected from another sketch of this part: a reference to that sketch's entity (FORMAT-SPEC 4,
        // Sketch, `sketchEntity`); the projection is the relation's meaning when it lies off this plane
        auto otherSketchRef = [&](const Ptr<SketchEntity>& se) -> json {
            Ptr<Base> re = se ? se->referencedEntity() : nullptr;
            // the component's origin projected into the sketch
            if (Ptr<ConstructionPoint> cp = re)
            {
                Ptr<ConstructionPoint> o = comp->originConstructionPoint();
                Ptr<ConstructionPoint> nat = cp->nativeObject() ? cp->nativeObject() : cp;
                if (nat == o || cp == o) return json{{"kind", "origin"}, {"by", "id"}};
            }
            // a model edge or vertex projected into the sketch: the relation is to that geometry (FORMAT-SPEC 4: a
            // relation entity may be a face/edge/vertex descriptor, as the SolidWorks exporter writes them)
            if (Ptr<BRepEdge> be = re) return descriptor(be);
            if (Ptr<BRepVertex> bv = re) return descriptor(bv);
            Ptr<SketchEntity> src = re;
            if (!src) return nullptr;
            // an entity of an Insert Part copy (attribute NCX/transferred): the source document's own entity
            if (Ptr<Sketch> ps = src->parentSketch())
            {
                Ptr<Sketch> nps = ps->nativeObject() ? ps->nativeObject() : ps;
                Ptr<Attribute> tr = nps->attributes()->itemByName("NCX", "transferred");
                Ptr<Attribute> eid = src->attributes()->itemByName("NCX", "id");
                if (!eid)
                {
                    // a proxy: the attribute is on its native entity
                    Ptr<SketchEntity> nat;
                    if (Ptr<SketchPoint> x = src) nat = x->nativeObject();
                    else if (Ptr<SketchLine> x = src) nat = x->nativeObject();
                    else if (Ptr<SketchCircle> x = src) nat = x->nativeObject();
                    else if (Ptr<SketchArc> x = src) nat = x->nativeObject();
                    if (nat) eid = nat->attributes()->itemByName("NCX", "id");
                }
                if (tr && eid)
                {
                    json r = json::parse(tr->value(), nullptr, false);
                    if (r.is_object())
                        return json{{"kind", "sketchEntity"}, {"by", "id"}, {"document", r.value("document", "")}, {"feature", r.value("feature", "")},
                                    {"id", eid->value()}};
                }
            }
            // in an assembly the source comes back as a proxy (another token): its native entity is the one written
            std::vector<std::string> toks{src->entityToken()};
            if (Ptr<SketchPoint> sp = src)
                if (Ptr<SketchPoint> nat = sp->nativeObject()) toks.push_back(nat->entityToken());
            if (Ptr<SketchLine> sl = src)
                if (Ptr<SketchLine> nat = sl->nativeObject()) toks.push_back(nat->entityToken());
            if (Ptr<SketchCircle> sc = src)
                if (Ptr<SketchCircle> nat = sc->nativeObject()) toks.push_back(nat->entityToken());
            if (Ptr<SketchArc> sa = src)
                if (Ptr<SketchArc> nat = sa->nativeObject()) toks.push_back(nat->entityToken());
            for (const std::string& tok : toks)
                for (auto& kv : sketchIds)
                {
                    if (kv.first == id) continue;
                    auto it = kv.second.find(tok);
                    if (it != kv.second.end()) return json{{"kind", "sketchEntity"}, {"by", "id"}, {"feature", kv.first}, {"id", it->second}};
                }
            return nullptr;
        };
        auto ref = [&](const Ptr<Base>& b) -> json {
            if (Ptr<SketchPoint> p = b)
            {
                if (p == originPt) return json{{"kind", "origin"}, {"by", "id"}};
                if (p->isReference()) return otherSketchRef(p);
            }
            if (Ptr<SketchEntity> se = b)
            {
                auto it = ids.find(se->entityToken());
                if (it != ids.end()) return it->second;
                if (se->isReference()) return otherSketchRef(se);
            }
            return nullptr;
        };
        // relations
        Ptr<GeometricConstraints> gcs = sk->geometricConstraints();
        for (size_t i = 0; i < gcs->count(); i++)
        {
            Ptr<GeometricConstraint> g = gcs->item(i);
            json c;
            std::vector<Ptr<Base>> ents;
            std::string t;
            if (Ptr<CoincidentConstraint> k = g) t = "coincident", ents = {k->point(), k->entity()};
            else if (Ptr<HorizontalConstraint> k = g) t = "horizontal", ents = {k->line()};
            else if (Ptr<VerticalConstraint> k = g) t = "vertical", ents = {k->line()};
            else if (Ptr<ParallelConstraint> k = g) t = "parallel", ents = {k->lineOne(), k->lineTwo()};
            else if (Ptr<PerpendicularConstraint> k = g) t = "perpendicular", ents = {k->lineOne(), k->lineTwo()};
            else if (Ptr<TangentConstraint> k = g) t = "tangent", ents = {k->curveOne(), k->curveTwo()};
            else if (Ptr<EqualConstraint> k = g) t = "equal", ents = {k->curveOne(), k->curveTwo()};
            else if (Ptr<ConcentricConstraint> k = g) t = "concentric", ents = {k->entityOne(), k->entityTwo()};
            else if (Ptr<MidPointConstraint> k = g) t = "midpoint", ents = {k->point(), k->midPointCurve()};
            else if (Ptr<CollinearConstraint> k = g) t = "collinear", ents = {k->lineOne(), k->lineTwo()};
            else if (Ptr<SymmetryConstraint> k = g) t = "symmetric", ents = {k->entityOne(), k->entityTwo(), k->symmetryLine()};
            else if (Ptr<HorizontalPointsConstraint> k = g) t = "horizontalPoints", ents = {k->pointOne(), k->pointTwo()};
            else if (Ptr<VerticalPointsConstraint> k = g) t = "verticalPoints", ents = {k->pointOne(), k->pointTwo()};
            else
            {
                d["constraints"].push_back(json{{"type", g->objectType()}, {"entities", json::array()}, {"supported", false}});
                continue;
            }
            json ej = json::array();
            bool ok = true;
            for (auto& e : ents)
            {
                json r = ref(e);
                if (r.is_null()) ok = false;
                ej.push_back(r);
            }
            if (!ok)
            {
                std::string what;
                for (auto& e : ents)
                    if (Ptr<SketchEntity> se = e)
                        if (se->isReference())
                        {
                            Ptr<Base> re = se->referencedEntity();
                            what = re ? std::string(" (projected from ") + re->objectType() + ")" : " (its source is not reported by Fusion)";
                        }
                notes.push_back(sk->name() + ": a " + t + " relation to projected geometry is not exported" + what);
                continue;
            }
            d["constraints"].push_back(json{{"type", t}, {"entities", ej}});
        }
        for (size_t i = 0; i < pts->count(); i++)
            if (pts->item(i)->isFixed() && pts->item(i) != originPt)
                if (auto r = ref(pts->item(i)); !r.is_null()) d["constraints"].push_back(json{{"type", "fix"}, {"entities", {r}}});
        // dimensions
        Ptr<SketchDimensions> dims = sk->sketchDimensions();
        for (size_t i = 0; i < dims->count(); i++)
        {
            Ptr<SketchDimension> dm = dims->item(i);
            json j{{"id", "d" + std::to_string(i + 1)}, {"driving", dm->isDriving()}};
            Ptr<ModelParameter> p = dm->parameter();
            std::string nat = p ? p->comment() : "";
            std::string nm = nat.find('@') != std::string::npos ? nat.substr(0, nat.find('@')) : (p ? p->name() : "D" + std::to_string(i + 1));
            j["name"] = nm;
            j["fullName"] = nm + "@" + sk->name();
            std::vector<Ptr<Base>> ents;
            if (Ptr<SketchLinearDimension> k = dm)
            {
                ents = {k->entityOne(), k->entityTwo()};
                DimensionOrientations o = k->orientation();
                j["kind"] = o == HorizontalDimensionOrientation ? "horizontal" : o == VerticalDimensionOrientation ? "vertical" : "distance";
                j["value"] = cm2mm(dm->value());
            }
            else if (Ptr<SketchOffsetDimension> k = dm)
                ents = {k->line(), k->entityTwo()}, j["kind"] = "distance", j["value"] = cm2mm(dm->value());
            else if (Ptr<SketchDiameterDimension> k = dm)
                ents = {k->entity()}, j["kind"] = "diameter", j["value"] = cm2mm(dm->value());
            else if (Ptr<SketchRadialDimension> k = dm)
                ents = {k->entity()}, j["kind"] = "radius", j["value"] = cm2mm(dm->value());
            else if (Ptr<SketchAngularDimension> k = dm)
                ents = {k->lineOne(), k->lineTwo()}, j["kind"] = "angle", j["value"] = rad2deg(dm->value());
            else
            {
                notes.push_back(sk->name() + ": a " + dm->objectType() + " is not exported");
                continue;
            }
            json ej = json::array();
            bool ok = true;
            for (auto& e : ents)
            {
                json r = ref(e);
                if (r.is_null()) ok = false;
                ej.push_back(r);
            }
            if (!ok)
            {
                notes.push_back(sk->name() + ": a dimension to projected geometry is not exported");
                continue;
            }
            j["entities"] = ej;
            if (Ptr<Point3D> tp = dm->textPosition()) j["textPosition"] = uv(tp);
            if (p && !p->expression().empty() && p->dependencyParameters() && p->dependencyParameters()->count() > 0) j["param"] = p->name();
            d["dimensions"].push_back(j);
        }
        return d;
    }

    // ---------------------------------------------------------------------------------- profiles
    json regionsOut = json::array();   // profileRef: the regions of the last profile read
    json profileRef(const Ptr<Base>& prof, json& contours)
    {
        regionsOut = json::array();
        // one sketch curve (a loft rail): its sketch
        if (Ptr<SketchCurve> c = prof)
        {
            std::string id = idOf(c->parentSketch());
            if (id.empty()) return nullptr;
            return json{{"kind", "feature"}, {"by", "id"}, {"id", id}};
        }
        // open curves (a surface extrude or revolve): the sketch they all come from, as a whole
        if (Ptr<Path> path = prof)
        {
            std::string sid;
            for (size_t i = 0; i < path->count(); i++)
            {
                Ptr<SketchCurve> c = path->item(i)->entity();
                std::string id = c ? idOf(c->parentSketch()) : "";
                if (id.empty() || (!sid.empty() && id != sid)) return nullptr;
                sid = id;
            }
            if (sid.empty()) return nullptr;
            return json{{"kind", "feature"}, {"by", "id"}, {"id", sid}};
        }
        // a feature made while its component's occurrence was active keeps its profiles as proxies (measured: the
        // model in the upright wrapper, common.h): they are read as their native profiles, in the sketch's own space
        auto natProfile = [](const Ptr<Profile>& q) -> Ptr<Profile> { return q && q->nativeObject() ? q->nativeObject() : q; };
        Ptr<Base> prof0 = prof;
        if (Ptr<ObjectCollection> oc = prof)
        {
            Ptr<ObjectCollection> n = ObjectCollection::create();
            for (size_t i = 0; i < oc->count(); i++)
                if (Ptr<Profile> q = oc->item(i)) n->add(natProfile(q));
                else n->add(oc->item(i));
            prof0 = n;
        }
        else if (Ptr<Profile> q = prof)
            prof0 = natProfile(q);
        return profileRefNative(prof0, contours);
    }

    json profileRefNative(const Ptr<Base>& prof, json& contours)
    {
        Ptr<Profile> p = prof;
        if (!p)
            if (Ptr<ObjectCollection> oc = prof)
                if (oc->count() > 0) p = oc->item(0);
        if (!p) return nullptr;
        Ptr<Sketch> sk = p->parentSketch();
        // a sketch of an Insert Part copy (attribute NCX/transferred): the source document's sketch, transferred
        // (FORMAT-SPEC 4 insertPart), the whole sketch as the source wrote it
        {
            Ptr<Sketch> nsk = sk && sk->nativeObject() ? sk->nativeObject() : sk;
            if (Ptr<Attribute> tr = nsk ? nsk->attributes()->itemByName("NCX", "transferred") : nullptr)
            {
                json r = json::parse(tr->value(), nullptr, false);
                if (r.is_object() && !r.value("document", "").empty() && !r.value("feature", "").empty())
                    return json{{"kind", "transferred"}, {"by", "id"}, {"document", r.value("document", "")}, {"feature", r.value("feature", "")},
                                {"name", nsk->name()}};
            }
        }
        std::string sid = idOf(sk);
        if (sid.empty()) return nullptr;
        // Selected regions when the feature uses fewer than the odd-even set (FORMAT-SPEC 4). A profile whose loops
        // are whole sketch curves goes out as `contours` (every loop: an inner one is a hole by nesting); any other
        // as a `region`: a point inside it, its area and clearance, and its `boundary` (per loop the ids of the
        // segments it runs along, a curve on no segment as {line} / {arc}). (Round 7: P49's circle segment went out
        // as the contour [s2, s1], which an importer reads as the circle's other part, and a ring as its outer
        // circle only.)
        // Written explicitly whenever the sketch has more than one profile: Fusion's "every profile" is not a whole
        // sketch by the odd-even rule when profiles share curves (adjacent regions, a revolve axis on the boundary:
        // the Dome Head's Revolve1 then failed in SolidWorks).
        if (sk->profiles()->count() > 1)
        {
            auto& ids = sketchIds[sid];
            std::vector<Ptr<Profile>> ps;
            if (Ptr<ObjectCollection> oc = prof)
                for (size_t i = 0; i < oc->count(); i++) ps.push_back(oc->item(i));
            else
                ps.push_back(p);
            auto idOfEntity = [&](const Ptr<SketchEntity>& se) -> std::string {
                if (!se) return "";
                auto it = ids.find(se->entityToken());
                return it == ids.end() ? "" : it->second;
            };
            // a profile curve's points in sketch space (mm); the geometry comes in model space, except on some
            // planes where it is already in sketch space (importer finding, round 6)
            auto curvePts = [&](const Ptr<ProfileCurve>& pc, int n) {
                std::vector<std::array<double, 2>> pts, raw;
                Ptr<Curve3D> g = pc->geometry();
                if (!g) return pts;
                Ptr<CurveEvaluator3D> ev = g->evaluator();
                double s0 = 0, s1 = 0;
                if (!ev->getParameterExtents(s0, s1)) return pts;
                bool off = false;
                for (int k = 0; k <= n; k++)
                {
                    Ptr<Point3D> q;
                    if (!ev->getPointAtParameter(s0 + (s1 - s0) * k / n, q)) continue;
                    Ptr<Point3D> t = sk->modelToSketchSpace(q);
                    if (std::fabs(t->z()) > 1e-6) off = true;
                    pts.push_back({cm2mm(t->x()), cm2mm(t->y())});
                    raw.push_back({cm2mm(q->x()), cm2mm(q->y())});
                }
                return off ? raw : pts;
            };
            auto curveLen = [](const Ptr<Curve3D>& g) {
                double s0 = 0, s1 = 0, L = -1;
                if (g && g->evaluator()->getParameterExtents(s0, s1)) g->evaluator()->getLengthAtParameter(s0, s1, L);
                return L;
            };
            for (auto& q : ps)
            {
                Ptr<ProfileLoops> ls = q->profileLoops();
                bool whole = true;
                for (size_t i = 0; i < ls->count() && whole; i++)
                    for (size_t k = 0; k < ls->item(i)->profileCurves()->count() && whole; k++)
                    {
                        Ptr<ProfileCurve> pc = ls->item(i)->profileCurves()->item(k);
                        Ptr<SketchCurve> sc = pc->sketchEntity();
                        if (!sc || idOfEntity(sc).empty() || std::fabs(curveLen(pc->geometry()) - sc->length()) > 1e-7) whole = false;
                    }
                if (whole)
                {
                    for (size_t i = 0; i < ls->count(); i++)
                    {
                        json c = json::array();
                        for (size_t k = 0; k < ls->item(i)->profileCurves()->count(); k++)
                            c.push_back(idOfEntity(ls->item(i)->profileCurves()->item(k)->sketchEntity()));
                        contours.push_back(c);
                    }
                    continue;
                }
                json boundary = json::array();
                std::vector<std::vector<std::array<double, 2>>> polys;
                for (size_t i = 0; i < ls->count(); i++)
                {
                    json loop = json::array();
                    std::string last;
                    for (size_t k = 0; k < ls->item(i)->profileCurves()->count(); k++)
                    {
                        Ptr<ProfileCurve> pc = ls->item(i)->profileCurves()->item(k);
                        auto pts = curvePts(pc, 32);
                        polys.push_back(pts);
                        std::string id = idOfEntity(pc->sketchEntity());
                        if (!id.empty())
                        {
                            if (id != last) loop.push_back(id);
                            last = id;
                            continue;
                        }
                        last.clear();
                        if (pts.size() < 2) continue;
                        Ptr<Curve3D> g = pc->geometry();
                        if (Ptr<Line3D>(g))
                            loop.push_back(json{{"line", json::array({json::array({pts.front()[0], pts.front()[1]}), json::array({pts.back()[0], pts.back()[1]})})}});
                        else
                        {
                            // centre and radius from three points of the arc
                            auto& A = pts.front();
                            auto& B = pts[pts.size() / 2];
                            auto& C = pts.back();
                            double d = 2 * (A[0] * (B[1] - C[1]) + B[0] * (C[1] - A[1]) + C[0] * (A[1] - B[1]));
                            if (std::fabs(d) < 1e-12) continue;
                            double a2 = A[0] * A[0] + A[1] * A[1], b2 = B[0] * B[0] + B[1] * B[1], c2 = C[0] * C[0] + C[1] * C[1];
                            double ux = (a2 * (B[1] - C[1]) + b2 * (C[1] - A[1]) + c2 * (A[1] - B[1])) / d;
                            double uy = (a2 * (C[0] - B[0]) + b2 * (A[0] - C[0]) + c2 * (B[0] - A[0])) / d;
                            loop.push_back(json{{"arc", json::array({json::array({ux, uy}), std::hypot(A[0] - ux, A[1] - uy)})}});
                        }
                    }
                    boundary.push_back(loop);
                }
                // a point well inside: on the row where the region is widest, the middle of the widest span
                double y0 = 1e300, y1 = -1e300, px = 0, py = 0, bestW = 0;
                for (auto& pl : polys)
                    for (auto& t : pl)
                    {
                        y0 = std::min(y0, t[1]);
                        y1 = std::max(y1, t[1]);
                    }
                for (int k = 1; k < 40 && y1 > y0; k++)
                {
                    double yy = y0 + (y1 - y0) * (k + 0.37) / 40.5;
                    std::vector<double> xs;
                    for (auto& pl : polys)
                        for (size_t i = 1; i < pl.size(); i++)
                            if ((pl[i][1] > yy) != (pl[i - 1][1] > yy))
                                xs.push_back(pl[i][0] + (yy - pl[i][1]) * (pl[i - 1][0] - pl[i][0]) / (pl[i - 1][1] - pl[i][1]));
                    std::sort(xs.begin(), xs.end());
                    for (size_t i = 0; i + 1 < xs.size(); i += 2)
                        if (xs[i + 1] - xs[i] > bestW)
                        {
                            bestW = xs[i + 1] - xs[i];
                            px = 0.5 * (xs[i] + xs[i + 1]);
                            py = yy;
                        }
                }
                double clearance = 1e300;
                for (auto& pl : polys)
                    for (size_t i = 1; i < pl.size(); i++)
                    {
                        double ax = pl[i - 1][0], ay = pl[i - 1][1], dx = pl[i][0] - ax, dy = pl[i][1] - ay, L2 = dx * dx + dy * dy;
                        double t = L2 > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / L2, 0.0, 1.0) : 0.0;
                        clearance = std::min(clearance, std::hypot(px - ax - t * dx, py - ay - t * dy));
                    }
                Ptr<AreaProperties> ap = q->areaProperties();
                regionsOut.push_back(json{{"point", json::array({px, py})}, {"area", ap ? cm2mm(cm2mm(ap->area())) : 0.0},
                                          {"clearance", clearance}, {"boundary", boundary}});
            }
            // Profiles that share a whole loop (a ring and the area inside it) make one area without that loop: a loop
            // listed an even number of times is interior to the union and goes, else it would be read as a hole (the
            // spec's nesting rule; P46's cope outlines were rebuilt hollow)
            std::map<std::vector<std::string>, int> seen;
            for (auto& c : contours)
            {
                std::vector<std::string> k;
                for (auto& x : c) k.push_back(x.get<std::string>());
                std::sort(k.begin(), k.end());
                seen[k]++;
            }
            json kept = json::array();
            std::set<std::vector<std::string>> done;
            for (auto& c : contours)
            {
                std::vector<std::string> k;
                for (auto& x : c) k.push_back(x.get<std::string>());
                std::sort(k.begin(), k.end());
                if (seen[k] % 2 == 1 && !done.count(k)) kept.push_back(c);
                done.insert(k);
            }
            contours = kept;
        }
        return json{{"kind", "feature"}, {"by", "id"}, {"id", sid}};
    }

    json extentJson(const Ptr<ExtentDefinition>& x)
    {
        if (Ptr<DistanceExtentDefinition> d = x) return json{{"type", "blind"}, {"depth", cm2mm(d->distance()->value())}};
        if (Ptr<ThroughAllExtentDefinition> t = x) return json{{"type", "throughAll"}};
        if (Ptr<AllExtentDefinition> t = x) return json{{"type", "throughAll"}};   // Fusion's "All"
        if (Ptr<ToEntityExtentDefinition> t = x)
        {
            json r{{"type", "upToSurface"}};
            if (Ptr<BRepFace> f = t->entity()) r["target"] = descriptor(f);
            else if (Ptr<BRepVertex> v = t->entity())
            {
                V3 pt = ptMm(v->geometry());
                json g{{"type", "point"}, {"params", {{"point", vj(pt)}}}, {"samples", json::array({vj(pt)})}, {"bbox", {pt[0], pt[1], pt[2], pt[0], pt[1], pt[2]}}};
                r = json{{"type", "upToVertex"}, {"target", {{"kind", "vertex"}, {"by", "geom"}, {"geom", g}}}};
            }
            else return nullptr;
            if (Ptr<ModelParameter> op = t->offset()) r["offset"] = cm2mm(op->value());
            return r;
        }
        return nullptr;
    }

    // a body by its mass properties (FORMAT-SPEC 5 body references), measured as the native body in the part's frame
    json bodyRef(Ptr<BRepBody> b)
    {
        if (b->nativeObject()) b = b->nativeObject();
        json fp = bodyFingerprint(b);
        return json{{"kind", "body"}, {"by", "geom"}, {"geom", {{"volume", fp["volume"]}, {"centroid", fp["centroid"]}, {"bbox", fp["bbox"]}}}};
    }

    // a written feature's payload id (a proxy is looked up as its native feature)
    std::string featureId(Ptr<Feature> f)
    {
        if (f && f->assemblyContext())
            if (Ptr<Feature> nf = f->parentComponent()->features()->itemByName(f->name())) f = nf;
        return f ? idOf(f) : "";
    }

    // a pattern's or mirror's input entities: features as `seeds`, bodies as `bodies`
    bool seedsJson(const Ptr<ObjectCollection>& in, json& d, std::string& why)
    {
        json seeds = json::array(), bodies = json::array();
        for (size_t i = 0; in && i < in->count(); i++)
        {
            Ptr<Base> o = in->item(i);
            if (Ptr<BRepBody> b = o) bodies.push_back(bodyRef(b));
            else if (Ptr<Feature> f = o)
            {
                std::string id = featureId(f);
                if (id.empty())
                {
                    why = "its seed feature " + f->name() + " was not written";
                    return false;
                }
                seeds.push_back(json{{"kind", "feature"}, {"by", "id"}, {"id", id}});
            }
            else
            {
                why = std::string("a seed of type ") + (o ? o->objectType() : "?") + " is not carried";
                return false;
            }
        }
        d["seeds"] = seeds;
        if (!bodies.empty()) d["bodies"] = bodies;
        return true;
    }

    // A direction or axis reference (a pattern's direction, a circular pattern's axis) and its direction in the part's
    // frame. Fusion's origin axes are `originAxis` (FORMAT-SPEC 5).
    json directionRef(Ptr<Base> ent, V3& dir, std::string& why)
    {
        if (Ptr<BRepEdge> ed = ent)
        {
            if (ed->nativeObject()) ed = ed->nativeObject();
            json r = descriptor(ed);
            if (r["geom"].contains("params") && r["geom"]["params"].contains("direction")) dir = v3(r["geom"]["params"]["direction"]);
            else if (r["geom"].contains("params") && r["geom"]["params"].contains("axis")) dir = v3(r["geom"]["params"]["axis"]);
            else
            {
                why = "its direction edge is not straight or circular";
                return nullptr;
            }
            return r;
        }
        if (Ptr<BRepFace> fc = ent)
        {
            if (fc->nativeObject()) fc = fc->nativeObject();
            json r = descriptor(fc);
            const json& p = r["geom"]["params"];
            if (p.contains("axis")) dir = v3(p["axis"]);
            else if (p.contains("normal")) dir = v3(p["normal"]);
            else
            {
                why = "its direction face is neither planar nor turned";
                return nullptr;
            }
            return r;
        }
        if (Ptr<ConstructionAxis> ax = ent)
        {
            if (ax->nativeObject()) ax = ax->nativeObject();
            const char* letter = ax == comp->xConstructionAxis() ? "x" : ax == comp->yConstructionAxis() ? "y" : ax == comp->zConstructionAxis() ? "z" : nullptr;
            if (Ptr<InfiniteLine3D> g = ax->geometry()) dir = unit(vecOf(g->direction()));
            if (letter) return json{{"kind", "originAxis"}, {"by", "id"}, {"axis", letter}};
            // an axis the import made on a cylindrical face (a pattern axis): the face
            if (Ptr<ConstructionAxisCircularFaceDefinition> cd = ax->definition())
                if (Ptr<BRepFace> fc = cd->circularFace()) return directionRef(fc, dir, why);
            std::string id = idOf(ax);
            if (!id.empty()) return json{{"kind", "feature"}, {"by", "id"}, {"id", id}};
            why = "its axis is not carried";
            return nullptr;
        }
        if (Ptr<ConstructionPlane> cp = ent)
        {
            json r = planeRef(cp);
            if (Ptr<Plane> g = (cp->nativeObject() ? cp->nativeObject() : cp)->geometry()) dir = unit(vecOf(g->normal()));
            if (r.value("kind", "") == "frame")
            {
                why = "its direction plane cannot be referenced";
                return nullptr;
            }
            return r;
        }
        if (Ptr<SketchLine> l = ent)
        {
            if (l->nativeObject()) l = l->nativeObject();
            std::string sid = idOf(l->parentSketch());
            auto it = sketchIds[sid].find(l->entityToken());
            if (sid.empty() || it == sketchIds[sid].end())
            {
                why = "its direction line is not in a written sketch";
                return nullptr;
            }
            dir = unit(sub(modelPointMm(l->endSketchPoint()), modelPointMm(l->startSketchPoint())));
            return json{{"kind", "sketchEntity"}, {"by", "id"}, {"feature", sid}, {"id", it->second}};
        }
        why = std::string("a direction of type ") + (ent ? ent->objectType() : "?") + " is not carried";
        return nullptr;
    }

    // ---- sheet metal helpers (edge flanges)
    bool sheetMetalWritten = false;      // the part's `sheetMetal` (its rule) is written before its first flange
    json preFeatures = json::array();    // features featureDef writes before the one it returns (a rule, flange profiles)
    std::vector<V3> flangeRise;          // the edge flange being written: per edge, the way its web rises (walk)
    std::vector<std::vector<V3>> flangeWeb;   // and the vertices of its web faces (its width and height, measured)

    static V3 outwardNormalOf(const Ptr<BRepFace>& f)
    {
        Ptr<Vector3D> n;
        if (f->evaluator()->getNormalAtPoint(f->pointOnFace(), n) && n) return unit(vecOf(n));
        Ptr<Plane> p = f->geometry();
        return p ? unit(vecOf(p->normal())) : V3{0, 0, 0};
    }
    // the outward normal of the broad planar face at a sheet's edge (the larger of its two faces)
    static V3 broadNormal(const Ptr<BRepEdge>& e)
    {
        Ptr<BRepFace> best;
        for (size_t i = 0; i < e->faces()->count(); i++)
        {
            Ptr<BRepFace> f = e->faces()->item(i);
            if (!Ptr<Plane>(f->geometry())) continue;
            if (!best || f->area() > best->area()) best = f;
        }
        return best ? outwardNormalOf(best) : V3{0, 0, 0};
    }
    // the outward normal of the planar face at the edge that lies most across this direction (the face a flange rises
    // from, or the one opposite; at 120 degrees the edge's side face is as far off as 0.5), or zero
    static V3 risingFaceNormal(const Ptr<BRepEdge>& e, const V3& y)
    {
        V3 best{0, 0, 0};
        double bd = 0.5;
        for (size_t i = 0; i < e->faces()->count(); i++)
        {
            Ptr<BRepFace> f = e->faces()->item(i);
            if (!Ptr<Plane>(f->geometry())) continue;
            V3 n = outwardNormalOf(f);
            if (std::fabs(dot(n, y)) > bd) bd = std::fabs(dot(n, y)), best = n;
        }
        return best;
    }
    // a straight edge of the part parallel to this one with its middle at this point
    Ptr<BRepEdge> parallelEdge(const Ptr<BRepEdge>& e, const V3& mid)
    {
        V3 d = unit(sub(ptMm(e->endVertex()->geometry()), ptMm(e->startVertex()->geometry())));
        for (auto& b : solids())
            for (size_t i = 0; i < b->edges()->count(); i++)
            {
                Ptr<BRepEdge> c = b->edges()->item(i);
                if (!Ptr<Line3D>(c->geometry())) continue;
                V3 a = ptMm(c->startVertex()->geometry()), z = ptMm(c->endVertex()->geometry());
                if (std::fabs(std::fabs(dot(unit(sub(z, a)), d)) - 1) > 1e-9) continue;
                if (dist(mul(add(a, z), 0.5), mid) < 1e-4) return c;
            }
        return nullptr;
    }
    // walk, for an edge flange: each edge's broad-face normal before it, then (with the flange computed) the side its
    // web lies on, by probing points beyond the bend just across the edge
    void readFlangeRise(const Ptr<TimelineObject>& t, const Ptr<FlangeFeature>& fl)
    {
        flangeRise.clear();
        Ptr<EdgeFlangeFeatureDefinition> ed = fl->definition();
        Ptr<SheetMetalRule> rule = comp->activeSheetMetalRule();
        if (!ed || !rule) return;
        double th = cm2mm(rule->thickness()->value()), r = cm2mm(rule->bendRadius()->value());
        double len = ed->distance() ? cm2mm(ed->distance()->value()) : 0;
        Ptr<Timeline> tl = des->timeline();
        t->rollTo(true);
        struct E
        {
            V3 mid, dir, n;
        };
        std::vector<E> es;
        Ptr<FlangeEdgeDefinitions> eds = ed->edgeDefinitions();
        for (size_t i = 0; eds && i < eds->count(); i++)
        {
            Ptr<BRepEdge> edge = eds->item(i)->edge();
            if (edge && edge->nativeObject()) edge = edge->nativeObject();
            if (!edge) return;
            V3 a = ptMm(edge->startVertex()->geometry()), b = ptMm(edge->endVertex()->geometry());
            es.push_back(E{mul(add(a, b), 0.5), unit(sub(b, a)), broadNormal(edge)});
        }
        t->rollTo(false);
        std::vector<Ptr<BRepBody>> bodies = solids();
        auto inside = [&](const V3& p) {
            for (auto& b : bodies)
                if (b->pointContainment(ptCm(p)) == PointInsidePointContainment) return true;
            return false;
        };
        // the web: the flange's flat faces across the edge (normal square to the edge, not parallel to the sheet); its
        // direction away from the edge, at any flange angle, and its vertices
        Ptr<BRepFaces> ff = fl->faces();
        flangeWeb.assign(es.size(), {});
        std::vector<V3> webDir(es.size(), V3{0, 0, 0});
        std::vector<double> webArea(es.size(), 0.0);   // the direction from the largest face (not the flange's thin top)
        for (size_t i = 0; ff && i < ff->count(); i++)
        {
            Ptr<BRepFace> f = ff->item(i);
            if (f->nativeObject()) f = f->nativeObject();
            if (!Ptr<Plane>(f->geometry())) continue;
            V3 nn = outwardNormalOf(f), pc = ptMm(f->pointOnFace());
            size_t best = es.size();
            double bd = 1e300;
            for (size_t k = 0; k < es.size(); k++)
            {
                if (std::fabs(dot(nn, es[k].dir)) > 1e-6 || std::fabs(std::fabs(dot(nn, es[k].n)) - 1) < 1e-6) continue;
                V3 q = sub(pc, es[k].mid);
                double d = norm(sub(q, mul(es[k].dir, dot(q, es[k].dir))));
                if (d < bd) bd = d, best = k;
            }
            if (best == es.size()) continue;
            V3 y = unit(cross(es[best].dir, nn));
            if (dot(sub(pc, es[best].mid), y) < 0) y = mul(y, -1);
            // a web face rises clear of the sheet (the sheet's own edge face beside the flange does not)
            double rise = 0;
            for (size_t v = 0; v < f->vertices()->count(); v++)
                rise = std::max(rise, dot(sub(ptMm(f->vertices()->item(v)->geometry()), es[best].mid), y));
            if (rise < th + 1e-6) continue;
            if (f->area() > webArea[best])
            {
                webArea[best] = f->area();
                webDir[best] = y;
            }
            for (size_t v = 0; v < f->vertices()->count(); v++) flangeWeb[best].push_back(ptMm(f->vertices()->item(v)->geometry()));
        }
        for (size_t k = 0; k < es.size(); k++)
            if (norm(webDir[k]) > 0.5 && !flangeWeb[k].empty())
            {
                flangeRise.push_back(webDir[k]);
                if (k + 1 == es.size())
                {
                    tl->moveToEnd();
                    return;
                }
            }
        flangeRise.clear();
        flangeWeb.clear();
        for (auto& x : es)
        {
            V3 w = unit(cross(x.dir, x.n)), rise{0, 0, 0};
            for (double s : {1.0, -1.0})
            {
                V3 y = mul(x.n, s);
                bool hit = false;
                for (double h : {r + th + 0.5 * std::max(len - r - th, 0.5), r + th + 0.25})
                    for (double o : {0.5 * th, -0.5 * th, 1.5 * th, -1.5 * th, r + 0.5 * th, -(r + 0.5 * th), r + 1.5 * th, -(r + 1.5 * th)})
                        if (!hit && inside(add(x.mid, add(mul(y, h), mul(w, o))))) hit = true;
                if (hit)
                {
                    rise = y;
                    break;
                }
            }
            if (norm(rise) < 0.5)
            {
                flangeRise.clear();
                break;
            }
            flangeRise.push_back(rise);
        }
        tl->moveToEnd();
    }

    // ---------------------------------------------------------------------------------- features
    // the feature at the rolled-back timeline position: a json feature or null (opaque)
    json featureDef(const Ptr<Base>& e, std::string& type, std::string& why)
    {
        if (Ptr<ExtrudeFeature> f = e)
        {
            bool cut = f->operation() == CutFeatureOperation;
            type = cut ? "cut" : "extrude";
            json contours = json::array();
            json prof = profileRef(f->profile(), contours);
            if (prof.is_null())
            {
                why = "its profile is not a rebuilt sketch";
                return nullptr;
            }
            json d{{"profile", prof}, {"merge", f->operation() == JoinFeatureOperation}};
            if (cut) d.erase("merge");   // a cut has no merge (SolidWorks' import passed false on and the cut failed)
            if (!contours.empty()) d["contours"] = contours;
            if (!regionsOut.empty()) d["regions"] = regionsOut;
            if (f->isSolid() == false)
            {
                type = "extrudedSurface";
            }
            Ptr<ExtentDefinition> x1 = f->extentOne(), x2 = f->extentTwo();
            bool neg = false;
            if (Ptr<SymmetricExtentDefinition> s = x1)
            {
                Ptr<ModelParameter> sp = s->distance();
                double dist = sp ? cm2mm(sp->value()) : 0;
                d["end"] = json{{"type", "midPlane"}, {"depth", s->isFullLength() ? dist : 2 * dist}};
            }
            else
            {
                json e1 = extentJson(x1);
                if (e1.is_null())
                {
                    why = "its extent type is not carried";
                    return nullptr;
                }
                d["end"] = e1;
                if (x2)
                {
                    json e2 = extentJson(x2);
                    if (e2.is_null())
                    {
                        why = "its second extent type is not carried";
                        return nullptr;
                    }
                    d["end2"] = e2;
                }
                // a one-side extent's direction: negative distances or the flip
                neg = extrudeNegative;   // measured with the feature computed (walk)

                if (d["end"].contains("depth")) d["end"]["depth"] = std::fabs(d["end"]["depth"].get<double>());
            }
            // importer: flip = reverse != cut  =>  reverse = neg != cut
            d["reverse"] = neg != cut;
            if (f->startExtent())
                if (Ptr<OffsetStartDefinition> os = f->startExtent())
                    if (Ptr<ModelParameter> op = os->offset())
                        d["start"] = json{{"type", "offset"}, {"distance", std::fabs(cm2mm(op->value()))}, {"reverse", op->value() < 0}};
            if (f->taperAngleOne() && std::fabs(f->taperAngleOne()->value()) > 1e-12)
            {
                why = "a tapered extrude is not carried yet";
                return nullptr;
            }
            if (f->isThinExtrude())
            {
                // FORMAT-SPEC 4 `thin`, as the importer reads it (features.cpp): SolidWorks' one-direction wall is
                // Fusion's Side2 for the same path and normal (measured on P19), the opposite direction Side1
                ThinExtrudeWallLocation l1 = f->thinExtrudeWallLocationOne();
                double k1 = f->thinExtrudeWallThicknessOne() ? cm2mm(f->thinExtrudeWallThicknessOne()->value()) : 0;
                double k2 = f->thinExtrudeWallThicknessTwo() ? cm2mm(f->thinExtrudeWallThicknessTwo()->value()) : 0;
                json th{{"thickness", k1}, {"thickness2", 0}, {"capEnds", false}, {"capThickness", 0}};
                if (l1 == Center) th["type"] = "midPlane";
                else if (k2 > 1e-9)
                {
                    th["type"] = "twoDirection";
                    th["thickness"] = l1 == Side2 ? k1 : k2;
                    th["thickness2"] = l1 == Side2 ? k2 : k1;
                }
                else th["type"] = l1 == Side2 ? "oneDirection" : "oppositeDirection";
                d["thin"] = th;
            }
            return d;
        }
        if (Ptr<RevolveFeature> f = e)
        {
            bool cut = f->operation() == CutFeatureOperation;
            type = cut ? "revolveCut" : "revolve";
            json contours = json::array();
            json prof = profileRef(f->profile(), contours);
            Ptr<SketchLine> axis = f->axis();
            if (prof.is_null() || !axis)
            {
                why = "its profile or axis is not sketch geometry";
                return nullptr;
            }
            std::string sid = idOf(axis->parentSketch());
            auto& ids = sketchIds[sid];
            Ptr<SketchLine> nat = axis->nativeObject() ? Ptr<SketchLine>(axis->nativeObject()) : axis;
            auto it = ids.find(nat->entityToken());
            if (it == ids.end())
            {
                why = "its axis is not in a rebuilt sketch";
                return nullptr;
            }
            json d{{"profile", prof}, {"axis", {{"kind", "sketchEntity"}, {"by", "id"}, {"feature", sid}, {"id", it->second}}},
                   {"merge", f->operation() == JoinFeatureOperation}, {"mode", "oneDirection"}};
            if (cut) d.erase("merge");   // a cut has no merge (P03's revolve cut failed in SolidWorks with merge false)
            if (!contours.empty()) d["contours"] = contours;
            if (!regionsOut.empty()) d["regions"] = regionsOut;
            if (!f->isSolid()) type = "revolvedSurface";
            Ptr<AngleExtentDefinition> a = f->extentDefinition();
            if (!a)
            {
                why = "its extent is not an angle";
                return nullptr;
            }
            double ang = rad2deg(a->angle()->value());
            if (a->isSymmetric()) d["mode"] = "midPlane";
            // importer: flip = reverse != (axisSense > 0), with the payload's axis = this line (sense +1)
            d["angle"] = std::fabs(ang);
            d["reverse"] = !(ang < 0);
            return d;
        }
        if (Ptr<PatchFeature> f = e)
        {
            // a planar surface from a closed sketch profile (FORMAT-SPEC 4 planarSurface)
            json contours = json::array();
            json prof = profileRef(f->boundaryCurve(), contours);
            if (prof.is_null())
            {
                why = "a patch whose boundary is not a planar sketch profile is not carried yet";
                return nullptr;
            }
            type = "planarSurface";
            return json{{"profile", prof}};
        }
        if (Ptr<StitchFeature> f = e)
        {
            // FORMAT-SPEC 4 knit: the surfaces as the features that made them (bodies are matched by name to the
            // bodies of the features already written)
            json surfaces = json::array();
            Ptr<ObjectCollection> ss = f->stitchSurfaces();
            for (size_t i = 0; ss && i < ss->count(); i++)
            {
                Ptr<BRepBody> b = ss->item(i);
                std::string fid;
                for (auto& kv : featureOfId)
                {
                    Ptr<Feature> ft = kv.second;
                    Ptr<BRepBodies> bs = ft ? ft->bodies() : nullptr;
                    for (size_t k = 0; bs && b && k < bs->count() && fid.empty(); k++)
                        if (bs->item(k)->name() == b->name()) fid = kv.first;
                }
                if (fid.empty())
                {
                    why = "a stitched surface that no exported feature made";
                    return nullptr;
                }
                surfaces.push_back(json{{"kind", "feature"}, {"by", "id"}, {"id", fid}});
            }
            type = "knit";
            Ptr<ModelParameter> tol = f->tolerance();
            bool solid = false;
            for (size_t i = 0; f->bodies() && i < f->bodies()->count(); i++) solid = solid || f->bodies()->item(i)->isSolid();
            return json{{"surfaces", surfaces}, {"formSolid", solid}, {"mergeEntities", false}, {"gapControl", false},
                        {"tolerance", tol ? cm2mm(tol->value()) : 0.0025}};
        }
        if (Ptr<FilletFeature> f = e)
        {
            // FORMAT-SPEC 4 `fillet`: constant edge sets as one constant fillet (`radii` one per edge when they
            // differ); one variable set on one edge as a variable fillet, its law from the intermediate radii the
            // importer places (straight: none; smooth: on r0 + (r1 - r0)(3s^2 - 2s^3))
            type = "fillet";
            Ptr<FilletEdgeSets> sets = f->edgeSets();
            if (!sets || sets->count() == 0)
            {
                why = "a fillet without edge sets";
                return nullptr;
            }
            json edges = json::array(), radii = json::array();
            bool allConstant = true;
            for (size_t k = 0; k < sets->count(); k++)
            {
                Ptr<ConstantRadiusFilletEdgeSet> cr = sets->item(k);
                if (!cr)
                {
                    allConstant = false;
                    break;
                }
                for (size_t i = 0; i < cr->edges()->count(); i++)
                {
                    edges.push_back(descriptor(cr->edges()->item(i)));
                    radii.push_back(cm2mm(cr->radius()->value()));
                }
            }
            if (allConstant)
            {
                json d{{"filletType", "constant"}, {"radius", radii[0]}, {"edges", edges}, {"propagate", false}};
                for (auto& r : radii)
                    if (std::fabs(r.get<double>() - radii[0].get<double>()) > 1e-9) d["radii"] = radii;
                return d;
            }
            Ptr<VariableRadiusFilletEdgeSet> vr = sets->count() == 1 ? Ptr<VariableRadiusFilletEdgeSet>(sets->item(0)) : nullptr;
            if (!vr || !vr->edges() || vr->edges()->count() != 1)
            {
                why = "only constant fillets and a variable fillet on one edge are carried yet";
                return nullptr;
            }
            Ptr<BRepEdge> ed = vr->edges()->item(0);
            if (ed->nativeObject()) ed = ed->nativeObject();
            double r0 = cm2mm(vr->startRadius()->value()), r1 = cm2mm(vr->endRadius()->value());
            std::string transition = "straight";
            Ptr<ParameterList> mr = vr->midRadii(), mp = vr->midPositions();
            if (mr && mr->count() > 0)
            {
                transition = "smooth";
                for (size_t i = 0; i < mr->count() && mp && i < mp->count(); i++)
                {
                    double t = mp->item(i)->value(), r = cm2mm(mr->item(i)->value());
                    if (std::fabs(r - (r0 + (r1 - r0) * (3 * t * t - 2 * t * t * t))) > 1e-6 * std::max(1.0, std::fabs(r1)))
                    {
                        why = "a variable fillet whose intermediate radii follow neither law the spec names";
                        return nullptr;
                    }
                }
            }
            return json{{"filletType", "variable"}, {"radius", 0}, {"edges", json::array({descriptor(ed)})},
                        {"vertexRadii", json::array({json{{"vertex", descriptor(ed->startVertex())}, {"radius", r0}},
                                                     json{{"vertex", descriptor(ed->endVertex())}, {"radius", r1}}})},
                        {"propagate", false}, {"transition", transition}};
        }
        if (Ptr<ChamferFeature> f = e)
        {
            type = "chamfer";
            Ptr<ChamferEdgeSets> sets = f->edgeSets();
            if (!sets || sets->count() < 1)
            {
                why = "a chamfer without edge sets";
                return nullptr;
            }
            // one payload chamfer: every set must be of one kind and size (the importer makes one set per edge)
            json d{{"edges", json::array()}, {"flipped", json::array()}, {"propagate", false}};
            std::string kind;
            for (size_t si = 0; si < sets->count(); si++)
            {
                Ptr<ChamferEdgeSet> cs = sets->item(si);
                Ptr<ObjectCollection> edges;
                std::string k;
                bool flipped = false;
                if (Ptr<EqualDistanceChamferEdgeSet> eq = cs)
                    k = "equalDistance", d["distance"] = cm2mm(Ptr<ModelParameter>(eq->distance())->value()), edges = eq->edges();
                else if (Ptr<DistanceAndAngleChamferEdgeSet> da = cs)
                    k = "angleDistance", d["distance"] = cm2mm(Ptr<ModelParameter>(da->distance())->value()),
                    d["angle"] = rad2deg(Ptr<ModelParameter>(da->angle())->value()), edges = da->edges(), flipped = da->isFlipped();
                else if (Ptr<TwoDistancesChamferEdgeSet> td = cs)
                    k = "distanceDistance", d["distance"] = cm2mm(Ptr<ModelParameter>(td->distanceOne())->value()),
                    d["distance2"] = cm2mm(Ptr<ModelParameter>(td->distanceTwo())->value()), edges = td->edges(), flipped = td->isFlipped();
                if (k.empty() || (!kind.empty() && k != kind))
                {
                    why = "a chamfer of mixed or unknown edge sets is not carried";
                    return nullptr;
                }
                kind = k;
                for (size_t i = 0; edges && i < edges->count(); i++)
                {
                    d["edges"].push_back(descriptor(Ptr<BRepEdge>(edges->item(i))));
                    d["flipped"].push_back(flipped);
                }
            }
            d["chamferType"] = kind;
            return d;
        }
        if (Ptr<RemoveFeature> rf = e)
        {
            type = "deleteBody";
            Ptr<BRepBody> b = rf->itemToRemove();
            if (!b)
            {
                why = "it removes something other than a body";
                return nullptr;
            }
            // a proxy (the model in the upright wrapper) is measured in the root's turned frame, the native body in the
            // part's own (P22's deleted bodies went out turned by the wrapper's 90 degrees and were not found)
            if (b->nativeObject()) b = b->nativeObject();
            json fp = bodyFingerprint(b);
            return json{{"bodies", {{{"kind", "body"}, {"by", "geom"}, {"geom", {{"volume", fp["volume"]}, {"centroid", fp["centroid"]}, {"bbox", fp["bbox"]}}}}}},
                        {"keep", false}};
        }
        if (Ptr<ConstructionPlane> p = e)
        {
            type = "plane";
            Ptr<ConstructionPlaneOffsetDefinition> od = p->definition();
            if (!od)
            {
                // another definition (on a path, three points...): the plane as it is, with no references
                Ptr<Plane> g = p->geometry();
                if (!g)
                {
                    why = "the plane has no geometry";
                    return nullptr;
                }
                return json{{"references", json::array()}, {"origin", vj(ptMm(g->origin()))}, {"normal", vj(unit(vecOf(g->normal())))},
                            {"note", std::string("a ") + p->definition()->objectType() + " plane, written as its position"}};
            }
            json base = planeRef(od->planarEntity());
            Ptr<Plane> g = p->geometry();
            // FORMAT-SPEC 4: the constraint as a word; the distance as a magnitude with `flip` for the side opposite
            // the reference's normal (the resulting origin and normal say the same unambiguously)
            double off = cm2mm(od->offset()->value());
            return json{{"references", {{{"ref", base}, {"constraint", "distance"}, {"value", std::fabs(off)}, {"flip", off < 0}}}},
                        {"origin", vj(ptMm(g->origin()))}, {"normal", vj(unit(vecOf(g->normal())))}};
        }
        // ---- patterns, mirrors, shell, draft, combine, split: as the importer reads them (features2.cpp), which
        // follows the SolidWorks exporter (FORMAT-SPEC 4: a definition not described there is as it writes it)
        if (Ptr<HoleFeature> f = e)
        {
            // FORMAT-SPEC 4 `hole`: the face it starts on, its centres, type, end and sizes, as the importer reads
            // them (features3.cpp). The centres are where the drilled cylinders' axes (read with the hole computed,
            // walk) meet the start face; that holds however Fusion positioned them.
            type = "hole";
            if (holeAxes.empty())
            {
                why = "its drilled cylinders were not found";
                return nullptr;
            }
            Ptr<HolePositionDefinition> pd = f->holePositionDefinition();
            Ptr<Base> start;
            if (Ptr<PointHolePositionDefinition> d = pd) start = d->planarEntity();
            else if (Ptr<AtCenterHolePositionDefinition> d = pd) start = d->planarEntity();
            else if (Ptr<OnEdgeHolePositionDefinition> d = pd) start = d->planarEntity();
            else if (Ptr<PlaneAndOffsetsHolePositionDefinition> d = pd) start = d->planarEntity();
            else
            {
                Ptr<SketchPoint> sp;
                if (Ptr<SketchPointHolePositionDefinition> d = pd) sp = d->sketchPoint();
                else if (Ptr<SketchPointsHolePositionDefinition> d = pd)
                    if (d->sketchPoints() && d->sketchPoints()->count() > 0) sp = d->sketchPoints()->item(0);
                if (sp)
                {
                    Ptr<Sketch> sk = sp->parentSketch();
                    if (sk->nativeObject()) sk = sk->nativeObject();
                    start = sk->referencePlane();
                    // a face sketch may report no reference plane: the face lying in the sketch plane
                    if (!Ptr<BRepFace>(start))
                        if (Ptr<BRepFace> f = faceInPlane(ptMm(sk->origin()), unit(cross(vecOf(sk->xDirection()), vecOf(sk->yDirection())))))
                            start = f;
                }
            }
            json face = start ? planeRef(start) : json(nullptr);
            if (face.is_null() || face.value("kind", "") != "face")
            {
                why = "the face the hole starts on cannot be referenced";
                return nullptr;
            }
            Ptr<BRepFace> sf = start;
            if (sf && sf->nativeObject()) sf = sf->nativeObject();
            Ptr<Plane> pl = sf ? Ptr<Plane>(sf->geometry()) : nullptr;
            if (!pl)
            {
                why = "only holes on planar faces are carried";
                return nullptr;
            }
            V3 po = ptMm(pl->origin()), pn = unit(vecOf(pl->normal()));
            json points = json::array();
            for (auto& ax : holeAxes)
            {
                double den = dot(ax.second, pn);
                if (std::fabs(den) < 1e-9) continue;
                double t = dot(sub(po, ax.first), pn) / den;
                points.push_back(vj(add(ax.first, mul(ax.second, t))));
            }
            std::string kind = f->holeType() == CounterboreHoleType ? "counterbore" : f->holeType() == CountersinkHoleType ? "countersink" : "simple";
            auto mm = [](const Ptr<ModelParameter>& p) { return p ? cm2mm(p->value()) : 0.0; };
            auto deg = [](const Ptr<ModelParameter>& p) { return p ? rad2deg(p->value()) : 0.0; };
            json sizes{{"holeDiameter", mm(f->holeDiameter())}, {"drillAngle", deg(f->tipAngle())}, {"measuredDiameters", holeDiameters}};
            if (kind == "counterbore")
            {
                sizes["counterBoreDiameter"] = mm(f->counterboreDiameter());
                sizes["counterBoreDepth"] = mm(f->counterboreDepth());
            }
            if (kind == "countersink")
            {
                sizes["counterSinkDiameter"] = mm(f->countersinkDiameter());
                sizes["counterSinkAngle"] = deg(f->countersinkAngle());
            }
            json end;
            Ptr<ExtentDefinition> x = f->extentDefinition();
            if (Ptr<DistanceExtentDefinition> dd = x) end = json{{"type", "blind"}, {"depth", cm2mm(dd->distance()->value())}};
            else if (Ptr<ThroughAllExtentDefinition> ta = x) end = json{{"type", "throughAll"}, {"depth", 0}};
            else if (Ptr<AllExtentDefinition> al = x) end = json{{"type", "throughAll"}, {"depth", 0}};   // Fusion's "All"
            else
            {
                why = std::string("only blind and through-all holes are carried yet (this one ends by ") + (x ? x->objectType() : "nothing") + ")";
                return nullptr;
            }
            return json{{"face", face}, {"points", points}, {"holeType", kind}, {"end", end}, {"sizes", sizes}};
        }
        if (Ptr<FlangeFeature> f = e)
        {
            // Sheet metal (FORMAT-SPEC 4): the component's rule goes out as a `sheetMetal` feature before its first
            // flange; base and contour flanges as `baseFlange`, as the importer reads them (sheetmetal.cpp)
            Ptr<SheetMetalRule> rule = comp->activeSheetMetalRule();
            if (!rule)
            {
                why = "a flange without a sheet-metal rule";
                return nullptr;
            }
            double t = cm2mm(rule->thickness()->value()), r = cm2mm(rule->bendRadius()->value()), k = rule->kFactor();
            // the rule's bend relief as the part's default: its shape, and its width as a ratio of the thickness
            BendReliefShapes rs = rule->reliefShape();
            std::string reliefType = rs == TearBendReliefShape ? "tear" : rs == RoundBendReliefShape ? "obround" : "rectangular";
            double reliefRatio = rule->reliefWidth() && t > 0 ? cm2mm(rule->reliefWidth()->value()) / t : 0.5;
            auto ruleFirst = [&]() {
                if (sheetMetalWritten) return;
                sheetMetalWritten = true;
                preFeatures.push_back(json{{"id", fid()}, {"name", "Sheet-Metal"}, {"type", "sheetMetal"}, {"status", "mapped"},
                                           {"native", {{"app", "Fusion"}, {"type", "adsk::fusion::SheetMetalRule"}}},
                                           {"def", {{"thickness", t}, {"bendRadius", r}, {"bendAllowanceType", "kFactor"}, {"kFactor", k},
                                                    {"bendAllowance", 0}, {"autoRelief", true}, {"reliefType", reliefType}, {"reliefRatio", reliefRatio}}}});
            };
            if (Ptr<BaseFlangeFeatureDefinition> bd = f->definition())
            {
                type = "baseFlange";
                Ptr<ObjectCollection> oc = ObjectCollection::create();
                for (auto& p : bd->profiles()) oc->add(p);
                json contours = json::array();
                json prof = profileRef(oc, contours);
                if (prof.is_null())
                {
                    why = "its profile is not a written sketch";
                    return nullptr;
                }
                // importer: thickness along the sketch normal (SideOne) = reverseThickness
                json d{{"profile", prof}, {"thickness", t}, {"reverseThickness", bd->orientation() == SideOneFlangeOrientation},
                       {"bendRadius", r}, {"reverse", false}, {"kFactor", k}};
                if (!contours.empty()) d["contours"] = contours;
                if (!regionsOut.empty()) d["regions"] = regionsOut;
                ruleFirst();
                return d;
            }
            if (Ptr<ContourFlangeFeatureDefinition> cd = f->definition())
            {
                type = "baseFlange";
                std::vector<Ptr<Profile>> ps = cd->profiles();
                Ptr<Sketch> sk = ps.empty() ? nullptr : ps[0]->parentSketch();
                // an open profile may report no sketch: its curves' sketch
                if (!sk && !ps.empty())
                    if (Ptr<ProfileLoops> pl = ps[0]->profileLoops())
                        if (pl->count() > 0 && pl->item(0)->profileCurves()->count() > 0)
                            if (Ptr<SketchEntity> se = pl->item(0)->profileCurves()->item(0)->sketchEntity()) sk = se->parentSketch();
                if (sk && sk->nativeObject()) sk = sk->nativeObject();
                std::string sid = sk ? idOf(sk) : "";
                // Fusion's open profile may report neither its sketch nor its curves: the profile the source named
                // (provenance), else the written sketch whose box is the profile's
                if (sid.empty())
                {
                    json src = json::parse(attrOf(f, "def"), nullptr, false);
                    if (src.is_object() && src.contains("def") && src["def"].contains("profile"))
                    {
                        std::string pid = src["def"]["profile"].value("id", "");
                        if (usedIds.count(pid)) sid = pid;
                    }
                }
                if (sid.empty() && !ps.empty())
                    if (Ptr<BoundingBox3D> pb = ps[0]->boundingBox())
                        for (size_t i = 0; i < comp->sketches()->count() && sid.empty(); i++)
                        {
                            Ptr<Sketch> c = comp->sketches()->item(i);
                            Ptr<BoundingBox3D> cb = c->boundingBox();
                            std::string cid = idOf(c);
                            if (!cb || cid.empty()) continue;
                            if (dist(ptMm(cb->minPoint()), ptMm(pb->minPoint())) < 1e-4 && dist(ptMm(cb->maxPoint()), ptMm(pb->maxPoint())) < 1e-4) sid = cid;
                        }
                if (sid.empty())
                {
                    why = std::string("its profile is not a written sketch (") + (ps.empty() ? "no profiles" : !sk ? "no sketch" : "sketch " + sk->name() + " not written") + ")";
                    return nullptr;
                }
                double d1 = cd->distance() ? cm2mm(cd->distance()->value()) : 0;
                json d{{"profile", {{"kind", "feature"}, {"by", "id"}, {"id", sid}}}, {"thickness", t},
                       {"reverseThickness", cd->orientation() == SideTwoFlangeOrientation}, {"bendRadius", r}, {"kFactor", k}, {"reverse", d1 < 0}};
                if (cd->direction() == SymmetricFlangeDirection) d["end"] = json{{"type", "midPlane"}, {"depth", std::fabs(d1)}};
                else
                {
                    d["end"] = json{{"type", "blind"}, {"depth", std::fabs(d1)}};
                    if (cd->direction() == TwoSidesFlangeDirection && cd->distanceTwo())
                        d["end2"] = json{{"type", "blind"}, {"depth", std::fabs(cm2mm(cd->distanceTwo()->value()))}};
                }
                ruleFirst();
                return d;
            }
            if (Ptr<EdgeFlangeFeatureDefinition> ed = f->definition())
            {
                // An edge flange with the profile sketch SolidWorks needs: a rectangle on a plane the flange generates,
                // its base on the edge of the face the flange rises from, as wide as the edge and as high as the
                // flange's length from that face (90 degrees: outer virtual sharp t below it, inner virtual sharp at
                // it, bend tangent r above it). The rise direction was read with the flange computed (walk).
                type = "edgeFlange";
                double ang = ed->angle() ? std::fabs(rad2deg(ed->angle()->value())) : 90;
                bool measured = flangeWeb.size() == flangeRise.size() && !flangeWeb.empty();
                if (std::fabs(ang - 90) > 1e-6 && !measured)
                {
                    why = "an edge flange not at 90 degrees whose web could not be measured";
                    return nullptr;
                }
                if (ed->extentType() != DistanceFlangeExtentType)
                {
                    why = "an edge flange up to an object is not carried yet";
                    return nullptr;
                }
                double len = cm2mm(ed->distance()->value());
                FlangeHeightDatumTypes dt = ed->heightDatumType();
                double H = dt == OuterFacesFlangeHeightDatumType ? len - t : dt == InnerFacesFlangeHeightDatumType ? len : len + r;
                std::string datum = dt == OuterFacesFlangeHeightDatumType ? "outerVirtualSharp" : dt == InnerFacesFlangeHeightDatumType ? "innerVirtualSharp" : "bendTangent";
                BendPositionTypes bp = ed->bendPositionType();
                // Fusion's inside puts the flange's outer virtual sharp on the edge of the sheet's outer face (spec
                // `bendSharp`); at 90 degrees that is `materialInside`, the flange's outer face through the edge
                std::string position = bp == OutsideBendPositionType ? "materialOutside" : bp == StartEdgeBendPositionType ? "bendOutside"
                                     : bp == TangentToSideBendPositionType ? "bendTangent" : std::fabs(ang - 90) < 1e-6 ? "materialInside" : "bendSharp";
                double br = r;
                if (Ptr<BendRadiusOverride> bo = f->bendRadiusOverride())
                    if (bo->isOverridden() && bo->bendRadius()) br = cm2mm(bo->bendRadius()->value());
                Ptr<FlangeEdgeDefinitions> eds = ed->edgeDefinitions();
                if (!eds || eds->count() == 0 || flangeRise.size() != eds->count())
                {
                    why = "its edges or their rise could not be read";
                    return nullptr;
                }
                json edgesJ = json::array(), profilesJ = json::array();
                for (size_t i = 0; i < eds->count(); i++)
                {
                    Ptr<FlangeEdgeDefinition> fe = eds->item(i);
                    if (fe->widthType() != FullEdgeWidthFlangeWidthType && !measured)
                    {
                        why = "an edge flange of part width whose web could not be measured";
                        return nullptr;
                    }
                    Ptr<BRepEdge> edge = fe->edge();
                    if (edge && edge->nativeObject()) edge = edge->nativeObject();
                    Ptr<Line3D> ln = edge ? Ptr<Line3D>(edge->geometry()) : nullptr;
                    if (!ln)
                    {
                        why = "an edge flange on a curved edge is not carried";
                        return nullptr;
                    }
                    V3 a = ptMm(edge->startVertex()->geometry()), b = ptMm(edge->endVertex()->geometry());
                    V3 y = flangeRise[i];
                    // the edge of the face the flange rises from: this one, or its twin a thickness along the rise
                    V3 n = risingFaceNormal(edge, y);
                    if (dot(n, y) < 0)
                    {
                        Ptr<BRepEdge> twin = parallelEdge(edge, add(mul(add(a, b), 0.5), mul(n, -t)));
                        if (!twin)
                        {
                            why = "the edge on the face the flange rises from was not found";
                            return nullptr;
                        }
                        edge = twin;
                        a = ptMm(edge->startVertex()->geometry());
                        b = ptMm(edge->endVertex()->geometry());
                    }
                    double L = dist(a, b);
                    V3 x = unit(sub(b, a)), z = unit(cross(x, y));
                    // its span along the edge and its height, measured on the computed flange's web: a rectangle of
                    // those is the profile at any angle, width, datum and bend position (the formula above at 90
                    // degrees is the fallback)
                    double s0 = 0, s1 = L, Hi = H;
                    if (measured && !flangeWeb[i].empty())
                    {
                        s0 = 1e300, s1 = -1e300, Hi = -1e300;
                        for (auto& v : flangeWeb[i])
                        {
                            V3 q = sub(v, a);
                            s0 = std::min(s0, dot(q, x));
                            s1 = std::max(s1, dot(q, x));
                            Hi = std::max(Hi, dot(q, y));
                        }
                        if (std::fabs(s0) < 1e-6) s0 = 0;
                        if (std::fabs(s1 - L) < 1e-6) s1 = L;
                    }
                    std::string skId = fid();
                    usedIds.insert(skId);
                    std::string skName = f->name() + (eds->count() > 1 ? " profile " + std::to_string(i + 1) : " profile");
                    json ents = json::array({json{{"id", "p1"}, {"type", "point"}, {"p", {s0, 0}}}, json{{"id", "p2"}, {"type", "point"}, {"p", {s1, 0}}},
                                             json{{"id", "p3"}, {"type", "point"}, {"p", {s1, Hi}}}, json{{"id", "p4"}, {"type", "point"}, {"p", {s0, Hi}}},
                                             json{{"id", "s1"}, {"type", "line"}, {"start", {s0, 0}}, {"end", {s1, 0}}, {"startPoint", "p1"}, {"endPoint", "p2"}},
                                             json{{"id", "s2"}, {"type", "line"}, {"start", {s1, 0}}, {"end", {s1, Hi}}, {"startPoint", "p2"}, {"endPoint", "p3"}},
                                             json{{"id", "s3"}, {"type", "line"}, {"start", {s1, Hi}}, {"end", {s0, Hi}}, {"startPoint", "p3"}, {"endPoint", "p4"}},
                                             json{{"id", "s4"}, {"type", "line"}, {"start", {s0, Hi}}, {"end", {s0, 0}}, {"startPoint", "p4"}, {"endPoint", "p1"}}});
                    json cons = json::array({json{{"type", "horizontal"}, {"entities", {"s1"}}}, json{{"type", "vertical"}, {"entities", {"s2"}}},
                                             json{{"type", "horizontal"}, {"entities", {"s3"}}}, json{{"type", "vertical"}, {"entities", {"s4"}}}});
                    json dims = json::array({json{{"id", "d1"}, {"name", "D1"}, {"fullName", "D1@" + skName}, {"kind", "distance"}, {"entities", {"p1", "p4"}},
                                                  {"value", Hi}, {"driving", true}, {"textPosition", {(s0 + s1) / 2, Hi / 2}}}});
                    preFeatures.push_back(json{{"id", skId}, {"name", skName}, {"type", "sketch"}, {"status", "mapped"},
                                               {"native", {{"app", "Fusion"}, {"type", "flange profile (written for the flange)"}}},
                                               {"def", {{"plane", {{"kind", "generated"}, {"by", f->name()}}},
                                                        {"frame", {{"origin", vj(a)}, {"x", vj(x)}, {"z", vj(z)}}},
                                                        {"entities", ents}, {"constraints", cons}, {"dimensions", dims}, {"constrained", "under"}, {"hidden", true}}}});
                    edgesJ.push_back(descriptor(edge));
                    profilesJ.push_back(json{{"kind", "feature"}, {"by", "id"}, {"id", skId}});
                }
                ruleFirst();
                json d{{"edge", edgesJ[0]}, {"profile", profilesJ[0]}, {"angle", ang}, {"useDefaultRadius", std::fabs(br - r) < 1e-9}, {"bendRadius", br},
                       {"position", position}, {"useDefaultRelief", true}, {"reliefType", reliefType}, {"useReliefRatio", true}, {"reliefRatio", reliefRatio},
                       {"reliefWidth", 1}, {"reliefDepth", 1}, {"length", {{"type", "blind"}, {"distance", len}, {"datum", datum}, {"reverse", false}}}};
                if (edgesJ.size() > 1)
                {
                    d["edges"] = edgesJ;
                    d["profiles"] = profilesJ;
                }
                return d;
            }
            why = "a flange of another kind is not carried yet";
            return nullptr;
        }
        if (Ptr<UnfoldFeature> f = e)
        {
            // FORMAT-SPEC 4 `unfold`: the fixed face and the bends by their inner cylindrical faces, in the state the
            // feature saw (Fusion has no bend sub-features: `feature` and `sub` are left out)
            type = "unfold";
            Ptr<BRepFace> fixed = f->stationaryFace();
            if (fixed && fixed->nativeObject()) fixed = fixed->nativeObject();
            if (!fixed)
            {
                why = "its stationary face is gone";
                return nullptr;
            }
            std::vector<Ptr<BRepFace>> bends = f->bendFaces();
            if (f->isUnfoldAllBends() || bends.empty())
            {
                // every bend: the inner cylinders of the rule's bend radius
                bends.clear();
                Ptr<SheetMetalRule> rule = comp->activeSheetMetalRule();
                double r = rule ? cm2mm(rule->bendRadius()->value()) : 0;
                for (auto& b : solids())
                    for (size_t i = 0; i < b->faces()->count(); i++)
                        if (Ptr<Cylinder> cy = b->faces()->item(i)->geometry())
                            if (std::fabs(cm2mm(cy->radius()) - r) < 1e-6) bends.push_back(b->faces()->item(i));
            }
            json bj = json::array();
            int k = 0;
            for (auto& bf : bends)
            {
                Ptr<BRepFace> nb = bf->nativeObject() ? bf->nativeObject() : bf;
                bj.push_back(json{{"name", "Bend" + std::to_string(++k)}, {"face", descriptor(nb)}});
            }
            return json{{"fixedFace", descriptor(fixed)}, {"bends", bj}};
        }
        // ---- more sheet metal (FORMAT-SPEC 4 hem, sketchedBend, breakCorner, fold, offsetFaces, convertToSheetMetal).
        // Where Fusion's settings do not say which of the source system's own settings give the same shape (a hem's or
        // bend's position, its sense), the feature carries its result (walk) and an importer picks the setting whose
        // geometry is that result, as this add-in's importer does the other way.
        if (Ptr<HemFeature> f = e)
        {
            type = "hem";
            Ptr<HemFeatureDefinition> hd = f->definition();
            Ptr<BRepEdge> edge = hd ? hd->hemEdge() : nullptr;
            if (edge && edge->nativeObject()) edge = edge->nativeObject();
            if (!edge)
            {
                why = "its edge is gone";
                return nullptr;
            }
            auto mm = [](const Ptr<ModelParameter>& p) { return p ? cm2mm(p->value()) : 0.0; };
            BendPositionTypes bp = hd->bendPositionType();
            json d{{"edges", json::array({descriptor(edge)})}, {"reverse", hd->isFlipped()}, {"miterGap", 0}, {"angle", 180},
                   {"position", bp == StartEdgeBendPositionType || bp == OutsideBendPositionType ? "outside" : "inside"}};
            if (Ptr<OpenHemFeatureDefinition> h = hd)
            {
                double g = mm(h->gap());
                // SolidWorks' closed hem is Fusion's open hem with SolidWorks' small gap (this add-in's import, P28)
                d["hemType"] = g <= 0.1 + 1e-9 ? "closed" : "open";
                d["length"] = mm(h->length());
                d["gap"] = g;
            }
            else if (Ptr<FlatHemFeatureDefinition> h = hd)
            {
                d["hemType"] = "closed";
                d["length"] = mm(h->length());
                d["gap"] = 0;
            }
            else if (Ptr<RolledHemFeatureDefinition> h = hd)
            {
                d["hemType"] = "rolled";
                d["radius"] = mm(h->radius());
                d["angle"] = h->angle() ? rad2deg(h->angle()->value()) : 180;
            }
            else if (Ptr<TeardropHemFeatureDefinition> h = hd)
            {
                d["hemType"] = "teardrop";
                d["radius"] = mm(h->radius());
                d["length"] = mm(h->length());
                d["gap"] = mm(h->gap());
            }
            else
            {
                why = "a rope or double hem has no counterpart in the spec yet";
                return nullptr;
            }
            return d;
        }
        if (Ptr<FoldFeature> f = e)
        {
            // a fold on one sketch line: a sketched bend about that sketch (the sketch must hold only that line: a
            // sketched bend bends along every line of its sketch)
            type = "sketchedBend";
            Ptr<FoldBendLineDefinitions> bls = f->bendLines();
            if (!bls || bls->count() != 1)
            {
                why = "a fold on several lines is not carried yet";
                return nullptr;
            }
            Ptr<FoldBendLineDefinition> bl = bls->item(0);
            Ptr<SketchLine> line = bl->bendLine();
            if (line && line->nativeObject()) line = line->nativeObject();
            std::string sid = line ? idOf(line->parentSketch()) : "";
            if (sid.empty())
            {
                why = "its bend line is not in a written sketch";
                return nullptr;
            }
            Ptr<BRepFace> fixed = f->stationaryFace();
            if (fixed && fixed->nativeObject()) fixed = fixed->nativeObject();
            if (!fixed)
            {
                why = "its stationary face is gone";
                return nullptr;
            }
            Ptr<SheetMetalRule> rule = comp->activeSheetMetalRule();
            if (!rule)
            {
                why = "the part has no sheet-metal rule";
                return nullptr;
            }
            double r = cm2mm(rule->bendRadius()->value()), br = r, t = cm2mm(rule->thickness()->value()), k = rule->kFactor();
            if (Ptr<BendRadiusOverride> bo = f->bendRadiusOverride())
                if (bo->isOverridden() && bo->bendRadius()) br = cm2mm(bo->bendRadius()->value());
            double ang = bl->bendAngle() ? std::fabs(rad2deg(bl->bendAngle()->value())) : 90;
            // the bend goes in a sketch of its own holding only its line (a sketched bend bends along every line of
            // its sketch), on the sheet's face lying in the line's plane (SolidWorks makes a sketched bend only from a
            // sketch on a face of the sheet), else by its frame. The line is written where the spec's position
            // `materialInside` puts it, the bend's outer virtual sharp: Fusion's line marks the start, centre or end
            // of the bend zone in the flat (its developed length, by the rule's K-factor), and the outer virtual sharp
            // lies (radius + thickness) tan(angle / 2) past the start, towards the side that moves.
            auto& ids = sketchIds[sid];
            auto lit = ids.find(line->entityToken());
            json src;
            for (auto& x : features)
                if (x.value("id", "") == sid) src = x;
            if (lit == ids.end() || src.is_null())
            {
                why = "its bend line is not in the written sketch";
                return nullptr;
            }
            Ptr<Sketch> lsk = line->parentSketch();
            V3 so = ptMm(lsk->origin()), sx = unit(vecOf(lsk->xDirection())), sy = unit(vecOf(lsk->yDirection()));
            Ptr<BRepFace> onFace = faceInPlane(so, unit(cross(sx, sy)));
            std::string lineId = lit->second, p1, p2;
            json keep = json::array();
            V3 a2{0, 0, 0}, b2{0, 0, 0};
            for (auto& en : src["def"]["entities"])
                if (en.value("id", "") == lineId)
                {
                    p1 = en.value("startPoint", "");
                    p2 = en.value("endPoint", "");
                    a2 = {en["start"][0].get<double>(), en["start"][1].get<double>(), 0};
                    b2 = {en["end"][0].get<double>(), en["end"][1].get<double>(), 0};
                }
            // in the sketch's 2D frame: the unit normal to the line pointing away from the stationary face
            V3 dl = unit(sub(b2, a2)), n2{-dl[1], dl[0], 0};
            V3 c3 = sub(ptMm(fixed->pointOnFace()), so), c2{dot(c3, sx), dot(c3, sy), 0};
            if (dot(sub(c2, a2), n2) > 0) n2 = mul(n2, -1);
            FoldBendLinePositionTypes lp = bl->linePosition();
            double zone = deg2rad(ang) * (br + k * t);   // the bend zone's developed length
            double start = lp == StartFoldBendLinePositionType ? 0 : lp == CenterFoldBendLinePositionType ? -zone / 2 : -zone;
            double shift = start + (br + t) * std::tan(deg2rad(ang) / 2);
            V3 d2 = mul(n2, shift);
            auto moved = [&](const json& p) { return json::array({p[0].get<double>() + d2[0], p[1].get<double>() + d2[1]}); };
            for (auto& en : src["def"]["entities"])
            {
                std::string id = en.value("id", "");
                if (id == lineId)
                {
                    json m = en;
                    m["start"] = moved(en["start"]);
                    m["end"] = moved(en["end"]);
                    keep.push_back(m);
                }
                else if (en.value("type", "") == "point" && (id == p1 || id == p2))
                {
                    json m = en;
                    m["p"] = moved(en["p"]);
                    keep.push_back(m);
                }
            }
            std::string bid = fid();
            usedIds.insert(bid);
            json def = src["def"];
            def["entities"] = keep;
            def["constraints"] = json::array();
            def["dimensions"] = json::array();
            def["hidden"] = true;
            def["constrained"] = "under";
            def["plane"] = onFace ? descriptor(onFace) : json{{"kind", "frame"}};
            preFeatures.push_back(json{{"id", bid}, {"name", f->name() + " line"}, {"type", "sketch"}, {"status", "mapped"},
                                       {"native", {{"app", "Fusion"}, {"type", "fold line (written for the bend)"}}}, {"def", def}});
            return json{{"line", {{"kind", "feature"}, {"by", "id"}, {"id", bid}}}, {"fixedFace", descriptor(fixed)},
                        {"angle", ang}, {"reverse", false}, {"position", "materialInside"},
                        {"useDefaultRadius", std::fabs(br - r) < 1e-9}, {"radius", br}};
        }
        if (Ptr<SheetMetalChamferFeature> f = e)
        {
            type = "breakCorner";
            Ptr<SheetMetalChamferEdgeSets> sets = f->edgeSets();
            if (!sets || sets->count() != 1)
            {
                why = "a sheet-metal chamfer with several sets is not carried yet";
                return nullptr;
            }
            json ents = json::array();
            for (auto& ed : sets->item(0)->edges()) ents.push_back(descriptor(ed->nativeObject() ? ed->nativeObject() : ed));
            return json{{"entities", ents}, {"breakType", "chamfer"}, {"distance", cm2mm(sets->item(0)->distance()->value())}};
        }
        if (Ptr<SheetMetalFilletFeature> f = e)
        {
            type = "breakCorner";
            Ptr<SheetMetalFilletEdgeSets> sets = f->edgeSets();
            if (!sets || sets->count() != 1)
            {
                why = "a sheet-metal fillet with several sets is not carried yet";
                return nullptr;
            }
            json ents = json::array();
            for (auto& ed : sets->item(0)->edges()) ents.push_back(descriptor(ed->nativeObject() ? ed->nativeObject() : ed));
            return json{{"entities", ents}, {"breakType", "fillet"}, {"distance", cm2mm(sets->item(0)->radius()->value())}};
        }
        if (Ptr<RefoldFeature> f = e)
        {
            // a refold: the fixed face and each bend by the face of its flat strip that faces the way the fixed face
            // does (FORMAT-SPEC 4 fold), in the flat state the feature sees
            type = "fold";
            Ptr<UnfoldFeature> uf = f->unfoldFeature();
            if (uf && uf->nativeObject()) uf = uf->nativeObject();
            Ptr<BRepFace> fixed = uf ? uf->stationaryFace() : nullptr;
            if (fixed && fixed->nativeObject()) fixed = fixed->nativeObject();
            if (!fixed)
            {
                why = "its unfold's stationary face is gone";
                return nullptr;
            }
            V3 fn = outwardNormalOf(fixed);
            json bj = json::array();
            Ptr<BRepFaces> made = uf->faces();
            int k = 0;
            for (size_t i = 0; made && i < made->count(); i++)
            {
                Ptr<BRepFace> s = made->item(i);
                if (s->nativeObject()) s = s->nativeObject();
                if (!Ptr<Plane>(s->geometry()) || dot(outwardNormalOf(s), fn) < 1 - 1e-9 || s == fixed) continue;
                bj.push_back(json{{"name", "Bend" + std::to_string(++k)}, {"face", descriptor(s)}});
            }
            json d{{"fixedFace", descriptor(fixed)}};
            // the unfold it reverses: an importer can take that unfold's bends again. Fusion merges a flat strip
            // into the face beside it, so the strips may not be faces of their own: then the bends are the unfold's
            std::string uid = featureId(uf);
            json ub;
            for (auto& x : features)
                if (x.value("id", "") == uid && x.value("type", "") == "unfold") ub = x["def"].value("bends", json());
            if (!uid.empty() && !ub.is_null()) d["unfold"] = json{{"kind", "feature"}, {"by", "id"}, {"id", uid}};
            if (bj.empty())
            {
                if (ub.is_null())
                {
                    why = "its bends' flat strips were not found, nor its unfold's bends";
                    return nullptr;
                }
                bj = ub;
            }
            d["bends"] = bj;
            return d;
        }
        if (Ptr<OffsetFacesFeature> f = e)
        {
            // FORMAT-SPEC 4 offsetFaces: faces moved along their normals by a distance (SolidWorks: Move Face, offset)
            type = "offsetFaces";
            json faces = json::array();
            for (auto& fc : f->inputFaces()) faces.push_back(descriptor(fc->nativeObject() ? fc->nativeObject() : fc));
            if (faces.empty())
            {
                why = "an offset of no faces";
                return nullptr;
            }
            return json{{"faces", faces}, {"distance", f->distance() ? cm2mm(f->distance()->value()) : 0.0}};
        }
        if (e && std::string(e->objectType()) == "adsk::fusion::Feature" && attrOf(e, "source").empty())
        {
            // Fusion's own convert to sheet metal (the API has no class for it): a sheet of the rule's thickness made
            // from a plate body. Written as the spec's convertToSheetMetal from the plate's largest flat face, with
            // no bends (the result decides the thickness sense)
            Ptr<TimelineObject> to = Ptr<Feature>(e) ? Ptr<Feature>(e)->timelineObject() : nullptr;
            std::string tn = to ? to->name() : "";
            Ptr<SheetMetalRule> rule = comp->activeSheetMetalRule();
            if (rule && tn.find("Convert") != std::string::npos)
            {
                type = "convertToSheetMetal";
                Ptr<BRepFace> big;
                for (auto& b : solids())
                    for (size_t i = 0; i < b->faces()->count(); i++)
                        if (Ptr<Plane>(b->faces()->item(i)->geometry()) && (!big || b->faces()->item(i)->area() > big->area())) big = b->faces()->item(i);
                if (!big)
                {
                    why = "no flat face to convert from";
                    return nullptr;
                }
                return json{{"thickness", cm2mm(rule->thickness()->value())}, {"bendRadius", cm2mm(rule->bendRadius()->value())},
                            {"fixedFace", descriptor(big)}, {"bendEdges", json::array()}, {"reverse", false}, {"keepBody", false}};
            }
        }
        if (Ptr<SweepFeature> f = e)
        {
            // FORMAT-SPEC 4 `sweep` / `sweepCut`, with the SolidWorks exporter's defaults for what Fusion's sweep
            // does not have; the path and guide (sketches as a whole) before the profile, whose regions profileRef
            // leaves in regionsOut
            bool cut = f->operation() == CutFeatureOperation;
            type = cut ? "sweepCut" : "sweep";
            json dummy = json::array();
            json path = profileRef(f->path(), dummy);
            if (path.is_null())
            {
                why = "its path is not one written sketch";
                return nullptr;
            }
            json guides = json::array();
            if (Ptr<Path> g = f->guideRail())
            {
                json gr = profileRef(g, dummy);
                if (gr.is_null())
                {
                    why = "its guide rail is not one written sketch";
                    return nullptr;
                }
                guides.push_back(gr);
            }
            if (f->twistAngle() && std::fabs(f->twistAngle()->value()) > 1e-12)
            {
                why = "a twisted sweep is not carried yet";
                return nullptr;
            }
            json contours = json::array();
            json prof = profileRef(f->profile(), contours);
            if (prof.is_null())
            {
                why = "its profile is not a written sketch";
                return nullptr;
            }
            json d{{"profile", prof}, {"path", path}, {"twistControl", 0}, {"pathAlignment", 0}, {"twistAngle", 0},
                   {"tangentPropagation", false}, {"maintainTangency", false}, {"advancedSmoothing", false}, {"mergeSmoothFaces", true},
                   {"alignWithEndFaces", false}, {"startTangency", 0}, {"endTangency", 0}, {"direction", -1}};
            if (!cut) d["merge"] = f->operation() == JoinFeatureOperation;
            if (!guides.empty()) d["guides"] = guides;
            if (!contours.empty()) d["contours"] = contours;
            if (!regionsOut.empty()) d["regions"] = regionsOut;
            return d;
        }
        if (Ptr<LoftFeature> f = e)
        {
            // FORMAT-SPEC 4 `loft` / `loftCut`: sections are whole sketches; the feature's result goes with it (walk),
            // as systems align sections with different segment counts differently
            bool cut = f->operation() == CutFeatureOperation;
            type = cut ? "loftCut" : "loft";
            json sections = json::array(), guides = json::array(), centerline;
            json dummy = json::array();
            Ptr<LoftSections> ls = f->loftSections();
            for (size_t i = 0; ls && i < ls->count(); i++)
            {
                json r = profileRef(ls->item(i)->entity(), dummy);
                if (r.is_null() || !regionsOut.empty())
                {
                    why = "a loft section that is not a whole written sketch";
                    return nullptr;
                }
                sections.push_back(r);
            }
            Ptr<LoftCenterLineOrRails> rails = f->centerLineOrRails();
            for (size_t i = 0; rails && i < rails->count(); i++)
            {
                Ptr<LoftCenterLineOrRail> rl = rails->item(i);
                json r = profileRef(rl->entity(), dummy);
                if (r.is_null())
                {
                    why = "a loft rail that is not a written sketch";
                    return nullptr;
                }
                if (rl->isCenterLine()) centerline = r;
                else guides.push_back(r);
            }
            json d{{"sections", sections}, {"closed", f->isClosed()}, {"maintainTangency", true}, {"startTangency", 0}, {"endTangency", 0},
                   {"startTangentLength", 1}, {"endTangentLength", 1}, {"reverseStartTangent", false}, {"reverseEndTangent", false}};
            if (!cut) d["merge"] = f->operation() == JoinFeatureOperation;
            if (!guides.empty()) d["guides"] = guides;
            if (!centerline.is_null()) d["centerline"] = centerline;
            return d;
        }
        if (Ptr<RectangularPatternFeature> f = e)
        {
            type = "linearPattern";
            json d{{"geometryPattern", false}};
            if (!seedsJson(f->inputEntities(), d, why)) return nullptr;
            Ptr<PatternElements> els = f->patternElements();
            bool spacing = f->patternDistanceType() == SpacingPatternDistanceType;
            struct Dir
            {
                const char* key;
                Ptr<Base> ent;
                Ptr<ModelParameter> q, dist;
                bool symmetric;
            };
            std::vector<Dir> dirs{{"dir1", f->directionOneEntity(), f->quantityOne(), f->distanceOne(), f->isSymmetricInDirectionOne()},
                                  {"dir2", f->directionTwoEntity(), f->quantityTwo(), f->distanceTwo(), f->isSymmetricInDirectionTwo()}};
            for (auto& dd : dirs)
            {
                int n = dd.q ? static_cast<int>(std::lround(dd.q->value())) : 1;
                if (!dd.ent || n <= 1) continue;
                if (dd.symmetric)
                {
                    why = "a pattern symmetric about its seed is not carried yet";
                    return nullptr;
                }
                V3 dir{0, 0, 0};
                json ref = directionRef(dd.ent, dir, why);
                if (ref.is_null()) return nullptr;
                double dist = dd.dist ? std::fabs(cm2mm(dd.dist->value())) : 0;
                double sp = spacing ? dist : (n > 1 ? dist / (n - 1) : 0);
                // which way the instances run: from an instance moved along this direction only
                bool reverse = false;
                for (size_t i = 0; els && i < els->count(); i++)
                {
                    Ptr<Matrix3D> m = els->item(i)->transform();
                    if (!m) continue;
                    std::vector<double> a = m->asArray();
                    V3 t{cm2mm(a[3]), cm2mm(a[7]), cm2mm(a[11])};
                    double along = dot(t, unit(dir));
                    if (std::fabs(along) < 1e-6 || norm(sub(t, mul(unit(dir), along))) > 1e-6) continue;
                    reverse = along < 0;
                    break;
                }
                d[dd.key] = json{{"ref", ref}, {"reverse", reverse}, {"spacing", sp}, {"count", n}};
            }
            if (!d.contains("dir1"))
            {
                why = "a pattern without a first direction";
                return nullptr;
            }
            json skipped = json::array();
            for (size_t i = 0; els && i < els->count(); i++)
                if (els->item(i)->isSuppressed()) skipped.push_back(static_cast<int>(i));
            if (!skipped.empty()) d["skipped"] = skipped;
            return d;
        }
        if (Ptr<CircularPatternFeature> f = e)
        {
            type = "circularPattern";
            json d{{"geometryPattern", false}, {"equalSpacing", true}};
            if (!seedsJson(f->inputEntities(), d, why)) return nullptr;
            if (f->isSymmetric())
            {
                why = "a circular pattern symmetric about its seed is not carried yet";
                return nullptr;
            }
            V3 dir{0, 0, 0};
            json ax = directionRef(f->axis(), dir, why);
            if (ax.is_null()) return nullptr;
            double total = f->totalAngle() ? rad2deg(f->totalAngle()->value()) : 360.0;
            d["axis"] = ax;
            d["count"] = f->quantity() ? static_cast<int>(std::lround(f->quantity()->value())) : 1;
            d["angle"] = std::fabs(total);
            d["reverse"] = total < 0;   // importer: a reversed pattern turns by -angle
            json skipped = json::array();
            Ptr<PatternElements> els = f->patternElements();
            for (size_t i = 0; els && i < els->count(); i++)
                if (els->item(i)->isSuppressed()) skipped.push_back(static_cast<int>(i));
            if (!skipped.empty()) d["skipped"] = skipped;
            return d;
        }
        if (Ptr<MirrorFeature> f = e)
        {
            json d;
            if (!seedsJson(f->inputEntities(), d, why)) return nullptr;
            json plane = planeRef(f->mirrorPlane());
            if (plane.value("kind", "") == "frame")
            {
                why = "its mirror plane is not a plane or face that can be referenced";
                return nullptr;
            }
            d["plane"] = plane;
            if (d.contains("bodies") && !d["bodies"].empty())
            {
                if (!d["seeds"].empty())
                {
                    why = "a mirror of features and bodies together is not carried";
                    return nullptr;
                }
                type = "mirrorBody";
                d.erase("seeds");
                d["merge"] = f->isCombine();
                d["knit"] = false;
            }
            else
            {
                type = "mirror";
                d["geometryPattern"] = false;
            }
            return d;
        }
        if (Ptr<ShellFeature> f = e)
        {
            type = "shell";
            json faces = json::array();
            Ptr<ObjectCollection> in = f->inputEntities();
            for (size_t i = 0; in && i < in->count(); i++)
                if (Ptr<BRepFace> fc = in->item(i)) faces.push_back(descriptor(fc));
            double inside = f->insideThickness() ? cm2mm(f->insideThickness()->value()) : 0;
            double outside = f->outsideThickness() ? cm2mm(f->outsideThickness()->value()) : 0;
            if (inside > 1e-9 && outside > 1e-9)
            {
                why = "a shell with both inside and outside thickness is not carried";
                return nullptr;
            }
            return json{{"thickness", inside > 1e-9 ? inside : outside}, {"outward", inside > 1e-9 ? 0 : 1}, {"faces", faces}};
        }
        if (Ptr<DraftFeature> f = e)
        {
            type = "draft";
            Ptr<AngleExtentDefinition> ad = f->draftDefinition();
            if (!ad || !ad->angle())
            {
                why = "only a draft by one angle is carried";
                return nullptr;
            }
            json neutral = planeRef(f->plane());
            if (neutral.value("kind", "") == "frame")
            {
                why = "its neutral plane cannot be referenced";
                return nullptr;
            }
            json faces = json::array();
            for (auto& fc : f->inputFaces()) faces.push_back(descriptor(fc));
            return json{{"neutral", neutral}, {"faces", faces}, {"angle", std::fabs(rad2deg(ad->angle()->value()))},
                        {"reverse", f->isDirectionFlipped() != (ad->angle()->value() < 0)}};
        }
        if (Ptr<CombineFeature> f = e)
        {
            type = "combine";
            if (f->isKeepToolBodies())
            {
                why = "a combine that keeps its tool bodies is not carried yet";
                return nullptr;
            }
            Ptr<BRepBody> target = f->targetBody();
            if (!target)
            {
                why = "its target body is gone";
                return nullptr;
            }
            json tools = json::array();
            Ptr<ObjectCollection> tb = f->toolBodies();
            for (size_t i = 0; tb && i < tb->count(); i++)
                if (Ptr<BRepBody> b = tb->item(i)) tools.push_back(bodyRef(b));
            FeatureOperations op = f->operation();
            return json{{"operation", op == CutFeatureOperation ? "subtract" : op == IntersectFeatureOperation ? "common" : "add"},
                        {"main", bodyRef(target)}, {"bodies", tools}};
        }
        if (Ptr<SplitBodyFeature> f = e)
        {
            type = "split";
            Ptr<Base> tool = f->splittingTool();
            json t;
            if (Ptr<ConstructionPlane> cp = tool) t = planeRef(cp);
            else if (Ptr<BRepFace> fc = tool) t = descriptor(fc);
            if (t.is_null() || t.value("kind", "") == "frame")
            {
                why = "its splitting tool is not a plane or face that can be referenced";
                return nullptr;
            }
            // the bodies it splits (Fusion splits only these; FORMAT-SPEC 4 split `bodies`)
            json targets = json::array();
            Ptr<ObjectCollection> sb = f->splitBodies();
            for (size_t i = 0; sb && i < sb->count(); i++)
                if (Ptr<BRepBody> b = sb->item(i)) targets.push_back(bodyRef(b));
            json d{{"tools", json::array({t})}, {"consume", false}};
            if (!targets.empty() && static_cast<int>(targets.size()) < static_cast<int>(solids().size())) d["bodies"] = targets;
            return d;
        }
        why = std::string(e ? e->objectType() : "?") + " has no NCX mapping in this exporter yet";
        return nullptr;
    }

    // the bodies after the current timeline position, as a STEP text (a carried result)
    bool resultStep(json& result, const std::vector<json>& before)
    {
        fs::path tmp = tempDir() / ("export_result_" + std::to_string(GetTickCount64()) + ".step");
        Ptr<ExportManager> em = des->exportManager();
        Ptr<STEPExportOptions> o = em->createSTEPExportOptions(tmp.u8string(), comp);
        if (!o || !em->execute(o)) return false;
        result = json{{"format", "step"}, {"replaces", before}, {"bodies", json::array()}, {"data", readFile(tmp)}};
        for (auto& b : allBodies())
            result["bodies"].push_back(json{{"name", b->name()}, {"sheet", !b->isSolid()}, {"fingerprint", resultFingerprint(b)}});
        std::error_code ec;
        fs::remove(tmp, ec);
        return true;
    }

    // Provenance (replay.cpp): the NCX feature a timeline object was built from, {id, type, k}, or null
    static Ptr<Attributes> attributesOfEntity(const Ptr<Base>& e)
    {
        if (Ptr<Feature> f = e) return f->attributes();
        if (Ptr<Sketch> s = e) return s->attributes();
        if (Ptr<ConstructionPlane> c = e) return c->attributes();
        return nullptr;
    }
    static std::string attrOf(const Ptr<Base>& e, const char* name)
    {
        Ptr<Attributes> as = attributesOfEntity(e);
        Ptr<Attribute> a = as ? as->itemByName("NCX", name) : nullptr;
        return a ? a->value() : "";
    }
    static json sourceTag(const Ptr<Base>& e)
    {
        json j = json::parse(attrOf(e, "source"), nullptr, false);
        return j.is_object() ? j : json(nullptr);
    }
    // the source types a Fusion object of this type writes natively as (anything else built it as an equivalent)
    static bool nativeFamily(const std::string& srcType, const std::string& fusionType)
    {
        static const std::map<std::string, std::string> fam{
            {"sketch", "adsk::fusion::Sketch"}, {"sketch3d", "adsk::fusion::Sketch"}, {"extrude", "adsk::fusion::ExtrudeFeature"},
            {"cut", "adsk::fusion::ExtrudeFeature"}, {"extrudedSurface", "adsk::fusion::ExtrudeFeature"},
            {"revolve", "adsk::fusion::RevolveFeature"}, {"revolveCut", "adsk::fusion::RevolveFeature"},
            {"revolvedSurface", "adsk::fusion::RevolveFeature"}, {"fillet", "adsk::fusion::FilletFeature"},
            {"chamfer", "adsk::fusion::ChamferFeature"}, {"plane", "adsk::fusion::ConstructionPlane"},
            {"planarSurface", "adsk::fusion::PatchFeature"}, {"knit", "adsk::fusion::StitchFeature"},
            {"deleteBody", "adsk::fusion::RemoveFeature"}};
        auto it = fam.find(srcType);
        return it != fam.end() && it->second == fusionType;
    }

    void walk()
    {
        Ptr<Timeline> tl = des->timeline();
        // this component's timeline objects (native: a feature of a nested component comes as a proxy in the root's
        // context, the model in the upright wrapper; read in its component's own frame), with their provenance
        struct Item
        {
            Ptr<TimelineObject> t;
            Ptr<Base> e;
            json tag;
        };
        std::vector<Item> items;
        for (size_t i = 0; i < tl->count(); i++)
        {
            Ptr<TimelineObject> t = tl->item(i);
            Ptr<Base> e = t->entity();
            if (!e) continue;
            if (Ptr<Feature> f = e)
            {
                if (f->assemblyContext())
                    if (Ptr<Feature> nf = f->parentComponent()->features()->itemByName(f->name())) e = nf;
            }
            else if (Ptr<Sketch> s = e)
            {
                if (s->nativeObject()) e = s->nativeObject();
            }
            else if (Ptr<ConstructionPlane> p = e)
            {
                if (p->nativeObject()) e = p->nativeObject();
            }
            Ptr<Component> owner;
            if (Ptr<Feature> f = e) owner = f->parentComponent();
            else if (Ptr<Sketch> s = e) owner = s->parentComponent();
            else if (Ptr<ConstructionPlane> p = e) owner = p->component();
            else continue;   // occurrences, joints: the assembly's business
            if (owner != comp) continue;
            items.push_back(Item{t, e, sourceTag(e)});
        }
        // ids: an object an NCX import made keeps its feature's payload id, so the source's references stay valid
        for (auto& it : items)
            if (it.tag.is_object()) reservedIds.insert(it.tag.value("id", ""));
        auto tokenOf = [](const Ptr<Base>& e) {
            return Ptr<Feature>(e) ? Ptr<Feature>(e)->entityToken() : Ptr<Sketch>(e) ? Ptr<Sketch>(e)->entityToken() : Ptr<ConstructionPlane>(e)->entityToken();
        };
        for (size_t ii = 0; ii < items.size(); ii++)
        {
            Ptr<TimelineObject> t = items[ii].t;
            Ptr<Base> e = items[ii].e;
            const json tag = items[ii].tag;
            // between two features: the timeline is at its end and every feature in its own suppression state,
            // so a cancel here leaves the design as it was
            progressStep(docName + ": reading " + t->name());
            ProgressNoCancel finishThisFeature;
            // Feature passthrough: the objects one source feature became (its group), when Fusion built it as something
            // else (an equivalent: Insert Part as sketch copies, a convert to sheet metal as a body and a conversion, a
            // carried result), or as several objects, or when it is not written natively below: written as the source's
            // definition while the part's shape right after the group is what the import left (NCX/after).
            size_t groupEnd = ii + 1;
            json srcDef;
            // an Insert Part's copied sketches are always written as the insertPart they stand for (they are Fusion's form
            // of it, whatever the passthrough setting): features refer to the source document's entities through it
            if (tag.is_object() && tag.value("k", 0) == 0 && (passthrough || tag.value("type", "") == "insertPart"))
            {
                while (groupEnd < items.size() && items[groupEnd].tag.is_object() && items[groupEnd].tag.value("id", "") == tag.value("id", "") &&
                       items[groupEnd].tag.value("k", 0) > 0)
                    groupEnd++;
                srcDef = json::parse(attrOf(e, "def"), nullptr, false);
                if (!srcDef.is_object() || srcDef.value("status", "") == "opaque" || !srcDef.contains("def")) srcDef = nullptr;
            }
            auto passedThrough = [&]() -> bool {
                if (srcDef.is_null()) return false;
                json after = json::parse(attrOf(e, "after"), nullptr, false);
                items[groupEnd - 1].t->rollTo(false);
                bool same = sameShape(after, shapeSignature(solids()));
                tl->moveToEnd();
                if (!same) return false;
                std::string sid = srcDef.value("id", "");
                usedIds.insert(sid);
                for (size_t k = ii; k < groupEnd; k++) idOfToken[tokenOf(items[k].e)] = sid;
                features.push_back(srcDef);
                notes.push_back(docName + ": " + srcDef.value("name", "") + " written as the source's own definition (Fusion built it as " +
                                std::to_string(groupEnd - ii) + " object(s), first " + e->objectType() + "; the part is as imported there)");
                ii = groupEnd - 1;
                return true;
            };
            bool nativeFirst = srcDef.is_null() || (groupEnd == ii + 1 && nativeFamily(srcDef.value("type", ""), e->objectType()));
            if (!nativeFirst && passedThrough()) continue;
            std::string id;
            if (tag.is_object() && tag.value("k", 0) == 0 && !usedIds.count(tag.value("id", ""))) id = tag.value("id", "");
            else id = fid();
            usedIds.insert(id);
            std::string name = t->name();
            if (Ptr<Sketch> s = e) name = s->name();
            else if (Ptr<Feature> f = e) name = f->name();
            idOfToken[tokenOf(e)] = id;
            // A suppressed feature is read as it is when active: unsuppressed for the moment (Fusion does not compute
            // a suppressed feature, so neither its definition's geometry nor its result could be read), written with
            // its suppression state, and suppressed again before the next feature.
            bool wasSuppressed = false;
            if (t->isSuppressed())
            {
                if (!t->isSuppressed(false) || t->isSuppressed())
                {
                    features.push_back(json{{"id", id}, {"name", name}, {"type", "opaque"}, {"status", "opaque"},
                                            {"reason", "suppressed in Fusion and could not be unsuppressed to read it"},
                                            {"suppressed", {{"cfg:Default", true}}}, {"native", {{"app", "Fusion"}, {"type", e->objectType()}}}});
                    continue;
                }
                wasSuppressed = true;
            }
            if (Ptr<ExtrudeFeature> ex = e)
            {
                t->rollTo(false);
                extrudeNegative = sideFacesBelow(ex);
            }
            // a hole's drilled cylinders exist only with it computed: their axes and diameters, read here
            holeAxes.clear();
            holeDiameters = json::array();
            if (Ptr<HoleFeature> hf = e)
            {
                t->rollTo(false);
                double r0 = hf->holeDiameter() ? cm2mm(hf->holeDiameter()->value()) / 2 : 0;
                std::vector<double> ds;
                Ptr<BRepFaces> sfs = hf->sideFaces();
                for (size_t i = 0; sfs && i < sfs->count(); i++)
                {
                    Ptr<BRepFace> fc = sfs->item(i);
                    if (fc->nativeObject()) fc = fc->nativeObject();
                    Ptr<Cylinder> cy = fc->geometry();
                    if (!cy) continue;
                    double r = cm2mm(cy->radius());
                    if (std::none_of(ds.begin(), ds.end(), [&](double x) { return std::fabs(x - 2 * r) < 1e-6; })) ds.push_back(2 * r);
                    if (std::fabs(r - r0) > 1e-6) continue;
                    V3 o = ptMm(cy->origin()), a = unit(vecOf(cy->axis()));
                    bool dup = false;
                    for (auto& h : holeAxes)
                        if (norm(cross(sub(o, h.first), h.second)) < 1e-6) dup = true;
                    if (!dup) holeAxes.push_back({o, a});
                }
                std::sort(ds.begin(), ds.end());
                for (double x : ds) holeDiameters.push_back(x);
                tl->moveToEnd();
            }
            flangeRise.clear();
            if (Ptr<FlangeFeature> fl = e) readFlangeRise(t, fl);
            // descriptors are measured in the state the feature saw: roll back to just before it
            t->rollTo(true);
            std::vector<json> before;
            for (auto& b : allBodies()) before.push_back(resultFingerprint(b));
            json nf{{"id", id}, {"name", name}, {"native", {{"app", "Fusion"}, {"type", e->objectType()}}}};
            std::string type, why;
            json def;
            if (Ptr<Sketch> s = e)
            {
                tl->moveToEnd();
                t->rollTo(false);
                if (isNonPlanar(s))
                {
                    type = "sketch3d";
                    def = sketch3dDef(s, id);
                }
                else
                {
                    type = "sketch";
                    def = sketchDef(s, id);
                }
            }
            else
            {
                bool smBefore = sheetMetalWritten;
                preFeatures = json::array();
                def = featureDef(e, type, why);
                if (def.is_null())
                {
                    sheetMetalWritten = smBefore;
                    preFeatures = json::array();
                }
            }
            tl->moveToEnd();
            // not written natively: the source's definition, if the part is still as imported there
            if (def.is_null() && !wasSuppressed && passedThrough())
            {
                usedIds.erase(id);
                continue;
            }
            if (!def.is_null())
            {
                for (auto& pf : preFeatures) features.push_back(pf);   // the part's rule, a flange's profiles
                preFeatures = json::array();
                nf["type"] = type;
                nf["status"] = "mapped";
                nf["def"] = def;
                if (Ptr<Feature> ft = e) featureOfId[id] = ft;
                // these carry their result (FORMAT-SPEC 4: systems differ there; an importer checks or chooses its
                // settings by it)
                static const std::set<std::string> withResult{"loft", "loftCut", "hem", "sketchedBend", "breakCorner", "fold", "unfold",
                                                              "offsetFaces", "convertToSheetMetal", "edgeFlange", "baseFlange",
                                                              "linearPattern", "circularPattern"};
                if (withResult.count(type) || (type == "fillet" && def.value("filletType", "") == "variable"))
                {
                    t->rollTo(false);
                    json result;
                    if (resultStep(result, before)) nf["result"] = result;
                    tl->moveToEnd();
                }
            }
            else
            {
                nf["type"] = "opaque";
                nf["status"] = "opaque";
                nf["reason"] = why;
                // its effect as geometry: the bodies right after it
                t->rollTo(false);
                json result;
                if (resultStep(result, before)) nf["result"] = result;
                tl->moveToEnd();
            }
            if (wasSuppressed)
            {
                nf["suppressed"] = {{"cfg:Default", true}};
                if (!t->isSuppressed(true)) notes.push_back(name + ": could not be suppressed again after reading it (" + lastError() + ")");
                tl->moveToEnd();
            }
            features.push_back(nf);
        }
        tl->moveToEnd();
    }

    json document()
    {
        json d{{"id", docId}, {"kind", "part"}, {"name", docName}, {"sourceFile", docName + ".f3d"},
               {"configurations", {{{"id", "cfg:Default"}, {"name", "Default"}, {"values", json::object()}}}},
               {"activeConfiguration", "cfg:Default"}, {"parameters", json::array()}, {"equations", json::array()},
               {"features", features}, {"bodies", json::array()}};
        int k = 0;
        for (auto& b : solids()) d["bodies"].push_back(json{{"id", "body:" + std::to_string(++k)}, {"name", b->name()}, {"fingerprint", bodyFingerprint(b)}});
        d["materials"] = materialsJson();
        json apps = appearancesJson();
        if (!apps.empty()) d["appearances"] = apps;
        json props = propertiesJson();
        if (!props.empty()) d["customProperties"] = props;
        return d;
    }

    // FORMAT-SPEC 4 materials: the component's material. One an NCX import chose for a source material is written
    // under the source's name and database (attribute NCX/material) while the component keeps it, so the source
    // system gets its own material back; any other under Fusion's name and library.
    json materialsJson()
    {
        json out = json::array();
        Ptr<Material> m = comp->material();
        if (!m) return out;
        json src;
        if (Ptr<Attribute> a = comp->attributes()->itemByName("NCX", "material")) src = json::parse(a->value(), nullptr, false);
        if (src.is_object() && src.value("fusion", "") == m->name())
        {
            // the source's material, or none when the source had none (Fusion's default stood in for it)
            if (!src.value("name", "").empty())
                out.push_back(json{{"configuration", "Default"}, {"name", src["name"]}, {"database", src.value("database", "")}});
        }
        else
        {
            std::string lib;
            if (Ptr<MaterialLibrary> l = m->parent()) lib = l->name();
            out.push_back(json{{"configuration", "Default"}, {"name", m->name()}, {"database", lib.empty() ? "Fusion" : lib}});
        }
        return out;
    }

    // FORMAT-SPEC 4 appearances: colours a body or face has of its own (not its material's). Every solid with the
    // same body appearance -> the document's. A colour an NCX import made is written with the source's exact
    // values (root attribute group NCX.appearance, by appearance name); any other as Fusion's colour and opacity.
    json appearancesJson()
    {
        json out = json::array();
        Ptr<Component> rootComp = des->rootComponent();
        auto colour = [&](const Ptr<Appearance>& a, json item) -> json {
            if (!a) return nullptr;
            if (Ptr<Attribute> rec = rootComp->attributes()->itemByName("NCX.appearance", a->name()))
            {
                json r = json::parse(rec->value(), nullptr, false);
                if (r.is_object() && r.contains("color"))
                {
                    item["kind"] = "color";
                    item["color"] = r["color"];
                    if (r.contains("optical") && !r["optical"].empty()) item["optical"] = r["optical"];
                    return item;
                }
            }
            Ptr<Color> c = a->color();
            if (!c)
            {
                notes.push_back(docName + ": appearance \"" + a->name() + "\" has no plain colour: not written");
                return nullptr;
            }
            item["kind"] = "color";
            item["color"] = json::array({c->red() / 255.0, c->green() / 255.0, c->blue() / 255.0});
            item["optical"] = json{{"transparency", 1.0 - c->opacity() / 255.0}};
            return item;
        };
        std::vector<Ptr<BRepBody>> ss = solids(), own;
        for (auto& b : ss)
            if (b->appearanceSourceType() == BodyAppearanceSource && b->appearance()) own.push_back(b);
        bool whole = !own.empty() && own.size() == ss.size();
        for (auto& b : own)
            if (b->appearance()->name() != own[0]->appearance()->name()) whole = false;
        if (whole)
        {
            json it = colour(own[0]->appearance(), json{{"target", {{"kind", "document"}, {"by", "id"}}}});
            if (!it.is_null()) out.push_back(it);
        }
        else
            for (auto& b : own)
            {
                json fp = bodyFingerprint(b);
                json it = colour(b->appearance(), json{{"target", {{"kind", "body"}, {"by", "geom"},
                                                                    {"geom", {{"volume", fp["volume"]}, {"centroid", fp["centroid"]}, {"bbox", fp["bbox"]}}}}}});
                if (!it.is_null()) out.push_back(it);
            }
        for (auto& b : ss)
            for (size_t i = 0; i < b->faces()->count(); i++)
            {
                Ptr<BRepFace> f = b->faces()->item(i);
                if (f->appearanceSourceType() != FaceAppearanceSource || !f->appearance()) continue;
                json it = colour(f->appearance(), json{{"target", descriptor(f)}});
                if (!it.is_null()) out.push_back(it);
            }
        return out;
    }

    // FORMAT-SPEC 4 custom properties: those an NCX import kept as attributes (group NCX.properties, with their
    // typed and resolved values), else Fusion's description and a part number other than the component's name.
    json propertiesJson()
    {
        json out = json::array();
        Ptr<Attributes> as = comp->attributes();
        std::set<std::string> have;
        for (size_t i = 0; as && i < as->count(); i++)
        {
            Ptr<Attribute> a = as->item(i);
            if (a->groupName() != "NCX.properties") continue;
            json p = json::parse(a->value(), nullptr, false);
            if (!p.is_object()) continue;
            have.insert(p.value("name", ""));
            out.push_back(p);
        }
        auto has = [&](std::initializer_list<const char*> names) {
            for (auto n : names)
                for (auto& h : have)
                    if (_stricmp(h.c_str(), n) == 0) return true;
            return false;
        };
        if (!comp->description().empty() && !has({"Description"}))
            out.push_back(json{{"name", "Description"}, {"type", "text"}, {"value", comp->description()}, {"resolved", comp->description()}});
        if (!comp->partNumber().empty() && comp->partNumber() != comp->name() && !has({"PartNo", "Part Number", "PartNumber"}))
            out.push_back(json{{"name", "PartNo"}, {"type", "text"}, {"value", comp->partNumber()}, {"resolved", comp->partNumber()}});
        return out;
    }
};


// ---------------------------------------------------------------------------------------- joints -> mates
// Fusion positions components with joints; NCX carries mates (FORMAT-SPEC 5, "Mates"). Every component goes out at
// its place (`transform`), so a mate only has to say what may still move. Each joint becomes the mates that leave
// the same freedom, made from faces the two components really share:
//   rigid        -> lock
//   revolute     -> concentric (coaxial cylinders) + coincident (a face across the axis)
//   cylindrical  -> concentric
//   slider       -> two coincident face pairs whose normals are across the slide direction
//   planar       -> one coincident face pair along the plane's normal
// Ball and pin-slot joints, and joints whose faces cannot be found, are written `opaque` (the components still
// arrive in place). A joint to the assembly itself uses the assembly's origin planes.
std::string sanitizeId(const std::string& s);   // below

struct MateWriter
{
    std::vector<std::string>& notes;
    json mates = json::array();
    int n = 0;
    explicit MateWriter(std::vector<std::string>& nt) : notes(nt) {}

    struct PlaneFace
    {
        Ptr<BRepFace> face;   // proxy (assembly frame); null for an assembly origin plane
        std::string originPlane;
        V3 p, nrm;
    };
    struct CylFace
    {
        Ptr<BRepFace> face;
        V3 p, axis;
    };

    // The assembly being written: its Fusion component, the ids of the components written in it, and one occurrence
    // of it in the design's root context (null for the design root) through which its parts' faces are measured.
    Ptr<Component> asmComp;
    std::set<std::string> written;
    Ptr<Occurrence> asmCtx;

    // An occurrence as a mate side of this assembly (FORMAT-SPEC 5, Mates: {component, path?}): the chain of
    // occurrences below the assembly, each id written as its occurrence's name ("c:<name>", unique in its parent).
    // Null when the occurrence is not below the assembly or its first level was not written.
    json who(const Ptr<Occurrence>& o)
    {
        if (!o) return nullptr;
        std::vector<std::string> segs;
        Ptr<Occurrence> p = o;
        for (; p; p = p->assemblyContext())
        {
            if (p->component() == asmComp) break;
            segs.push_back("c:" + sanitizeId(p->name()));
        }
        if (segs.empty()) return nullptr;
        std::reverse(segs.begin(), segs.end());
        if (!written.count(segs[0])) return nullptr;
        json s{{"component", segs[0]}};
        if (segs.size() > 1) s["path"] = json(std::vector<std::string>(segs.begin() + 1, segs.end()));
        return s;
    }
    std::string idOf(const Ptr<Occurrence>& o)
    {
        json s = who(o);
        return s.is_null() ? "?" : s["component"].get<std::string>();
    }

    // the bodies of an occurrence and of every occurrence under it (a joint on a sub-assembly occurrence: its
    // faces are its parts'); each face proxy's assemblyContext() names the part it belongs to
    static void bodiesUnder(const Ptr<Occurrence>& o, std::vector<Ptr<BRepBody>>& out)
    {
        if (!o) return;
        Ptr<BRepBodies> bs = o->bRepBodies();
        for (size_t i = 0; bs && i < bs->count(); i++) out.push_back(bs->item(i));
        Ptr<OccurrenceList> ch = o->childOccurrences();
        for (size_t i = 0; ch && i < ch->count(); i++) bodiesUnder(ch->item(i), out);
    }

    std::vector<PlaneFace> planes(const Ptr<Occurrence>& o)
    {
        std::vector<PlaneFace> v;
        if (!o)
        {
            v.push_back({nullptr, "front", {0, 0, 0}, {0, 0, 1}});
            v.push_back({nullptr, "top", {0, 0, 0}, {0, 1, 0}});
            v.push_back({nullptr, "right", {0, 0, 0}, {1, 0, 0}});
            return v;
        }
        std::vector<Ptr<BRepBody>> bs;
        bodiesUnder(o, bs);
        for (size_t i = 0; i < bs.size(); i++)
            for (size_t k = 0; k < bs[i]->faces()->count(); k++)
            {
                Ptr<BRepFace> f = bs[i]->faces()->item(k);
                Ptr<Plane> g = f->geometry();
                if (!g) continue;
                // the outward normal (the plane's own normal ignores the face's orientation)
                V3 nrm = unit(vecOf(g->normal()));
                Ptr<Vector3D> nv;
                if (f->evaluator()->getNormalAtPoint(f->pointOnFace(), nv) && nv) nrm = unit(vecOf(nv));
                v.push_back({f, "", ptMm(f->pointOnFace()), nrm});
            }
        return v;
    }

    std::vector<CylFace> cylinders(const Ptr<Occurrence>& o)
    {
        std::vector<CylFace> v;
        if (!o) return v;
        std::vector<Ptr<BRepBody>> bs;
        bodiesUnder(o, bs);
        for (size_t i = 0; i < bs.size(); i++)
            for (size_t k = 0; k < bs[i]->faces()->count(); k++)
            {
                Ptr<BRepFace> f = bs[i]->faces()->item(k);
                Ptr<Cylinder> g = f->geometry();
                if (g) v.push_back({f, ptMm(g->origin()), unit(vecOf(g->axis()))});
            }
        return v;
    }

    // the reference of one side: a face descriptor in its component's own frame, or an assembly origin plane
    json side(const Ptr<Occurrence>& o0, const Ptr<BRepFace>& face, const std::string& originPlane)
    {
        if (!o0) return json{{"ref", {{"kind", "originPlane"}, {"by", "id"}, {"plane", originPlane}}}};
        // the part the face belongs to (a sub-assembly occurrence's face is its part's: the side is the sub-assembly
        // with the path down to that part); measured in the design's root context (the upright wrapper, or the
        // sub-assembly's place in it)
        Ptr<Occurrence> o = (face && face->assemblyContext()) ? face->assemblyContext() : o0;
        Ptr<Occurrence> ctx = asmCtx ? o->createForAssemblyContext(asmCtx) : nullptr;
        setMeasureContext(ctx ? ctx : o);
        json d = descriptor(face->nativeObject() ? face->nativeObject() : face);
        setMeasureContext(nullptr);
        json s = who(o);
        if (s.is_null()) s = json{{"component", "?"}};
        s["ref"] = d;
        return s;
    }

    // coincident face pairs between the two sides whose normal is along `along` (or, with across, perpendicular
    // to it)
    std::vector<json> coincidentPairs(const Ptr<Occurrence>& a, const Ptr<Occurrence>& b, const V3& dir, bool across, size_t want)
    {
        std::vector<json> out;
        std::vector<V3> used;
        auto pa = planes(a), pb = planes(b);
        for (auto& fa : pa)
        {
            double c = std::fabs(dot(fa.nrm, dir));
            if (across ? c > 1e-6 : c < 1 - 1e-9) continue;
            bool dup = false;
            for (auto& u : used)
                if (std::fabs(std::fabs(dot(u, fa.nrm)) - 1) < 1e-9) dup = true;
            if (dup) continue;
            for (auto& fb : pb)
            {
                if (std::fabs(std::fabs(dot(fa.nrm, fb.nrm)) - 1) > 1e-9 || std::fabs(dot(sub(fb.p, fa.p), fa.nrm)) > 1e-4) continue;
                json m{{"type", "coincident"}, {"refs", json::array({side(a, fa.face, fa.originPlane), side(b, fb.face, fb.originPlane)})},
                       {"alignment", dot(fa.nrm, fb.nrm) < 0 ? "antiAligned" : "aligned"}};
                out.push_back(m);
                used.push_back(fa.nrm);
                break;
            }
            if (out.size() >= want) break;
        }
        return out;
    }

    json concentric(const Ptr<Occurrence>& a, const Ptr<Occurrence>& b, const V3& origin, const V3& axis)
    {
        auto on = [&](const CylFace& c) {
            if (norm(cross(c.axis, axis)) > 1e-6) return false;
            V3 w = sub(origin, c.p);
            return norm(sub(w, mul(c.axis, dot(w, c.axis)))) < 1e-3;
        };
        CylFace* ca = nullptr;
        CylFace* cb = nullptr;
        auto la = cylinders(a), lb = cylinders(b);
        for (auto& c : la)
            if (!ca && on(c)) ca = &c;
        for (auto& c : lb)
            if (!cb && on(c)) cb = &c;
        if (!ca || !cb) return nullptr;
        return json{{"type", "concentric"}, {"refs", json::array({side(a, ca->face, ""), side(b, cb->face, "")})}, {"alignment", "closest"}};
    }

    void add(json m, const std::string& name, const std::string& status = "mapped", const std::string& note = "")
    {
        m["id"] = "m:" + std::to_string(++n);
        m["name"] = name;
        m["status"] = status;
        if (!note.empty()) m[status == "opaque" ? "reason" : "note"] = note;
        mates.push_back(m);
    }

    // what each joint became, for the motion links that couple joints (gear, rack and pinion, screw)
    struct JointInfo
    {
        JointTypes type;
        Ptr<Occurrence> mover, other;
        V3 origin, axis;
    };
    std::map<std::string, JointInfo> info;   // by joint name

    json cylinderSide(const Ptr<Occurrence>& o, const V3& origin, const V3& axis)
    {
        for (auto& c : cylinders(o))
        {
            if (norm(cross(c.axis, axis)) > 1e-6) continue;
            V3 w = sub(origin, c.p);
            if (norm(sub(w, mul(c.axis, dot(w, c.axis)))) < 1e-3) return side(o, c.face, "");
        }
        return nullptr;
    }

    void motionLink(const std::string& name, const std::string& j1, const std::string& j2, double v1, double v2, bool reversed)
    {
        // a link within one joint (a cylindrical joint's turn and slide: a screw) names that joint twice
        auto a = info.find(j1), b = (j2.empty() || j2 == j1) ? info.end() : info.find(j2);
        auto opaque = [&](const std::string& why) { add(json{{"type", "other"}, {"refs", json::array()}}, name, "opaque", why); };
        if (a == info.end()) return opaque("its joint was not exported");
        auto rot = [](JointTypes t) { return t == RevoluteJointType || t == CylindricalJointType; };
        if (b != info.end() && rot(a->second.type) && rot(b->second.type))
        {
            json r1 = cylinderSide(a->second.mover, a->second.origin, a->second.axis), r2 = cylinderSide(b->second.mover, b->second.origin, b->second.axis);
            if (r1.is_null() || r2.is_null()) return opaque("a gear has no cylindrical face on its axis");
            // the importer's reading inverted: turning joint one by v1 turns joint two by v2, ratio a:b = v2:v1
            add(json{{"type", "gear"}, {"refs", json::array({r1, r2})}, {"params", {{"ratio", {std::fabs(v2), std::fabs(v1)}}, {"reverse", reversed}}}}, name);
            return;
        }
        if (b != info.end() && ((rot(a->second.type) && b->second.type == SliderJointType) || (a->second.type == SliderJointType && rot(b->second.type))))
        {
            bool aTurns = rot(a->second.type);
            auto& pin = aTurns ? a->second : b->second;
            auto& rack = aTurns ? b->second : a->second;
            double turn = aTurns ? v1 : v2, travel = aTurns ? v2 : v1;
            json rp = cylinderSide(pin.mover, pin.origin, pin.axis);
            auto rf = planes(rack.mover);
            if (rp.is_null() || rf.empty()) return opaque("the rack or the pinion has no face to refer to");
            json rr = side(rack.mover, rf[0].face, "");
            rr["role"] = "rack";
            rp["role"] = "pinion";
            double perTurn = std::fabs(travel) * 360.0 / std::max(1e-9, std::fabs(turn));
            add(json{{"type", "rackPinion"}, {"refs", json::array({rr, rp})},
                     {"params", {{"pitchDiameter", perTurn / kPi}, {"travelPerTurn", perTurn}, {"byTravel", true}, {"reverse", reversed}}}}, name);
            return;
        }
        if (b == info.end() && a->second.type == CylindricalJointType)
        {
            json r1 = cylinderSide(a->second.mover, a->second.origin, a->second.axis), r2 = cylinderSide(a->second.other, a->second.origin, a->second.axis);
            if (r1.is_null() || r2.is_null()) return opaque("the screw has no cylindrical faces on its axis");
            add(json{{"type", "screw"}, {"refs", json::array({r1, r2})},
                     {"params", {{"pitch", std::fabs(v2) * 360.0 / std::max(1e-9, std::fabs(v1))}, {"reverse", reversed}}}}, name);
            return;
        }
        opaque("a motion link between these joint types has no NCX mate");
    }

    // axisOverride: the joint's axis (revolute, cylindrical, slider) or plane normal (planar) when Fusion does not
    // report it (a sub-assembly's joint read back without its occurrences: the import's record, joints.cpp)
    void joint(const std::string& name, Ptr<Occurrence> a, Ptr<Occurrence> b, const Ptr<JointMotion>& motion, const V3& origin,
               const V3* axisOverride = nullptr, bool fastener = false)
    {
        auto vecOr = [&](const Ptr<Vector3D>& v) { return axisOverride ? *axisOverride : v ? unit(vecOf(v)) : V3{0, 0, 1}; };
        V3 jaxis = axisOverride ? *axisOverride : V3{0, 0, 1};
        if (Ptr<RevoluteJointMotion> r = motion) jaxis = vecOr(r->rotationAxisVector());
        if (Ptr<CylindricalJointMotion> c = motion) jaxis = vecOr(c->rotationAxisVector());
        if (Ptr<SliderJointMotion> sl = motion) jaxis = vecOr(sl->slideDirectionVector());
        if (Ptr<PlanarJointMotion> pl = motion) jaxis = vecOr(pl->normalDirectionVector());
        if (!a && b) std::swap(a, b);
        if (!a)
        {
            add(json{{"type", "other"}, {"refs", json::array()}}, name, "opaque", "a joint between no components");
            return;
        }
        if (idOf(a) == "?" || (b && idOf(b) == "?"))
        {
            add(json{{"type", "other"}, {"refs", json::array()}}, name, "opaque", "a joint to a component that is not exported (hidden, or outside this assembly)");
            return;
        }
        JointTypes t = motion->jointType();
        {
            JointInfo ji{t, a, b, origin, jaxis};
            info[name] = ji;
        }
        auto lockTo = [&]() {
            json front{{"kind", "originPlane"}, {"by", "id"}, {"plane", "front"}};
            json ra = who(a);
            ra["ref"] = front;
            json refs = json::array({ra});
            if (b)
            {
                json rb = who(b);
                rb["ref"] = front;
                refs.push_back(rb);
            }
            else
                refs.push_back(json{{"ref", front}});
            return json{{"type", "lock"}, {"refs", refs}};
        };
        if (t == RigidJointType)
        {
            add(lockTo(), name);
            return;
        }
        if (t == RevoluteJointType || t == CylindricalJointType)
        {
            V3 axis = jaxis;
            json con = concentric(a, b, origin, axis);
            if (con.is_null())
            {
                add(json{{"type", "other"}, {"refs", json::array()}}, name, "opaque", "no coaxial cylindrical faces on both components for its axis");
                return;
            }
            add(con, name + (t == RevoluteJointType ? " (axis)" : ""));
            if (t == RevoluteJointType)
            {
                auto cc = coincidentPairs(a, b, axis, false, 1);
                if (cc.empty() && fastener)
                {
                    // a fastener the import seated on another part (attribute NCX/joint): held on this partner by a
                    // lock, so it cannot slide off; it loses only its spin
                    mates.erase(mates.size() - 1);
                    n--;
                    add(lockTo(), name, "approximate",
                        "a fastener's revolute joint with no faces meeting across its axis on its partner: written as a lock, so it "
                        "stays on its partner (it does not spin)");
                    return;
                }
                if (cc.empty())
                    notes.push_back("joint " + name + ": no faces meet across its axis: written as a concentric mate only (it may also slide)");
                else
                    add(cc[0], name + " (face)");
            }
            return;
        }
        if (t == SliderJointType)
        {
            V3 d = jaxis;
            auto cc = coincidentPairs(a, b, d, true, 2);
            if (cc.size() < 2)
            {
                add(json{{"type", "other"}, {"refs", json::array()}}, name, "opaque", "no two face pairs that meet along its slide direction");
                return;
            }
            for (size_t i = 0; i < cc.size(); i++) add(cc[i], name + " (" + std::to_string(i + 1) + ")");
            return;
        }
        if (t == PlanarJointType)
        {
            auto cc = coincidentPairs(a, b, jaxis, false, 1);
            if (cc.empty())
            {
                add(json{{"type", "other"}, {"refs", json::array()}}, name, "opaque", "no faces meet in its plane");
                return;
            }
            add(cc[0], name);
            return;
        }
        add(json{{"type", "other"}, {"refs", json::array()}}, name, "opaque", "Fusion joint type " + std::to_string(t) + " has no NCX mate");
    }
};

std::string sanitizeId(const std::string& s)
{
    std::string r;
    for (unsigned char c : s) r += (std::isalnum(c) || c == '_' || c == '-') ? static_cast<char>(c) : '_';
    return r;
}

} // namespace

json exportFileInner(const fs::path& out, const json& opts);

// A parameter's name as the file it came from wrote it: a global variable an NCX import made (its comment "NCX: global
// variable W of <document>": Fusion refuses unit symbols as names, so W became W_), a dimension it named ("D1@SK_Base"),
// else Fusion's own name.
std::string nativeParamName(const Ptr<Parameter>& p)
{
    std::string c = p->comment();
    const std::string pre = "NCX: global variable ";
    if (c.rfind(pre, 0) == 0)
    {
        std::string r = c.substr(pre.size());
        size_t of = r.rfind(" of ");
        return of == std::string::npos ? r : r.substr(0, of);
    }
    if (c.find('@') != std::string::npos && c.find(' ') == std::string::npos) return c;
    return p->name();
}

// A Fusion expression in the neutral grammar (FORMAT-SPEC 4, Equations): parameter names quoted under their native
// names, lengths in mm and angles in degrees ("( W_ ) * 1 mm" -> ( "W" ), "10 cm" -> 10 * 10).
std::string neutralExpression(const Ptr<Design>& des, const std::string& ex)
{
    static const std::map<std::string, std::string> units{{"mm", ""}, {"cm", " * 10"}, {"m", " * 1000"}, {"in", " * 25.4"}, {"ft", " * 304.8"},
                                                          {"deg", ""}, {"rad", " * 180 / pi"}};
    static const std::set<std::string> fns{"sin", "cos", "tan", "asin", "acos", "atan", "sqrt", "abs", "min", "max", "pi", "round", "floor", "ceil", "exp", "ln", "log"};
    std::string out;
    for (size_t k = 0; k < ex.size();)
    {
        if (std::isalpha(static_cast<unsigned char>(ex[k])) || ex[k] == '_')
        {
            size_t j = k;
            while (j < ex.size() && (std::isalnum(static_cast<unsigned char>(ex[j])) || ex[j] == '_')) j++;
            std::string w = ex.substr(k, j - k);
            auto u = units.find(w);
            if (u != units.end())
            {
                // "( x ) * 1 mm": the factor of one is dropped with its unit
                size_t e = out.find_last_not_of(' ');
                if (e != std::string::npos && e >= 2 && out.compare(e - 2, 3, "* 1") == 0 && (e < 3 || !std::isdigit(static_cast<unsigned char>(out[e - 3])) && out[e - 3] != '.'))
                    out = out.substr(0, e - 2);
                while (!out.empty() && out.back() == ' ') out.pop_back();
                out += u->second;
            }
            else if (fns.count(w))
                out += w;
            else
            {
                Ptr<Parameter> q = des->allParameters()->itemByName(w);
                out += "\"" + (q ? nativeParamName(q) : w) + "\"";
            }
            k = j;
        }
        else
            out += ex[k++];
    }
    // trim
    size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    return a == std::string::npos ? "" : out.substr(a, b - a + 1);
}

// The export with progress and cancel: a cancelled export writes no file and leaves the design as it was (the
// walk is only cancelled between two features, with the timeline at its end).
json exportFile(const fs::path& out, const json& opts)
{
    if (opts.value("cancelAfter", -1) >= 0) progressCancelAfter(opts["cancelAfter"].get<int>());
    progressBegin("Export NCX: " + out.filename().u8string(), 10, opts.value("progress", true));
    json res;
    try
    {
        res = exportFileInner(out, opts);
    }
    catch (const Cancelled& c)
    {
        setMeasureContext(nullptr);
        if (Ptr<Design> des = Application::get()->activeProduct())
            if (des->timeline()) des->timeline()->moveToEnd();
        res = json{{"ok", true}, {"status", "cancelled"}, {"cancelledAt", c.where}, {"file", out.u8string()},
                   {"notes", json::array({"cancelled by the user at " + c.where + "; no file was written"})}};
    }
    if (res.is_object()) res["progress"] = progressTimeline();
    progressEnd();
    return res;
}

json exportFileInner(const fs::path& out, const json& opts)
{
    Ptr<Application> app = Application::get();
    Ptr<Design> des = app->activeProduct();
    if (!des) return json{{"ok", false}, {"error", "the active document is not a design"}};
    if (des->designType() != ParametricDesignType) return json{{"ok", false}, {"error", "only parametric designs carry a history to export"}};
    // an imported model placed upright (common.h) is written from its wrapper component, in the file's frame
    Ptr<Occurrence> wrapOcc = findModelWrapper(des);
    Ptr<Component> root = wrapOcc ? wrapOcc->component() : des->rootComponent();
    std::string rootName = root->name();
    if (rootName.empty() || rootName[0] == '(')
    {
        rootName = out.filename().u8string();
        for (const char* ext : {".step", ".stp", ".ncx"})
            if (rootName.size() > strlen(ext) && rootName.compare(rootName.size() - strlen(ext), strlen(ext), ext) == 0)
                rootName = rootName.substr(0, rootName.size() - strlen(ext));
    }
    json model{{"format", "ncx"}, {"schema", "0.1"}, {"generator", {{"app", "Fusion"}, {"version", app->version()}, {"exporter", std::string("ncx-fusion ") + kImplVersion}}},
               {"units", {{"length", "mm"}, {"angle", "deg"}, {"mass", "kg"}}}, {"documents", json::array()}};
    // which way is up (FORMAT-SPEC 4): a design imported from NCX keeps its file's frame and up axis (the import
    // records it on the root component); a design made in Fusion is up along Fusion's own up axis (its default
    // modelling orientation)
    {
        std::string up;
        if (Ptr<Attribute> a = des->rootComponent()->attributes()->itemByName("NCX", "orientation")) up = a->value();
        if (up.empty())
        {
            bool zUp = app->preferences() && app->preferences()->generalPreferences() &&
                       app->preferences()->generalPreferences()->defaultModelingOrientation() == ZUpModelingOrientation;
            up = zUp ? "z" : "y";
        }
        model["orientation"] = json{{"up", up}};
    }
    json notes = json::array();
    // the component definitions: every component with bodies or features is a part document
    std::vector<Ptr<Component>> parts;
    std::map<std::string, std::string> docOf;   // component entity token -> document id
    std::map<std::string, std::string> subAsmDoc;   // sub-assembly component entity token -> its document id
    // Document passthrough: a document an NCX import made that Fusion has left as it was (its state as recorded,
    // host.h recordDocument) is written as the source's own document, history, mates and all; a document an import made
    // keeps the source's id whichever way it is written, so references between documents stay valid. Off with
    // {"passthrough": false} (to test the native export of imported models).
    const bool passthrough = opts.value("passthrough", true);
    std::set<std::string> idsUsed;
    std::map<std::string, Ptr<Component>> subAsmComp;   // sub-assembly component entity token -> the component
    auto docIdFor = [&](const Ptr<Component>& c, const std::string& name) {
        std::string id;
        if (Ptr<Attribute> a = c->attributes()->itemByName("NCX", "documentId")) id = a->value();
        if (id.empty()) id = "doc:" + sanitizeId(name);
        // a second Fusion component built from one document (a sub-assembly placed rigid and flexible) gets its own id
        std::string base = id;
        for (int k = 2; idsUsed.count(id); k++) id = base + "#" + std::to_string(k);
        idsUsed.insert(id);
        return id;
    };
    // the recorded source document when the component is as the import left it, else null
    auto unchangedSource = [&](const Ptr<Component>& c) -> json {
        if (!passthrough) return nullptr;
        Ptr<Attribute> st = c->attributes()->itemByName("NCX", "state");
        Ptr<Attribute> dd = c->attributes()->itemByName("NCX", "document");
        if (!st || !dd) return nullptr;
        if (!sameState(json::parse(st->value(), nullptr, false), documentState(c))) return nullptr;
        json d = json::parse(dd->value(), nullptr, false);
        return d.is_object() ? d : json(nullptr);
    };
    Ptr<OccurrenceList> occs = root->allOccurrences();
    for (size_t i = 0; occs && i < occs->count(); i++)
    {
        Ptr<Component> c = occs->item(i)->component();
        if (docOf.count(c->entityToken())) continue;
        // an import's partial rebuild of a part that fell back (hidden, for diagnosis) is not part of the model
        if (c->attributes()->itemByName("NCX", "partialRebuild") || c->name().size() > 18 &&
                                                                         c->name().compare(c->name().size() - 18, 18, " (partial rebuild)") == 0)
            continue;
        if (c->occurrences()->count() > 0)
        {
            // a sub-assembly: its own assembly document (written below, when an assembly places it)
            if (c->bRepBodies()->count() > 0)
                notes.push_back("sub-assembly " + c->name() + ": its own bodies (beside its components) are not written");
            subAsmDoc[c->entityToken()] = "";   // its id below, rigid builds first
            subAsmComp[c->entityToken()] = c;
            continue;
        }
        docOf[c->entityToken()] = docIdFor(c, c->name());
        parts.push_back(c);
    }
    for (int pass = 0; pass < 2; pass++)   // sub-assembly ids: the rigid builds first, then " (flexible)" copies
        for (auto& kv : subAsmDoc)
        {
            if (!kv.second.empty()) continue;
            Ptr<Component> c = subAsmComp[kv.first];
            if (!c) continue;
            bool flexCopy = c->name().size() > 11 && c->name().compare(c->name().size() - 11, 11, " (flexible)") == 0;
            if (flexCopy != (pass == 1)) continue;
            kv.second = docIdFor(c, c->name());
        }
    bool assembly = !parts.empty();
    if (!assembly) parts.push_back(root);
    progressSetTotal(static_cast<int>(des->timeline()->count() + (occs ? occs->count() : 0)) + 6);
    json userParams = json::array();
    for (size_t i = 0; i < des->userParameters()->count(); i++)
    {
        Ptr<UserParameter> p = des->userParameters()->item(i);
        userParams.push_back(json{{"name", nativeParamName(p)}, {"value", p->unit().empty() ? p->value() : (p->unit() == "deg" ? rad2deg(p->value()) : cm2mm(p->value()))},
                                  {"unit", p->unit().empty() ? "none" : (p->unit() == "deg" ? "deg" : "mm")}, {"expression", neutralExpression(des, p->expression())},
                                  {"native", p->expression()}});
    }
    for (size_t pi = 0; pi < parts.size(); pi++)
    {
        DocWriter w;
        w.des = des;
        w.comp = parts[pi];
        w.passthrough = passthrough;
        Ptr<Occurrence> ctx;
        for (size_t i = 0; occs && i < occs->count() && !ctx; i++)
            if (occs->item(i)->component() == parts[pi] && !occs->item(i)->assemblyContext()) ctx = occs->item(i);
        if (wrapOcc)
        {
            Ptr<Occurrence> p = ctx ? ctx->createForAssemblyContext(wrapOcc) : nullptr;
            ctx = p ? p : (parts[pi] == root ? wrapOcc : ctx);
        }
        setMeasureContext(ctx);
        w.docName = parts[pi] == root ? rootName : parts[pi]->name();
        w.docId = parts[pi] == root ? docIdFor(root, w.docName) : docOf[parts[pi]->entityToken()];
        json src = unchangedSource(parts[pi]);
        if (!src.is_null())
        {
            src["id"] = w.docId;
            model["documents"].push_back(src);
            notes.push_back(w.docName + ": as its NCX import left it: written as the source's own document");
            setMeasureContext(nullptr);
            continue;
        }
        progressStep(w.docName + ": reading its parameters");
        w.walk();
        json d = w.document();
        // user parameters are design-wide in Fusion: written with the part whose equations use them (below)
        // equations: dimension parameters whose expression is not a plain number
        for (auto& nf : d["features"])
            if (nf.value("type", "") == "sketch")
                for (auto& dm : nf["def"]["dimensions"])
                    if (dm.contains("param"))
                    {
                        Ptr<Parameter> p = des->allParameters()->itemByName(dm["param"].get<std::string>());
                        if (!p) continue;
                        bool foreign = false;
                        Ptr<ParameterList> deps = p->dependencyParameters();
                        for (size_t q = 0; deps && q < deps->count(); q++)
                            if (Ptr<ModelParameter> mp = deps->item(q))
                                if (mp->component() != parts[pi]) foreign = true;
                        if (foreign)
                        {
                            notes.push_back(w.docName + ": " + dm.value("fullName", "") + " follows another component's parameter: written as its value");
                            continue;
                        }
                        std::string ex = p->expression();
                        d["equations"].push_back(json{{"target", dm.value("fullName", "")}, {"expression", neutralExpression(des, ex)}, {"native", ex}, {"status", "mapped"}});
                    }
        setMeasureContext(nullptr);
        for (auto& n : w.notes) notes.push_back(n);
        model["documents"].push_back(d);
    }
    // The design's user parameters go with the document whose equations name them (the layout of an imported
    // layout-driven model; L01's re-export put them on the first part, and the layout's equations then named
    // parameters of another document); none names them: the first part.
    {
        // a global variable a passed-through document carries already is not written again
        std::set<std::string> carried;
        for (auto& d : model["documents"])
            for (auto& p : d.value("parameters", json::array())) carried.insert(p.value("name", ""));
        json rest = json::array();
        for (auto& up : userParams)
            if (!carried.count(up.value("name", ""))) rest.push_back(up);
        userParams = rest;
    }
    if (!userParams.empty() && !model["documents"].empty())
    {
        size_t best = 0;
        int bestN = 0;
        for (size_t i = 0; i < model["documents"].size(); i++)
        {
            int n = 0;
            for (auto& eq : model["documents"][i].value("equations", json::array()))
                for (auto& up : userParams)
                    if (eq.value("expression", "").find("\"" + up.value("name", "") + "\"") != std::string::npos) n++;
            if (n > bestN)
            {
                bestN = n;
                best = i;
            }
        }
        model["documents"][best]["parameters"] = userParams;
    }
    // the assembly
    json fp = json{{"bodies", json::array()}};
    if (assembly)
    {
        // The file's fingerprint: every solid of every part shown, where the design places it (in the file's frame)
        for (size_t i = 0; i < occs->count(); i++)
        {
            Ptr<Occurrence> o = occs->item(i);
            if (!o->isLightBulbOn() || o->childOccurrences()->count() > 0) continue;
            bool shown = true;
            for (Ptr<Occurrence> p = o->assemblyContext(); p; p = p->assemblyContext())
                if (!p->isLightBulbOn()) shown = false;
            if (!shown || !docOf.count(o->component()->entityToken())) continue;
            for (size_t b = 0; b < o->component()->bRepBodies()->count(); b++)
                if (o->component()->bRepBodies()->item(b)->isSolid())
                    fp["bodies"].push_back(bodyFingerprint(o->component()->bRepBodies()->item(b), o->transform2()));
        }
        // Assemblies are written nested, as the design holds them (FORMAT-SPEC 5): the root (or the upright
        // wrapper, which is transparent) and each sub-assembly it shows, once per Fusion component, each with its
        // own components (transforms relative to it) and its own joints as mates. A component held to its parent
        // (Ground to Parent, as the import holds a rigid sub-assembly's parts and the wrapper's fixed ones) or
        // grounded is `fixed`. A sub-assembly whose parts are not all held to it moves with the parent's joints:
        // `flexible`. (Written flat, the parts held only by Ground to Parent had no mate at all and came apart in
        // SolidWorks; measured on the folding step.)
        std::set<std::string> asmWritten;
        auto heldToParent = [](const Ptr<Occurrence>& o) { return o->isGrounded() || o->isGroundToParent(); };
        auto markSuppressed = [&](MateWriter& w, size_t from, bool supp, const std::string& jn) {
            if (!supp) return;
            for (size_t k = from; k < w.mates.size(); k++) w.mates[k]["suppressed"] = {{"cfg:active", true}};
            notes.push_back(jn + ": suppressed joint written as suppressed mate(s)");
        };
        std::function<json(const Ptr<Component>&, const std::string&, const std::string&, const Ptr<Occurrence>&)> writeAssembly;
        writeAssembly = [&](const Ptr<Component>& c, const std::string& id, const std::string& name, const Ptr<Occurrence>& ctxOcc) -> json {
            // An assembly as its NCX import left it: the source's own document (its mates, limits and all), each
            // component where Fusion has it now (a mechanism moved in Fusion goes out as it was left; the source's
            // mates still hold it), its sub-assemblies written once each (passed through or not) under their ids.
            json src = unchangedSource(c);
            if (!src.is_null())
            {
                src["id"] = id;
                std::map<std::string, Ptr<Occurrence>> occById;
                for (size_t i = 0; i < c->occurrences()->count(); i++)
                {
                    Ptr<Occurrence> o = c->occurrences()->item(i);
                    if (Ptr<Attribute> at = o->attributes()->itemByName("NCX", "component"))
                    {
                        json r = json::parse(at->value(), nullptr, false);
                        if (r.is_object()) occById[r.value("id", "")] = o;
                    }
                }
                for (auto& ce : src["components"])
                {
                    auto it = occById.find(ce.value("id", ""));
                    if (it != occById.end()) ce["transform"] = rowMajorMm(it->second->transform2());
                    std::string sd = ce.value("document", "");
                    if (asmWritten.count(sd)) continue;
                    for (auto& kv : subAsmDoc)
                        if (kv.second == sd && subAsmComp[kv.first])
                        {
                            asmWritten.insert(sd);
                            Ptr<Component> oc = subAsmComp[kv.first];
                            Ptr<Occurrence> sub;
                            Ptr<OccurrenceList> all = des->rootComponent()->allOccurrencesByComponent(oc);
                            if (all && all->count() > 0) sub = all->item(0);
                            model["documents"].push_back(writeAssembly(oc, sd, oc->name(), sub));
                            break;
                        }
                }
                notes.push_back(name + ": as its NCX import left it: written as the source's own document, components where they are now");
                return src;
            }
            json a{{"id", id}, {"kind", "assembly"}, {"name", name}, {"sourceFile", name + ".f3d"},
                   {"configurations", {{{"id", "cfg:Default"}, {"name", "Default"}}}}, {"activeConfiguration", "cfg:Default"},
                   {"features", json::array()}, {"components", json::array()}, {"mates", json::array()}};
            std::vector<std::string> mnotes;
            MateWriter mw(mnotes);
            mw.asmComp = c;
            mw.asmCtx = ctxOcc;
            Ptr<Component> designRoot = des->rootComponent();
            for (size_t i = 0; i < c->occurrences()->count(); i++)
            {
                Ptr<Occurrence> o = c->occurrences()->item(i);
                progressStep(name + ": component " + o->name());
                if (!o->isLightBulbOn()) continue;
                Ptr<Component> oc = o->component();
                std::string cid = "c:" + sanitizeId(o->name());
                json ce{{"id", cid}, {"configuration", "Default"}, {"name", o->name()}, {"transform", rowMajorMm(o->transform2())},
                        {"fixed", heldToParent(o)}};
                auto st = subAsmDoc.find(oc->entityToken());
                if (st != subAsmDoc.end())
                {
                    bool rigid = true;
                    for (size_t k = 0; k < oc->occurrences()->count(); k++)
                        if (oc->occurrences()->item(k)->isLightBulbOn() && !heldToParent(oc->occurrences()->item(k))) rigid = false;
                    if (!asmWritten.count(st->second))
                    {
                        asmWritten.insert(st->second);
                        // its parts' faces are measured through its place in the design's root context
                        Ptr<Occurrence> sub;
                        Ptr<OccurrenceList> all = designRoot->allOccurrencesByComponent(oc);
                        if (all && all->count() > 0) sub = all->item(0);
                        json sd = writeAssembly(oc, st->second, oc->name(), sub);
                        if (sd["components"].empty())
                        {
                            asmWritten.erase(st->second);
                            continue;
                        }
                        model["documents"].push_back(sd);
                    }
                    ce["document"] = st->second;
                    if (!rigid) ce["flexible"] = true;
                }
                else if (docOf.count(oc->entityToken()))
                    ce["document"] = docOf[oc->entityToken()];
                else
                    continue;   // a part not written (a partial rebuild)
                a["components"].push_back(ce);
                mw.written.insert(cid);
            }
            // joints -> mates; a suppressed joint's mates go out suppressed (FORMAT-SPEC 5, Mates). Assemblies list no
            // configurations, so the key is "cfg:active", as the SolidWorks exporter writes it
            for (size_t i = 0; i < c->joints()->count(); i++)
            {
                Ptr<Joint> j = c->joints()->item(i);
                progressTick(name + ": joint " + j->name());
                if (!j->occurrenceOne() && ctxOcc)
                    if (Ptr<Joint> pj = j->createForAssemblyContext(ctxOcc))
                        if (pj->occurrenceOne()) j = pj;
                V3 origin{0, 0, 0};
                Ptr<Base> g1 = j->geometryOrOriginOne();
                if (Ptr<JointGeometry> g = g1) origin = ptMm(g->origin());
                else if (Ptr<JointOrigin> jo = g1) origin = ptMm(jo->geometry()->origin());
                size_t n0 = mw.mates.size();
                mw.joint(j->name(), j->occurrenceOne(), j->occurrenceTwo(), j->jointMotion(), origin);
                markSuppressed(mw, n0, j->isSuppressed(), j->name());
            }
            for (size_t i = 0; i < c->asBuiltJoints()->count(); i++)
            {
                Ptr<AsBuiltJoint> j = c->asBuiltJoints()->item(i);
                progressTick(name + ": joint " + j->name());
                // Read through a sub-assembly's component, an as-built joint the import made in it has no occurrences
                // or geometry, also through its proxy (measured on L12's arm). The import records them on the joint
                // (attribute NCX/joint, joints.cpp: the occurrences' names, a point on its axis and its axis, in the
                // sub-assembly's frame), and whether it holds a fastener.
                Ptr<Occurrence> one = j->occurrenceOne(), two = j->occurrenceTwo();
                V3 origin = j->geometry() ? ptMm(j->geometry()->origin()) : V3{0, 0, 0};
                V3 recAxis{0, 0, 1};
                bool recorded = false, fastener = false;
                if (Ptr<Attribute> at = j->attributes()->itemByName("NCX", "joint"))
                {
                    json r = json::parse(at->value(), nullptr, false);
                    auto byName = [&](const std::string& n) -> Ptr<Occurrence> {
                        for (size_t k = 0; !n.empty() && k < c->occurrences()->count(); k++)
                            if (c->occurrences()->item(k)->name() == n) return c->occurrences()->item(k);
                        return nullptr;
                    };
                    if (r.is_object())
                    {
                        fastener = r.value("fastener", false);
                        if (!one)
                        {
                            one = byName(r.value("one", ""));
                            two = byName(r.value("two", ""));
                            if (r.contains("origin")) origin = v3(r["origin"]);
                            if (r.contains("axis")) recAxis = v3(r["axis"]);
                            recorded = (bool)one;
                        }
                    }
                }
                size_t n0 = mw.mates.size();
                mw.joint(j->name(), one, two, j->jointMotion(), origin, recorded ? &recAxis : nullptr, fastener);
                markSuppressed(mw, n0, j->isSuppressed(), j->name());
            }
            for (size_t i = 0; i < c->motionLinks()->count(); i++)
            {
                Ptr<MotionLink> ml = c->motionLinks()->item(i);
                if (ml->isSuppressed()) continue;
                auto jname = [](const Ptr<Base>& j) -> std::string {
                    if (Ptr<Joint> jj = j) return jj->name();
                    if (Ptr<AsBuiltJoint> ab = j) return ab->name();
                    return "";
                };
                // values: an angle parameter is radians, a distance centimetres; the link only needs their ratio, in
                // degrees and millimetres
                auto val = [](const Ptr<ModelParameter>& p) {
                    if (!p) return 0.0;
                    return p->unit() == "deg" ? rad2deg(p->value()) : cm2mm(p->value());
                };
                mw.motionLink(ml->name(), jname(ml->jointOne()), jname(ml->jointTwo()), val(ml->valueOne()), val(ml->valueTwo()), ml->isReversed());
            }
            // a joint Fusion reads back without its occurrences (seen inside rigid sub-assemblies) holds nothing the
            // `fixed` components do not: left out, with a note, rather than written as an empty opaque mate
            json kept = json::array();
            for (auto& m : mw.mates)
                if (m.value("status", "") == "opaque" && m.value("reason", "") == "a joint between no components")
                    notes.push_back(name + ": joint " + m.value("name", "") + " is read back without its components: not written");
                else
                    kept.push_back(m);
            a["mates"] = kept;
            for (auto& m : mnotes) notes.push_back(name + ": " + m);
            return a;
        };
        json a = writeAssembly(root, docIdFor(root, rootName), rootName, wrapOcc);
        model["documents"].push_back(a);
        model["root"] = a["id"];
    }
    else
    {
        model["root"] = model["documents"][0]["id"];
        fp = json{{"bodies", json::array()}};
        for (auto& b : model["documents"][0]["bodies"]) fp["bodies"].push_back(b["fingerprint"]);
    }
    model["fingerprint"] = fp;
    // documents in dependency order, as the SolidWorks exporter writes them (a layout before the parts inserting it,
    // parts before the assemblies placing them): every document after the documents it names
    {
        json docs = model["documents"], ordered = json::array();
        std::set<std::string> ids, done;
        for (auto& d : docs) ids.insert(d.value("id", ""));
        std::function<void(const json&, std::set<std::string>&)> names = [&](const json& o, std::set<std::string>& acc) {
            if (o.is_object())
                for (auto it = o.begin(); it != o.end(); ++it)
                {
                    if ((it.key() == "document" || it.key() == "source") && it.value().is_string() && ids.count(it.value().get<std::string>()))
                        acc.insert(it.value().get<std::string>());
                    else
                        names(it.value(), acc);
                }
            else if (o.is_array())
                for (auto& x : o) names(x, acc);
        };
        std::vector<std::set<std::string>> needs;
        for (auto& d : docs)
        {
            std::set<std::string> acc;
            names(d, acc);
            acc.erase(d.value("id", ""));
            needs.push_back(acc);
        }
        while (ordered.size() < docs.size())
        {
            size_t pick = docs.size();
            for (size_t i = 0; i < docs.size() && pick == docs.size(); i++)
            {
                if (done.count(docs[i].value("id", ""))) continue;
                if (std::all_of(needs[i].begin(), needs[i].end(), [&](const std::string& n) { return done.count(n) > 0; })) pick = i;
            }
            if (pick == docs.size())   // a cycle (invalid; the validator says so): the rest in their order
                for (size_t i = 0; i < docs.size() && pick == docs.size(); i++)
                    if (!done.count(docs[i].value("id", ""))) pick = i;
            done.insert(docs[pick].value("id", ""));
            ordered.push_back(docs[pick]);
        }
        model["documents"] = ordered;
    }
    // validate, export the geometry, embed
    std::string mj = model.dump();
    char* rep = nullptr;
    int vs = ncx_model_validate(mj.c_str(), mj.size(), &rep);
    std::string vrep = rep ? rep : "";
    ncx_free(rep);
    progressStep("writing the geometry (Fusion's STEP export)");
    fs::path stepTmp = tempDir() / ("export_" + std::to_string(GetTickCount64()) + ".step");
    Ptr<ExportManager> em = des->exportManager();
    Ptr<STEPExportOptions> so = em->createSTEPExportOptions(stepTmp.u8string(), root);
    if (!so || !em->execute(so)) return json{{"ok", false}, {"error", "Fusion's STEP export failed: " + lastError()}};
    std::string step = readFile(stepTmp);
    std::error_code ec;
    fs::remove(stepTmp, ec);
    progressStep("embedding the NCX payload");
    char* outStep = nullptr;
    size_t outLen = 0;
    char* wrep = nullptr;
    std::string optsJson = json{{"generator", std::string("ncx-fusion ") + kImplVersion + " Fusion " + app->version()}}.dump();
    int ws = ncx_payload_write(step.data(), step.size(), mj.c_str(), mj.size(), optsJson.c_str(), &outStep, &outLen, &wrep);
    std::string wr = wrep ? wrep : "";
    ncx_free(wrep);
    if (ws != NCX_OK)
    {
        ncx_free(outStep);
        return json{{"ok", false}, {"error", "ncx_payload_write: " + statusName(ws) + " " + wr}};
    }
    writeFile(out, std::string(outStep, outLen));
    ncx_free(outStep);
    if (opts.value("dumpModel", false)) writeFile(fs::path(out.wstring() + L".json"), model.dump(1));
    int opaque = 0, mapped = 0;
    for (auto& d : model["documents"])
        for (auto& f : d.value("features", json::array())) (f.value("status", "") == "opaque" ? opaque : mapped)++;
    return json{{"ok", true}, {"file", out.u8string()}, {"validate", statusName(vs)}, {"validateReport", json::parse(vrep.empty() ? "{}" : vrep, nullptr, false)},
                {"mapped", mapped}, {"opaque", opaque}, {"notes", notes}};
}

json exportJob(const json& req)
{
    if (!req.contains("file")) return json{{"ok", false}, {"error", "export needs a file"}};
    return exportFile(fs::u8path(req["file"].get<std::string>()), req);
}

json uiExport(const json& req)
{
    Ptr<UserInterface> ui = Application::get()->userInterface();
    std::string path = req.value("file", "");
    if (path.empty())
    {
        Ptr<FileDialog> dlg = ui->createFileDialog();
        dlg->title("Export NCX");
        dlg->filter("NCX file (*.ncx.step);;STEP file (*.step)");
        if (dlg->showSave() != DialogOK) return json{{"ok", true}, {"cancelled", true}};
        path = dlg->filename();
    }
    json res = exportFile(fs::u8path(path), req);
    if (!req.value("quiet", false))
    {
        if (res.value("status", "") == "cancelled") ui->messageBox("NCX export cancelled; no file was written.", "NCX");
        else if (!res.value("ok", false)) ui->messageBox("NCX export failed: " + res.value("error", "?"), "NCX");
        else
        {
            std::string msg = "NCX export: " + std::to_string(res.value("mapped", 0)) + " features carried";
            if (res.value("opaque", 0) > 0) msg += ", " + std::to_string(res.value("opaque", 0)) + " as fixed geometry (no NCX mapping yet)";
            for (auto& n : res.value("notes", json::array())) msg += "\n- " + n.get<std::string>();
            ui->messageBox(msg, "NCX");
        }
    }
    return res;
}

} // namespace ncxf
