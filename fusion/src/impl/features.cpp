// Replay: solid features (extrude/cut, revolve, fillet, chamfer) and reference geometry.
#include "host.h"
#include "neutral.h"
#include "replay.h"

#include <algorithm>

namespace ncxf {

namespace {

Ptr<ValueInput> mmValue(double mm) { return ValueInput::createByReal(mm2cm(mm)); }
Ptr<ValueInput> degValue(double deg) { return ValueInput::createByReal(deg2rad(deg)); }

// A closed loop of a profile as a polygon in sketch space (cm).
std::vector<std::array<double, 2>> loopPolygon(const Ptr<Sketch>& sk, const Ptr<ProfileLoop>& loop)
{
    std::vector<std::array<double, 2>> poly, raw;
    bool offPlane = false;
    Ptr<ProfileCurves> pcs = loop->profileCurves();
    for (size_t i = 0; i < pcs->count(); i++)
    {
        Ptr<Curve3D> g = pcs->item(i)->geometry();
        if (!g) continue;
        Ptr<CurveEvaluator3D> ev = g->evaluator();
        double s = 0, t = 0;
        if (!ev->getParameterExtents(s, t)) continue;
        for (int k = 0; k < 24; k++)
        {
            Ptr<Point3D> p;
            if (!ev->getPointAtParameter(s + (t - s) * k / 24.0, p)) continue;
            // profile curve geometry taken as model space and brought into the sketch plane; when that leaves
            // it off the plane, it already was in sketch space (measured on sketches on XZ-parallel planes: taken
            // as model space, every point landed on one line and nested circles no longer nested)
            Ptr<Point3D> q = sk->modelToSketchSpace(p);
            if (std::fabs(q->z()) > 1e-6) offPlane = true;
            poly.push_back({q->x(), q->y()});
            raw.push_back({p->x(), p->y()});
        }
    }
    return offPlane ? raw : poly;
}

bool insidePoly(const std::vector<std::array<double, 2>>& poly, double x, double y)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        if (((poly[i][1] > y) != (poly[j][1] > y)) &&
            (x < (poly[j][0] - poly[i][0]) * (y - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0]))
            in = !in;
    }
    return in;
}

double polyDistance(const std::vector<std::array<double, 2>>& poly, double x, double y)
{
    double best = 1e300;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        double ax = poly[j][0], ay = poly[j][1], bx = poly[i][0], by = poly[i][1];
        double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        double t = l2 > 0 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(x - (ax + t * dx), y - (ay + t * dy)));
    }
    return best;
}

Ptr<ProfileLoop> outerLoop(const Ptr<Profile>& p)
{
    Ptr<ProfileLoops> ls = p->profileLoops();
    for (size_t i = 0; i < ls->count(); i++)
        if (ls->item(i)->isOuter()) return ls->item(i);
    return ls->count() ? ls->item(0) : nullptr;
}

using Poly = std::vector<std::array<double, 2>>;

// A point inside a region given by its loops (outer and inner polygons, any order): on the horizontal line through
// the region where it is widest, the middle of the widest span between two crossings (even-odd over all loops).
// The curves of a profile loop, each as its own polyline in sketch space (cm), ends included: a set of segments
// whose crossing parity is right whatever direction each curve runs in.
std::vector<Poly> loopCurves(const Ptr<Sketch>& sk, const Ptr<ProfileLoop>& loop)
{
    std::vector<Poly> res;
    Ptr<ProfileCurves> pcs = loop->profileCurves();
    for (size_t i = 0; i < pcs->count(); i++)
    {
        Ptr<Curve3D> g = pcs->item(i)->geometry();
        if (!g) continue;
        Ptr<CurveEvaluator3D> ev = g->evaluator();
        double s = 0, t = 0;
        if (!ev->getParameterExtents(s, t)) continue;
        // the profile curve's geometry is taken as model space and brought into the sketch; when that leaves it
        // off the sketch plane it already was in sketch space (measured: a sketch on the XZ plane)
        Poly pl, raw;
        bool offPlane = false;
        for (int k = 0; k <= 32; k++)
        {
            Ptr<Point3D> p;
            if (!ev->getPointAtParameter(s + (t - s) * k / 32.0, p)) continue;
            Ptr<Point3D> q = sk->modelToSketchSpace(p);
            if (std::fabs(q->z()) > 1e-6) offPlane = true;
            pl.push_back({q->x(), q->y()});
            raw.push_back({p->x(), p->y()});
        }
        res.push_back(offPlane ? raw : pl);
    }
    return res;
}

// A point inside a region given as polylines of all its boundary curves (any order and direction): on the
// horizontal line where the region is widest, the middle of the widest span between two crossings.
bool interiorPoint(const std::vector<Poly>& loops, double& x, double& y)
{
    double y0 = 1e300, y1 = -1e300;
    for (auto& l : loops)
        for (auto& q : l)
        {
            y0 = std::min(y0, q[1]);
            y1 = std::max(y1, q[1]);
        }
    if (!(y1 > y0)) return false;
    double bestW = 0;
    for (int k = 1; k < 40; k++)
    {
        double yy = y0 + (y1 - y0) * (k + 0.37) / 40.5;   // off the sample rows
        std::vector<double> xs;
        for (auto& l : loops)
            for (size_t i = 1; i < l.size(); i++)
                if ((l[i][1] > yy) != (l[i - 1][1] > yy))
                    xs.push_back(l[i][0] + (yy - l[i][1]) * (l[i - 1][0] - l[i][0]) / (l[i - 1][1] - l[i][1]));
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2)
            if (xs[i + 1] - xs[i] > bestW)
            {
                bestW = xs[i + 1] - xs[i];
                x = 0.5 * (xs[i] + xs[i + 1]);
                y = yy;
            }
    }
    return bestW > 0;
}

// Whether a point is enclosed by a closed contour given as unordered polylines (ray crossing parity).
bool enclosedBy(const std::vector<Poly>& contour, double x, double y)
{
    bool in = false;
    for (auto& pl : contour)
        for (size_t i = 1; i < pl.size(); i++)
        {
            const auto& a = pl[i - 1];
            const auto& b = pl[i];
            if (((a[1] > y) != (b[1] > y)) && (x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0])) in = !in;
        }
    return in;
}

// A sketch curve as a polyline in sketch space (cm).
Poly curvePolyline(const Ptr<SketchCurve>& c, int n = 48)
{
    Poly res;
    Ptr<Curve3D> g;   // sketch space (each curve type's geometry())
    if (Ptr<SketchLine> l = c) g = l->geometry();
    else if (Ptr<SketchArc> a = c) g = a->geometry();
    else if (Ptr<SketchCircle> ci = c) g = ci->geometry();
    else if (Ptr<SketchEllipse> e = c) g = e->geometry();
    else if (Ptr<SketchEllipticalArc> ea = c) g = ea->geometry();
    else if (Ptr<SketchFittedSpline> fs = c) g = fs->geometry();
    else if (Ptr<SketchFixedSpline> xs = c) g = xs->geometry();
    else if (Ptr<SketchControlPointSpline> cs = c) g = cs->geometry();
    if (!g) return res;
    Ptr<CurveEvaluator3D> ev = g->evaluator();
    double s = 0, t = 0;
    if (!ev->getParameterExtents(s, t)) return res;
    for (int k = 0; k <= n; k++)
    {
        Ptr<Point3D> p;
        if (ev->getPointAtParameter(s + (t - s) * k / n, p)) res.push_back({p->x(), p->y()});
    }
    return res;
}

} // namespace

std::string featureName(const Ptr<Base>& obj)
{
    if (Ptr<Feature> f = obj) return f->name();
    if (Ptr<Sketch> s = obj) return s->name();
    if (Ptr<ConstructionPlane> p = obj) return p->name();
    if (Ptr<ConstructionAxis> a = obj) return a->name();
    if (Ptr<ConstructionPoint> p = obj) return p->name();
    return "";
}

