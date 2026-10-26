// Replay: patterns, mirrors, multibody features, shell, draft, sweep, loft, surfaces, split, holes and the
// features that only mark something (weldment, sheet-metal defaults, flat pattern, cosmetic thread).
#include "host.h"
#include "replay.h"

#include <algorithm>

namespace ncxf {

namespace {

Ptr<ValueInput> mmValue(double mm) { return ValueInput::createByReal(mm2cm(mm)); }
Ptr<ValueInput> degValue(double deg) { return ValueInput::createByReal(deg2rad(deg)); }
Ptr<ValueInput> count(int n) { return ValueInput::createByReal(static_cast<double>(n)); }

void approx(json& nf, const std::string& note)
{
    nf["status"] = "approximate";
    nf["note"] = nf.contains("note") && !nf["note"].get<std::string>().empty() ? nf["note"].get<std::string>() + "; " + note : note;
}

// The direction an edge or axis entity runs in Fusion (unit, model space).
bool fusionDirection(const Ptr<Base>& e, V3& dir)
{
    if (Ptr<BRepEdge> ed = e)
    {
        Ptr<CurveEvaluator3D> ev = ed->evaluator();
        double s, t;
        Ptr<Point3D> a, b;
        if (ev && ev->getParameterExtents(s, t) && ev->getPointAtParameter(s, a) && ev->getPointAtParameter(t, b))
        {
            dir = unit(sub(ptMm(b), ptMm(a)));
            return norm(dir) > 0;
        }
    }
    if (Ptr<ConstructionAxis> ax = e)
    {
        Ptr<InfiniteLine3D> l = ax->geometry();
        if (l)
        {
            dir = unit(vecOf(l->direction()));
            return true;
        }
    }
    if (Ptr<SketchLine> sl = e)
    {
        dir = unit(sub(modelPointMm(sl->endSketchPoint()), modelPointMm(sl->startSketchPoint())));
        return true;
    }
    return false;
}

// The direction a payload direction reference means (its descriptor's line direction), or none.
bool payloadDirection(const json& ref, V3& dir)
{
    if (ref.value("kind", "") == "edge" && ref.contains("geom") && ref["geom"].contains("params") &&
        ref["geom"]["params"].contains("direction"))
    {
        dir = unit(v3(ref["geom"]["params"]["direction"]));
        return true;
    }
    return false;
}

Ptr<TimelineObject> tlo(const Ptr<Base>& o)
{
    if (Ptr<Feature> f = o) return f->timelineObject();
    if (Ptr<Sketch> s = o) return s->timelineObject();
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------------------- seeds
std::string Replayer::seedsFor(const json& def, Ptr<ObjectCollection>& out, bool& bodiesMode)
{
    out = ObjectCollection::create();
    bodiesMode = false;
    if (def.contains("seeds"))
        for (auto& s : def["seeds"])
        {
            std::string why;
            Ptr<Base> o = refObject(s, why);
            if (!o) return "seed: " + why;
            out->add(o);
        }
    if (def.contains("bodies") && !def["bodies"].empty())
    {
        std::vector<Ptr<Base>> bs;
        std::string why = resolveEntities(def["bodies"], bs);
        if (!why.empty()) return "seed body: " + why;
        for (auto& b : bs) out->add(b);
        bodiesMode = true;
    }
    if (out->count() == 0) return "no seeds";
    return "";
}

// ---------------------------------------------------------------------------------------- linear pattern
std::string Replayer::replayLinearPattern(json& nf, const json& def)
{
    Ptr<ObjectCollection> seeds;
    bool bodiesMode;
    std::string why = seedsFor(def, seeds, bodiesMode);
    if (!why.empty()) return why;
    struct Dir
    {
        Ptr<Base> ent;
        V3 want;
        int n;
        double spacing;
    };
    std::vector<Dir> dirs;
    for (const char* k : {"dir1", "dir2"})
    {
        if (!def.contains(k)) continue;
        const json& d = def[k];
        Dir dd;
        dd.ent = refObject(d["ref"], why);
        if (!dd.ent) return std::string(k) + ": " + why;
        V3 pd{0, 0, 0};
        if (!payloadDirection(d["ref"], pd) && !fusionDirection(dd.ent, pd)) return std::string(k) + ": direction unknown";
        dd.want = d.value("reverse", false) ? mul(pd, -1) : pd;
        dd.n = d.value("count", 1);
        dd.spacing = d.value("spacing", 0.0);
        dirs.push_back(dd);
    }
    if (dirs.empty()) return "pattern without a direction";
    Ptr<RectangularPatternFeatures> rp = comp_->features()->rectangularPatternFeatures();
    auto signFor = [&](const Dir& d) {
        V3 fd;
        if (!fusionDirection(d.ent, fd)) return 1.0;
        return dot(fd, d.want) < 0 ? -1.0 : 1.0;
    };
    // Which way Fusion runs along an edge is its own business: the sign is guessed from the edge's parameter
    // direction, the other sign tried when Fusion finds no instance, and the instances checked afterwards.
    double s1 = signFor(dirs[0]), s2 = dirs.size() > 1 ? signFor(dirs[1]) : 1.0;
    Ptr<RectangularPatternFeature> f;
    std::string firstError;
    for (int attempt = 0; attempt < 4 && !f; attempt++)
    {
        double a1 = (attempt & 1) ? -s1 : s1, a2 = (attempt & 2) ? -s2 : s2;
        if (attempt >= 2 && dirs.size() < 2) break;
        Ptr<RectangularPatternFeatureInput> in =
            rp->createInput(seeds, dirs[0].ent, count(dirs[0].n), mmValue(a1 * dirs[0].spacing), SpacingPatternDistanceType);
        if (!in) return "pattern input: " + lastError();
        if (dirs.size() > 1) in->setDirectionTwo(dirs[1].ent, count(dirs[1].n), mmValue(a2 * dirs[1].spacing));
        else
        {
            // without a second direction Fusion's input keeps its UI default of 3 instances there, all at
            // distance 0: every instance would be made three times over (measured)
            in->quantityTwo(count(1));
            in->distanceTwo(mmValue(0));
        }
        in->patternComputeOption(AdjustPatternCompute);
        size_t before = rp->count();
        f = rp->add(in);
        if (!f && firstError.empty()) firstError = lastError();
        // Fusion can make the feature (in an error state) and still return null: remove it before retrying
        if (!f && rp->count() > before)
        {
            Ptr<RectangularPatternFeature> bad = rp->item(rp->count() - 1);
            if (bad) bad->deleteMe();
        }
    }
    if (!f) return "pattern failed: " + firstError;
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    // the instances must lie along the wanted directions
    auto fixSense = [&](size_t di) {
        Ptr<PatternElements> els = f->patternElements();
        V3 other = dirs.size() > 1 ? dirs[1 - di].want : V3{0, 0, 0};
        for (size_t i = 0; els && i < els->count(); i++)
        {
            std::vector<double> a = els->item(i)->transform()->asArray();
            V3 t{cm2mm(a[3]), cm2mm(a[7]), cm2mm(a[11])};
            double along = dot(t, unit(dirs[di].want));
            if (std::fabs(along) < 1e-6 || (dirs.size() > 1 && std::fabs(dot(t, unit(other))) > 1e-6)) continue;
            if (along < 0)
            {
                Ptr<ModelParameter> p = di == 0 ? f->distanceOne() : f->distanceTwo();
                if (p) p->value(-p->value());
            }
            return;
        }
    };
    fixSense(0);
    if (dirs.size() > 1) fixSense(1);
    Ptr<PatternElements> els;
    // skipped instances (index 0 = the seed; measured on P22: SolidWorks counts the seed as instance 0).
    // PatternElement.isSuppressed does not work through the API here (it fails with "Didn't roll editing feature
    // back", or returns true with "API Function not yet implemented" and changes nothing: measured), so:
    //  - a body pattern: the skipped instances' bodies are removed (same geometry);
    //  - a feature pattern whose kept instances are still evenly spaced: count and spacing are changed to make
    //    exactly those; otherwise the skips are reported.
    if (def.contains("skipped") && !def["skipped"].empty())
    {
        std::set<int> skip;
        for (auto& sk : def["skipped"]) skip.insert(sk.get<int>());
        auto posOf = [&](int k) {
            int k1 = dirs.size() > 1 ? k % dirs[0].n : k, k2 = dirs.size() > 1 ? k / dirs[0].n : 0;
            return add(mul(dirs[0].want, k1 * dirs[0].spacing), dirs.size() > 1 ? mul(dirs[1].want, k2 * dirs[1].spacing) : V3{0, 0, 0});
        };
        if (bodiesMode)
        {
            // the bodies the pattern made at the skipped positions (seed centroid moved by the instance offset)
            std::vector<Ptr<BRepBody>> seedBodies;
            for (size_t i = 0; i < seeds->count(); i++)
                if (Ptr<BRepBody> b = seeds->item(i)) seedBodies.push_back(b);
            Ptr<BRepBodies> made = f->bodies();
            for (int k : skip)
                for (auto& sb : seedBodies)
                {
                    V3 want = add(ptMm(sb->getPhysicalProperties(LowCalculationAccuracy)->centerOfMass()), posOf(k));
                    for (size_t i = 0; made && i < made->count(); i++)
                    {
                        Ptr<BRepBody> b = made->item(i);
                        json fp = bodyFingerprint(b);
                        if (dist(v3(fp["centroid"]), want) < 1e-3)
                        {
                            if (!comp_->features()->removeFeatures()->add(b)) return "could not remove skipped instance " + std::to_string(k);
                            break;
                        }
                    }
                }
            approx(nf, "skipped instances removed with a Remove feature after the pattern (Fusion's API cannot suppress pattern instances)");
        }
        else if (dirs.size() == 1)
        {
            std::vector<int> kept;
            for (int k = 0; k < dirs[0].n; k++)
                if (!skip.count(k)) kept.push_back(k);
            bool even = kept.size() >= 2 && kept[0] == 0;
            for (size_t i = 2; even && i < kept.size(); i++)
                if (kept[i] - kept[i - 1] != kept[1] - kept[0]) even = false;
            if (!even) return "skipped instances that leave an uneven pattern cannot be made (Fusion's API cannot suppress pattern instances)";
            int step = kept[1] - kept[0];
            Ptr<ModelParameter> q = f->quantityOne();
            Ptr<ModelParameter> d = f->distanceOne();
            if (!q || !d) return "pattern parameters not accessible";
            q->value(static_cast<double>(kept.size()));
            d->value(d->value() * step);
            approx(nf, "skipped instances left out by making the pattern " + std::to_string(kept.size()) + " instances " +
                           std::to_string(step) + " spacings apart (Fusion's API cannot suppress pattern instances)");
        }
        else
            return "skipped instances in a two-direction feature pattern cannot be made (Fusion's API cannot suppress pattern instances)";
    }
    return "";
}

// ---------------------------------------------------------------------------------------- circular pattern
std::string Replayer::replayCircularPattern(json& nf, const json& def)
{
    Ptr<ObjectCollection> seeds;
    bool bodiesMode;
    std::string why = seedsFor(def, seeds, bodiesMode);
    if (!why.empty()) return why;
    Ptr<Base> axis = def.contains("axis") ? refObject(def["axis"], why) : nullptr;
    if (!axis) return "axis: " + why;
    if (Ptr<BRepFace> f = axis)
    {
        // a cylindrical face: its axis, as a construction axis
        Ptr<ConstructionAxisInput> ai = comp_->constructionAxes()->createInput();
        if (!ai->setByCircularFace(f)) return "pattern axis from a face: " + lastError();
        Ptr<ConstructionAxis> ax = comp_->constructionAxes()->add(ai);
        if (!ax) return "pattern axis: " + lastError();
        ax->isLightBulbOn(false);
        axis = ax;
    }
    int n = def.value("count", 1);
    double ang = def.value("angle", 360.0);
    bool equal = def.value("equalSpacing", true);
    double total = equal ? ang : ang * (n - 1);
    if (def.value("reverse", false)) total = -total;
    Ptr<CircularPatternFeatures> cp = comp_->features()->circularPatternFeatures();
    Ptr<CircularPatternFeatureInput> in = cp->createInput(seeds, axis);
    if (!in) return "pattern input: " + lastError();
    in->quantity(count(n));
    in->totalAngle(degValue(total));
    in->isSymmetric(false);
    in->patternComputeOption(AdjustPatternCompute);
    Ptr<CircularPatternFeature> f = cp->add(in);
    if (!f) return "circular pattern failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    if (def.contains("skipped") && !def["skipped"].empty()) return "skipped instances of a circular pattern not applied yet";
    return "";
}

// ---------------------------------------------------------------------------------------- mirror
std::string Replayer::replayMirror(json& nf, const json& def, bool bodies_)
{
    Ptr<ObjectCollection> seeds;
    bool bodiesMode;
    std::string why = seedsFor(def, seeds, bodiesMode);
    if (!why.empty()) return why;
    Ptr<Base> plane = def.contains("plane") ? refObject(def["plane"], why) : nullptr;
    if (!plane) return "mirror plane: " + why;
    Ptr<MirrorFeatures> mf = comp_->features()->mirrorFeatures();
    Ptr<MirrorFeatureInput> in = mf->createInput(seeds, plane);
    if (!in) return "mirror input: " + lastError();
    if (bodies_) in->isCombine(def.value("merge", false));
    else in->patternComputeOption(AdjustPatternCompute);
    Ptr<MirrorFeature> f = mf->add(in);
    if (!f) return "mirror failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

// ---------------------------------------------------------------------------------------- bodies
std::string Replayer::replayDeleteBody(json& nf, const json& def)
{
    if (def.value("keep", false)) return "delete/keep body (keep the listed bodies) not supported yet";
    std::vector<Ptr<Base>> bs;
    std::string why = resolveEntities(def["bodies"], bs);
    if (!why.empty()) return why;
    Ptr<Feature> last;
    for (auto& b : bs)
    {
        Ptr<RemoveFeature> r = comp_->features()->removeFeatures()->add(b);
        if (!r) return "remove body failed: " + lastError();
        last = r;
    }
    name(last, nf.value("name", ""));
    features[nf.value("id", "")] = last;
    return "";
}

std::string Replayer::replayCombine(json& nf, const json& def)
{
    std::string op = def.value("operation", "add");
    std::string why;
    Ptr<Base> main = def.contains("main") ? refObject(def["main"], why) : nullptr;
    if (!main) return "main body: " + why;
    std::vector<Ptr<Base>> tools;
    why = resolveEntities(def["bodies"], tools);
    if (!why.empty()) return why;
    Ptr<ObjectCollection> tc = ObjectCollection::create();
    for (auto& t : tools) tc->add(t);
    Ptr<CombineFeatures> cf = comp_->features()->combineFeatures();
    Ptr<CombineFeatureInput> in = cf->createInput(main, tc);
    in->operation(op == "subtract" ? CutFeatureOperation : op == "common" ? IntersectFeatureOperation : JoinFeatureOperation);
    in->isKeepToolBodies(false);
    Ptr<CombineFeature> f = cf->add(in);
    if (!f) return "combine failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

std::string Replayer::replayMoveBody(json& nf, const json& def)
{
    std::vector<Ptr<Base>> bs;
    std::string why = resolveEntities(def["bodies"], bs);
    if (!why.empty()) return why;
    bool copy = def.value("copy", false);
    int copies = std::max(1, def.value("copies", 1));
    if (copy && copies != 1) return "move/copy with several copies not supported yet";
    Ptr<ObjectCollection> targets = ObjectCollection::create();
    if (copy)
    {
        for (auto& b : bs)
        {
            Ptr<CopyPasteBody> cp = comp_->features()->copyPasteBodies()->add(b);
            if (!cp) return "copy body failed: " + lastError();
            Ptr<BRepBodies> made = cp->bodies();
            for (size_t i = 0; made && i < made->count(); i++) targets->add(made->item(i));
        }
    }
    else
        for (auto& b : bs) targets->add(b);
    Ptr<Matrix3D> m = Matrix3D::create();
    if (def.contains("translate"))
    {
        V3 t = v3(def["translate"]);
        m->translation(Vector3D::create(mm2cm(t[0]), mm2cm(t[1]), mm2cm(t[2])));
    }
    else if (def.contains("rotate"))
    {
        const json& r = def["rotate"];
        V3 o = r.contains("origin") ? v3(r["origin"]) : V3{0, 0, 0};
        V3 a = v3(r["angles"]);
        // rotations about x, then y, then z through the origin (the order is not defined with several angles:
        // the spec makes such a feature approximate and checks it by geometry)
        V3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int k = 0; k < 3; k++)
        {
            if (std::fabs(a[k]) < 1e-12) continue;
            Ptr<Matrix3D> rk = Matrix3D::create();
            rk->setToRotation(deg2rad(a[k]), vec(axes[k]), ptCm(o));
            m->transformBy(rk);
        }
    }
    else
        return "move body without a translation or rotation";
    // With the part's occurrence active (the upright wrapper) Fusion applies a free move in that occurrence's assembly
    // frame (P27 moved its body to the wrong place): the move expressed there, W M W^-1
    if (occurrence)
        if (Ptr<Matrix3D> w = occurrence->transform2())
        {
            Ptr<Matrix3D> wi = w->copy();
            wi->invert();
            Ptr<Matrix3D> c = wi->copy();
            c->transformBy(m);   // M W^-1 (transformBy multiplies on the left: this = other * this)
            c->transformBy(w);   // W M W^-1
            m = c;
        }
    Ptr<MoveFeatures> mf = comp_->features()->moveFeatures();
    Ptr<MoveFeatureInput> in = mf->createInput2(targets);
    if (!in || !in->defineAsFreeMove(m)) return "move input: " + lastError();
    Ptr<MoveFeature> f = mf->add(in);
    if (!f) return "move failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

// ---------------------------------------------------------------------------------------- shell / draft
std::string Replayer::replayShell(json& nf, const json& def)
{
    std::vector<Ptr<Base>> faces;
    std::string why = resolveEntities(def["faces"], faces);
    if (!why.empty()) return why;
    Ptr<ObjectCollection> oc = ObjectCollection::create();
    for (auto& f : faces) oc->add(f);
    if (faces.empty())
        for (auto& b : bodies(true)) oc->add(b);
    Ptr<ShellFeatures> sf = comp_->features()->shellFeatures();
    Ptr<ShellFeatureInput> in = sf->createInput(oc, false);
    double t = def.value("thickness", 0.0);
    if (def.value("outward", 0) != 0) in->outsideThickness(mmValue(t));
    else in->insideThickness(mmValue(t));
    Ptr<ShellFeature> f = sf->add(in);
    if (!f) return "shell failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

std::string Replayer::replayDraft(json& nf, const json& def)
{
    std::vector<Ptr<Base>> faces;
    std::string why = resolveEntities(def["faces"], faces);
    if (!why.empty()) return why;
    Ptr<Base> neutral = def.contains("neutral") ? refObject(def["neutral"], why) : nullptr;
    if (!neutral) return "neutral plane: " + why;
    std::vector<Ptr<BRepFace>> fs;
    for (auto& f : faces) fs.push_back(f);
    Ptr<DraftFeatures> df = comp_->features()->draftFeatures();
    Ptr<DraftFeatureInput> in = df->createInput(fs, neutral, false);
    if (!in) return "draft input: " + lastError();
    in->setSingleAngle(false, degValue(def.value("angle", 0.0)));
    in->isDirectionFlipped(def.value("reverse", false));
    Ptr<DraftFeature> f = df->add(in);
    if (!f) return "draft failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

// ---------------------------------------------------------------------------------------- sweep / loft
// A chain of a sketch's curves as a Path. In the upright wrapper a part is built with its occurrence active, and Fusion
// then makes no path of the native curves (P13's sweep, P19's thin wall): the same curves of the sketch's proxy in that
// occurrence are used instead.
Ptr<Path> Replayer::pathOf(const Ptr<ObjectCollection>& curves, const Ptr<Sketch>& sk)
{
    Ptr<Path> p = Path::create(curves, connectedChainedCurves);
    if (p || !occurrence || !sk) return p;
    Ptr<Sketch> ps = sk->createForAssemblyContext(occurrence);
    if (!ps) return nullptr;
    Ptr<SketchCurves> cs = sk->sketchCurves(), pcs = ps->sketchCurves();
    Ptr<ObjectCollection> proxies = ObjectCollection::create();
    for (size_t k = 0; k < curves->count(); k++)
        for (size_t i = 0; i < cs->count() && i < pcs->count(); i++)
            if (cs->item(i) == curves->item(k)) proxies->add(pcs->item(i));
    return proxies->count() == curves->count() ? Path::create(proxies, connectedChainedCurves) : nullptr;
}

Ptr<Path> Replayer::pathFor(const json& ref, std::string& why)
{
    if (ref.value("kind", "") == "feature")
    {
        auto it = sketches_.find(ref.value("id", ""));
        if (it == sketches_.end())
        {
            why = "path sketch " + ref.value("id", "") + " was not rebuilt";
            return nullptr;
        }
        Ptr<ObjectCollection> curves = ObjectCollection::create();
        Ptr<SketchCurves> cs = it->second.sketch->sketchCurves();
        for (size_t i = 0; i < cs->count(); i++)
            if (!cs->item(i)->isConstruction() && !cs->item(i)->isReference()) curves->add(cs->item(i));
        if (curves->count() == 0)
        {
            why = "path sketch has no curves";
            return nullptr;
        }
        Ptr<Path> p = pathOf(curves, it->second.sketch);
        if (!p) why = "Fusion made no path of the sketch's curves: " + lastError();
        return p;
    }
    Ptr<Base> o = refObject(ref, why);
    if (!o) return nullptr;
    return comp_->features()->createPath(o, true);
}

std::string Replayer::replaySweep(json& nf, const json& def, bool cut)
{
    std::string why;
    json pdef{{"profile", def["profile"]}};
    Ptr<ObjectCollection> prof = profilesFor(pdef, why);
    if (!prof) return "profile: " + why;
    Ptr<Path> path = def.contains("path") ? pathFor(def["path"], why) : nullptr;
    if (!path) return "path: " + why;
    FeatureOperations op = cut ? CutFeatureOperation : (def.value("merge", true) && !bodies(true).empty() ? JoinFeatureOperation : NewBodyFeatureOperation);
    Ptr<SweepFeatures> sf = comp_->features()->sweepFeatures();
    Ptr<Base> profile = prof->count() == 1 ? prof->item(0) : Ptr<Base>(prof);
    Ptr<SweepFeatureInput> in = sf->createInput(profile, path, op);
    if (!in) return "sweep input: " + lastError();
    if (def.contains("guides") && !def["guides"].empty())
    {
        Ptr<Path> g = pathFor(def["guides"][0], why);
        if (!g) return "guide: " + why;
        in->guideRail(g);
        if (def["guides"].size() > 1) nf["note"] = "only the first guide curve is used (Fusion takes one guide rail)";
    }
    in->orientation(PerpendicularOrientationType);
    Ptr<SweepFeature> f = sf->add(in);
    if (!f) return "sweep failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

std::string Replayer::replayLoft(json& nf, const json& def, bool cut)
{
    FeatureOperations op = cut ? CutFeatureOperation : (def.value("merge", true) && !bodies(true).empty() ? JoinFeatureOperation : NewBodyFeatureOperation);
    Ptr<LoftFeatures> lf = comp_->features()->loftFeatures();
    Ptr<LoftFeatureInput> in = lf->createInput(op);
    for (auto& s : def["sections"])
    {
        std::string why;
        Ptr<ObjectCollection> prof = profilesFor(json{{"profile", s}}, why);
        if (!prof || prof->count() == 0)
        {
            // a point or an open curve section
            Ptr<Base> o = refObject(s, why);
            if (!o) return "section: " + why;
            in->loftSections()->add(o);
            continue;
        }
        in->loftSections()->add(prof->item(0));
    }
    if (def.contains("guides"))
        for (auto& g : def["guides"])
        {
            std::string why;
            Ptr<Path> p = pathFor(g, why);
            if (!p) return "guide: " + why;
            in->centerLineOrRails()->addRail(p);
        }
    in->isClosed(def.value("closed", false));
    Ptr<LoftFeature> f = lf->add(in);
    if (!f) return "loft failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

// ---------------------------------------------------------------------------------------- surfaces / split
std::string Replayer::replaySurfaceExtrude(json& nf, const json& def)
{
    std::string why;
    Ptr<Path> path = pathFor(def["profile"], why);
    if (!path) return "profile: " + why;
    bool mirrored = false;
    auto it = sketches_.find(def["profile"].value("id", ""));
    if (it != sketches_.end()) mirrored = it->second.mirrored;
    Ptr<ExtrudeFeatures> ef = comp_->features()->extrudeFeatures();
    Ptr<ExtrudeFeatureInput> in = ef->createInput(path, NewBodyFeatureOperation);
    in->isSolid(false);
    const json& e = def["end"];
    if (e.value("type", "") == "midPlane") in->setSymmetricExtent(mmValue(e.value("depth", 0.0)), true);
    else if (e.value("type", "") == "blind")
        in->setOneSideExtent(DistanceExtentDefinition::create(mmValue(e.value("depth", 0.0))),
                             (def.value("reverse", false) != mirrored) ? NegativeExtentDirection : PositiveExtentDirection);
    else return "surface extrude end condition " + e.value("type", "") + " not supported";
    Ptr<ExtrudeFeature> f = ef->add(in);
    if (!f) return "surface extrude failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

std::string Replayer::replaySurfaceRevolve(json& nf, const json& def)
{
    std::string why;
    auto it = sketches_.find(def["profile"].value("id", ""));
    if (it == sketches_.end()) return "profile sketch not rebuilt";
    // the axis: the payload's, else the sketch's construction line
    Ptr<Base> axis;
    if (def.contains("axis")) axis = refObject(def["axis"], why);
    Ptr<ObjectCollection> curves = ObjectCollection::create();
    Ptr<SketchCurves> cs = it->second.sketch->sketchCurves();
    for (size_t i = 0; i < cs->count(); i++)
    {
        if (cs->item(i)->isReference()) continue;
        if (cs->item(i)->isConstruction())
        {
            if (!axis && Ptr<SketchLine>(cs->item(i))) axis = cs->item(i);
            continue;
        }
        curves->add(cs->item(i));
    }
    if (!axis) return "revolve axis not found";
    Ptr<Path> path = pathOf(curves, it->second.sketch);
    Ptr<RevolveFeatures> rf = comp_->features()->revolveFeatures();
    Ptr<RevolveFeatureInput> in = rf->createInput(path, axis, NewBodyFeatureOperation);
    in->isSolid(false);
    double a = def.value("angle", 360.0);
    bool flip = def.value("reverse", false) != it->second.mirrored;
    in->setAngleExtent(def.value("mode", "") == "midPlane", degValue(flip ? -a : a));
    Ptr<RevolveFeature> f = rf->add(in);
    if (!f) return "surface revolve failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

std::string Replayer::replayPlanarSurface(json& nf, const json& def)
{
    std::string why;
    Ptr<ObjectCollection> prof = profilesFor(def, why);
    if (!prof) return "profile: " + why;
    Ptr<PatchFeatures> pf = comp_->features()->patchFeatures();
    Ptr<PatchFeatureInput> in = pf->createInput(prof->count() == 1 ? prof->item(0) : Ptr<Base>(prof), NewBodyFeatureOperation);
    Ptr<PatchFeature> f = pf->add(in);
    if (!f) return "planar surface failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

std::string Replayer::replayKnit(json& nf, const json& def)
{
    Ptr<ObjectCollection> surfaces = ObjectCollection::create();
    for (auto& s : def["surfaces"])
    {
        std::string why;
        Ptr<Base> o = refObject(s, why);
        if (!o) return "surface: " + why;
        if (Ptr<Feature> f = o)
        {
            Ptr<BRepBodies> bs = f->bodies();
            for (size_t i = 0; bs && i < bs->count(); i++) surfaces->add(bs->item(i));
        }
        else
            surfaces->add(o);
    }
    Ptr<StitchFeatures> sf = comp_->features()->stitchFeatures();
    Ptr<StitchFeatureInput> in = sf->createInput(surfaces, mmValue(std::max(def.value("tolerance", 0.0025), 0.001)), NewBodyFeatureOperation);
    if (!in) return "knit input: " + lastError();
    Ptr<StitchFeature> f = sf->add(in);
    if (!f) return "knit failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    if (def.value("formSolid", false)) return "";
    return "";
}

std::string Replayer::replaySplit(json& nf, const json& def)
{
    if (!def.contains("tools") || def["tools"].size() != 1) return "split with several tools not supported yet";
    std::string why;
    Ptr<Base> tool = refObject(def["tools"][0], why);
    if (!tool) return "split tool: " + why;
    std::vector<Ptr<BRepBody>> solids = bodies(true);
    if (solids.empty()) return "nothing to split";
    Ptr<ObjectCollection> targets = ObjectCollection::create();
    for (auto& b : solids) targets->add(b);
    Ptr<SplitBodyFeatures> sf = comp_->features()->splitBodyFeatures();
    Ptr<SplitBodyFeatureInput> in = sf->createInput(targets, tool, true);
    Ptr<SplitBodyFeature> f = sf->add(in);
    if (!f) return "split failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}

// ---------------------------------------------------------------------------------------- markers
std::string Replayer::replayMarker(json& nf, const std::string& why)
{
    // features that change no geometry in Fusion terms: reported, nothing built
    nf["status"] = "approximate";
    nf["note"] = why;
    markers.insert(nf.value("id", ""));
    return "";
}

// ---------------------------------------------------------------------------------------- face fillet
// A face fillet between two face sets that meet: the same rounding as a constant fillet on the edges the two sets
// share (Fusion has no face fillet).
std::string Replayer::replayFaceFillet(json& nf, const json& def)
{
    std::vector<Ptr<Base>> f1, f2;
    std::string why = resolveEntities(def["faces1"], f1);
    if (why.empty()) why = resolveEntities(def["faces2"], f2);
    if (!why.empty()) return why;
    Ptr<ObjectCollection> edges = ObjectCollection::create();
    for (auto& a : f1)
    {
        Ptr<BRepEdges> es = Ptr<BRepFace>(a)->edges();
        for (size_t i = 0; i < es->count(); i++)
        {
            Ptr<BRepFaces> fs = es->item(i)->faces();
            for (size_t j = 0; j < fs->count(); j++)
                for (auto& b : f2)
                    if (fs->item(j) == Ptr<BRepFace>(b)) edges->add(es->item(i));
        }
    }
    if (edges->count() == 0) return "the two face sets do not meet: a face fillet across a gap has no Fusion counterpart";
    Ptr<FilletFeatures> ff = comp_->features()->filletFeatures();
    Ptr<FilletFeatureInput> in = ff->createInput();
    in->edgeSetInputs()->addConstantRadiusEdgeSet(edges, mmValue(def.value("radius", 0.0)), true);
    Ptr<FilletFeature> f = ff->add(in);
    if (!f) return "fillet failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    nf["status"] = "approximate";
    nf["note"] = "face fillet rebuilt as an edge fillet on the edges where its two face sets meet (Fusion has no face fillet)";
    return "";
}

} // namespace ncxf
