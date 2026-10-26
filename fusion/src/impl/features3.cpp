// Replay: features Fusion has no direct counterpart for, built as their functional equivalents (each reported
// approximate with the reason), and Hole Wizard holes.
#include "host.h"
#include "neutral.h"
#include "replay.h"

#include <algorithm>

namespace ncxf {

namespace {

Ptr<ValueInput> mmValue(double mm) { return ValueInput::createByReal(mm2cm(mm)); }
Ptr<ValueInput> degValue(double deg) { return ValueInput::createByReal(deg2rad(deg)); }

void approx(json& nf, const std::string& note)
{
    nf["status"] = "approximate";
    nf["note"] = nf.contains("note") && !nf["note"].get<std::string>().empty() ? nf["note"].get<std::string>() + "; " + note : note;
}

} // namespace

// ---------------------------------------------------------------------------------------- vertex chamfer
// SolidWorks' vertex chamfer cuts the corner off with the plane through three points, at the given distances
// along the corner's three edges. Fusion has no vertex chamfer: the body is split by that plane (through three
// sketch points) and the corner piece removed. `legs` (one {direction, distance} per edge at the vertex) say
// which distance belongs to which edge; files without them give only `distances`, and the order is chosen whose
// plane is a face of the file's own geometry.
std::string Replayer::replayVertexChamfer(json& nf, const json& def)
{
    std::string why;
    Ptr<Base> vo = def.contains("vertex") ? refObject(def["vertex"], why) : nullptr;
    Ptr<BRepVertex> v = vo;
    if (!v) return "chamfer vertex: " + why;
    std::vector<double> d;
    for (auto& x : def.value("distances", json::array())) d.push_back(x.get<double>());
    if (d.size() != 3 && def.contains("legs") && def["legs"].size() == 3)
    {
        d.clear();
        for (auto& l : def["legs"]) d.push_back(l.value("distance", 0.0));
    }
    if (d.size() != 3) return "vertex chamfer needs three distances";
    V3 p0 = ptMm(v->geometry());
    std::vector<V3> dirs;
    Ptr<BRepEdges> es = v->edges();
    for (size_t i = 0; i < es->count(); i++)
    {
        Ptr<BRepEdge> e = es->item(i);
        V3 a = ptMm(e->startVertex()->geometry()), b = ptMm(e->endVertex()->geometry());
        dirs.push_back(unit(dist(a, p0) < dist(b, p0) ? sub(b, a) : sub(a, b)));
    }
    if (dirs.size() != 3) return "the chamfer vertex does not join three edges";
    std::vector<int> perm{0, 1, 2};
    std::vector<int> best = perm;
    bool found = false;
    bool byLegs = false;
    if (def.contains("legs") && def["legs"].size() == 3)
    {
        // each leg's direction picks its edge
        std::vector<int> pick(3, -1);
        for (int k = 0; k < 3; k++)
        {
            V3 want = unit(v3(def["legs"][k]["direction"]));
            for (int e = 0; e < 3; e++)
                if (dot(dirs[e], want) > 0.999) pick[k] = e;
        }
        if (pick[0] >= 0 && pick[1] >= 0 && pick[2] >= 0 && pick[0] != pick[1] && pick[1] != pick[2] && pick[0] != pick[2])
        {
            for (int k = 0; k < 3; k++)
            {
                best[pick[k]] = k;
                d[k] = def["legs"][k].value("distance", d[k]);
            }
            found = byLegs = true;
        }
    }
    if (!found) do
    {
        V3 q[3];
        for (int k = 0; k < 3; k++) q[k] = add(p0, mul(dirs[k], d[perm[k]]));
        V3 n = unit(cross(sub(q[1], q[0]), sub(q[2], q[0])));
        if (refBodies)
            for (auto& rb : *refBodies)
                for (auto& f : rb.value("faces", json::array()))
                {
                    const json& g = f["geom"];
                    if (g.value("type", "") != "plane") continue;
                    V3 rn = unit(v3(g["params"]["normal"])), rp = v3(g["params"]["point"]);
                    if (std::fabs(std::fabs(dot(rn, n)) - 1) < 1e-6 && std::fabs(dot(sub(q[0], rp), rn)) < 1e-4)
                    {
                        best = perm;
                        found = true;
                    }
                }
    } while (!found && std::next_permutation(perm.begin(), perm.end()));
    // three points on a sketch, the plane through them, split, remove the corner piece
    Ptr<Sketch> sk = comp_->sketches()->add(comp_->xYConstructionPlane());
    sk->name(nf.value("name", "") + " points");
    Ptr<ObjectCollection> pts = ObjectCollection::create();
    for (int k = 0; k < 3; k++)
    {
        Ptr<SketchPoint> sp = sk->sketchPoints()->add(sk->modelToSketchSpace(ptCm(add(p0, mul(dirs[k], d[best[k]])))));
        pts->add(sp);
    }
    sk->isVisible(false);
    Ptr<ConstructionPlaneInput> pi = comp_->constructionPlanes()->createInput();
    if (!pi->setByThreePoints(pts->item(0), pts->item(1), pts->item(2))) return "chamfer plane: " + lastError();
    Ptr<ConstructionPlane> pl = comp_->constructionPlanes()->add(pi);
    if (!pl) return "chamfer plane: " + lastError();
    pl->isLightBulbOn(false);
    Ptr<BRepBody> body = v->body();
    double before = body->volume();
    Ptr<SplitBodyFeatureInput> si = comp_->features()->splitBodyFeatures()->createInput(body, pl, true);
    Ptr<SplitBodyFeature> sf = comp_->features()->splitBodyFeatures()->add(si);
    if (!sf) return "chamfer split: " + lastError();
    // the corner piece: the smaller of the split's bodies
    Ptr<BRepBodies> parts = sf->bodies();
    Ptr<BRepBody> corner;
    for (size_t i = 0; parts && i < parts->count(); i++)
        if (!corner || parts->item(i)->volume() < corner->volume()) corner = parts->item(i);
    if (!corner || corner->volume() > before / 2) return "chamfer split made no corner piece";
    Ptr<RemoveFeature> rf = comp_->features()->removeFeatures()->add(corner);
    if (!rf) return "chamfer corner removal: " + lastError();
    name(rf, nf.value("name", ""));
    features[nf.value("id", "")] = rf;
    approx(nf, std::string("vertex chamfer rebuilt as a split by the plane through its three points and removal of the corner (Fusion has no vertex chamfer)") +
                   (byLegs ? "" : found ? "; distance order taken from the file's geometry (no legs)" : "; distance order not confirmed by the file's geometry"));
    return "";
}

// ---------------------------------------------------------------------------------------- point at a face
// A point at the centre of a planar face (SolidWorks reference point type 4): Fusion's construction points
// have no face-centre definition. A sketch point on the face at the recorded position stands in for it; it
// stays on the face but does not follow the face's centre when the face changes size.
std::string Replayer::replayFacePoint(json& nf, const json& def, const Ptr<BRepFace>& face)
{
    if (!def.contains("point")) return "face point without a position";
    Ptr<Sketch> sk = comp_->sketches()->add(face);
    if (!sk) return "sketch on the face: " + lastError();
    sk->name(nf.value("name", "") + " position");
    std::vector<Ptr<SketchCurve>> auto_;
    for (size_t i = 0; i < sk->sketchCurves()->count(); i++)
        if (sk->sketchCurves()->item(i)->isReference()) auto_.push_back(sk->sketchCurves()->item(i));
    for (auto& c : auto_) c->deleteMe();
    Ptr<SketchPoint> sp = sk->sketchPoints()->add(sk->modelToSketchSpace(ptCm(v3(def["point"]))));
    sk->isVisible(false);
    Ptr<ConstructionPointInput> in = comp_->constructionPoints()->createInput();
    if (!in->setByPoint(sp)) return "point: " + lastError();
    Ptr<ConstructionPoint> p = comp_->constructionPoints()->add(in);
    if (!p) return "point: " + lastError();
    name(p, nf.value("name", ""));
    features[nf.value("id", "")] = p;
    approx(nf, "point at the centre of a face rebuilt as a point at the same position on that face (Fusion has no face-centre point)");
    return "";
}

// ---------------------------------------------------------------------------------------- coordinate system
// Fusion has no coordinate system feature; a joint origin at the same point with the same x axis is the
// equivalent a Fusion user places components and joints with.
std::string Replayer::replayCoordinateSystem(json& nf, const json& def)
{
    std::string why;
    Ptr<Base> o = def.contains("origin") ? refObject(def["origin"], why) : nullptr;
    if (!o) return "coordinate system origin: " + why;
    Ptr<JointGeometry> g = JointGeometry::createByPoint(o);
    if (!g) return "joint origin geometry: " + lastError();
    Ptr<JointOriginInput> in = comp_->jointOrigins()->createInput(g);
    if (def.contains("xAxis") && !def["xAxis"].empty())
    {
        Ptr<Base> x = refObject(def["xAxis"][0], why);
        if (x) in->xAxisEntity(x);
    }
    Ptr<JointOrigin> jo = comp_->jointOrigins()->add(in);
    if (!jo) return "joint origin: " + lastError();
    jo->name(nf.value("name", ""));
    features[nf.value("id", "")] = jo;
    markers.insert(nf.value("id", ""));
    approx(nf, "coordinate system rebuilt as a joint origin at the same point (Fusion has no coordinate system feature)");
    return "";
}

// ---------------------------------------------------------------------------------------- thin revolve
// Fusion's revolve has no thin option: the open profile is offset by the wall thickness in its sketch, closed
// at both ends, and that region revolved. The wall lies on the same side as SolidWorks puts a one-direction
// thin wall: to the right of the profile's direction about the source sketch normal (see FINDINGS, P19).
std::string Replayer::replayThinRevolve(json& nf, const json& def)
{
    std::string why;
    auto it = sketches_.find(def["profile"].value("id", ""));
    if (it == sketches_.end()) return "profile sketch not rebuilt";
    SketchMap& m = it->second;
    Ptr<Base> axis = def.contains("axis") ? refObject(def["axis"], why) : nullptr;
    if (!axis) return "axis: " + why;
    const json& th = def["thin"];
    std::string tt = th.value("type", "oneDirection");
    double k1 = th.value("thickness", 0.0), k2 = th.value("thickness2", 0.0);
    std::vector<Ptr<SketchLine>> lines;
    Ptr<SketchCurves> cs = m.sketch->sketchCurves();
    for (size_t i = 0; i < cs->count(); i++)
        if (!cs->item(i)->isConstruction() && !cs->item(i)->isReference())
        {
            Ptr<SketchLine> l = cs->item(i);
            if (!l) return "thin revolve of a profile other than one line not supported yet";
            lines.push_back(l);
        }
    if (lines.size() != 1) return "thin revolve of a profile other than one line not supported yet";
    V3 a = m.sketchToModel(lines[0]->startSketchPoint()->geometry()), b = m.sketchToModel(lines[0]->endSketchPoint()->geometry());
    // payload direction: the line as the payload has it (start -> end)
    for (auto& e : sketchDefs_[def["profile"].value("id", "")]["entities"])
        if (e.value("type", "") == "line")
        {
            a = m.src.toModel(e["start"][0].get<double>(), e["start"][1].get<double>());
            b = m.src.toModel(e["end"][0].get<double>(), e["end"][1].get<double>());
        }
    V3 right = unit(cross(unit(sub(b, a)), m.src.z));
    double off1 = 0, off2 = 0;   // wall from off1 to off2 along `right`
    if (tt == "midPlane") off1 = -k1 / 2, off2 = k1 / 2;
    else if (tt == "twoDirection") off1 = -k2, off2 = k1;
    else if (tt == "oppositeDirection") off1 = -k1, off2 = 0;
    else off1 = 0, off2 = k1;
    V3 q[4] = {add(a, mul(right, off1)), add(b, mul(right, off1)), add(b, mul(right, off2)), add(a, mul(right, off2))};
    Ptr<SketchLines> sl = m.sketch->sketchCurves()->sketchLines();
    std::vector<Ptr<SketchLine>> made;
    Ptr<SketchPoint> first, prev;
    for (int k = 0; k < 4; k++)
    {
        Ptr<Base> from = prev ? Ptr<Base>(prev) : Ptr<Base>(m.toSketch(q[k]));
        Ptr<Base> to = k == 3 ? Ptr<Base>(first) : Ptr<Base>(m.toSketch(q[(k + 1) % 4]));
        Ptr<SketchLine> l = sl->addByTwoPoints(from, to);
        if (!l) return "thin wall outline: " + lastError();
        if (!first) first = l->startSketchPoint();
        prev = l->endSketchPoint();
        made.push_back(l);
    }
    lines[0]->isConstruction(true);   // the source's open profile stays, as the wall's reference line
    Ptr<Profiles> ps = m.sketch->profiles();
    if (!ps || ps->count() == 0) return "thin wall outline made no profile";
    Ptr<ObjectCollection> prof = ObjectCollection::create();
    prof->add(ps->item(0));
    FeatureOperations op = def.value("merge", true) && !bodies(true).empty() ? JoinFeatureOperation : NewBodyFeatureOperation;
    Ptr<RevolveFeatures> rf = comp_->features()->revolveFeatures();
    Ptr<RevolveFeatureInput> in = rf->createInput(prof, axis, op);
    double ang = def.value("angle", 360.0);
    bool flip = def.value("reverse", false) != (axisSense(def["axis"], axis) > 0);
    if (def.value("mode", "") == "midPlane") in->setAngleExtent(true, degValue(ang));
    else in->setAngleExtent(false, degValue(flip ? -ang : ang));
    Ptr<RevolveFeature> f = rf->add(in);
    if (!f) return "thin revolve failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    approx(nf, "thin revolve rebuilt as a revolve of the wall's outline drawn in its sketch (Fusion's revolve has no thin option)");
    return "";
}