void Replayer::name(const Ptr<Base>& obj, const std::string& n)
{
    if (n.empty() || !obj) return;
    if (Ptr<Feature> f = obj) f->name(n);
    else if (Ptr<Sketch> s = obj) s->name(n);
    else if (Ptr<ConstructionPlane> p = obj) p->name(n);
    else if (Ptr<ConstructionAxis> a = obj) a->name(n);
    else if (Ptr<ConstructionPoint> p = obj) p->name(n);
}

// ---------------------------------------------------------------------------------------- references
std::string Replayer::resolveEntities(const json& refs, std::vector<Ptr<Base>>& out)
{
    for (auto& r : refs)
    {
        std::vector<Ptr<Base>> hit;
        json res = resolveRef(bodies(), r, hit);
        if (res.value("result", "") != "resolved")
        {
            std::string notes;
            if (res.contains("notes"))
                for (auto& n : res["notes"]) notes += (notes.empty() ? "" : "; ") + n.get<std::string>();
            return r.value("kind", "") + " reference " + res.value("result", "?") + (notes.empty() ? "" : " (" + notes + ")");
        }
        out.insert(out.end(), hit.begin(), hit.end());
    }
    return "";
}

Ptr<Base> Replayer::refObject(const json& r, std::string& why)
{
    std::string kind = r.value("kind", "");
    if (kind == "originPlane") return planeFor(r, why);
    if (kind == "origin") return comp_->originConstructionPoint();
    if (kind == "feature")
    {
        auto it = features.find(r.value("id", ""));
        if (it == features.end() || !it->second)
        {
            why = "feature " + r.value("id", "") + " was not rebuilt";
            return nullptr;
        }
        return it->second;
    }
    if (kind == "sketchEntity")
    {
        auto sit = sketches_.find(r.value("feature", ""));
        if (sit == sketches_.end())
        {
            why = "sketch " + r.value("feature", "") + " was not rebuilt";
            return nullptr;
        }
        auto it = sit->second.ents.find(r.value("id", ""));
        if (it == sit->second.ents.end())
        {
            why = "sketch entity " + r.value("id", "") + " missing";
            return nullptr;
        }
        return it->second;
    }
    if (kind == "face" || kind == "edge" || kind == "vertex" || kind == "body")
    {
        std::vector<Ptr<Base>> hit;
        json res = resolveRef(bodies(), r, hit);
        if (res.value("result", "") != "resolved" || hit.size() != 1)
        {
            why = kind + " reference " + res.value("result", "?") + (hit.size() > 1 ? " (several pieces)" : "");
            return nullptr;
        }
        return hit[0];
    }
    why = "reference kind " + kind + " not supported";
    return nullptr;
}

