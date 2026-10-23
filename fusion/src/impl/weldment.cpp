// Replay: weldment structural members (functional equivalent; Fusion has no weldment environment).
//
// Each path segment becomes one body: the profile's cross-section, drawn in a sketch on a plane normal to the
// segment at its start (a construction plane on the path, so it follows the path sketch), extruded up to the
// segment's end point. The profile is built from the payload's standard size:
//   square / rectangular tube  W x H x t : outer corner radius 2t, inner t (what the SolidWorks ISO library
//                                          profiles give: 20 x 20 x 2 has 133.70 mm^2, measured on P16)
//   pipe                       D x t     : two circles
// Corners are trimmed by the group's corner treatment (below). Reported approximate: the profile is drawn, not
// taken from a library.
#include "host.h"
#include "neutral.h"
#include "replay.h"

#include <algorithm>
#include <cstdio>
#include <regex>

namespace ncxf {

namespace {

std::vector<double> numbersIn(const std::string& s)
{
    std::vector<double> v;
    std::regex re("([0-9]+(?:\\.[0-9]+)?)");
    for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) v.push_back(std::stod((*it)[1]));
    return v;
}

// a rounded rectangle centred at (cx, cy) in sketch space (cm), half sizes a, b, corner radius r (cm)
void roundedRect(const Ptr<Sketch>& sk, double cx, double cy, double a, double b, double r)
{
    Ptr<SketchLines> L = sk->sketchCurves()->sketchLines();
    Ptr<SketchArcs> A = sk->sketchCurves()->sketchArcs();
    auto P = [&](double x, double y) { return Point3D::create(cx + x, cy + y, 0); };
    if (r <= 1e-9)
    {
        L->addTwoPointRectangle(P(-a, -b), P(a, b));
        return;
    }
    Ptr<SketchLine> l1 = L->addByTwoPoints(P(-a + r, -b), P(a - r, -b));
    Ptr<SketchLine> l2 = L->addByTwoPoints(P(a, -b + r), P(a, b - r));
    Ptr<SketchLine> l3 = L->addByTwoPoints(P(a - r, b), P(-a + r, b));
    Ptr<SketchLine> l4 = L->addByTwoPoints(P(-a, b - r), P(-a, -b + r));
    const double s = std::sqrt(0.5);
    A->addByThreePoints(l1->endSketchPoint(), P(a - r + r * s, -b + r - r * s), l2->startSketchPoint());
    A->addByThreePoints(l2->endSketchPoint(), P(a - r + r * s, b - r + r * s), l3->startSketchPoint());
    A->addByThreePoints(l3->endSketchPoint(), P(-a + r - r * s, b - r + r * s), l4->startSketchPoint());
    A->addByThreePoints(l4->endSketchPoint(), P(-a + r - r * s, -b + r - r * s), l1->startSketchPoint());
}


// One member being built: its path segment, the extensions at either end of the segment (entity start / end,
// mm, positive = longer), and its body once made.
struct Member
{
    Ptr<SketchLine> line;
    V3 a, b;                    // entity start / end (model mm)
    bool chainReversed = false; // the group's chain runs end -> start along this entity
    double extA = 0, extB = 0;  // extensions at a / b
    Ptr<Sketch> sketch;         // its profile sketch
    Ptr<ExtrudeFeature> feature;
    Ptr<BRepBody> body;
    struct Cut
    {
        V3 p, keep;             // miter: plane through p; keep the side `keep` points to
        int coper = -1;         // coped: the member whose outline is cut away
    };
    std::vector<Cut> cuts;
};

V3 boxCentre(const Ptr<BRepBody>& b)
{
    Ptr<BoundingBox3D> bb = b->boundingBox();
    return mul(add(ptMm(bb->minPoint()), ptMm(bb->maxPoint())), 0.5);
}

// The extrusion of a member's profile regions (`filled`: the whole outer outline, for a cope tool) from
// extA behind the segment's start to extB beyond its end. The direction and start-offset sign Fusion uses are
// checked on the body made (its box centre must be at the middle of the extended segment).
Ptr<ExtrudeFeature> extrudeMember(const Ptr<Component>& comp, const Member& m, bool filled, std::string& why)
{
    Ptr<ObjectCollection> prof = ObjectCollection::create();
    for (size_t i = 0; i < m.sketch->profiles()->count(); i++)
    {
        Ptr<Profile> p = m.sketch->profiles()->item(i);
        if (filled || p->profileLoops()->count() == 2) prof->add(p);
    }
    if (prof->count() == 0)
    {
        why = "member profile has no ring region";
        return nullptr;
    }
    V3 u = unit(sub(m.b, m.a));
    double seg = dist(m.a, m.b), len = seg + m.extA + m.extB;
    V3 mid = add(m.a, mul(u, (seg + m.extB - m.extA) / 2));
    Ptr<ExtrudeFeatures> ef = comp->features()->extrudeFeatures();
    for (ExtentDirections dir : {PositiveExtentDirection, NegativeExtentDirection})
        for (double sgn : {1.0, -1.0})
        {
            if (m.extA == 0 && sgn < 0) continue;
            Ptr<ExtrudeFeatureInput> in = ef->createInput(prof, NewBodyFeatureOperation);
            in->setOneSideExtent(DistanceExtentDefinition::create(ValueInput::createByReal(mm2cm(len))), dir);
            if (m.extA != 0) in->startExtent(OffsetStartDefinition::create(ValueInput::createByReal(mm2cm(sgn * m.extA))));
            Ptr<ExtrudeFeature> f = ef->add(in);
            if (!f) continue;
            Ptr<BRepBodies> bs = f->bodies();
            if (bs && bs->count() == 1 && dist(boxCentre(bs->item(0)), mid) < 1e-3 * std::max(1.0, len)) return f;
            f->deleteMe();
        }
    why = "member body: " + lastError();
    return nullptr;
}

// Remove the part of `body` on the far side of the plane through p with normal `keep` (kept: dot > 0). Returns
// the kept piece through `kept`.
std::string trimByPlane(const Ptr<Component>& comp, const Ptr<BRepBody>& body, const V3& p, const V3& keep, const std::string& name,
                        Ptr<BRepBody>& kept)
{
    V3 n = unit(keep);
    // three points of the plane, as sketch points (a 3D position on a sketch on the XY plane)
    V3 t1 = unit(cross(n, std::fabs(n[2]) < 0.9 ? V3{0, 0, 1} : V3{1, 0, 0}));
    V3 t2 = cross(n, t1);
    Ptr<Sketch> sk = comp->sketches()->add(comp->xYConstructionPlane());
    sk->name(name + " points");
    Ptr<SketchPoint> q[3];
    V3 at[3] = {p, add(p, mul(t1, 10)), add(p, mul(t2, 10))};
    for (int k = 0; k < 3; k++) q[k] = sk->sketchPoints()->add(sk->modelToSketchSpace(ptCm(at[k])));
    sk->isVisible(false);
    Ptr<ConstructionPlaneInput> pi = comp->constructionPlanes()->createInput();
    if (!pi->setByThreePoints(q[0], q[1], q[2])) return "corner plane: " + lastError();
    Ptr<ConstructionPlane> pl = comp->constructionPlanes()->add(pi);
    if (!pl) return "corner plane: " + lastError();
    pl->isLightBulbOn(false);
    pl->name(name + " plane");
    Ptr<BoundingBox3D> before = body->boundingBox();
    V3 lo = ptMm(before->minPoint()), hi = ptMm(before->maxPoint());
    Ptr<SplitBodyFeatureInput> si = comp->features()->splitBodyFeatures()->createInput(body, pl, true);
    Ptr<SplitBodyFeature> sf = comp->features()->splitBodyFeatures()->add(si);
    if (!sf) return "corner split: " + lastError();
    // the pieces: the component's bodies inside the split body's old box (the feature's own body list holds
    // only one of them)
    std::vector<Ptr<BRepBody>> drop;
    kept = nullptr;
    for (size_t i = 0; i < comp->bRepBodies()->count(); i++)
    {
        Ptr<BRepBody> b = comp->bRepBodies()->item(i);
        Ptr<BoundingBox3D> bb = b->boundingBox();
        V3 l = ptMm(bb->minPoint()), h = ptMm(bb->maxPoint());
        bool inside = true;
        for (int k = 0; k < 3; k++)
            if (l[k] < lo[k] - 1e-4 || h[k] > hi[k] + 1e-4) inside = false;
        if (!inside) continue;
        if (dot(sub(boxCentre(b), p), n) < 0) drop.push_back(b);
        else kept = b;
    }
    for (auto& b : drop)
    {
        Ptr<RemoveFeature> rf = comp->features()->removeFeatures()->add(b);
        if (!rf) return "corner piece removal: " + lastError();
        rf->name(name);
    }
    if (!kept) return "corner split left nothing on the member's side";
    return "";
}

} // namespace

