// Replay: sketches. Every entity is created at its solved (seed) position, points shared by several
// entities are shared in Fusion too, then relations and dimensions are added (FORMAT-SPEC 4, "Sketch").
//
// Coordinates go source sketch -> model (through the payload's frame) -> target sketch (Fusion's own sketch
// space), so a Fusion sketch whose axes differ from the source's (or whose normal is opposite) still lands
// right. Orientation-dependent items (horizontal / vertical relations and dimensions, arc direction) are
// translated through the same mapping.
#include "host.h"
#include "replay.h"

namespace ncxf {

Ptr<Point3D> SketchMap::toSketch(const V3& modelMm) const { return sketch->modelToSketchSpace(ptCm(modelMm)); }

V3 SketchMap::sketchToModel(const Ptr<Point3D>& p) const { return ptMm(sketch->sketchToModelSpace(p)); }

namespace {

V3 frameAxis(const json& f, const char* k, V3 dflt)
{
    return f.contains(k) ? unit(v3(f[k])) : dflt;
}

double angleOf(double x, double y) { return std::atan2(y, x); }

// Which target sketch axis a source direction runs along: 0 = x, 1 = y, -1 = neither.
int alongAxis(const SketchMap& m, const V3& dirModel)
{
    V3 tx = unit(vecOf(m.sketch->xDirection())), ty = unit(vecOf(m.sketch->yDirection()));
    if (std::fabs(std::fabs(dot(dirModel, tx)) - 1) < 1e-6) return 0;
    if (std::fabs(std::fabs(dot(dirModel, ty)) - 1) < 1e-6) return 1;
    return -1;
}

std::string sanitizeName(const std::string& s)
{
    std::string r;
    for (unsigned char c : s) r += (std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
    if (r.empty() || std::isdigit(static_cast<unsigned char>(r[0]))) r = "_" + r;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------------------------- plane
Ptr<Base> Replayer::planeFor(const json& pr, std::string& why)
{
    std::string kind = pr.value("kind", "");
    if (kind == "originPlane")
    {
        std::string p = pr.value("plane", "");
        if (p == "front") return comp_->xYConstructionPlane();
        if (p == "top") return comp_->xZConstructionPlane();
        if (p == "right") return comp_->yZConstructionPlane();
        why = "unknown origin plane " + p;
        return nullptr;
    }
    if (kind == "feature")
    {
        auto it = features.find(pr.value("id", ""));
        if (it == features.end() || !it->second)
        {
            why = "plane feature " + pr.value("id", "") + " was not rebuilt";
            return nullptr;
        }
        return it->second;
    }
    if (kind == "face")
    {
        std::vector<Ptr<Base>> hit;
        json r = resolveRef(bodies(), pr, hit);
        if (r.value("result", "") != "resolved" || hit.size() != 1)
        {
            why = "sketch face " + r.value("result", "?");
            return nullptr;
        }
        return hit[0];
    }
    if (kind == "frame")
    {
        // only the frame places the sketch: a planar face lying in it, else an origin plane parallel to it
        // (offset to it); both reported through the sketch's note by the caller when needed
        if (!currentFrame_) 
        {
            why = "a frame-only sketch plane without a frame";
            return nullptr;
        }
        V3 o = currentFrame_->o, z = currentFrame_->z;
        for (auto& b : bodies())
            for (size_t i = 0; i < b->faces()->count(); i++)
            {
                Ptr<BRepFace> f = b->faces()->item(i);
                Ptr<Plane> g = f->geometry();
                if (!g) continue;
                V3 n = unit(vecOf(g->normal()));
                if (std::fabs(std::fabs(dot(n, z)) - 1) < 1e-9 && std::fabs(dot(sub(o, ptMm(g->origin())), n)) < 1e-6) return f;
            }
        struct
        {
            V3 n;
            Ptr<ConstructionPlane> p;
        } bases[3] = {{{0, 0, 1}, comp_->xYConstructionPlane()}, {{0, 1, 0}, comp_->xZConstructionPlane()}, {{1, 0, 0}, comp_->yZConstructionPlane()}};
        for (auto& bp : bases)
            if (std::fabs(std::fabs(dot(bp.n, z)) - 1) < 1e-9)
            {
                double off = dot(o, bp.n);
                if (std::fabs(off) < 1e-9) return bp.p;
                Ptr<ConstructionPlaneInput> in = comp_->constructionPlanes()->createInput();
                in->setByOffset(bp.p, ValueInput::createByReal(mm2cm(off)));
                Ptr<ConstructionPlane> cp = comp_->constructionPlanes()->add(in);
                if (cp)
                {
                    cp->isLightBulbOn(false);
                    return cp;
                }
            }
        why = "no face or origin plane lies in the sketch's frame";
        return nullptr;
    }
    if (kind == "transferred")
    {
        // a plane the inserted part brought: the origin planes map to this part's own (identity transform)
        std::string f = pr.value("feature", pr.value("source", ""));
        std::string pl = pr.value("plane", "");
        if (f.rfind("plane:", 0) == 0) pl = f.substr(6);
        if (pl == "front") return comp_->xYConstructionPlane();
        if (pl == "top") return comp_->xZConstructionPlane();
        if (pl == "right") return comp_->yZConstructionPlane();
        auto it = features.find("T:" + pr.value("document", "") + "|" + f);
        if (it != features.end() && it->second) return it->second;
        why = "transferred plane " + pr.value("name", f) + " not available";
        return nullptr;
    }
    why = "sketch plane kind '" + kind + "' not supported yet";
    return nullptr;
}

// ---------------------------------------------------------------------------------------------- entities
Ptr<Base> Replayer::sketchEntityFor(SketchMap& m, const json& e, std::string& why)
{
    if (e.is_string())
    {
        auto it = m.ents.find(e.get<std::string>());
        if (it == m.ents.end())
        {
            why = "sketch entity " + e.get<std::string>() + " missing";
            return nullptr;
        }
        return it->second;
    }
    std::string kind = e.value("kind", "");
    if (kind == "origin") return m.origin;
    if (kind == "sketchEntity")
    {
        auto sit = sketches_.find(e.value("feature", ""));
        if (sit == sketches_.end())
        {
            why = "sketch " + e.value("feature", "") + " was not rebuilt";
            return nullptr;
        }
        auto it = sit->second.ents.find(e.value("id", ""));
        if (it == sit->second.ents.end())
        {
            why = "entity " + e.value("id", "") + " of another sketch missing";
            return nullptr;
        }
        // an entity of another sketch: project it into this one
        Ptr<ObjectCollection> pr = m.sketch->project(it->second);
        if (pr && pr->count() > 0) return pr->item(0);
        why = "could not project an entity of another sketch: " + lastError();
        return nullptr;
    }
    if (kind == "vertex" || kind == "edge")
    {
        std::vector<Ptr<Base>> hit;
        json r = resolveRef(bodies(), e, hit);
        if (r.value("result", "") != "resolved" || hit.size() != 1)
        {
            why = kind + " reference " + r.value("result", "?");
            return nullptr;
        }
        Ptr<ObjectCollection> pr = m.sketch->project(hit[0]);
        if (pr && pr->count() > 0) return pr->item(0);
        why = "could not project the " + kind + ": " + lastError();
        return nullptr;
    }
    if (kind == "originPlane" || kind == "feature")
    {
        Ptr<Base> pl = planeFor(e, why);
        if (!pl) return nullptr;
        // a plane perpendicular to the sketch projects as a line
        Ptr<ObjectCollection> pc = m.sketch->project(pl);
        if (pc && pc->count() > 0) return pc->item(0);
        std::vector<Ptr<SketchEntity>> cut = m.sketch->intersectWithSketchPlane({pl});
        if (!cut.empty()) return cut[0];
        why = "the plane could not be projected into the sketch: " + lastError();
        return nullptr;
    }
    why = "reference kind " + kind + " not supported in sketches";
    return nullptr;
}

// ---------------------------------------------------------------------------------------------- relations
std::string Replayer::addConstraint(SketchMap& m, const json& c)
{
    std::string t = c.value("type", "");
    if (!c.value("supported", true)) return "relation " + t + " (not supported by the source's exporter)";
    std::vector<Ptr<Base>> ob;
    for (auto& e : c["entities"])
    {
        std::string why;
        Ptr<Base> o = sketchEntityFor(m, e, why);
        if (!o) return "relation " + t + ": " + why;
        ob.push_back(o);
    }
    Ptr<GeometricConstraints> gc = m.sketch->geometricConstraints();
    auto pt = [](const Ptr<Base>& b) { return Ptr<SketchPoint>(b); };
    auto ln = [](const Ptr<Base>& b) { return Ptr<SketchLine>(b); };
    auto cv = [](const Ptr<Base>& b) { return Ptr<SketchCurve>(b); };
    Ptr<Base> made;
    if (t == "coincident" && ob.size() == 2)
    {
        Ptr<SketchPoint> p = pt(ob[0]);
        Ptr<Base> other = ob[1];
        if (!p)
        {
            p = pt(ob[1]);
            other = ob[0];
        }
        if (!p) return "relation coincident: no point among its entities";
        Ptr<SketchEntity> se = other;
        if (!se) return "relation coincident: entity is not a sketch entity";
        if (Ptr<SketchPoint> q = other)
            if (q == p) return "";
        made = gc->addCoincident(p, se);
    }
    else if ((t == "horizontal" || t == "vertical") && ob.size() == 1 && ln(ob[0]))
    {
        V3 dir = t == "horizontal" ? m.src.x : m.src.y;
        int ax = alongAxis(m, dir);
        if (ax < 0) return "relation " + t + ": the source axis is not along a Fusion sketch axis";
        made = ax == 0 ? Ptr<Base>(gc->addHorizontal(ln(ob[0]))) : Ptr<Base>(gc->addVertical(ln(ob[0])));
    }
    else if ((t == "horizontal" || t == "vertical" || t == "horizontalPoints" || t == "verticalPoints") && ob.size() == 2 &&
             pt(ob[0]) && pt(ob[1]))
    {
        V3 dir = (t[0] == 'h') ? m.src.x : m.src.y;
        int ax = alongAxis(m, dir);
        if (ax < 0) return "relation " + t + ": the source axis is not along a Fusion sketch axis";
        made = ax == 0 ? Ptr<Base>(gc->addHorizontalPoints(pt(ob[0]), pt(ob[1]))) : Ptr<Base>(gc->addVerticalPoints(pt(ob[0]), pt(ob[1])));
    }
    else if (t == "parallel" && ob.size() == 2 && ln(ob[0]) && ln(ob[1]))
        made = gc->addParallel(ln(ob[0]), ln(ob[1]));
    else if (t == "perpendicular" && ob.size() == 2 && ln(ob[0]) && ln(ob[1]))
        made = gc->addPerpendicular(ln(ob[0]), ln(ob[1]));
    else if (t == "collinear" && ob.size() == 2 && ln(ob[0]) && ln(ob[1]))
        made = gc->addCollinear(ln(ob[0]), ln(ob[1]));
    else if (t == "tangent" && ob.size() == 2 && cv(ob[0]) && cv(ob[1]))
        made = gc->addTangent(cv(ob[0]), cv(ob[1]));
    else if (t == "equal" && ob.size() == 2 && cv(ob[0]) && cv(ob[1]))
        made = gc->addEqual(cv(ob[0]), cv(ob[1]));
    else if ((t == "concentric" || t == "coradial") && ob.size() == 2 && cv(ob[0]) && cv(ob[1]))
    {
        made = gc->addConcentric(cv(ob[0]), cv(ob[1]));
        if (made && t == "coradial") made = gc->addEqual(cv(ob[0]), cv(ob[1]));
    }
    else if (t == "midpoint" && ob.size() == 2)
    {
        Ptr<SketchPoint> p = pt(ob[0]) ? pt(ob[0]) : pt(ob[1]);
        Ptr<SketchCurve> l = cv(ob[0]) ? cv(ob[0]) : cv(ob[1]);
        if (!p || !l) return "relation midpoint: needs a point and a curve";
        made = gc->addMidPoint(p, l);
    }
    else if (t == "symmetric" && ob.size() == 3 && ln(ob[2]))
        made = gc->addSymmetry(Ptr<SketchEntity>(ob[0]), Ptr<SketchEntity>(ob[1]), ln(ob[2]));
    else if (t == "fix")
    {
        for (auto& o : ob)
            if (Ptr<SketchEntity> se = o) se->isFixed(true);
        return "";
    }
    else
        return "relation " + t + " (" + std::to_string(ob.size()) + " entities) has no Fusion counterpart here";
    if (!made)
    {
        std::string err = lastError();
        // Fusion refuses a relation the others already imply (SolidWorks keeps it): nothing is lost
        if (err.find("OVER_CONSTRAINT") != std::string::npos) return "~relation " + t + " already implied by the others (Fusion refuses redundant relations)";
        return "relation " + t + " refused: " + err;
    }
    return "";
}

// ---------------------------------------------------------------------------------------------- dimensions
std::string Replayer::addDimension(SketchMap& m, const json& d)
{
    std::string kind = d.value("kind", "");
    std::string label = d.value("name", d.value("id", "?"));
    std::vector<Ptr<Base>> ob;
    for (auto& e : d["entities"])
    {
        std::string why;
        Ptr<Base> o = sketchEntityFor(m, e, why);
        if (!o) return "dimension " + label + ": " + why;
        ob.push_back(o);
    }
    double value = d.value("value", 0.0);
    V3 tp = m.src.o;
    if (d.contains("textPosition") && d["textPosition"].size() >= 2)
        tp = m.src.toModel(d["textPosition"][0].get<double>(), d["textPosition"][1].get<double>());
    Ptr<Point3D> text = m.toSketch(tp);
    text->z(0);
    Ptr<SketchDimensions> dims = m.sketch->sketchDimensions();
    Ptr<SketchDimension> made;
    bool drv = d.value("driving", true);   // a reference (driven) dimension is made driven from the start
    bool isAngle = kind == "angle";
    auto pt = [](const Ptr<Base>& b) { return Ptr<SketchPoint>(b); };
    auto ln = [](const Ptr<Base>& b) { return Ptr<SketchLine>(b); };

    if ((kind == "distance" || kind == "horizontal" || kind == "vertical") && ob.size() == 2 && pt(ob[0]) && pt(ob[1]))
    {
        // which measure the source dimension is: aligned, or along the source's x or y
        V3 a = m.sketchToModel(pt(ob[0])->geometry()), b = m.sketchToModel(pt(ob[1])->geometry());
        V3 dv = sub(b, a);
        double dx = std::fabs(dot(dv, m.src.x)), dy = std::fabs(dot(dv, m.src.y)), dd = norm(dv);
        int along = -1;   // -1 aligned, 0 source x, 1 source y
        if (kind == "horizontal") along = 0;
        else if (kind == "vertical") along = 1;
        else if (std::fabs(dd - value) > 1e-6)
        {
            if (std::fabs(dx - value) < 1e-6) along = 0;
            else if (std::fabs(dy - value) < 1e-6) along = 1;
        }
        DimensionOrientations o = AlignedDimensionOrientation;
        if (along >= 0)
        {
            int ax = alongAxis(m, along == 0 ? m.src.x : m.src.y);
            if (ax < 0) return "dimension " + label + ": its direction is not along a Fusion sketch axis";
            o = ax == 0 ? HorizontalDimensionOrientation : VerticalDimensionOrientation;
        }
        made = dims->addDistanceDimension(pt(ob[0]), pt(ob[1]), o, text, drv);
    }
    else if (kind == "distance" && ob.size() == 1 && ln(ob[0]))
        made = dims->addDistanceDimension(ln(ob[0])->startSketchPoint(), ln(ob[0])->endSketchPoint(), AlignedDimensionOrientation, text, drv);
    else if (kind == "distance" && ob.size() == 2 && (ln(ob[0]) || ln(ob[1])))
    {
        Ptr<SketchLine> l = ln(ob[0]) ? ln(ob[0]) : ln(ob[1]);
        Ptr<SketchEntity> other = ln(ob[0]) ? Ptr<SketchEntity>(ob[1]) : Ptr<SketchEntity>(ob[0]);
        made = dims->addOffsetDimension(l, other, text, drv);
    }
    else if (kind == "diameter" && ob.size() == 1)
        made = dims->addDiameterDimension(Ptr<SketchCurve>(ob[0]), text, drv);
    else if (kind == "radius" && ob.size() == 1)
        made = dims->addRadialDimension(Ptr<SketchCurve>(ob[0]), text, drv);
    else if (isAngle && ob.size() == 2 && ln(ob[0]) && ln(ob[1]))
        made = dims->addAngularDimension(ln(ob[0]), ln(ob[1]), text, drv);
    else
        return "dimension " + label + " (" + kind + ", " + std::to_string(ob.size()) + " entities) has no Fusion counterpart here";
    if (!made) return "dimension " + label + " refused: " + lastError();
    if (!drv) return "";
    Ptr<ModelParameter> p = made->parameter();
    if (p)
    {
        double want = isAngle ? deg2rad(value) : mm2cm(value);
        if (std::fabs(p->value() - want) > 1e-9) p->value(want);
        // native full name, e.g. D1@SK_Base -> D1_SK_Base (Fusion parameter names are identifiers);
        // the native name is kept in the comment
        std::string nat = d.value("name", "");
        std::string sk = m.sketch->name();
        if (!nat.empty())
        {
            std::string nm = sanitizeName(nat + "_" + sk);
            if (!p->name(nm)) p->name(nm + "_" + d.value("id", ""));
            p->comment(nat + "@" + sk);
            registerParameter(nat + "@" + sk, p);
        }
        // the neutral key an equation's targetRef {kind: dimension, feature, id} names
        if (!m.id.empty() && d.contains("id")) registerParameter("dim:" + m.id + "|" + d.value("id", ""), p);
    }
    return "";
}

// ---------------------------------------------------------------------------------------------- the sketch
std::string Replayer::replaySketch(json& nf, const json& def, const Ptr<Base>& planeOverride)
{
    std::string why;
    const json& pr = def["plane"];
    if (!planeOverride && pr.value("kind", "") == "generated")
    {
        // replayed by the feature that generates its plane (a hole's position sketch, an edge flange's profile)
        deferred_[pr.value("by", "") + "|" + nf.value("id", "")] = nf;   // a feature can generate several
        return "~deferred";
    }
    Frame fr0;
    fr0.o = v3(def["frame"]["origin"]);
    fr0.z = def["frame"].contains("z") ? unit(v3(def["frame"]["z"])) : V3{0, 0, 1};
    currentFrame_ = &fr0;
    Ptr<Base> plane = planeOverride ? planeOverride : planeFor(pr, why);
    std::string planeNote;
    if (!plane && pr.value("kind", "") == "feature")
    {
        // its plane feature was not rebuilt: the sketch's frame still places it (a face or an offset origin plane)
        std::string why2;
        plane = planeFor(json{{"kind", "frame"}}, why2);
        if (plane) planeNote = "placed by its frame: " + why;
    }
    currentFrame_ = nullptr;
    if (!plane) return "sketch plane: " + why;
    Ptr<Sketch> sk = comp_->sketches()->add(plane);
    if (!sk) return "could not create a sketch on the plane: " + lastError();
    name(sk, nf.value("name", ""));
    // a sketch made on a face gets the face's edges projected into it (Fusion's "auto project" preference):
    // they are not the source's geometry and would make extra profiles
    {
        std::vector<Ptr<SketchCurve>> auto_;
        Ptr<SketchCurves> cs = sk->sketchCurves();
        for (size_t i = 0; i < cs->count(); i++)
            if (cs->item(i)->isReference()) auto_.push_back(cs->item(i));
        for (auto& c : auto_) c->deleteMe();
    }
    SketchMap m;
    m.sketch = sk;
    m.id = nf.value("id", "");
    const json& fr = def["frame"];
    m.src.o = v3(fr["origin"]);
    m.src.x = frameAxis(fr, "x", {1, 0, 0});
    m.src.z = frameAxis(fr, "z", {0, 0, 1});
    m.src.y = unit(cross(m.src.z, m.src.x));
    V3 tz = unit(cross(vecOf(sk->xDirection()), vecOf(sk->yDirection())));
    m.mirrored = dot(m.src.z, tz) < 0;
    // the component origin in this sketch: the sketch's own origin point when it is the projected origin
    {
        Ptr<SketchPoint> op = sk->originPoint();
        V3 at = op ? m.sketchToModel(op->geometry()) : V3{1e9, 0, 0};
        V3 proj = sub(V3{0, 0, 0}, mul(tz, dot(sub(V3{0, 0, 0}, at), tz)));   // origin projected on the plane
        if (op && dist(at, V3{0, 0, 0}) < 1e-6) m.origin = op;
        else
        {
            Ptr<ObjectCollection> pc = sk->project(comp_->originConstructionPoint());
            if (pc && pc->count() > 0) m.origin = pc->item(0);
            else if (op && dist(at, proj) < 1e-6) m.origin = op;
        }
    }
    sk->isComputeDeferred(true);
    std::vector<std::string> notes, splineInfo;
    if (!planeNote.empty()) notes.push_back(planeNote);
    const json& ents = def["entities"];
    for (auto& e : ents)
        if (e.value("type", "") == "point") m.modelPos[e["id"]] = m.src.toModel(e["p"][0].get<double>(), e["p"][1].get<double>());
    auto posOf = [&](const json& uv) { return m.toSketch(m.src.toModel(uv[0].get<double>(), uv[1].get<double>())); };
    // a point argument: the existing sketch point for that id, else a new position
    auto ptArg = [&](const json& e, const char* idKey, const char* posKey) -> Ptr<Base> {
        std::string id = e.value(idKey, "");
        auto it = m.ents.find(id);
        if (!id.empty() && it != m.ents.end()) return it->second;
        return posOf(e[posKey]);
    };
    auto own = [&](const std::string& id, const Ptr<SketchPoint>& p) {
        if (id.empty() || !p) return;
        auto it = m.ents.find(id);
        if (it == m.ents.end()) m.ents[id] = p;
        else if (Ptr<SketchPoint> old = it->second)
            if (old != p) old->merge(p);
    };
    Ptr<SketchCurves> curves = sk->sketchCurves();
    for (auto& e : ents)
    {
        std::string t = e.value("type", ""), id = e.value("id", "");
        Ptr<SketchCurve> made;
        if (t == "point") continue;
        if (t == "line")
        {
            Ptr<SketchLine> l = curves->sketchLines()->addByTwoPoints(ptArg(e, "startPoint", "start"), ptArg(e, "endPoint", "end"));
            if (l)
            {
                own(e.value("startPoint", ""), l->startSketchPoint());
                own(e.value("endPoint", ""), l->endSketchPoint());
            }
            made = l;
        }
        else if (t == "circle")
        {
            Ptr<SketchCircle> c = curves->sketchCircles()->addByCenterRadius(ptArg(e, "centerPoint", "center"), mm2cm(e.value("radius", 0.0)));
            if (c) own(e.value("centerPoint", ""), c->centerSketchPoint());
            made = c;
        }
        else if (t == "arc")
        {
            bool ccw = e.value("ccw", true) != m.mirrored;
            Ptr<Point3D> c = posOf(e["center"]), s = posOf(e["start"]), en = posOf(e["end"]);
            double r = std::hypot(s->x() - c->x(), s->y() - c->y());
            double as = angleOf(s->x() - c->x(), s->y() - c->y()), ae = angleOf(en->x() - c->x(), en->y() - c->y());
            double sweep = ccw ? ae - as : as - ae;
            while (sweep <= 1e-12) sweep += 2 * kPi;
            while (sweep > 2 * kPi + 1e-12) sweep -= 2 * kPi;
            double am = ccw ? as + sweep / 2 : as - sweep / 2;
            Ptr<Point3D> mid = Point3D::create(c->x() + r * std::cos(am), c->y() + r * std::sin(am), 0);
            Ptr<SketchArc> a = curves->sketchArcs()->addByThreePoints(ptArg(e, "startPoint", "start"), mid, ptArg(e, "endPoint", "end"));
            if (a)
            {
                // Fusion orders an arc's points counter-clockwise about the sketch normal, so which end is
                // which is decided by position
                V3 ps = m.src.toModel(e["start"][0].get<double>(), e["start"][1].get<double>());
                V3 pe = m.src.toModel(e["end"][0].get<double>(), e["end"][1].get<double>());
                for (Ptr<SketchPoint> sp : {a->startSketchPoint(), a->endSketchPoint()})
                {
                    V3 at = m.sketchToModel(sp->geometry());
                    if (dist(at, ps) < 1e-5) own(e.value("startPoint", ""), sp);
                    else if (dist(at, pe) < 1e-5) own(e.value("endPoint", ""), sp);
                }
                own(e.value("centerPoint", ""), a->centerSketchPoint());
            }
            made = a;
        }
        else if (t == "ellipse")
        {
            Ptr<Point3D> mj = posOf(e["major"]), mn = posOf(e["minor"]);
            Ptr<SketchEllipse> el = curves->sketchEllipses()->add(ptArg(e, "centerPoint", "center"), mj, mn);
            if (el) own(e.value("centerPoint", ""), el->centerSketchPoint());
            made = el;
        }
        else if (t == "spline" && e.contains("nurbs") && e["nurbs"].contains("controlPoints"))
        {
            // the exact curve (FORMAT-SPEC 4, Sketch): Fusion's fitted spline through the same fit points comes out
            // differently (P23: 0.3 % in area), so the spline is built from its NURBS, as a fixed spline. Its fit
            // points stay as sketch points on it (the intent), but they no longer reshape it.
            const json& nb = e["nurbs"];
            std::vector<Ptr<Point3D>> cps;
            for (auto& c : nb["controlPoints"]) cps.push_back(m.toSketch(m.src.toModel(c[0].get<double>(), c[1].get<double>())));
            for (auto& c : cps) c->z(0);
            std::vector<double> knots = nb.value("knots", std::vector<double>{});
            int degree = nb.value("degree", 3);
            bool periodic = nb.value("periodic", false);
            Ptr<NurbsCurve3D> nc = nb.contains("weights") && !nb["weights"].empty()
                                       ? NurbsCurve3D::createRational(cps, degree, knots, nb["weights"].get<std::vector<double>>(), periodic)
                                       : NurbsCurve3D::createNonRational(cps, degree, knots, periodic);
            Ptr<SketchFixedSpline> sp = nc ? curves->sketchFixedSplines()->addByNurbsCurve(nc) : nullptr;
            if (sp)
            {
                const json& ids = e.contains("pointIds") ? e["pointIds"] : json::array();
                for (size_t i = 0; i < ids.size() && i < e["points"].size(); i++)
                {
                    if (!ids[i].is_string()) continue;
                    V3 at = m.src.toModel(e["points"][i][0].get<double>(), e["points"][i][1].get<double>());
                    Ptr<SketchPoint> own_;
                    for (Ptr<SketchPoint> q : {sp->startSketchPoint(), sp->endSketchPoint()})
                        if (q && dist(m.sketchToModel(q->geometry()), at) < 1e-5) own_ = q;
                    if (!own_)
                    {
                        auto it = m.ents.find(ids[i].get<std::string>());
                        own_ = it != m.ents.end() ? Ptr<SketchPoint>(it->second) : sk->sketchPoints()->add(m.toSketch(at));
                        if (own_) sk->geometricConstraints()->addCoincident(own_, sp);
                    }
                    own(ids[i].get<std::string>(), own_);
                }
                splineInfo.push_back("spline " + id + " built from its exact NURBS as a fixed spline (Fusion's fitted spline through the fit points differs from the source's)");
            }
            made = sp;
        }
        else if (t == "spline")
        {
            Ptr<ObjectCollection> pts = ObjectCollection::create();
            const json& ids = e.contains("pointIds") ? e["pointIds"] : json::array();
            for (size_t i = 0; i < e["points"].size(); i++)
            {
                std::string pid = i < ids.size() && ids[i].is_string() ? ids[i].get<std::string>() : "";
                auto it = m.ents.find(pid);
                if (!pid.empty() && it != m.ents.end()) pts->add(it->second);
                else pts->add(posOf(e["points"][i]));
            }
            Ptr<SketchFittedSpline> sp = curves->sketchFittedSplines()->add(pts);
            if (sp)
            {
                Ptr<SketchPointList> fp = sp->fitPoints();
                for (size_t i = 0; fp && i < fp->count() && i < ids.size(); i++)
                    if (ids[i].is_string()) own(ids[i].get<std::string>(), fp->item(i));
                if (e.contains("handles") && !e["handles"].empty()) notes.push_back("spline " + id + ": tangent handles not applied");
            }
            made = sp;
        }
        else
        {
            notes.push_back("entity " + id + " (" + t + ") not rebuilt");
            continue;
        }
        if (!made)
        {
            notes.push_back("could not create " + id + ": " + lastError());
            continue;
        }
        if (e.value("construction", false)) made->isConstruction(true);
        m.ents[id] = made;
    }
    // points no entity owns
    for (auto& e : ents)
    {
        if (e.value("type", "") != "point" || m.ents.count(e["id"])) continue;
        Ptr<SketchPoint> p = sk->sketchPoints()->add(posOf(e["p"]));
        if (p) m.ents[e["id"]] = p;
    }
    sk->isComputeDeferred(false);
    std::vector<std::string> infos = splineInfo;
    if (def.contains("constraints"))
        for (auto& c : def["constraints"])
        {
            std::string w = addConstraint(m, c);
            if (!w.empty() && w[0] == '~') infos.push_back(w.substr(1));
            else if (!w.empty()) notes.push_back(w);
        }
    if (def.contains("dimensions"))
        for (auto& d : def["dimensions"])
        {
            std::string w = addDimension(m, d);
            if (!w.empty()) notes.push_back(w);
        }
    if (def.value("constrained", "") == "full" && !sk->isFullyConstrained())
        notes.push_back("the source sketch is fully defined, the rebuilt one is not");
    features[nf.value("id", "")] = sk;
    sketches_[nf.value("id", "")] = m;
    sketchDefs_[nf.value("id", "")] = def;
    if (!infos.empty())
    {
        std::string all;
        for (auto& n : infos) all += (all.empty() ? "" : "; ") + n;
        nf["info"] = all;
    }
    if (!notes.empty())
    {
        std::string all;
        for (auto& n : notes) all += (all.empty() ? "" : "; ") + n;
        nf["note"] = nf.contains("note") ? nf["note"].get<std::string>() + "; " + all : all;
        if (nf.value("status", "") == "mapped") nf["status"] = "approximate";
    }
    return "";
}


// ---------------------------------------------------------------------------------------------- 3D sketch
// A 3D sketch (FORMAT-SPEC 4, sketch3d): Fusion sketches hold 3D geometry, so it is a sketch on the XY plane
// whose entities sit at their model coordinates. alongX / alongY become horizontal / vertical (the XY plane's
// axes), alongZ a line perpendicular to the XY plane.
std::string Replayer::replaySketch3D(json& nf, const json& def)
{
    Ptr<Sketch> sk = comp_->sketches()->add(comp_->xYConstructionPlane());
    if (!sk) return "could not create a sketch: " + lastError();
    name(sk, nf.value("name", ""));
    SketchMap m;
    m.sketch = sk;
    m.id = nf.value("id", "");
    m.space = "3d";
    m.origin = sk->originPoint();
    auto pos = [&](const json& p) { return m.toSketch(v3(p)); };
    auto ptArg = [&](const json& e, const char* idKey, const char* posKey) -> Ptr<Base> {
        auto it = m.ents.find(e.value(idKey, ""));
        if (it != m.ents.end()) return it->second;
        return pos(e[posKey]);
    };
    auto own = [&](const std::string& id, const Ptr<SketchPoint>& p) {
        if (id.empty() || !p) return;
        auto it = m.ents.find(id);
        if (it == m.ents.end()) m.ents[id] = p;
        else if (Ptr<SketchPoint> old = it->second)
            if (old != p) old->merge(p);
    };
    std::vector<std::string> notes;
    sk->isComputeDeferred(true);
    for (auto& e : def["entities"])
    {
        std::string t = e.value("type", ""), id = e.value("id", "");
        Ptr<SketchCurve> made;
        if (t == "point") continue;
        if (t == "line")
        {
            Ptr<SketchLine> l = sk->sketchCurves()->sketchLines()->addByTwoPoints(ptArg(e, "startPoint", "start"), ptArg(e, "endPoint", "end"));
            if (l)
            {
                own(e.value("startPoint", ""), l->startSketchPoint());
                own(e.value("endPoint", ""), l->endSketchPoint());
            }
            made = l;
        }
        else if (t == "arc")
        {
            V3 c = v3(e["center"]), a = v3(e["start"]), b = v3(e["end"]);
            V3 nrm = e.contains("normal") ? unit(v3(e["normal"])) : unit(cross(sub(a, c), sub(b, c)));
            if (!e.value("ccw", true)) nrm = mul(nrm, -1);
            // the middle of the arc: half the sweep from start, about the normal
            V3 u = sub(a, c), w = sub(b, c);
            double r = norm(u);
            double ang = std::atan2(dot(nrm, cross(u, w)), dot(u, w));
            if (ang <= 0) ang += 2 * kPi;
            V3 x = unit(u), y = unit(cross(nrm, x));
            V3 mid = add(c, add(mul(x, r * std::cos(ang / 2)), mul(y, r * std::sin(ang / 2))));
            Ptr<SketchArc> arc = sk->sketchCurves()->sketchArcs()->addByThreePoints(ptArg(e, "startPoint", "start"), m.toSketch(mid), ptArg(e, "endPoint", "end"));
            if (arc)
            {
                for (Ptr<SketchPoint> sp : {arc->startSketchPoint(), arc->endSketchPoint()})
                {
                    V3 at = m.sketchToModel(sp->geometry());
                    if (dist(at, a) < 1e-5) own(e.value("startPoint", ""), sp);
                    else if (dist(at, b) < 1e-5) own(e.value("endPoint", ""), sp);
                }
                own(e.value("centerPoint", ""), arc->centerSketchPoint());
            }
            made = arc;
        }
        else
        {
            notes.push_back("3D entity " + id + " (" + t + ") not rebuilt");
            continue;
        }
        if (!made)
        {
            notes.push_back("could not create " + id + ": " + lastError());
            continue;
        }
        if (e.value("construction", false)) made->isConstruction(true);
        m.ents[id] = made;
    }
    for (auto& e : def["entities"])
        if (e.value("type", "") == "point" && !m.ents.count(e["id"]))
            if (Ptr<SketchPoint> p = sk->sketchPoints()->add(pos(e["p"]))) m.ents[e["id"]] = p;
    sk->isComputeDeferred(false);
    for (auto c : def.value("constraints", json::array()))
    {
        std::string t = c.value("type", "");
        if (t == "alongX") c["type"] = "horizontal";
        if (t == "alongY") c["type"] = "vertical";
        std::string w;
        if (t == "alongZ")
        {
            std::string why;
            Ptr<Base> o = c["entities"].size() == 1 ? sketchEntityFor(m, c["entities"][0], why) : nullptr;
            Ptr<SketchCurve> cv = o;
            if (!cv || !sk->geometricConstraints()->addPerpendicularToSurface(cv, comp_->xYConstructionPlane()))
                w = "relation alongZ refused: " + lastError();
        }
        else
            w = addConstraint(m, c);
        if (!w.empty() && w[0] != '~') notes.push_back(w);
    }
    for (auto& d : def.value("dimensions", json::array()))
    {
        std::string w = addDimension(m, d);
        if (!w.empty()) notes.push_back(w);
    }
    features[nf.value("id", "")] = sk;
    sketches_[nf.value("id", "")] = m;
    sketchDefs_[nf.value("id", "")] = def;
    if (!notes.empty())
    {
        std::string all;
        for (auto& x : notes) all += (all.empty() ? "" : "; ") + x;
        nf["status"] = "approximate";
        nf["note"] = all;
    }
    return "";
}

} // namespace ncxf