// ---------------------------------------------------------------------------------------- Hole Wizard
// Fusion's hole feature, with the position sketch the payload carries (FORMAT-SPEC 4, `hole`). The kind and end
// come from `holeType` and `end` (words; files from the SolidWorks add-in 0.4.0 and earlier only have the
// wizard's own numbers, read through the same tables). A size the payload gives as 0 is taken from
// `sizes.measuredDiameters` (the hole's cylinder diameters, ascending), else measured on the file's own geometry:
// the cylinder coaxial with the hole that is not the counterbore.
std::string Replayer::replayHole(json& nf, const json& def)
{
    std::string why;
    Ptr<Base> fo = def.contains("face") ? refObject(def["face"], why) : nullptr;
    Ptr<BRepFace> face = fo;
    if (!face) return "hole face: " + why;
    const json& sz = def.contains("sizes") ? def["sizes"] : json::object();
    const json& wz = def.contains("wizard") ? def["wizard"] : json::object();
    std::string kind = neutral::field(def, "holeType", "holeKind");
    std::string endType;
    if (def.contains("end")) endType = neutral::field(def["end"], "type", "endCondition");
    else endType = neutral::field(wz, "endCondition", "endCondition", "blind");
    double cbD = sz.value("counterBoreDiameter", 0.0), cbDepth = sz.value("counterBoreDepth", 0.0);
    double csD = sz.value("counterSinkDiameter", 0.0), csA = sz.value("counterSinkAngle", 0.0);
    double holeD = sz.value("holeDiameter", 0.0);
    if (holeD <= 0) holeD = sz.value("diameter", 0.0);
    if (holeD <= 0) holeD = sz.value("tapDrillDiameter", 0.0);
    std::vector<double> measured;
    for (auto& d : sz.value("measuredDiameters", json::array()))
        if (d.is_number()) measured.push_back(d.get<double>());
    if (!measured.empty())
    {
        if (holeD <= 0) holeD = measured.front();
        if ((kind == "counterbore" || kind == "counterboreSlot") && cbD <= 0 && measured.size() > 1) cbD = measured.back();
    }
    if (kind.empty() && cbD > 0) kind = "counterbore";
    if (kind.empty() && csD > 0) kind = "countersink";
    if (kind.empty()) kind = "simple";
    if (kind == "slot" || kind == "counterboreSlot" || kind == "countersinkSlot") return "slot holes have no Fusion hole counterpart";
    std::vector<V3> pts;
    for (auto& p : def["points"]) pts.push_back(v3(p));
    if (pts.empty()) return "hole without positions";
    // outward normal of the face at the first point: the hole runs against it
    Ptr<Point3D> at;
    V3 n{0, 0, 1};
    {
        Ptr<SurfaceEvaluator> ev = face->evaluator();
        Ptr<Point2D> uv;
        Ptr<Vector3D> nv;
        if (ev->getParameterAtPoint(ptCm(pts[0]), uv) && ev->getNormalAtParameter(uv, nv)) n = unit(vecOf(nv));
    }
    double depth = def.contains("end") ? def["end"].value("depth", 0.0) : wz.value("depth", 0.0);
    bool through = endType == "throughAll" || endType == "throughAllBoth";
    if (!through && endType != "blind" && depth <= 0) depth = 0;   // up to a face/next: its depth is measured below
    if ((holeD <= 0 || (depth <= 0 && !through)) && refBodies)
    {
        // measure on the file's geometry: radii of cylinders coaxial with the hole
        std::vector<double> radii;
        double reach = 0;
        for (auto& rb : *refBodies)
            for (auto& f : rb.value("faces", json::array()))
            {
                const json& g = f["geom"];
                if (g.value("type", "") != "cylinder") continue;
                V3 ax = unit(v3(g["params"]["axis"])), o = v3(g["params"]["origin"]);
                if (norm(cross(ax, n)) > 1e-6) continue;
                V3 w = sub(pts[0], o);
                double off = norm(sub(w, mul(ax, dot(w, ax))));
                if (off > 1e-3) continue;
                radii.push_back(g["params"]["radius"].get<double>());
                const json& bb = g["bbox"];
                for (int c = 0; c < 8; c++)
                {
                    V3 corner{bb[(c & 1) ? 3 : 0].get<double>(), bb[(c & 2) ? 4 : 1].get<double>(), bb[(c & 4) ? 5 : 2].get<double>()};
                    reach = std::max(reach, -dot(sub(corner, pts[0]), n));
                }
            }
        std::sort(radii.begin(), radii.end());
        for (double r : radii)
            if (holeD <= 0 && std::fabs(2 * r - cbD) > 1e-6 && std::fabs(2 * r - csD) > 1e-6) holeD = 2 * r;
        if (depth <= 0) depth = reach;
        if (holeD > 0) approx(nf, "drilled diameter " + std::to_string(holeD) + " mm measured on the file's geometry (the payload gives 0 and no measuredDiameters)");
    }
    if (holeD <= 0) return "hole diameter unknown (the payload's sizes give 0 and the file's geometry shows no coaxial cylinder)";
    // the position sketch (a sketch on the hole's face), from the payload when it carries one
    Ptr<ObjectCollection> sps = ObjectCollection::create();
    auto dit = deferred_.lower_bound(nf.value("name", "") + "|");
    if (dit != deferred_.end() && dit->first.rfind(nf.value("name", "") + "|", 0) != 0) dit = deferred_.end();
    if (dit != deferred_.end())
    {
        json sk = dit->second;
        std::string w = replaySketch(sk, sk["def"], face);
        if (!w.empty()) return "position sketch: " + w;
        report_.add(docName_, sk.value("name", ""), sk.value("status", "") == "approximate" ? "approximate" : "rebuilt", sk.value("note", ""));
        Ptr<Sketch> s = features[sk.value("id", "")];
        for (auto& e : sk["def"]["entities"])
            if (e.value("type", "") == "point")
            {
                auto pit = sketches_[sk.value("id", "")].ents.find(e["id"]);
                if (pit != sketches_[sk.value("id", "")].ents.end()) sps->add(pit->second);
            }
        deferred_.erase(dit);
    }
    if (sps->count() == 0)
    {
        Ptr<Sketch> s = comp_->sketches()->add(face);
        std::vector<Ptr<SketchCurve>> auto_;
        for (size_t i = 0; i < s->sketchCurves()->count(); i++)
            if (s->sketchCurves()->item(i)->isReference()) auto_.push_back(s->sketchCurves()->item(i));
        for (auto& c : auto_) c->deleteMe();
        for (auto& p : pts) sps->add(s->sketchPoints()->add(s->modelToSketchSpace(ptCm(p))));
    }
    Ptr<HoleFeatures> hf = comp_->features()->holeFeatures();
    if (kind == "tapped" || kind == "pipeTap")
        approx(nf, "tapped hole rebuilt as its tap drill (" + std::to_string(holeD) + " mm); the thread is not modelled");
    if (!through && endType != "blind")
        approx(nf, "end condition " + endType + " rebuilt as a blind depth of " + std::to_string(depth) + " mm measured on the file's geometry");
    // the input, drilled along the position sketch's default direction or against it (an input Fusion refused
    // cannot be used again)
    std::string inWhy;
    auto make = [&](bool against) -> Ptr<HoleFeature> {
        Ptr<HoleFeatureInput> in;
        if (kind == "counterbore" && cbD > 0) in = hf->createCounterboreInput(mmValue(holeD), mmValue(cbD), mmValue(cbDepth));
        else if (kind == "countersink" && csD > 0) in = hf->createCountersinkInput(mmValue(holeD), mmValue(csD), degValue(csA));
        else in = hf->createSimpleInput(mmValue(holeD));
        if (!in) return inWhy = "hole input: " + lastError(), nullptr;
        Ptr<ObjectCollection> at = ObjectCollection::create();   // a fresh selection each time
        for (size_t i = 0; i < sps->count(); i++) at->add(sps->item(i));
        if (!in->setPositionBySketchPoints(at)) return inWhy = "hole positions: " + lastError(), nullptr;
        bool ok = through ? in->setAllExtent(against ? NegativeExtentDirection : PositiveExtentDirection) : in->setDistanceExtent(mmValue(depth));
        if (!ok) return inWhy = "hole extent: " + lastError(), nullptr;
        if (sz.value("drillAngle", 0.0) > 0) in->tipAngle(degValue(sz.value("drillAngle", 118.0)));
        if (against && !through) in->isDefaultDirection(false);
        // the part's solids as its targets: in a sheet-metal component a hole otherwise looks only for
        // sheet-metal bodies and misses a plain body a carried result brought
        if (comp_->activeSheetMetalRule()) in->participantBodies(bodies(true));
        return hf->add(in);
    };
    Ptr<HoleFeature> f = make(false);
    if (!f) logLine("hole " + nf.value("name", "") + " along the sketch normal: " + inWhy + " " + lastError());
    if (!f && inWhy.empty() && lastError().find("NO_TARGET_BODY") != std::string::npos)
    {
        // drilled away from the material: the position sketch's plane faces the other way from the face it lies on
        f = make(true);
        if (!f) logLine("hole " + nf.value("name", "") + " against the sketch normal: " + inWhy + " " + lastError());
        if (f) nf["info"] = "drilled against the position sketch's normal (the face the hole starts on faces the other way)";
    }
    if (!f && !inWhy.empty()) return inWhy;
    if (!f) return "hole failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    if (!wz.value("size", "").empty()) nf["info"] = "Hole Wizard " + wz.value("size", "") + " rebuilt as a Fusion hole";
    return "";
}


