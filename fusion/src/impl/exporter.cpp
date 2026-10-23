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

    std::string fid() { return "f:" + std::to_string(++n); }
    bool extrudeNegative = false;   // the extrude being written went against its profile's normal

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
        V3 nrm = unit(cross(vecOf(sk->xDirection()), vecOf(sk->yDirection())));
        V3 o = ptMm(sk->origin());
        double sum = 0;
        for (size_t k = 0; k < sf->count(); k++) sum += dot(sub(ptMm(sf->item(k)->pointOnFace()), o), nrm);
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
        if (Ptr<ConstructionPlane> cp = p)
        {
            if (cp == comp->xYConstructionPlane()) return json{{"kind", "originPlane"}, {"by", "id"}, {"plane", "front"}};
            if (cp == comp->xZConstructionPlane()) return json{{"kind", "originPlane"}, {"by", "id"}, {"plane", "top"}};
            if (cp == comp->yZConstructionPlane()) return json{{"kind", "originPlane"}, {"by", "id"}, {"plane", "right"}};
            std::string id = idOf(cp);
            if (!id.empty()) return json{{"kind", "feature"}, {"by", "id"}, {"id", id}};
            return json{{"kind", "frame"}};
        }
        if (Ptr<BRepFace> f = p) return descriptor(f);
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
                Ptr<Arc3D> g = a->worldGeometry();
                e["normal"] = vj(g ? unit(vecOf(g->normal())) : V3{0, 0, 1});
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
    json sketchDef(const Ptr<Sketch>& sk, const std::string& id)
    {
        auto& ids = sketchIds[id];
        Ptr<Matrix3D> m = sk->transform();   // sketch -> component space
        V3 o = ptMm(sk->origin()), x = unit(vecOf(sk->xDirection())), y = unit(vecOf(sk->yDirection()));
        json plane = planeRef(sk->referencePlane());
        if (plane.value("kind", "") == "frame")
        {
            // Fusion gives no reference plane for some face sketches: the face lying in the sketch plane
            V3 nz = unit(cross(x, y));
            for (auto& b : solids())
                for (size_t i = 0; i < b->faces()->count(); i++)
                {
                    Ptr<BRepFace> f = b->faces()->item(i);
                    Ptr<Plane> g = f->geometry();
                    if (!g) continue;
                    V3 nn = unit(vecOf(g->normal()));
                    if (std::fabs(std::fabs(dot(nn, nz)) - 1) < 1e-9 && std::fabs(dot(sub(o, ptMm(g->origin())), nn)) < 1e-6)
                    {
                        plane = descriptor(f);
                        notes.pop_back();
                        break;
                    }
                }
        }
        json d{{"plane", plane},
               {"frame", {{"origin", vj(o)}, {"x", vj(x)}, {"z", vj(unit(cross(x, y)))}}},
               {"entities", json::array()}, {"constraints", json::array()}, {"dimensions", json::array()},
               {"constrained", sk->isFullyConstrained() ? "full" : "under"}};
        auto uv = [&](const Ptr<Point3D>& p) { return json::array({cm2mm(p->x()), cm2mm(p->y())}); };
        int np = 0, ns = 0;
        Ptr<SketchPoint> originPt = sk->originPoint();
        auto pointId = [&](const Ptr<SketchPoint>& p) -> std::string {
            if (!p) return "";
            std::string tok = p->entityToken();
            auto it = ids.find(tok);
            if (it != ids.end()) return it->second;
            std::string pid = "p" + std::to_string(++np);
            ids[tok] = pid;
            d["entities"].push_back(json{{"id", pid}, {"type", "point"}, {"p", uv(p->geometry())}});
            return pid;
        };
        Ptr<SketchCurves> cs = sk->sketchCurves();
        for (size_t i = 0; i < cs->count(); i++)
        {
            Ptr<SketchCurve> c = cs->item(i);
            if (c->isReference()) continue;
            std::string sid = "s" + std::to_string(++ns);
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
            pointId(p);
        }
        auto ref = [&](const Ptr<Base>& b) -> json {
            if (Ptr<SketchPoint> p = b)
            {
                if (p == originPt) return json{{"kind", "origin"}, {"by", "id"}};
                if (p->isReference()) return nullptr;
            }
            if (Ptr<SketchEntity> se = b)
            {
                auto it = ids.find(se->entityToken());
                if (it != ids.end()) return it->second;
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
                notes.push_back(sk->name() + ": a " + t + " relation to projected geometry is not exported");
                continue;
            }
            d["constraints"].push_back(json{{"type", t}, {"entities", ej}});
        }
        for (size_t i = 0; i < pts->count(); i++)
            if (pts->item(i)->isFixed())
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
    json profileRef(const Ptr<Base>& prof, json& contours)
    {
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
        Ptr<Profile> p = prof;
        if (!p)
            if (Ptr<ObjectCollection> oc = prof)
                if (oc->count() > 0) p = oc->item(0);
        if (!p) return nullptr;
        Ptr<Sketch> sk = p->parentSketch();
        std::string sid = idOf(sk);
        if (sid.empty()) return nullptr;
        // selected regions when the feature uses fewer than the odd-even set: write their outer loops as contours
        size_t used = 1;
        if (Ptr<ObjectCollection> oc = prof) used = oc->count();
        if (used != sk->profiles()->count())
        {
            auto& ids = sketchIds[sid];
            std::vector<Ptr<Profile>> ps;
            if (Ptr<ObjectCollection> oc = prof)
                for (size_t i = 0; i < oc->count(); i++) ps.push_back(oc->item(i));
            else
                ps.push_back(p);
            for (auto& q : ps)
            {
                json c = json::array();
                Ptr<ProfileLoops> ls = q->profileLoops();
                for (size_t i = 0; i < ls->count(); i++)
                    if (ls->item(i)->isOuter())
                        for (size_t k = 0; k < ls->item(i)->profileCurves()->count(); k++)
                        {
                            Ptr<SketchEntity> se = ls->item(i)->profileCurves()->item(k)->sketchEntity();

                            auto it = se ? ids.find(se->entityToken()) : ids.end();
                            if (it != ids.end()) c.push_back(it->second);
                        }
                contours.push_back(c);
            }
        }
        return json{{"kind", "feature"}, {"by", "id"}, {"id", sid}};
    }

    json extentJson(const Ptr<ExtentDefinition>& x)
    {
        if (Ptr<DistanceExtentDefinition> d = x) return json{{"type", "blind"}, {"depth", cm2mm(d->distance()->value())}};
        if (Ptr<ThroughAllExtentDefinition> t = x) return json{{"type", "throughAll"}};
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
            if (!contours.empty()) d["contours"] = contours;
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
                why = "a thin extrude is not exported yet";
                return nullptr;
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
            type = "fillet";
            Ptr<FilletEdgeSets> sets = f->edgeSets();
            if (!sets || sets->count() != 1)
            {
                why = "a fillet with several edge sets is not carried yet";
                return nullptr;
            }
            Ptr<ConstantRadiusFilletEdgeSet> cr = sets->item(0);
            if (!cr)
            {
                why = "only constant-radius fillets are carried";
                return nullptr;
            }
            json d{{"filletType", "constant"}, {"radius", cm2mm(cr->radius()->value())}, {"edges", json::array()}, {"propagate", false}};
            for (size_t i = 0; i < cr->edges()->count(); i++) d["edges"].push_back(descriptor(cr->edges()->item(i)));
            return d;
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

    void walk()
    {
        Ptr<Timeline> tl = des->timeline();
        std::vector<Ptr<TimelineObject>> items;
        for (size_t i = 0; i < tl->count(); i++) items.push_back(tl->item(i));
        for (auto& t : items)
        {
            Ptr<Base> e = t->entity();
            if (!e) continue;
            // only this component's features
            Ptr<Component> owner;
            if (Ptr<Feature> f = e) owner = f->parentComponent();
            else if (Ptr<Sketch> s = e) owner = s->parentComponent();
            else if (Ptr<ConstructionPlane> p = e) owner = p->component();
            else continue;   // occurrences, joints: the assembly's business
            if (owner != comp) continue;
            std::string id = fid();
            std::string name = t->name();
            if (Ptr<Sketch> s = e) name = s->name();
            else if (Ptr<Feature> f = e) name = f->name();
            idOfToken[Ptr<Feature>(e) ? Ptr<Feature>(e)->entityToken() : Ptr<Sketch>(e) ? Ptr<Sketch>(e)->entityToken() : Ptr<ConstructionPlane>(e)->entityToken()] = id;
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
                def = featureDef(e, type, why);
            tl->moveToEnd();
            if (!def.is_null())
            {
                nf["type"] = type;
                nf["status"] = "mapped";
                nf["def"] = def;
                if (Ptr<Feature> ft = e) featureOfId[id] = ft;
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
        return d;
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
struct MateWriter
{
    std::map<std::string, std::string> compIdOfOcc;   // occurrence full path name -> component id
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

    std::string idOf(const Ptr<Occurrence>& o)
    {
        if (!o) return "";
        auto it = compIdOfOcc.find(o->fullPathName());
        return it == compIdOfOcc.end() ? "?" : it->second;
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
        Ptr<BRepBodies> bs = o->bRepBodies();
        for (size_t i = 0; bs && i < bs->count(); i++)
            for (size_t k = 0; k < bs->item(i)->faces()->count(); k++)
            {
                Ptr<BRepFace> f = bs->item(i)->faces()->item(k);
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
        Ptr<BRepBodies> bs = o->bRepBodies();
        for (size_t i = 0; bs && i < bs->count(); i++)
            for (size_t k = 0; k < bs->item(i)->faces()->count(); k++)
            {
                Ptr<BRepFace> f = bs->item(i)->faces()->item(k);
                Ptr<Cylinder> g = f->geometry();
                if (g) v.push_back({f, ptMm(g->origin()), unit(vecOf(g->axis()))});
            }
        return v;
    }

    // the reference of one side: a face descriptor in its component's own frame, or an assembly origin plane
    json side(const Ptr<Occurrence>& o, const Ptr<BRepFace>& face, const std::string& originPlane)
    {
        if (!o) return json{{"ref", {{"kind", "originPlane"}, {"by", "id"}, {"plane", originPlane}}}};
        setMeasureContext(o);
        json d = descriptor(face->nativeObject() ? face->nativeObject() : face);
        setMeasureContext(nullptr);
        return json{{"component", idOf(o)}, {"ref", d}};
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

    void joint(const std::string& name, Ptr<Occurrence> a, Ptr<Occurrence> b, const Ptr<JointMotion>& motion, const V3& origin)
    {
        if (!a && b) std::swap(a, b);
        if (!a)
        {
            add(json{{"type", "other"}, {"refs", json::array()}}, name, "opaque", "a joint between no components");
            return;
        }
        if (idOf(a) == "?" || (b && idOf(b) == "?"))
        {
            add(json{{"type", "other"}, {"refs", json::array()}}, name, "opaque", "a joint to a component that is not exported (a sub-assembly)");
            return;
        }
        JointTypes t = motion->jointType();
        {
            JointInfo ji{t, a, b, origin, {0, 0, 1}};
            if (Ptr<RevoluteJointMotion> r = motion) ji.axis = unit(vecOf(r->rotationAxisVector()));
            if (Ptr<CylindricalJointMotion> c = motion) ji.axis = unit(vecOf(c->rotationAxisVector()));
            if (Ptr<SliderJointMotion> sl = motion) ji.axis = unit(vecOf(sl->slideDirectionVector()));
            info[name] = ji;
        }
        auto lockTo = [&]() {
            json refs = json::array({json{{"component", idOf(a)}, {"ref", {{"kind", "originPlane"}, {"by", "id"}, {"plane", "front"}}}}});
            refs.push_back(b ? json{{"component", idOf(b)}, {"ref", {{"kind", "originPlane"}, {"by", "id"}, {"plane", "front"}}}}
                             : json{{"ref", {{"kind", "originPlane"}, {"by", "id"}, {"plane", "front"}}}});
            return json{{"type", "lock"}, {"refs", refs}};
        };
        if (t == RigidJointType)
        {
            add(lockTo(), name);
            return;
        }
        if (t == RevoluteJointType || t == CylindricalJointType)
        {
            V3 axis{0, 0, 1};
            if (Ptr<RevoluteJointMotion> r = motion) axis = unit(vecOf(r->rotationAxisVector()));
            if (Ptr<CylindricalJointMotion> c = motion) axis = unit(vecOf(c->rotationAxisVector()));
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
                if (cc.empty())
                    notes.push_back("joint " + name + ": no faces meet across its axis: written as a concentric mate only (it may also slide)");
                else
                    add(cc[0], name + " (face)");
            }
            return;
        }
        if (t == SliderJointType)
        {
            Ptr<SliderJointMotion> s = motion;
            V3 d = unit(vecOf(s->slideDirectionVector()));
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
            Ptr<PlanarJointMotion> p = motion;
            auto cc = coincidentPairs(a, b, unit(vecOf(p->normalDirectionVector())), false, 1);
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

json exportFile(const fs::path& out, const json& opts)
{
    Ptr<Application> app = Application::get();
    Ptr<Design> des = app->activeProduct();
    if (!des) return json{{"ok", false}, {"error", "the active document is not a design"}};
    if (des->designType() != ParametricDesignType) return json{{"ok", false}, {"error", "only parametric designs carry a history to export"}};
    Ptr<Component> root = des->rootComponent();
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
    json notes = json::array();
    // the component definitions: every component with bodies or features is a part document
    std::vector<Ptr<Component>> parts;
    std::map<std::string, std::string> docOf;   // component entity token -> document id
    Ptr<OccurrenceList> occs = root->allOccurrences();
    for (size_t i = 0; occs && i < occs->count(); i++)
    {
        Ptr<Component> c = occs->item(i)->component();
        if (docOf.count(c->entityToken())) continue;
        if (c->occurrences()->count() > 0)
        {
            notes.push_back("sub-assembly " + c->name() + ": nested assemblies are exported flattened");
        }
        docOf[c->entityToken()] = "doc:" + sanitizeId(c->name());
        parts.push_back(c);
    }
    bool assembly = !parts.empty();
    if (!assembly) parts.push_back(root);
    json userParams = json::array();
    for (size_t i = 0; i < des->userParameters()->count(); i++)
    {
        Ptr<UserParameter> p = des->userParameters()->item(i);
        userParams.push_back(json{{"name", p->name()}, {"value", p->unit().empty() ? p->value() : (p->unit() == "deg" ? rad2deg(p->value()) : cm2mm(p->value()))},
                                  {"unit", p->unit().empty() ? "none" : (p->unit() == "deg" ? "deg" : "mm")}, {"expression", p->expression()}});
    }
    for (size_t pi = 0; pi < parts.size(); pi++)
    {
        DocWriter w;
        w.des = des;
        w.comp = parts[pi];
        Ptr<Occurrence> ctx;
        for (size_t i = 0; occs && i < occs->count() && !ctx; i++)
            if (occs->item(i)->component() == parts[pi] && !occs->item(i)->assemblyContext()) ctx = occs->item(i);
        setMeasureContext(ctx);
        w.docName = parts[pi] == root ? rootName : parts[pi]->name();
        w.docId = parts[pi] == root ? "doc:" + sanitizeId(w.docName) : docOf[parts[pi]->entityToken()];
        w.walk();
        json d = w.document();
        if (pi == 0) d["parameters"] = userParams;   // design-wide in Fusion: written with the first part
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
                        // neutral grammar: parameter names quoted, units dropped ("W_ * 1 mm" -> "W_")
                        std::string neutral;
                        std::istringstream is(ex);
                        std::string tok, outEx;
                        size_t pos = ex.find(" * 1 ");
                        std::string core = pos != std::string::npos ? ex.substr(0, pos) : ex;
                        if (!core.empty() && core.front() == '(' && core.back() == ')') core = core.substr(1, core.size() - 2);
                        for (size_t k = 0; k < core.size();)
                        {
                            if (std::isalpha(static_cast<unsigned char>(core[k])) || core[k] == '_')
                            {
                                size_t j = k;
                                while (j < core.size() && (std::isalnum(static_cast<unsigned char>(core[j])) || core[j] == '_')) j++;
                                std::string w2 = core.substr(k, j - k);
                                static const std::set<std::string> fns{"sin", "cos", "tan", "sqrt", "abs", "min", "max", "mm", "cm", "deg", "rad"};
                                outEx += fns.count(w2) ? w2 : "\"" + w2 + "\"";
                                k = j;
                            }
                            else
                                outEx += core[k++];
                        }
                        d["equations"].push_back(json{{"target", dm.value("fullName", "")}, {"expression", outEx}, {"native", ex}, {"status", "mapped"}});
                    }
        setMeasureContext(nullptr);
        for (auto& n : w.notes) notes.push_back(n);
        model["documents"].push_back(d);
    }
    // the assembly
    json fp = json{{"bodies", json::array()}};
    if (assembly)
    {
        json a{{"id", "doc:" + sanitizeId(rootName)}, {"kind", "assembly"}, {"name", rootName}, {"sourceFile", rootName + ".f3d"},
               {"configurations", {{{"id", "cfg:Default"}, {"name", "Default"}}}}, {"activeConfiguration", "cfg:Default"},
               {"features", json::array()}, {"components", json::array()}, {"mates", json::array()}};
        int k = 0;
        for (size_t i = 0; i < occs->count(); i++)
        {
            Ptr<Occurrence> o = occs->item(i);
            if (!o->isLightBulbOn() || o->childOccurrences()->count() > 0) continue;
            a["components"].push_back(json{{"id", "c:" + sanitizeId(o->name())}, {"document", docOf[o->component()->entityToken()]},
                                           {"configuration", "Default"}, {"name", o->name()}, {"transform", rowMajorMm(o->transform2())},
                                           {"fixed", o->isGrounded()}});
            for (size_t b = 0; b < o->component()->bRepBodies()->count(); b++)
                if (o->component()->bRepBodies()->item(b)->isSolid())
                    fp["bodies"].push_back(bodyFingerprint(o->component()->bRepBodies()->item(b), o->transform2()));
            k++;
        }
        // joints -> mates
        std::vector<std::string> mnotes;
        MateWriter mw(mnotes);
        for (size_t i = 0; i < occs->count(); i++)
            mw.compIdOfOcc[occs->item(i)->fullPathName()] = "c:" + sanitizeId(occs->item(i)->name());
        for (size_t i = 0; i < root->joints()->count(); i++)
        {
            Ptr<Joint> j = root->joints()->item(i);
            if (j->isSuppressed()) continue;
            V3 origin{0, 0, 0};
            Ptr<Base> g1 = j->geometryOrOriginOne();
            if (Ptr<JointGeometry> g = g1) origin = ptMm(g->origin());
            else if (Ptr<JointOrigin> jo = g1) origin = ptMm(jo->geometry()->origin());
            mw.joint(j->name(), j->occurrenceOne(), j->occurrenceTwo(), j->jointMotion(), origin);
        }
        for (size_t i = 0; i < root->asBuiltJoints()->count(); i++)
        {
            Ptr<AsBuiltJoint> j = root->asBuiltJoints()->item(i);
            if (j->isSuppressed()) continue;
            V3 origin = j->geometry() ? ptMm(j->geometry()->origin()) : V3{0, 0, 0};
            mw.joint(j->name(), j->occurrenceOne(), j->occurrenceTwo(), j->jointMotion(), origin);
        }
        for (size_t i = 0; i < root->motionLinks()->count(); i++)
        {
            Ptr<MotionLink> ml = root->motionLinks()->item(i);
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
        a["mates"] = mw.mates;
        for (auto& m : mnotes) notes.push_back(m);
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
    // validate, export the geometry, embed
    std::string mj = model.dump();
    char* rep = nullptr;
    int vs = ncx_model_validate(mj.c_str(), mj.size(), &rep);
    std::string vrep = rep ? rep : "";
    ncx_free(rep);
    fs::path stepTmp = tempDir() / ("export_" + std::to_string(GetTickCount64()) + ".step");
    Ptr<ExportManager> em = des->exportManager();
    Ptr<STEPExportOptions> so = em->createSTEPExportOptions(stepTmp.u8string(), root);
    if (!so || !em->execute(so)) return json{{"ok", false}, {"error", "Fusion's STEP export failed: " + lastError()}};
    std::string step = readFile(stepTmp);
    std::error_code ec;
    fs::remove(stepTmp, ec);
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
        if (!res.value("ok", false)) ui->messageBox("NCX export failed: " + res.value("error", "?"), "NCX");
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