// ---------------------------------------------------------------------------------------- profiles
// The sketch's regions as the source extrudes a whole sketch: a region is material when an odd number of
// the sketch's closed contours enclose it (nested contours make holes, islands inside holes are material).
// With `contours`, only the regions bounded by those segment sets.
Ptr<ObjectCollection> Replayer::profilesFor(const json& def, std::string& why)
{
    const json& pr = def["profile"];
    std::string key;
    if (pr.value("kind", "") == "feature") key = pr.value("id", "");
    else if (pr.value("kind", "") == "transferred") key = "T:" + pr.value("document", "") + "|" + pr.value("feature", "");
    else
    {
        why = "profile reference kind " + pr.value("kind", "") + " not supported yet";
        return nullptr;
    }
    auto sit = sketches_.find(key);
    if (sit == sketches_.end())
    {
        why = "profile sketch " + pr.value("id", "") + " was not rebuilt";
        return nullptr;
    }
    SketchMap& m = sit->second;
    Ptr<Profiles> all0 = m.sketch->profiles();
    if (!all0 || all0->count() == 0)
    {
        why = "the sketch has no closed profile";
        return nullptr;
    }
    // The profiles bounded by the payload's own curves (`own`): the curves this add-in projects into a sketch to
    // hold its relations to model edges are not sketch geometry in the source, but Fusion closes profiles with them
    // (measured on a real part: a projected bend edge closed an extra region, and a whole-sketch cut took it too)
    struct ProfileList
    {
        std::vector<Ptr<Profile>> v;
        size_t count() const { return v.size(); }
        Ptr<Profile> item(size_t i) const { return v[i]; }
    } own, every;
    {
        std::vector<Ptr<SketchEntity>> mine;
        for (auto& kv : m.ents) mine.push_back(kv.second);
        for (size_t i = 0; i < all0->count(); i++)
        {
            Ptr<Profile> p = all0->item(i);
            every.v.push_back(p);
            bool ok = true;
            Ptr<ProfileLoops> ls = p->profileLoops();
            for (size_t k = 0; k < ls->count() && ok; k++)
            {
                Ptr<ProfileCurves> pcs = ls->item(k)->profileCurves();
                for (size_t c = 0; c < pcs->count() && ok; c++)
                {
                    Ptr<SketchEntity> se = pcs->item(c)->sketchEntity();
                    bool found = false;
                    for (auto& e : mine)
                        if (e == se) found = true;
                    if (!found) ok = false;
                }
            }
            if (ok) own.v.push_back(p);
        }
    }
    // regions may be bounded by model edges (measured: a region of a sketch on a sheet's face closed by the sheet's
    // edge), and contours select by their own segments: those see every profile
    const ProfileList* ps = &every;
    Ptr<ObjectCollection> res = ObjectCollection::create();
    bool hasContours = def.contains("contours") && !def["contours"].empty();
    bool hasRegions = def.contains("regions") && !def["regions"].empty();
    // regions (FORMAT-SPEC 4, extrude/cut): the profile of the rebuilt sketch that contains the region's point
    // (inside its outer loop, outside its inner loops; the smallest such profile), checked by area (1e-3)
    if (hasRegions)
    {
        std::vector<std::vector<Poly>> loops(ps->count());
        std::vector<double> areas(ps->count());
        for (size_t i = 0; i < ps->count(); i++)
        {
            // every loop's curves: a point inside the profile has odd crossing parity over all of them (inside
            // the outer loop and outside its holes)
            Ptr<ProfileLoops> ls = ps->item(i)->profileLoops();
            for (size_t k = 0; k < ls->count(); k++)
                for (auto& pl : loopCurves(m.sketch, ls->item(k))) loops[i].push_back(pl);
            Ptr<AreaProperties> ap = ps->item(i)->areaProperties();
            areas[i] = ap ? cm2mm(cm2mm(ap->area())) : 0.0;
        }
        std::set<size_t> taken;
        for (auto& rg : def["regions"])
        {
            Ptr<Point3D> q = m.toSketch(m.src.toModel(rg["point"][0].get<double>(), rg["point"][1].get<double>()));
            double want = rg.value("area", 0.0);
            int best = -1;
            for (size_t i = 0; i < ps->count(); i++)
            {
                if (loops[i].empty() || !enclosedBy(loops[i], q->x(), q->y())) continue;
                if (best < 0 || areas[i] < areas[best]) best = static_cast<int>(i);
            }
            char buf[160];
            if (best < 0)
            {
                std::snprintf(buf, sizeof buf, "no region of the rebuilt sketch contains the region point (%.3f, %.3f)", rg["point"][0].get<double>(),
                              rg["point"][1].get<double>());
                why = buf;
                return nullptr;
            }
            std::set<size_t> group{static_cast<size_t>(best)};
            double total = areas[best];
            if (want > 0 && total < want * (1 - 1e-3))
            {
                // the source's region split in two here by a curve that is not the source's sketch geometry (a
                // curve this add-in projected to hold a relation, e.g. a layout line): the neighbours across such
                // curves are taken while the area allows (measured on a real part: a layout line split a boss region)
                auto foreignCurves = [&](size_t i) {
                    std::vector<Ptr<SketchEntity>> v;
                    Ptr<ProfileLoops> ls = ps->item(i)->profileLoops();
                    for (size_t k = 0; k < ls->count(); k++)
                    {
                        Ptr<ProfileCurves> pcs = ls->item(k)->profileCurves();
                        for (size_t c = 0; c < pcs->count(); c++)
                        {
                            Ptr<SketchEntity> se = pcs->item(c)->sketchEntity();
                            bool mine = false;
                            for (auto& kv : m.ents)
                                if (kv.second == se) mine = true;
                            if (!mine) v.push_back(se);
                        }
                    }
                    return v;
                };
                std::vector<std::vector<Ptr<SketchEntity>>> foreign(ps->count());
                for (size_t i = 0; i < ps->count(); i++) foreign[i] = foreignCurves(i);
                for (bool grew = true; grew && total < want * (1 - 1e-3);)
                {
                    grew = false;
                    for (size_t i = 0; i < ps->count() && !grew; i++)
                    {
                        if (group.count(i) || taken.count(i) || total + areas[i] > want * (1 + 1e-3)) continue;
                        bool adjacent = false;
                        for (size_t g : group)
                            for (auto& a1 : foreign[g])
                                for (auto& a2 : foreign[i])
                                    if (a1 == a2) adjacent = true;
                        if (!adjacent) continue;
                        group.insert(i);
                        total += areas[i];
                        grew = true;
                    }
                }
            }
            if (want > 0 && total > want * (1 + 1e-3) && regionRetry_ < 2 && m.space != "3d")
            {
                // larger than the source's region: the source bounds regions of a sketch by the model edges in its
                // plane too (measured: a region of a sketch on a sheet's face closed by the sheet's edge), Fusion
                // only by sketch curves: those edges are projected into the sketch and the profiles tried again
                int made = 0;
                for (auto& b : bodies(true))
                    for (size_t i = 0; i < b->edges()->count(); i++)
                    {
                        Ptr<BRepEdge> e = b->edges()->item(i);
                        bool inPlane = true;
                        for (auto& q : edgePoints(e, {0.0, 0.37, 1.0}))
                            if (std::fabs(m.sketch->modelToSketchSpace(ptCm(q))->z()) > 1e-5) inPlane = false;
                        if (!inPlane) continue;
                        Ptr<ObjectCollection> pr = m.sketch->project(e);
                        if (pr) made += static_cast<int>(pr->count());
                    }
                if (made > 0)
                {
                    int was = regionRetry_;
                    regionRetry_ = 2;
                    std::string w2;
                    Ptr<ObjectCollection> again = profilesFor(def, w2);
                    regionRetry_ = was;
                    if (again) return again;
                    why = w2;
                    return nullptr;
                }
            }
            if (want > 0 && std::fabs(total - want) > 1e-3 * want && !regionRetry_)
            {
                // the curves this add-in projected (not the source's sketch geometry) made construction, so they
                // bound no profile, and the profiles tried again (the projections keep their relations)
                Ptr<SketchCurves> cs = m.sketch->sketchCurves();
                int made = 0;
                for (size_t i = 0; i < cs->count(); i++)
                {
                    Ptr<SketchCurve> c = cs->item(i);
                    if (!c || c->isConstruction()) continue;
                    bool mine = false;
                    for (auto& kv : m.ents)
                        if (kv.second == Ptr<SketchEntity>(c)) mine = true;
                    if (!mine && c->isConstruction(true)) made++;
                }
                if (made > 0)
                {
                    regionRetry_ = 1;
                    std::string w2;
                    Ptr<ObjectCollection> again = profilesFor(def, w2);
                    regionRetry_ = 0;
                    if (again) return again;
                }
            }
            if (want > 0 && std::fabs(total - want) > 1e-3 * want)
            {
                std::snprintf(buf, sizeof buf, "the region at (%.3f, %.3f) has %.3f mm2 in the rebuilt sketch, %.3f in the source",
                              rg["point"][0].get<double>(), rg["point"][1].get<double>(), total, want);
                why = buf;
                if (std::error_code dec; fs::is_directory(dataDir() / L"debug_regions", dec))   // developer
                    for (size_t i = 0; i < ps->count(); i++)
                    {
                        std::snprintf(buf, sizeof buf, " [%zu: %.2f%s]", i, areas[i], group.count(i) ? " in" : "");
                        why += buf;
                    }
                return nullptr;
            }
            for (size_t g : group)
                if (taken.insert(g).second) res->add(ps->item(g));
        }
        if (!hasContours) return res;
    }
    // Several contours, or a contour no profile of the rebuilt sketch has as its outer loop (other curves of the
    // sketch cross it, e.g. a polygon inside a circle): the profiles of the sketch inside the selected contours.
    // A contour wholly inside another selected one is a hole in it (an island in that hole is material again);
    // contours that cross are united. (Measured on a real model: a link's boss lists two corner circles as
    // contours beside regions that leave those circles' quarters out, so a contour is all it encloses.)
    auto exactContours = [&]() {
        if (def["contours"].size() != 1) return false;
        std::set<std::string> wanted;
        for (auto& s : def["contours"][0]) wanted.insert(s.get<std::string>());
        for (size_t i = 0; i < ps->count(); i++)
        {
            Ptr<ProfileCurves> pcs = outerLoop(ps->item(i))->profileCurves();
            std::set<std::string> have;
            for (size_t k = 0; k < pcs->count(); k++)
            {
                Ptr<SketchEntity> se = pcs->item(k)->sketchEntity();
                std::string id = "?";
                for (auto& kv : m.ents)
                    if (kv.second == se) id = kv.first;
                have.insert(id);
            }
            if (have == wanted) return true;
        }
        return false;
    };
    if (hasContours && !exactContours())
    {
        std::vector<std::vector<Poly>> contours;
        for (auto& want : def["contours"])
        {
            std::vector<Poly> c;
            for (auto& sid : want)
            {
                auto it = m.ents.find(sid.get<std::string>());
                Ptr<SketchCurve> sc = it == m.ents.end() ? nullptr : Ptr<SketchCurve>(it->second);
                if (!sc)
                {
                    why = "contour segment " + sid.get<std::string>() + " is not in the rebuilt sketch";
                    return nullptr;
                }
                c.push_back(curvePolyline(sc));
            }
            contours.push_back(c);
        }
        // nesting depth of each contour among the selected ones
        std::vector<int> depth(contours.size(), 0);
        for (size_t i = 0; i < contours.size(); i++)
            for (size_t j = 0; j < contours.size(); j++)
            {
                if (i == j) continue;
                int in = 0, n = 0;
                for (auto& pl : contours[i])
                    for (size_t k = 0; k < pl.size(); k += 4, n++)
                        if (enclosedBy(contours[j], pl[k][0] + 1e-7, pl[k][1] + 1.3e-7)) in++;
                if (n > 0 && in == n) depth[i]++;
            }
        size_t before = res->count();
        std::string diag;
        for (size_t i = 0; i < ps->count(); i++)
        {
            std::vector<Poly> loops;
            Ptr<ProfileLoops> ls = ps->item(i)->profileLoops();
            for (size_t k = 0; k < ls->count(); k++)
                for (auto& pl : loopCurves(m.sketch, ls->item(k))) loops.push_back(pl);
            double x = 0, y = 0;
            if (!interiorPoint(loops, x, y))
            {
                diag += " [profile " + std::to_string(i) + ": no interior point, " + std::to_string(loops.size()) + " curves]";
                continue;
            }
            int outerN = 0, holeN = 0;
            for (size_t c = 0; c < contours.size(); c++)
                if (enclosedBy(contours[c], x, y)) (depth[c] % 2 ? holeN : outerN)++;
            char db[120];
            std::snprintf(db, sizeof db, " [profile %zu at (%.3f, %.3f) cm: %d/%d]", i, x, y, outerN, holeN);
            diag += db;
            if (outerN > holeN)
            {
                bool dup = false;
                for (size_t k = 0; k < res->count(); k++)
                    if (res->item(k) == ps->item(i)) dup = true;
                if (!dup) res->add(ps->item(i));
            }
        }
        if (res->count() == before)
        {
            why = "no region of the rebuilt sketch lies inside the selected contours" + diag;
            return nullptr;
        }
        return res;
    }
    if (hasContours)
    {
        for (auto& want : def["contours"])
        {
            std::set<std::string> wanted;
            for (auto& s : want) wanted.insert(s.get<std::string>());
            bool found = false;
            for (size_t i = 0; i < ps->count() && !found; i++)
            {
                Ptr<ProfileLoop> ol = outerLoop(ps->item(i));
                std::set<std::string> have;
                Ptr<ProfileCurves> pcs = ol->profileCurves();
                for (size_t k = 0; k < pcs->count(); k++)
                {
                    Ptr<SketchEntity> se = pcs->item(k)->sketchEntity();
                    std::string id = "?";
                    for (auto& kv : m.ents)
                        if (kv.second == se) id = kv.first;
                    have.insert(id);
                }
                if (have == wanted)
                {
                    res->add(ps->item(i));
                    found = true;
                }
            }
            if (!found)
            {
                why = "a selected contour was not found in the rebuilt sketch";
                return nullptr;
            }
        }
        return res;
    }
    if (!own.v.empty()) ps = &own;   // the whole sketch: its own curves only
    // A profile is material when an even number of other profiles' outer loops enclose it (it then lies in no hole,
    // or in an island of a hole). Tested with a point inside it against the other outer loops, each as the
    // polylines of its curves (crossing parity: right whatever direction each curve runs in; a polygon made by
    // joining the curves' samples in loop order is not, when curves run against the loop)
    std::vector<std::vector<Poly>> outer(ps->count());
    std::vector<std::array<double, 2>> inside(ps->count());
    std::vector<bool> hasInside(ps->count(), false);
    for (size_t i = 0; i < ps->count(); i++)
    {
        outer[i] = loopCurves(m.sketch, outerLoop(ps->item(i)));
        std::vector<Poly> loops;
        Ptr<ProfileLoops> ls = ps->item(i)->profileLoops();
        for (size_t k = 0; k < ls->count(); k++)
            for (auto& pl : loopCurves(m.sketch, ls->item(k))) loops.push_back(pl);
        hasInside[i] = interiorPoint(loops, inside[i][0], inside[i][1]);
    }
    for (size_t i = 0; i < ps->count(); i++)
    {
        int depth = 0;
        for (size_t j = 0; j < ps->count(); j++)
            if (i != j && hasInside[i] && !outer[j].empty() && enclosedBy(outer[j], inside[i][0], inside[i][1])) depth++;
        if (depth % 2 == 0) res->add(ps->item(i));
    }
    if (res->count() == 0) why = "no material region found in the sketch";
    return res->count() ? res : nullptr;
}