// Structural members with their corner treatments (FORMAT-SPEC 4, structuralMember). Within a group, segments are
// taken in the group's order as a chain; at a corner where two members meet:
//   none   each member ends where its segment ends;
//   miter  both run past the corner and are cut on the plane bisecting it (gapWithinGroup split between them);
//   butt1  the member whose segment ends at the corner (in the chain's direction) runs to the outside of the
//          corner, the one that starts there stops against it (gapWithinGroup short of it);
//   butt2  the reverse.
// With connectedSegments copedCut the stopping member also runs to the outside and the other member's outline
// (its whole outer section, extruded as it is) is cut out of it. P46 shows the chain direction, not the sketch
// entity's own start -> end, decides which member ends at a corner (ISSUES.md).
std::string Replayer::replayStructuralMember(json& nf, const json& def)
{
    const json& pr = def.value("profile", json::object());
    std::string type = pr.value("type", ""), size = pr.value("size", "");
    std::vector<double> n = numbersIn(size);
    bool tube = type.find("tube") != std::string::npos && n.size() >= 3;
    bool pipe = type.find("pipe") != std::string::npos && n.size() >= 2;
    if (!tube && !pipe) return "structural member profile " + pr.value("standard", "") + " " + type + " " + size + " has no Fusion equivalent here";
    double halfW = n[0] / 2;   // tube W, pipe D
    std::string coping = neutral::field(def, "connectedSegments", "connectedSegments", "simpleCut");
    std::vector<std::string> notes;
    if (tube && std::fabs(n[0] - n[1]) > 1e-9) notes.push_back("rectangular section: corners assume its width lies in the path's plane");
    int made = 0;
    for (auto& g : def.value("groups", json::array()))
    {
        std::vector<Member> ms;
        for (auto& seg : g.value("segments", json::array()))
        {
            std::string why;
            Ptr<Base> e = refObject(seg, why);
            Member m;
            m.line = e;
            if (!m.line) return "member path segment: " + (why.empty() ? "only straight segments are supported" : why);
            m.a = ptMm(m.line->startSketchPoint()->worldGeometry());
            m.b = ptMm(m.line->endSketchPoint()->worldGeometry());
            ms.push_back(m);
        }
        // the chain's direction along each segment: segment i runs towards the point it shares with i+1
        for (size_t i = 0; i + 1 < ms.size(); i++)
        {
            Member &p = ms[i], &q = ms[i + 1];
            if (dist(p.a, q.a) < 1e-6 || dist(p.a, q.b) < 1e-6) p.chainReversed = true;
            if (i + 2 == ms.size()) q.chainReversed = dist(q.b, p.a) < 1e-6 || dist(q.b, p.b) < 1e-6;
        }
        std::string treat = neutral::field(g, "cornerTreatment", "cornerTreatment", "none");
        double gap = g.value("gapWithinGroup", 0.0);
        if (treat != "none" && treat != "miter" && treat != "butt1" && treat != "butt2")
        {
            notes.push_back("corner treatment '" + treat + "' not known: corners left untrimmed");
            treat = "none";
        }
        if (treat != "none")
            for (size_t i = 0; i < ms.size(); i++)
                for (size_t j = i + 1; j < ms.size(); j++)
                {
                    // a shared end point: the corner
                    bool iAtA = false, jAtA = false, found = false;
                    V3 P{};
                    for (int ei = 0; ei < 2 && !found; ei++)
                        for (int ej = 0; ej < 2 && !found; ej++)
                        {
                            V3 pi_ = ei ? ms[i].b : ms[i].a, pj = ej ? ms[j].b : ms[j].a;
                            if (dist(pi_, pj) < 1e-6)
                            {
                                found = true;
                                P = pi_;
                                iAtA = ei == 0;
                                jAtA = ej == 0;
                            }
                        }
                    if (!found) continue;
                    V3 ui = iAtA ? unit(sub(ms[i].b, ms[i].a)) : unit(sub(ms[i].a, ms[i].b));   // from P into member i
                    V3 uj = jAtA ? unit(sub(ms[j].b, ms[j].a)) : unit(sub(ms[j].a, ms[j].b));
                    if (norm(cross(ui, uj)) < 1e-6) continue;   // a straight continuation: no corner
                    double& exI = iAtA ? ms[i].extA : ms[i].extB;
                    double& exJ = jAtA ? ms[j].extA : ms[j].extB;
                    if (treat == "miter")
                    {
                        // both run past the corner (far enough for the angle), then each keeps its side of the bisector
                        double half = std::acos(std::clamp(dot(ui, uj), -1.0, 1.0)) / 2;
                        exI = exJ = halfW / std::max(0.2, std::tan(half)) + halfW;
                        V3 nrm = unit(sub(ui, uj));
                        ms[i].cuts.push_back({add(P, mul(nrm, gap / 2)), nrm, -1});
                        ms[j].cuts.push_back({add(P, mul(nrm, -gap / 2)), mul(nrm, -1), -1});
                        continue;
                    }
                    if (std::fabs(dot(ui, uj)) > 1e-6)
                    {
                        notes.push_back("a butt corner that is not square left untrimmed");
                        continue;
                    }
                    // which member ends at P along the chain
                    bool iEnds = iAtA == ms[i].chainReversed, jEnds = jAtA == ms[j].chainReversed;
                    if (iEnds == jEnds)
                    {
                        notes.push_back(std::string("a corner where both segments ") + (iEnds ? "end" : "start") + ": the entity directions decide");
                        iEnds = !iAtA;
                        jEnds = !jAtA;
                        if (iEnds == jEnds) continue;
                    }
                    bool iRuns = (treat == "butt1") == iEnds;   // butt1: the ending member runs to the outside
                    size_t runner = iRuns ? i : j, stopper = iRuns ? j : i;
                    double& exRun = iRuns ? exI : exJ;
                    double& exStop = iRuns ? exJ : exI;
                    exRun = halfW;
                    if (coping == "copedCut")
                    {
                        exStop = halfW;
                        ms[stopper].cuts.push_back({P, V3{0, 0, 0}, static_cast<int>(runner)});
                        if (gap > 0) notes.push_back("coped corner gap of " + std::to_string(gap) + " mm not applied");
                    }
                    else
                        exStop = -(halfW + gap);
                }
        // the members
        for (auto& m : ms)
        {
            Ptr<ConstructionPlaneInput> pi = comp_->constructionPlanes()->createInput();
            if (!pi->setByDistanceOnPath(m.line, ValueInput::createByReal(0))) return "member plane: " + lastError();
            Ptr<ConstructionPlane> pl = comp_->constructionPlanes()->add(pi);
            if (!pl) return "member plane: " + lastError();
            pl->isLightBulbOn(false);
            pl->name(nf.value("name", "") + " plane " + std::to_string(made + 1));
            m.sketch = comp_->sketches()->add(pl);
            if (!m.sketch) return "member profile sketch: " + lastError();
            m.sketch->name(nf.value("name", "") + " profile " + std::to_string(made + 1));
            Ptr<Point3D> c = m.sketch->modelToSketchSpace(m.line->startSketchPoint()->worldGeometry());
            if (tube)
            {
                double W = mm2cm(n[0]), H = mm2cm(n[1]), t = mm2cm(n[2]);
                roundedRect(m.sketch, c->x(), c->y(), W / 2, H / 2, 2 * t);
                roundedRect(m.sketch, c->x(), c->y(), W / 2 - t, H / 2 - t, t);
            }
            else
            {
                double D = mm2cm(n[0]), t = mm2cm(n[1]);
                m.sketch->sketchCurves()->sketchCircles()->addByCenterRadius(c, D / 2);
                m.sketch->sketchCurves()->sketchCircles()->addByCenterRadius(c, D / 2 - t);
            }
            std::string why;
            m.feature = extrudeMember(comp_, m, false, why);
            if (!m.feature) return why;
            m.feature->name(made == 0 ? nf.value("name", "") : nf.value("name", "") + "[" + std::to_string(made + 1) + "]");
            m.body = m.feature->bodies()->item(0);
            if (made == 0) features[nf.value("id", "")] = m.feature;
            made++;
        }
        // the corner cuts
        int k = 0;
        for (auto& m : ms)
            for (auto& cut : m.cuts)
            {
                std::string cname = nf.value("name", "") + " corner " + std::to_string(++k);
                if (cut.coper < 0)
                {
                    Ptr<BRepBody> kept;
                    std::string w = trimByPlane(comp_, m.body, cut.p, cut.keep, cname, kept);
                    if (!w.empty()) return w;
                    m.body = kept;
                }
                else
                {
                    std::string why;
                    Ptr<ExtrudeFeature> tool = extrudeMember(comp_, ms[cut.coper], true, why);
                    if (!tool) return "cope tool: " + why;
                    tool->name(cname + " outline");
                    Ptr<ObjectCollection> tools = ObjectCollection::create();
                    tools->add(tool->bodies()->item(0));
                    Ptr<BoundingBox3D> before = m.body->boundingBox();
                    V3 lo = ptMm(before->minPoint()), hi = ptMm(before->maxPoint());
                    Ptr<CombineFeatureInput> ci = comp_->features()->combineFeatures()->createInput(m.body, tools);
                    ci->operation(CutFeatureOperation);
                    ci->isKeepToolBodies(false);
                    Ptr<CombineFeature> cf = comp_->features()->combineFeatures()->add(ci);
                    if (!cf) return "cope: " + lastError();
                    cf->name(cname);
                    // the cope can leave slivers beyond the other member's rounded edges, detached from the
                    // member: only the largest piece is the member (as the source's coped member shows)
                    std::vector<Ptr<BRepBody>> pieces;
                    for (size_t i = 0; i < comp_->bRepBodies()->count(); i++)
                    {
                        Ptr<BRepBody> b = comp_->bRepBodies()->item(i);
                        Ptr<BoundingBox3D> bb = b->boundingBox();
                        V3 l = ptMm(bb->minPoint()), h = ptMm(bb->maxPoint());
                        bool inside = true;
                        for (int q = 0; q < 3; q++)
                            if (l[q] < lo[q] - 1e-4 || h[q] > hi[q] + 1e-4) inside = false;
                        if (inside) pieces.push_back(b);
                    }
                    std::sort(pieces.begin(), pieces.end(), [](const Ptr<BRepBody>& x, const Ptr<BRepBody>& y) { return x->volume() > y->volume(); });
                    if (!pieces.empty()) m.body = pieces[0];
                    for (size_t i = 1; i < pieces.size(); i++)
                    {
                        Ptr<RemoveFeature> rf = comp_->features()->removeFeatures()->add(pieces[i]);
                        if (rf) rf->name(cname + " sliver");
                    }
                }
            }
        if (treat != "none") notes.push_back("corners '" + treat + "'" + (coping == "copedCut" && treat != "miter" ? ", coped" : "") + " (" + std::to_string(k) + " cut feature(s))");
    }
    if (made == 0) return "structural member without segments";
    char note[256];
    std::snprintf(note, sizeof note, "%d member(s) built as extrusions of a drawn %s %s profile (Fusion has no weldments)", made, type.c_str(), size.c_str());
    std::string all = note;
    for (auto& x : notes) all += "; " + x;
    nf["status"] = "approximate";
    nf["note"] = all;
    looseReason = "weldment sections are drawn from their size, not taken from the source's profile library";
    return "";
}

} // namespace ncxf