// ---------------------------------------------------------------------------------------- variable fillet
// A variable fillet whose radii are given at the ends of each edge: Fusion's variable-radius edge set with a start
// and an end radius (the edge's own start vertex is Fusion's start). Radii at points along an edge are not built
// here (the feature then carries its result).
std::string Replayer::replayVariableFillet(json& nf, const json& def)
{
    std::vector<Ptr<Base>> edges;
    std::string why = resolveEntities(def.value("edges", json::array()), edges);
    if (!why.empty()) return why;
    struct VR
    {
        V3 p;
        double r;
    };
    std::vector<VR> vr;
    for (auto& v : def.value("vertexRadii", json::array()))
    {
        if (!v.contains("vertex") || !v["vertex"].contains("geom")) return "a vertex radius without its vertex";
        vr.push_back({v3(v["vertex"]["geom"]["params"]["point"]), v.value("radius", 0.0)});
    }
    if (def.contains("pointRadii") && !def["pointRadii"].empty()) return "radii at points along an edge are not built";
    // FORMAT-SPEC 4: `transition` says how the radius runs between the vertex radii: `straight` (in proportion to
    // the distance along the edge) or `smooth` (no sudden change of slope at the vertices). Fusion's variable fillet
    // with only end radii runs nearly straight (P30: 80.05 mm^3 removed where the smooth source removes 81.47, as a
    // straight law predicts); intermediate radii on the source's law make Fusion follow it.
    std::string transition = def.value("transition", std::string("smooth"));
    auto law = [&](double t) { return transition == "straight" ? t : t * t * (3 - 2 * t); };
    struct EdgeR
    {
        Ptr<BRepEdge> e;
        double ra, rb;
    };
    std::vector<EdgeR> es;
    for (auto& e : edges)
    {
        Ptr<BRepEdge> ed = e;
        if (!ed) return "a variable fillet on something other than an edge";
        V3 a = ptMm(ed->startVertex()->geometry()), b = ptMm(ed->endVertex()->geometry());
        double ra = -1, rb = -1;
        for (auto& v : vr)
        {
            if (dist(v.p, a) < 1e-3) ra = v.r;
            if (dist(v.p, b) < 1e-3) rb = v.r;
        }
        if (ra <= 0 || rb <= 0) return "a variable fillet edge without radii at both its ends";
        es.push_back({ed, ra, rb});
    }
    Ptr<FilletFeatures> ff = comp_->features()->filletFeatures();
    std::vector<json> before = solidFingerprints();
    // variants: intermediate radii on the source's law (several densities), and Fusion's own law
    std::vector<int> mids = transition == "straight" ? std::vector<int>{0, 9, 19} : std::vector<int>{9, 19, 39, 0};
    std::string detail, exchangeOk;
    int exchangeMids = -1;
    for (int pass = 0; pass < 2; pass++)
        for (int nm : mids)
        {
            if (pass == 1 && nm != exchangeMids) continue;
            Ptr<FilletFeatureInput> in = ff->createInput();
            for (auto& x : es)
            {
                Ptr<ObjectCollection> one = ObjectCollection::create();
                one->add(x.e);
                Ptr<VariableRadiusFilletEdgeSetInput> set =
                    in->edgeSetInputs()->addVariableRadiusEdgeSet(one, mmValue(x.ra), mmValue(x.rb), def.value("propagate", false));
                if (!set) return "variable fillet edge not accepted: " + lastError();
                if (nm > 0)
                {
                    std::vector<Ptr<ValueInput>> rs, ps;
                    for (int i = 1; i <= nm; i++)
                    {
                        double t = double(i) / (nm + 1);
                        rs.push_back(mmValue(x.ra + (x.rb - x.ra) * law(t)));
                        ps.push_back(ValueInput::createByReal(t));
                    }
                    if (!set->setMidRadii(rs, ps)) return "intermediate radii refused: " + lastError();
                }
            }
            Ptr<FilletFeature> f = ff->add(in);
            if (!f) return "variable fillet failed: " + lastError();
            if (!nf.contains("result"))
            {
                name(f, nf.value("name", ""));
                features[nf.value("id", "")] = f;
                if (nm > 0) approx(nf, "the source's " + transition + " radius law followed with " + std::to_string(nm) + " intermediate radii per edge (no result carried to check it)");
                return "";
            }
            bool native = pass == 0 && matchesCarriedResult(nf, before, &detail);
            bool exch = pass == 1 || (!native && matchesCarriedResult(nf, before, nullptr, "exchange"));
            if (native || pass == 1)
            {
                // the intermediate radii follow the end radii (each = start + (end - start) * law(t)), so editing
                // an end radius keeps the source's law
                Ptr<FilletEdgeSets> sets = f->edgeSets();
                for (size_t k = 0; nm > 0 && sets && k < sets->count(); k++)
                {
                    Ptr<VariableRadiusFilletEdgeSet> vs = sets->item(k);
                    if (!vs || !vs->startRadius() || !vs->endRadius() || !vs->midRadii()) continue;
                    std::string s = vs->startRadius()->name(), e = vs->endRadius()->name();
                    // the list is not in order of position: each radius is set from its own position
                    for (size_t i = 0; i < vs->midRadii()->count() && vs->midPositions() && i < vs->midPositions()->count(); i++)
                    {
                        double t = vs->midPositions()->item(i)->value();
                        char buf[200];
                        std::snprintf(buf, sizeof buf, "%s + (%s - %s) * %.12g", s.c_str(), e.c_str(), s.c_str(), law(t));
                        if (!vs->midRadii()->item(i)->expression(buf)) logLine(std::string("mid radius expression refused: ") + buf + ": " + lastError());
                    }
                }
                name(f, nf.value("name", ""));
                features[nf.value("id", "")] = f;
                std::string how = nm > 0 ? "Fusion variable fillet with " + std::to_string(nm) + " intermediate radii per edge on the source's " + transition + " law"
                                         : "Fusion variable fillet with the end radii (Fusion's own law)";
                if (native) nf["info"] = how + " (matches the carried result)";
                else
                    approx(nf, how + ": Fusion interpolates between its radii by its own law, so the fillet matches the source at exchange tolerance only (" +
                                   exchangeOk + ")");
                return "";
            }
            if (exch && exchangeMids < 0)
            {
                exchangeMids = nm;
                exchangeOk = detail;
            }
            f->deleteMe();
            if (pass == 0 && nm == mids.back() && exchangeMids < 0) pass = 2;   // nothing to retry
        }
    // Fusion's variable fillet gives no variant close to the source: the geometry decides, the carried result is used
    return "Fusion's variable fillet does not reproduce the source's radius law (" + detail + ")";
}

