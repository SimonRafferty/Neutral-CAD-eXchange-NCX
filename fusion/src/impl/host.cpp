#include "host.h"

#include <algorithm>
#include <limits>

namespace ncxf {

namespace {

const std::vector<double> kEdgeFractions = {0.1, 0.3, 0.5, 0.7, 0.9};
const double kWeakClearanceMm = 0.01;

Ptr<MeasureManager> measure() { return Application::get()->measureManager(); }

Ptr<Occurrence>& measureContext()
{
    static Ptr<Occurrence> occ;
    return occ;
}

// a native B-rep entity of the context occurrence's component, as a proxy in that occurrence
Ptr<Base> inContext(const Ptr<Base>& e, Ptr<Matrix3D>& xf)
{
    Ptr<Occurrence> occ = measureContext();
    xf = nullptr;
    if (!occ || !occ->isValid()) return e;
    Ptr<Component> c = occ->component();
    if (Ptr<BRepFace> f = e)
    {
        if (f->assemblyContext() || f->body()->parentComponent() != c) return e;
        xf = occ->transform2();
        return f->createForAssemblyContext(occ);
    }
    if (Ptr<BRepEdge> ed = e)
    {
        if (ed->assemblyContext() || ed->body()->parentComponent() != c) return e;
        xf = occ->transform2();
        return ed->createForAssemblyContext(occ);
    }
    return e;
}

json vjson(const V3& v) { return json::array({v[0], v[1], v[2]}); }

std::array<double, 6> boxOf(const Ptr<BoundingBox3D>& bb)
{
    if (!bb) return {0, 0, 0, 0, 0, 0};
    return {cm2mm(bb->minPoint()->x()), cm2mm(bb->minPoint()->y()), cm2mm(bb->minPoint()->z()),
            cm2mm(bb->maxPoint()->x()), cm2mm(bb->maxPoint()->y()), cm2mm(bb->maxPoint()->z())};
}

std::array<double, 6> transformBox(const std::array<double, 6>& b, const Ptr<Matrix3D>& xf)
{
    std::array<double, 6> r{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
    for (int i = 0; i < 8; i++)
    {
        V3 p{(i & 1) ? b[3] : b[0], (i & 2) ? b[4] : b[1], (i & 4) ? b[5] : b[2]};
        V3 q = applyMm(xf, p);
        for (int k = 0; k < 3; k++)
        {
            r[k] = std::min(r[k], q[k]);
            r[k + 3] = std::max(r[k + 3], q[k]);
        }
    }
    return r;
}

// Closest point on a B-rep entity (face/edge) to a point, both mm.
bool closest(const Ptr<Base>& entity, const V3& pMm, V3& out, double& d)
{
    progressTick();   // the heartbeat of every long geometric loop: Fusion stays responsive, Cancel is seen
    Ptr<Matrix3D> xf;
    Ptr<Base> e = inContext(entity, xf);
    V3 p = xf ? applyMm(xf, pMm) : pMm;
    Ptr<MeasureResults> r = measure()->measureMinimumDistance(e, ptCm(p));
    if (!r) return false;
    d = cm2mm(r->value());
    // positionOne is on geometryOne (the entity)
    Ptr<Point3D> a = r->positionOne();
    if (!a) return false;
    out = ptMm(a);
    if (xf)
    {
        Ptr<Matrix3D> inv = xf->copy();
        inv->invert();
        out = applyMm(inv, out);
    }
    return true;
}

// Interior points of a face from its triangulation (largest facets' centroids, moved onto the face).
std::vector<V3> interiorPoints(const Ptr<BRepFace>& f, size_t limit)
{
    std::vector<V3> pts;
    Ptr<TriangleMeshCalculator> calc = f->meshManager()->createMeshCalculator();
    if (!calc) return pts;
    calc->setQuality(NormalQualityTriangleMesh);
    Ptr<TriangleMesh> mesh = calc->calculate();
    if (!mesh) return pts;
    std::vector<double> c = mesh->nodeCoordinatesAsDouble();   // cm
    std::vector<int> idx = mesh->nodeIndices();
    std::vector<std::pair<double, V3>> cand;
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
    {
        V3 a{c[3 * idx[i]], c[3 * idx[i] + 1], c[3 * idx[i] + 2]};
        V3 b{c[3 * idx[i + 1]], c[3 * idx[i + 1] + 1], c[3 * idx[i + 1] + 2]};
        V3 d{c[3 * idx[i + 2]], c[3 * idx[i + 2] + 1], c[3 * idx[i + 2] + 2]};
        double area = 0.5 * norm(cross(sub(b, a), sub(d, a)));
        V3 m = mul(add(add(a, b), d), 1.0 / 3.0);
        cand.push_back({area, mul(m, 10.0)});   // to mm
    }
    std::stable_sort(cand.begin(), cand.end(), [](auto& x, auto& y) { return x.first > y.first; });
    // a planar face's facets lie on it: their centroids need no projection (one measurement each otherwise)
    bool planar = Ptr<Plane>(f->geometry()) != nullptr;
    for (size_t i = 0; i < cand.size() && pts.size() < limit; i++)
    {
        if (planar)
        {
            pts.push_back(cand[i].second);
            continue;
        }
        V3 q;
        double d;
        if (closest(f, cand[i].second, q, d)) pts.push_back(q);
    }
    return pts;
}

namespace {
// An edge as a polyline (mm, the edge's own frame), for cheap distance estimates.
std::vector<V3> edgePolyline(const Ptr<BRepEdge>& e, int n)
{
    std::vector<V3> res;
    Ptr<CurveEvaluator3D> ev = e->evaluator();
    double s = 0, t = 0;
    if (!ev || !ev->getParameterExtents(s, t)) return res;
    for (int k = 0; k <= n; k++)
    {
        Ptr<Point3D> p;
        if (ev->getPointAtParameter(s + (t - s) * k / n, p) && p) res.push_back(ptMm(p));
    }
    return res;
}

double polylineDistance(const std::vector<V3>& pl, const V3& p)
{
    double best = 1e300;
    if (pl.size() == 1) return dist(pl[0], p);
    for (size_t i = 1; i < pl.size(); i++)
    {
        V3 a = pl[i - 1], d = sub(pl[i], a);
        double l2 = dot(d, d);
        double u = l2 > 0 ? std::clamp(dot(sub(p, a), d) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, dist(add(a, mul(d, u)), p));
    }
    return best;
}
} // namespace

std::vector<V3> faceSamples(const Ptr<BRepFace>& f, size_t k, double& clearance)
{
    std::vector<V3> inside = interiorPoints(f, 60);
    clearance = 0;
    if (inside.empty()) return inside;
    std::vector<Ptr<BRepEdge>> edges;
    for (size_t i = 0; i < f->edges()->count(); i++) edges.push_back(f->edges()->item(i));
    // The distance to the boundary of every candidate, estimated on polylines of the edges (an exact measurement
    // per candidate and edge took seconds on faces with many edges), then measured exactly for the chosen point
    // against the edges nearest to it.
    std::vector<std::vector<V3>> polys;
    for (auto& e : edges) polys.push_back(edgePolyline(e, 32));
    auto estimate = [&](const V3& p, std::vector<std::pair<double, size_t>>* order) {
        if (edges.empty()) return 1.0;
        double m = 1e300;
        for (size_t i = 0; i < polys.size(); i++)
        {
            double d = polys[i].empty() ? distanceMm(edges[i], p) : polylineDistance(polys[i], p);
            if (order) order->push_back({d, i});
            m = std::min(m, d);
        }
        return m;
    };
    std::vector<std::pair<double, V3>> scored;
    for (auto& p : inside) scored.push_back({estimate(p, nullptr), p});
    std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    clearance = scored[0].first;
    if (!edges.empty())
    {
        std::vector<std::pair<double, size_t>> order;
        estimate(scored[0].second, &order);
        std::sort(order.begin(), order.end());
        double m = 1e300;
        for (size_t i = 0; i < order.size() && i < 4; i++) m = std::min(m, distanceMm(edges[order[i].second], scored[0].second));
        clearance = std::min(m, clearance);
    }
    std::vector<V3> chosen{scored[0].second};
    std::vector<V3> pool;
    for (size_t i = 0; i < std::max<size_t>(1, scored.size() / 2); i++) pool.push_back(scored[i].second);
    while (chosen.size() < k && chosen.size() < pool.size())
    {
        double best = -1;
        size_t bi = 0;
        for (size_t i = 0; i < pool.size(); i++)
        {
            double m = 1e300;
            for (auto& q : chosen) m = std::min(m, dist(pool[i], q));
            if (m > best)
            {
                best = m;
                bi = i;
            }
        }
        if (best <= 0) break;
        chosen.push_back(pool[bi]);
    }
    return chosen;
}

std::vector<V3> ownSamples(const Ptr<Base>& e)
{
    if (Ptr<BRepEdge> ed = e) return edgePoints(ed, {0.25, 0.5, 0.75});
    if (Ptr<BRepFace> f = e) return interiorPoints(f, 3);
    return {};
}

double coneErr(const V3& x, const V3& apex, const V3& u, double ang)
{
    V3 v = sub(x, apex);
    double h = dot(v, u), rho = std::sqrt(std::max(0.0, dot(v, v) - h * h));
    return std::min(std::fabs(rho * std::cos(ang) - h * std::sin(ang)), std::fabs(rho * std::cos(ang) + h * std::sin(ang)));
}

json pointsJson(const std::vector<V3>& pts)
{
    json a = json::array();
    for (auto& p : pts) a.push_back(vjson(p));
    return a;
}

} // namespace

void setMeasureContext(const Ptr<Occurrence>& occ) { measureContext() = occ; }

// ------------------------------------------------------------------------------------------- matrices
Ptr<Matrix3D> matrixFromRowMajorMm(const json& m)
{
    Ptr<Matrix3D> x = Matrix3D::create();
    std::vector<double> a(16);
    for (int i = 0; i < 16; i++) a[i] = m.at(i).get<double>();
    for (int r = 0; r < 3; r++) a[r * 4 + 3] = mm2cm(a[r * 4 + 3]);
    x->setWithArray(a);
    return x;
}

json rowMajorMm(const Ptr<Matrix3D>& m)
{
    std::vector<double> a = m->asArray();
    for (int r = 0; r < 3; r++) a[r * 4 + 3] = cm2mm(a[r * 4 + 3]);
    return a;
}

V3 applyMm(const Ptr<Matrix3D>& m, const V3& p)
{
    std::vector<double> a = m->asArray();
    return {a[0] * p[0] + a[1] * p[1] + a[2] * p[2] + cm2mm(a[3]), a[4] * p[0] + a[5] * p[1] + a[6] * p[2] + cm2mm(a[7]),
            a[8] * p[0] + a[9] * p[1] + a[10] * p[2] + cm2mm(a[11])};
}

// ------------------------------------------------------------------------------------------- fingerprint
// A material with a density, for bodies that have none: Fusion reports a zero centre of mass and zero
// moments for a body without material (every body its STEP reader makes), so one is assigned before measuring.
Ptr<Material> anyMaterial()
{
    static Ptr<Material> cached;
    if (cached) return cached;
    Ptr<MaterialLibraries> libs = Application::get()->materialLibraries();
    for (size_t i = 0; libs && i < libs->count() && !cached; i++)
    {
        Ptr<Materials> ms = libs->item(i)->materials();
        if (Ptr<Material> m = ms->itemByName("Steel")) cached = m;
        for (size_t k = 0; !cached && k < ms->count() && k < 50; k++) cached = ms->item(k);
    }
    return cached;
}

json bodyFingerprint(const Ptr<BRepBody>& b, const Ptr<Matrix3D>& xf)
{
    Ptr<PhysicalProperties> pp = b->getPhysicalProperties(VeryHighCalculationAccuracy);
    if (!(pp->density() > 0) && !b->isTemporary())
    {
        if (Ptr<Material> m = anyMaterial())
        {
            b->material(m);
            pp = b->getPhysicalProperties(VeryHighCalculationAccuracy);
        }
    }
    double vol = pp->volume(), area = pp->area();
    double rho = pp->density();
    if (!(rho > 0) && vol > 0) rho = pp->mass() / vol;
    double i1 = 0, i2 = 0, i3 = 0;
    pp->getPrincipalMomentsOfInertia(i1, i2, i3);
    // kg cm^2 at the material's density -> cm^5 at density 1 -> mm^5
    std::vector<double> mom{i1, i2, i3};
    for (auto& m : mom) m = rho > 0 ? m / rho * 1e5 : 0.0;
    std::sort(mom.begin(), mom.end());
    V3 c = ptMm(pp->centerOfMass());
    std::array<double, 6> box = boxOf(b->preciseBoundingBox());
    if (xf)
    {
        c = applyMm(xf, c);
        box = transformBox(box, xf);
    }
    for (auto& v : c)
        if (std::fabs(v) < 5e-12) v = 0.0;
    json fp{{"volume", cm2mm(cm2mm(cm2mm(vol)))},
            {"area", cm2mm(cm2mm(area))},
            {"centroid", vjson(c)},
            {"moments", mom},
            {"bbox", box},
            {"faces", b->faces()->count()},
            {"edges", b->edges()->count()}};
    // FORMAT-SPEC 6: a body with a face that is not a plane, cylinder, cone, sphere or torus is freeform; the
    // exchange profile compares such a body with the looser freeform tolerances (whichever side marks it)
    if (isFreeform(b)) fp["freeform"] = true;
    return fp;
}

bool isFreeform(const Ptr<BRepBody>& b)
{
    Ptr<BRepFaces> fs = b->faces();
    for (size_t i = 0; fs && i < fs->count(); i++)
    {
        Ptr<Surface> s = fs->item(i)->geometry();
        if (!s) continue;
        SurfaceTypes t = s->surfaceType();
        if (t != PlaneSurfaceType && t != CylinderSurfaceType && t != ConeSurfaceType && t != SphereSurfaceType && t != TorusSurfaceType)
            return true;
    }
    return false;
}

json fingerprintOf(const std::vector<Ptr<BRepBody>>& bodies)
{
    json a = json::array();
    for (auto& b : bodies) a.push_back(bodyFingerprint(b));
    return json{{"bodies", a}};
}

// ------------------------------------------------------------------------------------------- geometry facts
std::string surfaceInfo(const Ptr<BRepFace>& f, json& p)
{
    p = json::object();
    Ptr<Surface> s = f->geometry();
    if (Ptr<Plane> pl = s)
    {
        p["normal"] = vjson(unit(vecOf(pl->normal())));
        p["point"] = vjson(ptMm(pl->origin()));
        return "plane";
    }
    if (Ptr<Cylinder> cy = s)
    {
        p["axis"] = vjson(unit(vecOf(cy->axis())));
        p["origin"] = vjson(ptMm(cy->origin()));
        p["radius"] = cm2mm(cy->radius());
        return "cylinder";
    }
    if (Ptr<Cone> co = s)
    {
        V3 o = ptMm(co->origin()), u = unit(vecOf(co->axis()));
        double r = cm2mm(co->radius()), ang = co->halfAngle();
        double k = r / std::tan(ang);
        V3 ap1 = sub(o, mul(u, k)), ap2 = add(o, mul(u, k));
        std::vector<V3> qs = interiorPoints(f, 3);
        double e1 = 0, e2 = 0;
        for (auto& q : qs)
        {
            e1 += coneErr(q, ap1, u, ang);
            e2 += coneErr(q, ap2, u, ang);
        }
        p["axis"] = vjson(u);
        p["apex"] = vjson(e1 <= e2 ? ap1 : ap2);
        p["semiAngle"] = ang;   // radians, as the core's matcher reads it
        return "cone";
    }
    if (Ptr<Sphere> sp = s)
    {
        p["center"] = vjson(ptMm(sp->origin()));
        p["radius"] = cm2mm(sp->radius());
        return "sphere";
    }
    if (Ptr<Torus> to = s)
    {
        p["center"] = vjson(ptMm(to->origin()));
        p["axis"] = vjson(unit(vecOf(to->axis())));
        p["majorRadius"] = cm2mm(to->majorRadius());
        p["minorRadius"] = cm2mm(to->minorRadius());
        return "torus";
    }
    return "bspline";
}

std::string curveInfo(const Ptr<BRepEdge>& e, json& p)
{
    p = json::object();
    Ptr<Curve3D> c = e->geometry();
    if (Ptr<Line3D> l = c)
    {
        V3 a = ptMm(l->startPoint()), b = ptMm(l->endPoint());
        p["direction"] = vjson(unit(sub(b, a)));
        p["point"] = vjson(a);
        return "line";
    }
    if (Ptr<Arc3D> a = c)
    {
        p["center"] = vjson(ptMm(a->center()));
        p["axis"] = vjson(unit(vecOf(a->normal())));
        p["radius"] = cm2mm(a->radius());
        return "circle";
    }
    if (Ptr<Circle3D> ci = c)
    {
        p["center"] = vjson(ptMm(ci->center()));
        p["axis"] = vjson(unit(vecOf(ci->normal())));
        p["radius"] = cm2mm(ci->radius());
        return "circle";
    }
    if (Ptr<Ellipse3D>(c) || Ptr<EllipticalArc3D>(c)) return "ellipse";
    return "bspline";
}

// A body's fingerprint for a result. A surface body has no volume or mass: its centroid is the area-weighted
// centre of its faces and its moments are zero (Fusion's mass properties of a surface give neither).
json resultFingerprint(const Ptr<BRepBody>& b)
{
    json fp = bodyFingerprint(b);
    if (b->isSolid()) return fp;
    double a = 0;
    V3 c{0, 0, 0};
    for (size_t i = 0; i < b->faces()->count(); i++)
    {
        Ptr<BRepFace> f = b->faces()->item(i);
        double fa = f->area();
        a += fa;
        c = add(c, mul(ptMm(f->centroid()), fa));
    }
    if (a > 0) c = mul(c, 1.0 / a);
    for (auto& v : c)
        if (std::fabs(v) < 5e-12) v = 0.0;
    fp["volume"] = 0.0;
    fp["centroid"] = json::array({c[0], c[1], c[2]});
    fp["moments"] = json::array({0.0, 0.0, 0.0});
    return fp;
}

double distanceMm(const Ptr<Base>& entity, const V3& pMm)
{
    V3 q;
    double d;
    if (!closest(entity, pMm, q, d)) return std::numeric_limits<double>::infinity();
    return d;
}

std::vector<V3> edgePoints(const Ptr<BRepEdge>& e, const std::vector<double>& fractions)
{
    std::vector<V3> res;
    Ptr<CurveEvaluator3D> ev = e->evaluator();
    double s = 0, t = 0;
    if (!ev || !ev->getParameterExtents(s, t)) return res;
    for (double f : fractions)
    {
        Ptr<Point3D> p;
        if (ev->getPointAtParameter(s + f * (t - s), p) && p) res.push_back(ptMm(p));
    }
    return res;
}

double edgeLengthMm(const Ptr<BRepEdge>& e) { return cm2mm(e->length()); }

std::array<double, 6> boxMm(const Ptr<Base>& entity)
{
    if (Ptr<BRepFace> f = entity) return boxOf(f->boundingBox());
    if (Ptr<BRepBody> b = entity) return boxOf(b->boundingBox());
    if (Ptr<BRepEdge> e = entity)
    {
        std::vector<double> fr;
        for (int i = 0; i <= 32; i++) fr.push_back(i / 32.0);
        std::array<double, 6> r{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
        for (auto& p : edgePoints(e, fr))
            for (int k = 0; k < 3; k++)
            {
                r[k] = std::min(r[k], p[k]);
                r[k + 3] = std::max(r[k + 3], p[k]);
            }
        return r;
    }
    if (Ptr<BRepBody> b = entity) return boxOf(b->boundingBox());
    return {0, 0, 0, 0, 0, 0};
}

json descriptor(const Ptr<Base>& entity)
{
    json g;
    std::string kind;
    json p;
    if (Ptr<BRepFace> f = entity)
    {
        kind = "face";
        std::string t = surfaceInfo(f, p);
        g["type"] = t;
        g["params"] = p;
        bool analytic = t != "bspline";
        double clearance = 0;
        g["samples"] = pointsJson(faceSamples(f, analytic ? 5 : 9, clearance));
        g["clearance"] = clearance;
        if (clearance < kWeakClearanceMm) g["weak"] = true;
        g["area"] = cm2mm(cm2mm(f->area()));
    }
    else if (Ptr<BRepEdge> e = entity)
    {
        kind = "edge";
        g["type"] = curveInfo(e, p);
        g["params"] = p;
        g["samples"] = pointsJson(edgePoints(e, kEdgeFractions));
        double len = edgeLengthMm(e);
        g["length"] = len;
        Ptr<BRepFaces> fs = e->faces();
        if (fs && fs->count() == 2 && fs->item(0) == fs->item(1)) g["seam"] = true;
        if (len < 1e-9) g["degenerate"] = true;
    }
    g["bbox"] = boxMm(entity);
    return json{{"kind", kind}, {"by", "geom"}, {"geom", g}};
}

// ------------------------------------------------------------------------------------------- resolution
Target::Target(const std::vector<Ptr<BRepBody>>& bs, const std::string& kind) : bodies(bs)
{
    if (kind != "face" && kind != "edge") return;
    for (size_t b = 0; b < bs.size(); b++)
    {
        if (kind == "face")
        {
            Ptr<BRepFaces> fs = bs[b]->faces();
            for (size_t i = 0; i < fs->count(); i++)
            {
                index[kind + ":" + std::to_string(b) + ":" + std::to_string(i)] = ents.size();
                ids.push_back(kind + ":" + std::to_string(b) + ":" + std::to_string(i));
                ents.push_back(fs->item(i));
            }
        }
        else
        {
            Ptr<BRepEdges> es = bs[b]->edges();
            for (size_t i = 0; i < es->count(); i++)
            {
                index[kind + ":" + std::to_string(b) + ":" + std::to_string(i)] = ents.size();
                ids.push_back(kind + ":" + std::to_string(b) + ":" + std::to_string(i));
                ents.push_back(es->item(i));
            }
        }
    }
}

std::vector<std::string> Target::neighbours(size_t i) const
{
    std::vector<std::string> res;
    std::vector<Ptr<Base>> others;
    if (Ptr<BRepFace> f = ents[i])
    {
        Ptr<BRepEdges> es = f->edges();
        for (size_t k = 0; k < es->count(); k++)
        {
            Ptr<BRepFaces> fs = es->item(k)->faces();
            for (size_t j = 0; j < fs->count(); j++) others.push_back(fs->item(j));
        }
    }
    else if (Ptr<BRepEdge> e = ents[i])
    {
        for (Ptr<BRepVertex> v : {e->startVertex(), e->endVertex()})
        {
            if (!v) continue;
            Ptr<BRepEdges> es = v->edges();
            for (size_t j = 0; j < es->count(); j++) others.push_back(es->item(j));
        }
    }
    for (auto& o : others)
        for (size_t j = 0; j < ents.size(); j++)
            if (j != i && ents[j] == o)
            {
                if (std::find(res.begin(), res.end(), ids[j]) == res.end()) res.push_back(ids[j]);
                break;
            }
    return res;
}

namespace {

json resolveVertex(const std::vector<Ptr<BRepBody>>& bodies, const json& ref, std::vector<Ptr<Base>>& matched, double tol0)
{
    V3 want = v3(ref["geom"]["params"]["point"]);
    std::vector<Ptr<BRepVertex>> all;
    for (auto& b : bodies)
        for (size_t i = 0; i < b->vertices()->count(); i++) all.push_back(b->vertices()->item(i));
    for (double tol : {tol0, tol0 * 10})
    {
        std::vector<Ptr<BRepVertex>> hits;
        for (auto& v : all)
            if (dist(ptMm(v->geometry()), want) <= tol) hits.push_back(v);
        if (hits.size() == 1)
        {
            matched.push_back(hits[0]);
            return json{{"result", "resolved"}, {"tolerance", tol}};
        }
        if (hits.size() > 1) return json{{"result", "ambiguous"}, {"notes", {std::to_string(hits.size()) + " vertices coincide"}}};
    }
    return json{{"result", "unresolved"}, {"notes", {"no vertex at the point"}}};
}

json resolveBody(const std::vector<Ptr<BRepBody>>& bodies, const json& ref, std::vector<Ptr<Base>>& matched)
{
    const json& g = ref["geom"];
    double vol = g.value("volume", 0.0);
    V3 c = v3(g["centroid"]);
    std::vector<Ptr<BRepBody>> hits;
    for (auto& b : bodies)
    {
        json fp = bodyFingerprint(b);
        double v = fp["volume"].get<double>();
        // the exchange profile's tolerance: a body rebuilt as a functional equivalent (a drawn weldment section)
        // is that close, not to the native one
        if (std::fabs(v - vol) <= 2e-4 * std::max(std::fabs(vol), 1e-9) && dist(v3(fp["centroid"]), c) <= 0.01) hits.push_back(b);
    }
    if (hits.size() == 1)
    {
        matched.push_back(hits[0]);
        return json{{"result", "resolved"}};
    }
    return json{{"result", hits.empty() ? "unresolved" : "ambiguous"}, {"notes", {std::to_string(hits.size()) + " bodies match"}}};
}

} // namespace

json resolveRef(const std::vector<Ptr<BRepBody>>& bodies, const json& ref, std::vector<Ptr<Base>>& matched, double tolerance)
{
    matched.clear();
    std::string kind = ref.value("kind", "");
    if (kind == "vertex") return resolveVertex(bodies, ref, matched, tolerance);
    if (kind == "body") return resolveBody(bodies, ref, matched);
    Target tgt(bodies, kind);
    const json& geom = ref["geom"];
    std::vector<V3> samples;
    for (auto& s : geom["samples"]) samples.push_back(v3(s));
    std::set<size_t> keep;
    bool hasBox = geom.contains("bbox") && geom["bbox"].size() == 6;
    std::array<double, 6> rb{};
    if (hasBox)
        for (int k = 0; k < 6; k++) rb[k] = geom["bbox"][k].get<double>();
    for (size_t i = 0; i < tgt.ents.size(); i++)
    {
        if (hasBox)
        {
            auto b = boxMm(tgt.ents[i]);
            if (b[3] < rb[0] - 1 || b[0] > rb[3] + 1 || b[4] < rb[1] - 1 || b[1] > rb[4] + 1 || b[5] < rb[2] - 1 || b[2] > rb[5] + 1)
                continue;
        }
        keep.insert(i);
    }
    for (size_t i : std::vector<size_t>(keep.begin(), keep.end()))
        for (auto& n : tgt.neighbours(i)) keep.insert(tgt.index[n]);
    json cands = json::array();
    for (size_t i : keep)
    {
        Ptr<Base> e = tgt.ents[i];
        json p;
        std::string t;
        if (Ptr<BRepFace> f = e) t = surfaceInfo(f, p);
        else t = curveInfo(Ptr<BRepEdge>(e), p);
        json d = json::array();
        for (auto& s : samples) d.push_back(distanceMm(e, s));
        json c{{"id", tgt.ids[i]}, {"type", t}, {"params", p}, {"distances", d}, {"samples", pointsJson(ownSamples(e))},
               {"neighbors", tgt.neighbours(i)}};
        if (Ptr<BRepFace> f = e) c["area"] = cm2mm(cm2mm(f->area()));
        else c["length"] = edgeLengthMm(Ptr<BRepEdge>(e));
        cands.push_back(c);
    }
    json req{{"ref", ref}, {"tolerance", tolerance}, {"candidates", cands}};
    json res = coreMatch(req);
    // developer: keep the request of a reference that did not resolve (when %LOCALAPPDATA%\NcxFusion\dump exists)
    if (res.value("result", "") != "resolved")
    {
        std::error_code ec;
        fs::path dir = dataDir() / L"dump";
        if (fs::is_directory(dir, ec))
        {
            static int k = 0;
            writeFile(dir / ("unresolved_" + std::to_string(++k) + ".json"), json{{"request", req}, {"result", res}}.dump(1));
        }
    }
    if (res.value("result", "") == "resolved")
        for (auto& id : res["ids"]) matched.push_back(tgt.ents[tgt.index[id.get<std::string>()]]);
    return res;
}

// ------------------------------------------------------------------------------------------- face by face
json describeFaces(const Ptr<BRepBody>& b, const std::string& progressMessage)
{
    json faces = json::array();
    size_t n = b->faces()->count();
    for (size_t i = 0; i < n; i++)
    {
        if (!progressMessage.empty()) progressTick(progressMessage + ", face " + std::to_string(i + 1) + " of " + std::to_string(n));
        faces.push_back(descriptor(b->faces()->item(i)));
    }
    return faces;
}

json faceCompare(const json& reference, const std::vector<Ptr<BRepBody>>& rebuilt, double tolerance, const std::string& progressMessage)
{
    Target tgt(rebuilt, "face");
    std::vector<std::array<double, 6>> boxes;
    json info = json::array();
    std::vector<std::vector<std::string>> nb;
    for (size_t i = 0; i < tgt.ents.size(); i++)
    {
        if (!progressMessage.empty())
            progressTick(progressMessage + ": measuring rebuilt face " + std::to_string(i + 1) + " of " + std::to_string(tgt.ents.size()));
        Ptr<BRepFace> f = tgt.ents[i];
        boxes.push_back(boxMm(f));
        json p;
        std::string t = surfaceInfo(f, p);
        nb.push_back(tgt.neighbours(i));
        info.push_back(json{{"id", tgt.ids[i]}, {"type", t}, {"params", p}, {"area", cm2mm(cm2mm(f->area()))},
                            {"samples", pointsJson(ownSamples(f))}, {"neighbors", nb.back()}});
    }
    // pair each reference body with the rebuilt body nearest in volume and centroid
    std::vector<json> rfp;
    for (auto& rb : reference) rfp.push_back(rb["fingerprint"]);
    std::vector<json> tfp;
    for (auto& b : rebuilt) tfp.push_back(bodyFingerprint(b));
    std::vector<int> pairOf(rfp.size(), -1);
    std::set<int> used;
    for (size_t i = 0; i < rfp.size(); i++)
    {
        double best = 1e300;
        int bi = -1;
        for (size_t j = 0; j < tfp.size(); j++)
        {
            if (used.count(static_cast<int>(j))) continue;
            double rv = rfp[i]["volume"].get<double>(), tv = tfp[j]["volume"].get<double>();
            double cost = std::fabs(rv - tv) / std::max(rv, 1e-12) + dist(v3(rfp[i]["centroid"]), v3(tfp[j]["centroid"]));
            if (cost < best)
            {
                best = cost;
                bi = static_cast<int>(j);
            }
        }
        pairOf[i] = bi;
        if (bi >= 0) used.insert(bi);
    }
    json faces = json::array();
    int n = 0;
    for (size_t bi = 0; bi < reference.size(); bi++)
    {
        std::string scope = pairOf[bi] < 0 ? "" : "face:" + std::to_string(pairOf[bi]) + ":";
        for (auto& desc : reference[bi]["faces"])
        {
            if (!progressMessage.empty()) progressTick(progressMessage + ": comparing face " + std::to_string(n + 1));
            const json& geom = desc["geom"];
            std::array<double, 6> rb{};
            for (int k = 0; k < 6; k++) rb[k] = geom["bbox"][k].get<double>();
            std::set<size_t> keep;
            for (size_t i = 0; i < boxes.size(); i++)
            {
                if (!scope.empty() && tgt.ids[i].rfind(scope, 0) != 0) continue;
                auto& b = boxes[i];
                if (b[3] < rb[0] - 1 || b[0] > rb[3] + 1 || b[4] < rb[1] - 1 || b[1] > rb[4] + 1 || b[5] < rb[2] - 1 || b[2] > rb[5] + 1)
                    continue;
                keep.insert(i);
            }
            for (size_t i : std::vector<size_t>(keep.begin(), keep.end()))
                for (auto& id : nb[i])
                    if (scope.empty() || id.rfind(scope, 0) == 0) keep.insert(tgt.index[id]);
            json cands = json::array();
            for (size_t i : keep)
            {
                json c = info[i];
                json d = json::array();
                for (auto& s : geom["samples"]) d.push_back(distanceMm(tgt.ents[i], v3(s)));
                c["distances"] = d;
                cands.push_back(c);
            }
            faces.push_back(json{{"id", "ref:" + std::to_string(n++)}, {"ref", desc}, {"candidates", cands}});
        }
    }
    json targets = json::array();
    for (auto& x : info) targets.push_back(json{{"id", x["id"]}, {"area", x["area"]}});
    if (!progressMessage.empty()) progressTick(progressMessage + ": matching " + std::to_string(n) + " faces");
    return coreFaceCompare(json{{"tolerance", tolerance}, {"faces", faces}, {"targets", targets}});
}

} // namespace ncxf