// ---------------------------------------------------------------------------------------- extrude / cut
std::string Replayer::replayExtrude(json& nf, const json& def, bool cut)
{
    std::string why;
    Ptr<ObjectCollection> prof = profilesFor(def, why);
    Ptr<Base> profile = prof;
    if (!prof && def.contains("thin") && def["profile"].value("kind", "") == "feature")
    {
        // a thin feature on an open sketch: the sketch's curves as a path
        auto it = sketches_.find(def["profile"].value("id", ""));
        if (it != sketches_.end())
        {
            Ptr<ObjectCollection> curves = ObjectCollection::create();
            Ptr<SketchCurves> cs = it->second.sketch->sketchCurves();
            for (size_t i = 0; i < cs->count(); i++)
                if (!cs->item(i)->isConstruction() && !cs->item(i)->isReference()) curves->add(cs->item(i));
            if (curves->count() > 0) profile = Path::create(curves, connectedChainedCurves);
        }
    }
    if (!profile) return "profile: " + why;
    bool mirrored = false;
    {
        const json& pr = def["profile"];
        std::string key = pr.value("kind", "") == "transferred" ? "T:" + pr.value("document", "") + "|" + pr.value("feature", "") : pr.value("id", "");
        auto it = sketches_.find(key);
        if (it != sketches_.end()) mirrored = it->second.mirrored;
    }
    FeatureOperations op = cut ? CutFeatureOperation : (def.value("merge", true) ? JoinFeatureOperation : NewBodyFeatureOperation);
    if (!cut && bodies(true).empty()) op = NewBodyFeatureOperation;
    Ptr<ExtrudeFeatures> ef = comp_->features()->extrudeFeatures();
    // Link to thickness (FORMAT-SPEC 4, cut in a sheet-metal body): Fusion's expressions cannot name the rule's
    // thickness (measured: "Expression is invalid", and the rule's thickness refuses a user parameter), so the
    // depth runs up to the sheet's opposite face, which follows the thickness
    Ptr<BRepFace> thickTarget = (def.value("linkToThickness", false) && comp_->activeSheetMetalRule() && def["end"].value("type", "") == "blind")
                                    ? sheetOppositeFace(prof, def["end"].value("depth", 0.0))
                                    : nullptr;
    // A cut is tried: 0 as recorded (with the part's solids as participants, so a body a carried result brought
    // is found); 1 as recorded without participants (a sheet-metal body is not cut when it is named as a
    // participant: P12); 2, 3 the other way (with, without); 4 through all both ways. 2-4 are reported approximate.
    // 5, 6: a through-all cut that Fusion refuses ("body not found to extrude through": measured on a two-sided
    // through-all cut sketched on a part's outer face, and on a sheet-metal body) as blind cuts past every body:
    // one-sided in the recorded direction, then both sides
    bool anyThroughAll = def["end"].value("type", "") == "throughAll" || (def.contains("end2") && def["end2"].value("type", "") == "throughAll");
    const int attempts = cut ? (anyThroughAll ? 7 : 5) : 1;
    double pastAll = 10;
    for (auto& b : bodies(true))
    {
        auto bb = boxMm(b);
        pastAll = std::max(pastAll, 2 * std::hypot(bb[3] - bb[0], bb[4] - bb[1], bb[5] - bb[2]) + 10);
    }
    std::string lastErrorSeen;
    // the order: as recorded first, then the blind cut past every body in the recorded direction (5) before the
    // other direction is tried, so a through-all Fusion refuses is not made on the wrong side
    static const int order[] = {0, 1, 5, 2, 3, 4, 6};
    for (int ai = 0; ai < 7; ai++)
    {
    int attempt = order[ai];
    if (attempt >= attempts) continue;
    bool lastTry = true;
    for (int k = ai + 1; k < 7; k++)
        if (order[k] < attempts) lastTry = false;
    Ptr<ExtrudeFeatureInput> in = ef->createInput(profile, op);
    if (!in) return "extrude input: " + lastError();
    // the source extrudes along its sketch normal unless reversed; Fusion's positive side is its own sketch normal
    // A boss runs along the source's sketch normal unless reversed; a cut runs against it (into the material,
    // SolidWorks' default for a cut). Fusion's positive side is its own sketch normal, opposite when mirrored.
    bool flip = ((def.value("reverse", false) != cut) != mirrored) != (attempt == 2 || attempt == 3);

    auto extentFor = [&](const json& e, std::string& w) -> Ptr<ExtentDefinition> {
        std::string t = e.value("type", "");
        if (t == "blind" && thickTarget && &e == &def["end"]) return ToEntityExtentDefinition::create(thickTarget, false);
        if (t == "blind") return DistanceExtentDefinition::create(mmValue(e.value("depth", 0.0)));
        if (t == "throughAll" && flip && &e == &def["end"] && !def.contains("end2") && def.contains("start") &&
            def["start"].value("type", "") == "offset" && def["start"].value("distance", 0.0) != 0)
        {
            // Fusion refuses a through-all extrusion against the sketch normal from an offset start ("body not
            // found to extrude through", probe offsettest; with the normal it works): a blind depth past every body
            double span = 0;
            for (auto& b : bodies(true))
            {
                auto bb = boxMm(b);
                span = std::max(span, std::hypot(bb[3] - bb[0], bb[4] - bb[1], bb[5] - bb[2]));
            }
            nf["status"] = "approximate";
            nf["note"] = "through all from an offset start against the sketch normal made as a blind cut past every body (Fusion refuses that through-all)";
            return DistanceExtentDefinition::create(mmValue(2 * span + std::fabs(def["start"].value("distance", 0.0)) + 10));
        }
        if (t == "throughAll") return ThroughAllExtentDefinition::create();
        if (t == "upToVertex" || t == "upToSurface" || t == "offsetFromSurface" || t == "upToBody")
        {
            if (!e.contains("target"))
            {
                w = t + " without a target";
                return nullptr;
            }
            Ptr<Base> target = refObject(e["target"], w);
            if (!target) return nullptr;
            Ptr<ToEntityExtentDefinition> te = ToEntityExtentDefinition::create(target, false, mmValue(e.value("offset", 0.0)));
            return te;
        }
        w = "end condition " + t + " not supported";
        return nullptr;
    };
    const json& e1 = def["end"];
    std::string t1 = e1.value("type", "");
    Ptr<ValueInput> taper1 = e1.contains("draft") ? degValue((e1.value("draftOutward", false) ? 1 : -1) * e1.value("draft", 0.0)) : nullptr;
    bool ok = false;
    if (t1 == "midPlane")
        ok = in->setSymmetricExtent(mmValue(e1.value("depth", 0.0)), true, taper1);
    else if (def.contains("end2"))
    {
        const json& e2 = def["end2"];
        Ptr<ExtentDefinition> x1 = extentFor(e1, why);
        if (!x1) return why;
        Ptr<ExtentDefinition> x2 = extentFor(e2, why);
        if (!x2) return why;
        Ptr<ValueInput> taper2 = e2.contains("draft") ? degValue((e2.value("draftOutward", false) ? 1 : -1) * e2.value("draft", 0.0)) : nullptr;
        ok = flip ? in->setTwoSidesExtent(x2, x1, taper2, taper1) : in->setTwoSidesExtent(x1, x2, taper1, taper2);
    }
    else
    {
        Ptr<ExtentDefinition> x1 = extentFor(e1, why);
        if (!x1) return why;
        ok = in->setOneSideExtent(x1, flip ? NegativeExtentDirection : PositiveExtentDirection, taper1);
    }
    if (!ok) return "extent not accepted: " + lastError();
    if (def.contains("start"))
    {
        const json& s = def["start"];
        std::string st = s.value("type", "");
        double d = s.value("distance", 0.0) * (s.value("reverse", false) ? -1 : 1) * (mirrored ? -1 : 1);
        // the offset runs along the sketch normal in both systems (probe offsettest)
        if (st == "offset") in->startExtent(OffsetStartDefinition::create(mmValue(d)));
        else if (st == "surface" || st == "vertex")
        {
            Ptr<Base> ent = s.contains("target") ? refObject(s["target"], why) : nullptr;
            if (!ent) return "start " + st + ": " + why;
            in->startExtent(FromEntityStartDefinition::create(ent, mmValue(d)));
        }
    }
    if (def.contains("thin"))
    {
        const json& th = def["thin"];
        std::string tt = th.value("type", "oneDirection");
        double k1 = th.value("thickness", 0.0), k2 = th.value("thickness2", 0.0);
        // SolidWorks' one-direction wall lies on the opposite side from Fusion's Side1 for the same path and
        // normal (measured on P19: SolidWorks' default fills the outside of the corner); a mirrored sketch swaps
        bool swDefaultIsSide2 = !mirrored;
        ThinExtrudeWallLocation dflt = swDefaultIsSide2 ? Side2 : Side1, other = swDefaultIsSide2 ? Side1 : Side2;
        if (tt == "midPlane") in->setThinExtrude(Center, mmValue(k1));
        else if (tt == "twoDirection") in->setThinExtrude(dflt, mmValue(k1), other, mmValue(k2));
        else in->setThinExtrude(tt == "oppositeDirection" ? other : dflt, mmValue(k1));
    }
    // an assembly-level cut changes exactly the components it lists
    if (def.contains("components") && asmOccs)
    {
        std::vector<Ptr<BRepBody>> parts;
        for (auto& c : def["components"])
        {
            auto it = asmOccs->find(doc_.value("id", "") + "|" + c.get<std::string>());
            if (it == asmOccs->end()) return "component " + c.get<std::string>() + " of the cut was not placed";
            Ptr<BRepBodies> bs = it->second->bRepBodies();
            for (size_t i = 0; bs && i < bs->count(); i++) parts.push_back(bs->item(i));
        }
        in->participantBodies(parts);
    }
    else if (cut && (attempt == 0 || attempt == 2))
        in->participantBodies(bodies(true));   // a body a base feature brought (a carried result) is not found otherwise
    if (attempt == 4)
        in->setTwoSidesExtent(ThroughAllExtentDefinition::create(), ThroughAllExtentDefinition::create());
    if (attempt == 5)
        in->setOneSideExtent(DistanceExtentDefinition::create(mmValue(pastAll)), flip ? NegativeExtentDirection : PositiveExtentDirection);
    if (attempt == 6)
        in->setTwoSidesExtent(DistanceExtentDefinition::create(mmValue(pastAll)), DistanceExtentDefinition::create(mmValue(pastAll)));
    Ptr<ExtrudeFeature> f = ef->add(in);
    if (!f)
    {
        std::string err = lastError();
        if (attempt < 5) lastErrorSeen = err;
        if (!lastTry) continue;
        // where the profile and the bodies are (a cut that finds no body: is the profile off the part?)
        std::string where;
        if (prof)
        {
            std::array<double, 6> pb{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
            for (size_t k = 0; k < prof->count(); k++)
                if (Ptr<Profile> pp = prof->item(k))
                {
                    Ptr<BoundingBox3D> bx = pp->boundingBox();
                    if (!bx) continue;
                    std::array<double, 6> b{cm2mm(bx->minPoint()->x()), cm2mm(bx->minPoint()->y()), cm2mm(bx->minPoint()->z()),
                                            cm2mm(bx->maxPoint()->x()), cm2mm(bx->maxPoint()->y()), cm2mm(bx->maxPoint()->z())};
                    for (int q = 0; q < 3; q++) pb[q] = std::min(pb[q], b[q]), pb[q + 3] = std::max(pb[q + 3], b[q + 3]);
                }
            char wb[200];
            std::snprintf(wb, sizeof wb, " (profile box %.1f %.1f %.1f / %.1f %.1f %.1f mm", pb[0], pb[1], pb[2], pb[3], pb[4], pb[5]);
            where = wb;
            for (auto& bb : bodies(true))
            {
                auto b = boxMm(bb);
                std::snprintf(wb, sizeof wb, "; body box %.1f %.1f %.1f / %.1f %.1f %.1f", b[0], b[1], b[2], b[3], b[4], b[5]);
                where += wb;
            }
            where += ")";
        }
        return std::string(cut ? "cut" : "extrude") + " failed: " + err + where;
    }
    // Fusion can return a feature whose compute failed: that is a failure too
    std::string msg = f->timelineObject() ? f->timelineObject()->errorOrWarningMessage() : "";
    bool bad = f->timelineObject() && (f->timelineObject()->healthState() == ErrorFeatureHealthState ||
                                       msg.find("Compute Failed") != std::string::npos);
    if (bad)
    {
        f->deleteMe();
        // a cut that finds no body on its side: the source's sense of "into the material" differs from this
        // face's here (seen on a face of a body a carried result brought, P12): the other side is tried once
        if (!lastTry && (msg.find("No target body") != std::string::npos || msg.find("body not found") != std::string::npos)) continue;
        return std::string(cut ? "cut" : "extrude") + " failed: " + msg;
    }
    if (attempt >= 5)
    {
        nf["status"] = "approximate";
        nf["note"] = std::string("through all made as a blind cut past every body") + (attempt == 6 ? " on both sides" : "") +
                     " (Fusion refused the through-all: " + lastErrorSeen + ")";
    }
    else if (attempt >= 2)
    {
        nf["status"] = "approximate";
        nf["note"] = attempt < 4 ? "cut made in the opposite direction: the recorded one finds no material"
                                  : "cut made through all in both directions: the recorded direction finds no material in Fusion";
    }
    else if (attempt == 1)
        nf["info"] = "cut without naming its target bodies (Fusion does not cut a sheet-metal body named as a participant)";
    else if (f->timelineObject() && f->timelineObject()->healthState() == WarningFeatureHealthState)
        nf["info"] = "Fusion warns: " + msg;
    name(f, nf.value("name", ""));
    // the feature's dimensions as SolidWorks names them (D1 = direction 1 depth, D2 = direction 2 depth)
    {
        std::string fn = nf.value("name", "");
        // and by the neutral key an equation's targetRef {kind: featureField, feature, field} names
        std::string fk = "field:" + nf.value("id", "") + "|";
        if (Ptr<DistanceExtentDefinition> d1 = f->extentOne())
        {
            registerParameter("D1@" + fn, d1->distance());
            registerParameter(fk + "end.depth", d1->distance());
        }
        if (Ptr<SymmetricExtentDefinition> s1 = f->extentOne())
        {
            registerParameter("D1@" + fn, s1->distance());
            registerParameter(fk + "end.depth", s1->distance());
        }
        if (Ptr<DistanceExtentDefinition> d2 = f->extentTwo())
        {
            registerParameter("D2@" + fn, d2->distance());
            registerParameter(fk + "end2.depth", d2->distance());
        }
    }
    features[nf.value("id", "")] = f;
    sheetCutOptions(nf, def, f, prof, cut);
    break;
    }
    return "";
}

// ---------------------------------------------------------------------------------------- sheet-metal cut options
// FORMAT-SPEC 4, "cut in a sheet-metal body":
//  - linkToThickness: the depth is the sheet's thickness and follows it. Fusion's rule thickness is not a design
//    parameter, so a user parameter holds the thickness, the rule's thickness is set to it and the depth's
//    expression names it: editing the parameter changes the sheet and the cut together.
//  - normalCut: SolidWorks cuts normal to the sheet's faces wherever the cut crosses it. Fusion has no such option;
//    a straight cut is the same exactly when the profile lies on a flat region of the sheet, which shows as the cut
//    removing (profile area) x (depth). Otherwise the straight cut is reported approximate.
// The planar face of the part's solids parallel to the profile's sketch plane at `depthMm` from it, under the
// profile (its first curve's start, projected): the sheet's other side for a cut linked to the thickness.
Ptr<BRepFace> Replayer::sheetOppositeFace(const Ptr<ObjectCollection>& prof, double depthMm)
{
    if (!prof || prof->count() == 0 || depthMm <= 0) return nullptr;
    Ptr<Profile> p = prof->item(0);
    Ptr<Sketch> sk = p ? p->parentSketch() : nullptr;
    if (!sk || !p->profileLoops() || p->profileLoops()->count() == 0) return nullptr;
    Ptr<ProfileCurves> pcs = p->profileLoops()->item(0)->profileCurves();
    if (!pcs || pcs->count() == 0) return nullptr;
    Ptr<SketchCurve> cv = pcs->item(0)->sketchEntity();
    Ptr<BoundingBox3D> cb = cv ? cv->boundingBox() : nullptr;   // sketch space
    if (!cb) return nullptr;
    Ptr<Point3D> at = Point3D::create((cb->minPoint()->x() + cb->maxPoint()->x()) / 2, (cb->minPoint()->y() + cb->maxPoint()->y()) / 2, 0);
    V3 o = ptMm(sk->sketchToModelSpace(Point3D::create(0, 0, 0)));
    V3 x = ptMm(sk->sketchToModelSpace(Point3D::create(1, 0, 0))), y = ptMm(sk->sketchToModelSpace(Point3D::create(0, 1, 0)));
    V3 n = unit(cross(sub(x, o), sub(y, o)));
    V3 q = ptMm(sk->sketchToModelSpace(at));
    Ptr<BRepFace> best;
    for (auto& b : bodies(true))
        for (size_t i = 0; i < b->faces()->count(); i++)
        {
            Ptr<BRepFace> f = b->faces()->item(i);
            Ptr<Plane> pl = f->geometry();
            if (!pl || std::fabs(std::fabs(dot(unit(vecOf(pl->normal())), n)) - 1) > 1e-9) continue;
            double off = dot(sub(ptMm(pl->origin()), q), n);
            if (std::fabs(std::fabs(off) - depthMm) > 1e-6) continue;
            V3 foot = add(q, mul(n, off));
            Ptr<BoundingBox3D> bb = f->boundingBox();
            V3 lo = ptMm(bb->minPoint()), hi = ptMm(bb->maxPoint());
            bool inside = true;
            for (int k = 0; k < 3; k++)
                if (foot[k] < lo[k] - 1e-4 || foot[k] > hi[k] + 1e-4) inside = false;
            if (!inside) continue;
            if (best) return nullptr;   // ambiguous
            best = f;
        }
    return best;
}

void Replayer::sheetCutOptions(json& nf, const json& def, const Ptr<ExtrudeFeature>& f, const Ptr<ObjectCollection>& prof, bool cut)
{
    auto approxNote = [&](const std::string& n) {
        nf["status"] = "approximate";
        nf["note"] = nf.contains("note") && !nf["note"].get<std::string>().empty() ? nf["note"].get<std::string>() + "; " + n : n;
    };
    auto addInfo = [&](const std::string& n) {
        nf["info"] = nf.contains("info") && !nf["info"].get<std::string>().empty() ? nf["info"].get<std::string>() + "; " + n : n;
    };
    Ptr<SheetMetalRule> rule = comp_->activeSheetMetalRule();
    // Link to thickness sets a blind depth; with a through-all end it changes nothing (the cut goes through anyway)
    if (def.value("linkToThickness", false) && def["end"].value("type", "") == "blind")
    {
        if (Ptr<ToEntityExtentDefinition> te = f->extentOne())
            addInfo("Link to thickness: the depth runs up to the sheet's opposite face, so it follows the thickness");
        else
            approxNote(std::string("Link to thickness not carried (") + (rule ? "no face of the sheet lies at the depth opposite the sketch" : "no sheet-metal rule") +
                       "): the depth is the thickness's value, fixed");
    }
    if (cut && def.value("normalCut", false))
    {
        double area = 0;
        for (size_t i = 0; prof && i < prof->count(); i++)
            if (Ptr<Profile> p = prof->item(i))
                if (Ptr<AreaProperties> ap = p->areaProperties(HighCalculationAccuracy)) area += ap->area() * 100.0;   // cm^2 -> mm^2
        double depth = def["end"].value("depth", 0.0);
        if (Ptr<DistanceExtentDefinition> d1 = f->extentOne())
            if (Ptr<ModelParameter> dp = d1->distance()) depth = std::fabs(dp->value()) * 10.0;
        double removed = 0;
        bool measured = false;
        if (Ptr<TimelineObject> to = f->timelineObject())
        {
            // the bodies' volume with the timeline just before the cut and after it
            int at = to->index();
            Ptr<Timeline> tl = design_->timeline();
            double before = 0, after = 0;
            for (auto& b : bodies(true)) after += b->volume();
            if (tl && to->rollTo(true))
            {
                for (auto& b : bodies(true)) before += b->volume();
                tl->moveToEnd();
                removed = (before - after) * 1000.0;   // cm^3 -> mm^3
                measured = at >= 0;
            }
        }
        if (measured && area > 0 && depth > 0 && std::fabs(removed - area * depth) <= 1e-6 * std::max(1.0, area * depth))
            addInfo("Normal cut made as a straight cut: the profile lies on a flat region of the sheet, where the two are the same");
        else
            approxNote("Normal cut made as a straight cut (Fusion has no cut normal to the sheet): it may remove different material where it crosses a bend");
    }
}

// ---------------------------------------------------------------------------------------- axis sense
int Replayer::axisSense(const json& r, const Ptr<Base>& ax)
{
    V3 want{0, 0, 0};
    bool have = false;
    std::string kind = r.value("kind", "");
    if (kind == "sketchEntity")
    {
        auto sd = sketchDefs_.find(r.value("feature", ""));
        auto sm = sketches_.find(r.value("feature", ""));
        if (sd != sketchDefs_.end() && sm != sketches_.end())
            for (auto& e : sd->second["entities"])
                if (e.value("id", "") == r.value("id", "") && e.value("type", "") == "line")
                {
                    V3 a = sm->second.src.toModel(e["start"][0].get<double>(), e["start"][1].get<double>());
                    V3 b = sm->second.src.toModel(e["end"][0].get<double>(), e["end"][1].get<double>());
                    want = unit(sub(b, a));
                    have = true;
                }
    }
    else if (kind == "feature" && doc_.contains("features"))
    {
        for (auto& f : doc_["features"])
            if (f.value("id", "") == r.value("id", "") && f.contains("def") && f["def"].contains("line"))
            {
                want = unit(sub(v3(f["def"]["line"]["end"]), v3(f["def"]["line"]["start"])));
                have = true;
            }
    }
    else if (kind == "edge" && r.contains("geom") && r["geom"].contains("params") && r["geom"]["params"].contains("direction"))
    {
        want = unit(v3(r["geom"]["params"]["direction"]));
        have = true;
    }
    else if (kind == "face" && r.contains("geom") && r["geom"].contains("params") && r["geom"]["params"].contains("axis"))
    {
        want = unit(v3(r["geom"]["params"]["axis"]));
        have = true;
    }
    if (!have) return 0;
    V3 got{0, 0, 0};
    if (Ptr<ConstructionAxis> ca = ax)
    {
        Ptr<InfiniteLine3D> l = ca->geometry();
        if (l) got = unit(vecOf(l->direction()));
    }
    else if (Ptr<SketchLine> sl = ax)
        got = unit(sub(ptMm(sl->endSketchPoint()->worldGeometry()), ptMm(sl->startSketchPoint()->worldGeometry())));
    else if (Ptr<BRepEdge> ed = ax)
    {
        Ptr<CurveEvaluator3D> ev = ed->evaluator();
        double s, t;
        Ptr<Point3D> a, b;
        if (ev->getParameterExtents(s, t) && ev->getPointAtParameter(s, a) && ev->getPointAtParameter(t, b)) got = unit(sub(ptMm(b), ptMm(a)));
    }
    if (norm(got) == 0) return 0;
    return dot(got, want) < 0 ? -1 : 1;
}

// ---------------------------------------------------------------------------------------- revolve
std::string Replayer::replayRevolve(json& nf, const json& def, bool cut)
{
    std::string why;
    if (def.contains("thin")) return replayThinRevolve(nf, def);
    Ptr<ObjectCollection> prof = profilesFor(def, why);
    if (!prof) return "profile: " + why;
    Ptr<Base> axis = def.contains("axis") ? refObject(def["axis"], why) : nullptr;
    if (!axis) return "axis: " + why;
    bool mirrored = false;
    auto it = sketches_.find(def["profile"].value("id", ""));
    if (it != sketches_.end()) mirrored = it->second.mirrored;
    FeatureOperations op = cut ? CutFeatureOperation : (def.value("merge", true) ? JoinFeatureOperation : NewBodyFeatureOperation);
    if (!cut && bodies(true).empty()) op = NewBodyFeatureOperation;
    Ptr<RevolveFeatures> rf = comp_->features()->revolveFeatures();
    Ptr<RevolveFeatureInput> in = rf->createInput(prof, axis, op);
    if (!in) return "revolve input: " + lastError();
    std::string mode = def.value("mode", "oneDirection");
    double a1 = def.value("angle", 360.0), a2 = def.value("angle2", 0.0);
    // The angle runs about the axis, and Fusion's axis may run the other way from the source's. Measured on
    // P20 (45 degrees about an axis the payload gives as start -> end): SolidWorks' unreversed revolve turns the
    // opposite way to Fusion's positive angle about the same direction.
    bool flip = def.value("reverse", false) != (axisSense(def["axis"], axis) > 0);
    (void)mirrored;
    bool ok;
    if (mode == "midPlane") ok = in->setAngleExtent(true, degValue(a1));
    else if (mode == "twoDirection") ok = in->setTwoSideAngleExtent(degValue(flip ? -a1 : a1), degValue(flip ? a2 : -a2));
    else ok = in->setAngleExtent(false, degValue(flip ? -a1 : a1));
    if (!ok) return "revolve extent not accepted: " + lastError();
    if (def.contains("thin")) return replayThinRevolve(nf, def);
    Ptr<RevolveFeature> f = rf->add(in);
    if (!f) return std::string(cut ? "revolve cut" : "revolve") + " failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

// ---------------------------------------------------------------------------------------- fillet / chamfer
std::string Replayer::replayFillet(json& nf, const json& def)
{
    std::string kind = def.value("filletType", "constant");
    if (kind == "face") return replayFaceFillet(nf, def);
    if (kind == "variable") return replayVariableFillet(nf, def);
    if (kind != "constant") return "fillet type " + kind + " not rebuilt";
    std::vector<Ptr<Base>> edges;
    std::string why = resolveEntities(def["edges"], edges);
    if (!why.empty()) return why;
    Ptr<ObjectCollection> oc = ObjectCollection::create();
    for (auto& e : edges) oc->add(e);
    Ptr<FilletFeatures> ff = comp_->features()->filletFeatures();
    Ptr<FilletFeatureInput> in = ff->createInput();
    if (def.contains("radii") && def["radii"].size() == edges.size() && edges.size() > 1)
    {
        for (size_t i = 0; i < edges.size(); i++)
        {
            Ptr<ObjectCollection> one = ObjectCollection::create();
            one->add(edges[i]);
            in->edgeSetInputs()->addConstantRadiusEdgeSet(one, mmValue(def["radii"][i].get<double>()), false);
        }
    }
    else if (!in->edgeSetInputs()->addConstantRadiusEdgeSet(oc, mmValue(def.value("radius", 0.0)), def.value("propagate", false)))
        return "fillet edges not accepted: " + lastError();
    Ptr<FilletFeature> f = ff->add(in);
    if (!f) return "fillet failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

std::string Replayer::replayChamfer(json& nf, const json& def)
{
    std::string type = def.value("chamferType", "");
    if (type == "vertex") return replayVertexChamfer(nf, def);
    std::vector<Ptr<Base>> edges;
    std::string why = resolveEntities(def["edges"], edges);
    if (!why.empty()) return why;
    Ptr<ChamferFeatures> cf = comp_->features()->chamferFeatures();
    Ptr<ChamferFeatureInput> in = cf->createInput2();
    bool prop = def.value("propagate", false);
    const json flips = def.contains("flipped") ? def["flipped"] : json::array();
    // one edge set per edge, so each keeps its own flip (which side the distance is measured on)
    for (size_t i = 0; i < edges.size(); i++)
    {
        Ptr<ObjectCollection> one = ObjectCollection::create();
        one->add(edges[i]);
        bool flipped = i < flips.size() && flips[i].is_boolean() && flips[i].get<bool>();
        bool ok;
        if (type == "angleDistance")
            ok = in->chamferEdgeSets()->addDistanceAndAngleChamferEdgeSet(one, mmValue(def.value("distance", 0.0)), degValue(def.value("angle", 45.0)), flipped, prop);
        else if (type == "distanceDistance")
            ok = in->chamferEdgeSets()->addTwoDistancesChamferEdgeSet(one, mmValue(def.value("distance", 0.0)), mmValue(def.value("distance2", 0.0)), flipped, prop);
        else
            ok = in->chamferEdgeSets()->addEqualDistanceChamferEdgeSet(one, mmValue(def.value("distance", 0.0)), prop);
        if (!ok) return "chamfer edge not accepted: " + lastError();
    }
    Ptr<ChamferFeature> f = cf->add(in);
    if (!f) return "chamfer failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

// ---------------------------------------------------------------------------------------- reference geometry
std::string Replayer::replayPlane(json& nf, const json& def)
{
    Ptr<ConstructionPlanes> planes = comp_->constructionPlanes();
    Ptr<ConstructionPlaneInput> in = planes->createInput();
    const json refs = def.contains("references") ? def["references"] : json::array();
    std::string why;
    bool set = false;
    V3 wantO = def.contains("origin") ? v3(def["origin"]) : V3{0, 0, 0};
    V3 wantN = def.contains("normal") ? unit(v3(def["normal"])) : V3{0, 0, 1};
    // an offset from one plane (constraint "distance"; 8 in files from the SolidWorks add-in 0.4.0 and earlier):
    // the one case the samples use. The sign comes from where the payload puts the plane, so `flip` needs no
    // separate handling.
    if (refs.size() == 1 && neutral::planeConstraint(refs[0]) == "distance")
    {
        const json& r = refs[0]["ref"].contains("ref") ? refs[0]["ref"]["ref"] : refs[0]["ref"];
        Ptr<Base> base = refObject(r, why);
        if (!base) return "plane reference: " + why;
        // the offset's sign: the side the resulting plane lies on, measured along the base plane's normal
        V3 bo{0, 0, 0}, bn{0, 0, 1};
        if (Ptr<ConstructionPlane> cp = base)
        {
            Ptr<Plane> g = cp->geometry();
            bo = ptMm(g->origin());
            bn = unit(vecOf(g->normal()));
        }
        else if (Ptr<BRepFace> f = base)
        {
            Ptr<Plane> g = f->geometry();
            if (!g) return "plane reference face is not planar";
            bo = ptMm(g->origin());
            bn = unit(vecOf(g->normal()));
        }
        double d = dot(sub(wantO, bo), bn);
        set = in->setByOffset(base, mmValue(d));
    }
    bool byPosition = false;
    if (!set && def.contains("origin") && def.contains("normal"))
    {
        // no usable reference: the plane where the payload puts it, as an offset of the parallel origin plane
        struct
        {
            V3 n;
            Ptr<ConstructionPlane> p;
        } bases[3] = {{{0, 0, 1}, comp_->xYConstructionPlane()}, {{0, 1, 0}, comp_->xZConstructionPlane()}, {{1, 0, 0}, comp_->yZConstructionPlane()}};
        for (auto& bp : bases)
            if (!set && std::fabs(std::fabs(dot(bp.n, wantN)) - 1) < 1e-9) set = byPosition = in->setByOffset(bp.p, mmValue(dot(wantO, bp.n)));
    }
    if (!set && def.contains("origin") && def.contains("normal"))
    {
        // any other plane the file places: through three points of it (sketch points on a hidden helper sketch)
        V3 t1 = unit(cross(wantN, std::fabs(wantN[2]) < 0.9 ? V3{0, 0, 1} : V3{1, 0, 0}));
        V3 t2 = cross(wantN, t1);
        Ptr<Sketch> sk = comp_->sketches()->add(comp_->xYConstructionPlane());
        if (sk)
        {
            sk->name(nf.value("name", "") + " points");
            V3 at[3] = {wantO, add(wantO, mul(t1, 10)), add(wantO, mul(t2, 10))};
            Ptr<SketchPoint> q[3];
            for (int k = 0; k < 3; k++) q[k] = sk->sketchPoints()->add(sk->modelToSketchSpace(ptCm(at[k])));
            sk->isVisible(false);
            set = byPosition = in->setByThreePoints(q[0], q[1], q[2]);
        }
    }
    if (!set) return "reference plane definition not supported (" + std::to_string(refs.size()) + " references)";
    Ptr<ConstructionPlane> p = planes->add(in);
    if (!p) return "plane failed: " + lastError();
    if (byPosition)
    {
        nf["status"] = "approximate";
        nf["note"] = "placed where the file puts it (an origin plane offset, or three points of it): its own references are not carried";
    }
    name(p, nf.value("name", ""));
    features[nf.value("id", "")] = p;
    return "";
}

std::string Replayer::replayAxis(json& nf, const json& def)
{
    Ptr<ConstructionAxes> axes = comp_->constructionAxes();
    Ptr<ConstructionAxisInput> in = axes->createInput();
    const json refs = def.contains("references") ? def["references"] : json::array();
    std::string why;
    bool set = false;
    if (refs.size() == 1)
    {
        Ptr<Base> r = refObject(refs[0], why);
        if (!r) return "axis reference: " + why;
        if (Ptr<BRepFace> f = r) set = in->setByCircularFace(f);
        else if (Ptr<BRepEdge> e = r) set = in->setByEdge(e);
        else if (Ptr<SketchLine> l = r) set = in->setByEdge(l);
    }
    else if (refs.size() == 2)
    {
        Ptr<Base> a = refObject(refs[0], why);
        Ptr<Base> b = a ? refObject(refs[1], why) : nullptr;
        if (!a || !b) return "axis reference: " + why;
        if (Ptr<BRepVertex>(a) || Ptr<ConstructionPoint>(a) || Ptr<SketchPoint>(a)) set = in->setByTwoPoints(a, b);
        else set = in->setByTwoPlanes(a, b);
    }
    if (!set) return "axis definition not supported: " + lastError();
    Ptr<ConstructionAxis> ax = axes->add(in);
    if (!ax) return "axis failed: " + lastError();
    name(ax, nf.value("name", ""));
    features[nf.value("id", "")] = ax;
    return "";
}

std::string Replayer::replayPoint(json& nf, const json& def)
{
    Ptr<ConstructionPoints> pts = comp_->constructionPoints();
    Ptr<ConstructionPointInput> in = pts->createInput();
    const json refs = def.contains("references") ? def["references"] : json::array();
    std::string why;
    bool set = false;
    if (refs.size() == 1)
    {
        Ptr<Base> r = refObject(refs[0], why);
        if (!r) return "point reference: " + why;
        if (Ptr<BRepVertex>(r) || Ptr<SketchPoint>(r)) set = in->setByPoint(r);
        else if (Ptr<BRepEdge> e = r) set = in->setByCenter(e);
        else if (Ptr<BRepFace> f = r)
            return replayFacePoint(nf, def, f);
    }
    if (!set) return "point definition not supported: " + lastError();
    Ptr<ConstructionPoint> p = pts->add(in);
    if (!p) return "point failed: " + lastError();
    name(p, nf.value("name", ""));
    features[nf.value("id", "")] = p;
    return "";
}

} // namespace ncxf