// ---------------------------------------------------------------------------------------- rib
// SolidWorks' rib (an open sketch thickened and extended to the body) and Fusion's rib take the same curves,
// thickness and side; the direction of material and the thickness side are chosen by the carried result.
std::string Replayer::replayRib(json& nf, const json& def)
{
    if (!nf.contains("result")) return "rib without a carried result to check the rebuild against";
    auto it = sketches_.find(def["profile"].value("id", ""));
    if (it == sketches_.end()) return "rib sketch not rebuilt";
    std::vector<Ptr<SketchCurve>> curves;
    Ptr<SketchCurves> cs = it->second.sketch->sketchCurves();
    for (size_t i = 0; i < cs->count(); i++)
        if (!cs->item(i)->isConstruction() && !cs->item(i)->isReference()) curves.push_back(cs->item(i));
    if (curves.empty()) return "rib sketch has no curves";
    if (neutral::field(def, "extrusion", "ribExtrusion", "parallelToSketch") != "parallelToSketch")
        return "a rib extruded normal to its sketch is not built here";
    std::vector<Ptr<BRepBody>> solids = bodies(true);
    if (solids.size() != 1) return "rib in a part with " + std::to_string(solids.size()) + " solids";
    double t = def.value("thickness", 0.0);
    bool sym = def.value("twoSided", true);
    std::vector<json> before = solidFingerprints();
    Ptr<RibFeatures> rf = comp_->features()->ribFeatures();
    std::string lastDetail;
    int tries = 0;
    for (bool flip : {false, true})
        for (bool fromTop : {false, true})
        {
            if (sym && fromTop) continue;
            Ptr<RibFeatureInput> in = rf->createInput(curves, mmValue(t), solids[0]);
            if (!in) return "rib input: " + lastError();
            in->isSymmetric(sym);
            if (!sym) in->isThicknessFromTop(fromTop);
            in->setToNextExtent();
            in->isFlipped(flip);
            if (def.contains("draft")) return "rib draft is not built here";
            tries++;
            Ptr<RibFeature> f = rf->add(in);
            if (!f || !matchesCarriedResult(nf, before, &lastDetail))
            {
                if (!f) lastDetail = "refused: " + lastError();
                if (f) f->deleteMe();
                continue;
            }
            name(f, nf.value("name", ""));
            features[nf.value("id", "")] = f;
            nf["info"] = std::string("Fusion rib to the next face") + (flip ? ", flipped" : "") + " (chosen by the carried result; " + std::to_string(tries) + " tried)";
            return "";
        }
    return "no Fusion rib variant gives the source's geometry (" + std::to_string(tries) + " tried; last " + lastDetail + ")";
}

} // namespace ncxf
