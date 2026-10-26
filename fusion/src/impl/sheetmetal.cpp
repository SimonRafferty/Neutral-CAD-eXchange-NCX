// Replay: sheet metal (FORMAT-SPEC 4: sheetMetal, baseFlange, flatPattern; the extras carry their results).
//
//  - sheetMetal (the part's defaults) -> a design sheet-metal rule (thickness, bend radius, K-factor), made the
//    component's active rule.
//  - baseFlange -> Fusion's base flange (closed profile) or contour flange (open profile).
//  - flatPattern -> nothing: Fusion makes the flat pattern on demand from any sheet-metal body.
// The flange extras (edge flange, hem, jog, miter flange, closed corner, sketched bend, ...) are rebuilt from
// their carried results until each has its own replay.
#include "host.h"
#include "neutral.h"
#include "replay.h"

namespace ncxf {

namespace {
Ptr<ValueInput> mmValue(double mm) { return ValueInput::createByReal(mm2cm(mm)); }
// A length as text with its unit. Some sheet-metal inputs read a real ValueInput in the design's units (mm), not
// centimetres: the sheet-metal chamfer's distance and a bend radius override (measured: 0.2 gave a 0.2 mm bend).
Ptr<ValueInput> mmText(double mm)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.10g mm", mm);
    return ValueInput::createByString(buf);
}
void approx(json& nf, const std::string& note)
{
    nf["status"] = "approximate";
    nf["note"] = nf.contains("note") && !nf["note"].get<std::string>().empty() ? nf["note"].get<std::string>() + "; " + note : note;
}
}

std::string Replayer::replaySheetMetal(json& nf, const json& def)
{
    Ptr<SheetMetalRules> lib = design_->librarySheetMetalRules();
    Ptr<SheetMetalRules> mine = design_->designSheetMetalRules();
    Ptr<SheetMetalRule> base = (mine && mine->count() > 0) ? mine->item(0) : (lib && lib->count() > 0 ? lib->item(0) : nullptr);
    if (!base) return "no sheet-metal rule to start from";
    Ptr<SheetMetalRule> rule = mine->addByCopy(base, "NCX " + docName_);
    if (!rule) return "could not make a sheet-metal rule: " + lastError();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.10g mm", def.value("thickness", 1.0));
    if (!rule->thickness()->expression(buf)) return "rule thickness refused: " + lastError();
    std::snprintf(buf, sizeof buf, "%.10g mm", def.value("bendRadius", 1.0));
    if (!rule->bendRadius()->expression(buf)) return "rule bend radius refused: " + lastError();
    if (def.contains("kFactor")) rule->kFactor(def.value("kFactor", 0.44));
    if (!comp_->activeSheetMetalRule(rule)) return "could not make the rule the component's: " + lastError();
    markers.insert(nf.value("id", ""));
    nf["info"] = "the component's sheet-metal rule " + rule->name();
    return "";
}

// Convert to Sheet Metal (FORMAT-SPEC 4): Fusion's own conversion (BRepBody::convertToSheetMetal) only turns a body
// that already is a sheet of the rule's thickness into a sheet-metal body; it does not make the sheet from a solid
// block, fixed face and bend edges as SolidWorks does. So the sheet is the feature's carried result (the source's
// sheet), made a Fusion sheet-metal body from the face on the fixed face's plane, with the part's rule set to the
// feature's thickness and bend radius: later sheet-metal features (edge flanges, cuts) build on it natively.
std::string Replayer::replayConvertToSheetMetal(json& nf, const json& def)
{
    if (!nf.contains("result") || !nf["result"].contains("bodies")) return "Fusion cannot make the sheet from the solid, and the file carries no result for it";
    // the part's own rule ("NCX <document>"), never one this component merely reports (a precaution: the rule this
    // function edits must not be another part's)
    Ptr<SheetMetalRule> rule = comp_->activeSheetMetalRule();
    if (!rule || rule->name() != "NCX " + docName_)
    {
        json sm{{"thickness", def.value("thickness", 1.0)}, {"bendRadius", def.value("bendRadius", 1.0)}};
        json marker{{"id", nf.value("id", "") + ":rule"}};
        std::string w = replaySheetMetal(marker, sm);
        if (!w.empty()) return "sheet-metal rule: " + w;
        rule = comp_->activeSheetMetalRule();
    }
    // the rule follows the feature (its thickness and bend radius are the feature's own)
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.10g mm", def.value("thickness", 1.0));
    if (std::fabs(cm2mm(rule->thickness()->value()) - def.value("thickness", 1.0)) > 1e-9 && !rule->thickness()->expression(buf))
        return "rule thickness refused: " + lastError();
    std::snprintf(buf, sizeof buf, "%.10g mm", def.value("bendRadius", 1.0));
    if (std::fabs(cm2mm(rule->bendRadius()->value()) - def.value("bendRadius", 1.0)) > 1e-9 && !rule->bendRadius()->expression(buf))
        return "rule bend radius refused: " + lastError();
    std::vector<Ptr<BRepBody>> before = bodies(true);
    std::string w = applyResult(nf);
    if (!w.empty()) return "its carried result could not be used: " + w;
    // the sheet: the solid body the result brought
    Ptr<BRepBody> sheet;
    for (auto& b : bodies(true))
    {
        bool old = false;
        for (auto& o : before)
            if (o == b) old = true;
        if (!old && !b->isSheetMetal()) sheet = b;
    }
    if (!sheet) return "~note:the sheet from its carried result is already a sheet-metal body";
    // the face on the fixed face's plane: by the reference (the sheet's outer face lies on the solid's), else the
    // largest planar face on that plane
    Ptr<BRepFace> fixed;
    if (def.contains("fixedFace"))
    {
        std::vector<Ptr<Base>> hit;
        json res = resolveRef({sheet}, def["fixedFace"], hit);
        if (res.value("result", "") == "resolved" && hit.size() == 1) fixed = hit[0];
        if (!fixed && def["fixedFace"].contains("geom") && def["fixedFace"]["geom"].value("type", "") == "plane")
        {
            const json& pp = def["fixedFace"]["geom"]["params"];
            V3 n = unit(v3(pp["normal"])), p0 = v3(pp["point"]);
            for (size_t i = 0; i < sheet->faces()->count(); i++)
            {
                Ptr<BRepFace> f = sheet->faces()->item(i);
                Ptr<Plane> pl = f->geometry();
                if (!pl) continue;
                V3 fn = unit(vecOf(pl->normal())), fo = ptMm(pl->origin());
                if (std::fabs(std::fabs(dot(fn, n)) - 1) > 1e-6 || std::fabs(dot(sub(fo, p0), n)) > 1e-3) continue;
                if (!fixed || f->area() > fixed->area()) fixed = f;
            }
        }
    }
    if (!fixed) return "~note:its carried result is used; the fixed face was not found on it, so the sheet is not a Fusion sheet-metal body";
    json fpBefore = bodyFingerprint(sheet);
    Ptr<Timeline> tl = design_->timeline();
    size_t n0 = tl ? tl->count() : 0;
    if (!sheet->convertToSheetMetal(fixed, rule))
    {
        nf["status"] = "approximate";
        nf["note"] = "carried as fixed geometry (its resulting bodies): Fusion refused to make it a sheet-metal body (" + lastError() + ")";
        return "";
    }
    // the conversion must keep the geometry: where Fusion's conversion changes it (its own corner reliefs or rip
    // gaps; the folding step's Short Link gained 4 mm^3), it is taken out again and the source's sheet stays a plain body
    bool changed = false;
    for (auto& b : bodies(true))
        if (b->isSheetMetal())
        {
            json fp = bodyFingerprint(b);
            if (std::fabs(fp["volume"].get<double>() - fpBefore["volume"].get<double>()) > 1e-6 * fpBefore["volume"].get<double>() + 1e-9)
                changed = true;
        }
    if (changed)
    {
        logLine(docName_ + ": convert to sheet metal changed the volume: taken out");
        if (tl && tl->count() > n0)
            if (Ptr<Feature> cf = tl->item(tl->count() - 1)->entity()) cf->deleteMe();
        nf["status"] = "approximate";
        nf["note"] = "carried as fixed geometry (its resulting bodies): Fusion's conversion to a sheet-metal body changed its shape, so it is "
                     "kept a plain body (later sheet-metal features that need it carry their results)";
        return "";
    }
    nf["status"] = "approximate";
    nf["note"] = "the sheet is the file's own result of the feature (Fusion converts only a body that already is a sheet), made a Fusion "
                 "sheet-metal body from its fixed face with the rule's thickness and bend radius; later sheet-metal features build on it";
    return "";
}

std::string Replayer::replayBaseFlange(json& nf, const json& def)
{
    std::string why;
    auto it = sketches_.find(def["profile"].value("id", ""));
    if (it == sketches_.end()) return "profile sketch not rebuilt";
    SketchMap& m = it->second;
    Ptr<FlangeFeatures> ff = comp_->features()->flangeFeatures();
    if (!comp_->activeSheetMetalRule()) return "no sheet-metal rule (the part's sheetMetal feature was not rebuilt)";
    Ptr<ObjectCollection> prof = profilesFor(def, why);
    // SolidWorks puts the thickness to the right of an open profile's direction, and against the normal of a
    // closed one, unless reversed (measured on P12, P28, P43)
    bool rev = def.value("reverseThickness", false);
    Ptr<FlangeFeature> f;
    if (prof && prof->count() > 0)
    {
        std::vector<Ptr<Profile>> ps;
        for (size_t i = 0; i < prof->count(); i++) ps.push_back(prof->item(i));
        Ptr<BaseFlangeInput> in = ff->createBaseFlangeInput(ps);
        if (!in) return "base flange input: " + lastError();
        bool along = rev != m.mirrored;   // thickness along the Fusion sketch normal?
        in->orientation(along ? SideOneFlangeOrientation : SideTwoFlangeOrientation);
        f = ff->add(in);
    }
    else
    {
        Ptr<ObjectCollection> curves = ObjectCollection::create();
        Ptr<SketchCurves> cs = m.sketch->sketchCurves();
        for (size_t i = 0; i < cs->count(); i++)
            if (!cs->item(i)->isConstruction() && !cs->item(i)->isReference()) curves->add(cs->item(i));
        Ptr<Profile> open = curves->count() ? comp_->createOpenProfile(curves, true) : nullptr;
        if (!open) return "open profile: " + lastError();
        const json& e1 = def.contains("end") ? def["end"] : json::object();
        double d1 = e1.value("depth", 0.0);
        Ptr<ContourFlangeInput> in = ff->createContourFlangeInput({open}, mmValue(d1));
        if (!in) return "contour flange input: " + lastError();
        bool ok;
        if (e1.value("type", "") == "midPlane") ok = in->setSymmetricDirection(mmValue(d1));
        else if (def.contains("end2")) ok = in->setTwoSidesDirection(mmValue(d1), mmValue(def["end2"].value("depth", 0.0)));
        else ok = in->setOneSideDirection(mmValue(def.value("reverse", false) != m.mirrored ? -d1 : d1));
        if (!ok) return "contour flange extent: " + lastError();
        in->orientation((rev != m.mirrored) ? SideTwoFlangeOrientation : SideOneFlangeOrientation);
        f = ff->add(in);
    }
    if (!f) return "base flange failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}


// ---------------------------------------------------------------------------------------- shared
namespace {

// A feature attempt that did not give the source's geometry: removed again, with whatever it left.
void dropFeature(const Ptr<Base>& f)
{
    if (Ptr<Feature> ft = f) ft->deleteMe();
}

bool healthy(const Ptr<Feature>& f)
{
    if (!f) return false;
    Ptr<TimelineObject> to = f->timelineObject();
    return !to || to->healthState() != ErrorFeatureHealthState;
}

const char* bendPositionName(BendPositionTypes t)
{
    switch (t)
    {
    case OutsideBendPositionType: return "outside";
    case InsideBendPositionType: return "inside";
    case StartEdgeBendPositionType: return "start edge";
    case TangentToSideBendPositionType: return "tangent to side";
    default: return "legacy";
    }
}

// The bend positions to try, the one matching the source's word first (FORMAT-SPEC 4: position).
std::vector<BendPositionTypes> bendPositions(const std::string& word)
{
    std::vector<BendPositionTypes> all{InsideBendPositionType, OutsideBendPositionType, StartEdgeBendPositionType, TangentToSideBendPositionType};
    BendPositionTypes first = InsideBendPositionType;
    if (word == "materialOutside") first = OutsideBendPositionType;
    else if (word == "bendOutside") first = StartEdgeBendPositionType;
    else if (word == "bendTangent") first = TangentToSideBendPositionType;
    std::vector<BendPositionTypes> v{first};
    for (auto t : all)
        if (t != first) v.push_back(t);
    return v;
}

// The outward normal of a face at a point on it (a plane's `normal` ignores the face's orientation; the face
// evaluator does not).
V3 outwardNormal(const Ptr<BRepFace>& f)
{
    Ptr<Vector3D> n;
    if (f->evaluator()->getNormalAtPoint(f->pointOnFace(), n) && n) return unit(vecOf(n));
    Ptr<Plane> p = f->geometry();
    return p ? unit(vecOf(p->normal())) : V3{0, 0, 0};
}

// The sheet's edge face at an edge: of the two planar faces meeting at the edge, the one whose width across the
// edge is the thickness (the other is a broad face of the sheet). Null if neither is.
Ptr<BRepFace> thicknessFaceAt(const Ptr<BRepEdge>& e, double t)
{
    V3 a = ptMm(e->startVertex()->geometry()), b = ptMm(e->endVertex()->geometry());
    V3 d = unit(sub(b, a));
    for (size_t i = 0; i < e->faces()->count(); i++)
    {
        Ptr<BRepFace> f = e->faces()->item(i);
        Ptr<Plane> p = f->geometry();
        if (!p) continue;
        V3 across = unit(cross(unit(vecOf(p->normal())), d));
        double lo = 1e300, hi = -1e300;
        for (size_t k = 0; k < f->vertices()->count(); k++)
        {
            double s = dot(sub(ptMm(f->vertices()->item(k)->geometry()), a), across);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        if (std::fabs(hi - lo - t) < 1e-4) return f;
    }
    return nullptr;
}

// An edge of the given bodies running from a to b (either way round).
Ptr<BRepEdge> edgeByEnds(const std::vector<Ptr<BRepBody>>& bodies, V3 a, V3 b, double tol = 1e-4)
{
    for (auto& body : bodies)
        for (size_t i = 0; i < body->edges()->count(); i++)
        {
            Ptr<BRepEdge> e = body->edges()->item(i);
            if (!e->startVertex() || !e->endVertex()) continue;
            V3 s = ptMm(e->startVertex()->geometry()), t = ptMm(e->endVertex()->geometry());
            if ((dist(s, a) < tol && dist(t, b) < tol) || (dist(s, b) < tol && dist(t, a) < tol)) return e;
        }
    return nullptr;
}

double solidVolume(const std::vector<Ptr<BRepBody>>& bodies)
{
    double v = 0;
    for (auto& b : bodies) v += cm2mm(cm2mm(cm2mm(b->volume())));
    return v;
}

// Extends the sheet by moving faces of its edges outward by d mm, as a functional equivalent where a source
// feature also extends the sheet (FORMAT-SPEC 4: edge flange `positionOffset`, jog `fixProjectedLength`).
// Fusion's offset faces first, then an extrusion of the faces themselves joined to the body (press-pull). The
// volume must grow by the faces' area times d, or the attempt is removed. Null with `why` if neither works.
Ptr<Feature> extendFaces(const Ptr<Component>& c, const std::vector<Ptr<BRepFace>>& faces, double d, std::string& why)
{
    std::vector<Ptr<BRepBody>> bs;
    for (size_t i = 0; i < c->bRepBodies()->count(); i++)
        if (c->bRepBodies()->item(i)->isSolid()) bs.push_back(c->bRepBodies()->item(i));
    double area = 0;
    for (auto& f : faces) area += cm2mm(cm2mm(f->area()));
    double v0 = solidVolume(bs);
    auto grewRight = [&]() {
        std::vector<Ptr<BRepBody>> now;
        for (size_t i = 0; i < c->bRepBodies()->count(); i++)
            if (c->bRepBodies()->item(i)->isSolid()) now.push_back(c->bRepBodies()->item(i));
        return std::fabs(solidVolume(now) - v0 - area * d) < 1e-6 * std::max(1.0, v0);
    };
    Ptr<OffsetFacesFeatures> of = c->features()->offsetFacesFeatures();
    Ptr<OffsetFacesFeatureInput> in = of ? of->createInput(faces, mmValue(d)) : nullptr;
    Ptr<OffsetFacesFeature> f = in ? of->add(in) : nullptr;
    if (healthy(f) && grewRight()) return f;
    why = f ? "offset faces gave another volume" : "offset faces refused: " + lastError();
    dropFeature(f);
    Ptr<ObjectCollection> prof = ObjectCollection::create();
    for (auto& fc : faces) prof->add(fc);
    for (double sgn : {1.0, -1.0})
    {
        Ptr<ExtrudeFeatureInput> ei = c->features()->extrudeFeatures()->createInput(prof, JoinFeatureOperation);
        if (!ei) break;
        ei->setDistanceExtent(false, mmValue(sgn * d));
        Ptr<ExtrudeFeature> ef = c->features()->extrudeFeatures()->add(ei);
        if (healthy(ef) && grewRight()) return ef;
        dropFeature(ef);
    }
    why += "; an extrusion of the faces did not extend the sheet either (" + lastError() + ")";
    return nullptr;
}

} // namespace

// The profile sketch of a feature that generates its plane (an edge flange's): its payload, from the deferred
// sketches. Null if not carried.
static const json* deferredSketch(const std::map<std::string, json>& deferred, const std::string& by, const std::string& id)
{
    auto it = deferred.find(by + "|" + id);
    return it == deferred.end() ? nullptr : &it->second;
}

// A deferred sketch whose generating feature was rebuilt natively: rebuilt on the plane its frame gives (a face of
// the new flange lies in it). Its dimensions are kept as the record of the source's intent.
std::string Replayer::replayDeferredSketchByFrame(const std::string& byFeature)
{
    std::string notes;
    for (auto it = deferred_.begin(); it != deferred_.end();)
    {
        if (it->first.rfind(byFeature + "|", 0) != 0)
        {
            ++it;
            continue;
        }
        json sk = it->second;
        json def = sk["def"];
        def["plane"] = json{{"kind", "frame"}};
        std::string w = replaySketch(sk, def);
        if (!w.empty())
        {
            markers.insert(sk.value("id", ""));
            report_.add(docName_, sk.value("name", ""), "approximate", "profile of " + byFeature + "; the sketch itself was not rebuilt: " + w);
        }
        else
        {
            Ptr<Sketch> s = features[sk.value("id", "")];
            if (s) s->isVisible(false);
            report_.add(docName_, sk.value("name", ""), sk.value("status", "") == "approximate" ? "approximate" : "rebuilt",
                        "profile of " + byFeature + " (Fusion's flange takes no sketch: the flange's height follows its dimension)" +
                            (sk.value("note", "").empty() ? "" : "; " + sk.value("note", "")));
        }
        it = deferred_.erase(it);
    }
    return notes;
}

// The flange's height parameter follows the profile sketch's dimension of the same value, so editing the sketch's
// dimension (as in the source) changes the flange.
void Replayer::linkFlangeHeight(const Ptr<FlangeFeature>& f, const std::vector<std::string>& profileSketchIds, double h, double built)
{
    Ptr<EdgeFlangeFeatureDefinition> d = f ? Ptr<EdgeFlangeFeatureDefinition>(f->definition()) : nullptr;
    if (!d || !d->distance()) return;
    for (auto& sid : profileSketchIds)
    {
        const json* sd = sketchDef(sid);
        if (!sd) continue;
        for (auto& dm : sd->value("dimensions", json::array()))
        {
            if (std::fabs(dm.value("value", 0.0) - h) > 1e-6 || !dm.value("driving", true)) continue;
            Ptr<Parameter> p = parameterFor("dim:" + sid + "|" + dm.value("id", ""));
            if (!p) continue;
            // the flange's own height may be measured from another line than the profile's (its `length` datum)
            char buf[64];
            std::snprintf(buf, sizeof buf, " + %.10g mm", built - h);
            std::string e = std::fabs(built - h) < 1e-9 ? p->name() : p->name() + buf;
            if (d->distance()->expression(e)) return;
        }
    }
}

// ---------------------------------------------------------------------------------------- edge flange
// SolidWorks' edge flange is an edge, a profile sketch (its outline gives the flange's height and width along the
// edge), an angle, a bend position and relief. Fusion's edge flange takes the edge, a height, a width definition,
// an angle, a bend position and a height datum. The height and width come from the profile; the datum, the bend
// position and the side are not pinned down by the payload in Fusion's terms, so each combination (the source's
// word first) is built and the one whose body is the feature's carried result is kept (FORMAT-SPEC 4: the
// geometry decides). Without a carried result, or when none matches, the feature falls back to its result.
std::string Replayer::replayEdgeFlange(json& nf, const json& def)
{
    if (!comp_->activeSheetMetalRule()) return "no sheet-metal rule";
    if (!nf.contains("result")) return "edge flange without a carried result to check the rebuild against";
    const json po = def.value("positionOffset", json::object());
    double offsetMm = po.value("distance", 0.0) * (po.value("reverse", false) ? -1.0 : 1.0);
    if (std::fabs(offsetMm) > 1e-9 && po.value("type", std::string("blind")) != "blind")
        return "edge flange position offset of type '" + po.value("type", std::string("?")) + "' is not built (only blind)";
    std::string fname = nf.value("name", "");
    json edgeRefs = def.contains("edges") ? def["edges"] : json::array({def["edge"]});
    json profRefs = def.contains("profiles") ? def["profiles"] : json::array({def["profile"]});
    if (edgeRefs.size() != profRefs.size()) return "edges and profiles differ in number";
    struct EdgeSpec
    {
        Ptr<BRepEdge> edge;
        V3 a{0, 0, 0}, b{0, 0, 0};   // the source's edge ends (mm)
        double L = 0, tmin = 0, tmax = 0, h = 0;
    };
    std::vector<EdgeSpec> specs;
    for (size_t k = 0; k < edgeRefs.size(); k++)
    {
        std::string why;
        EdgeSpec s;
        s.edge = refObject(edgeRefs[k], why);
        if (!s.edge) return "flange edge: " + why;
        const json* sk = deferredSketch(deferred_, fname, profRefs[k].value("id", ""));
        if (!sk) return "flange profile sketch not carried";
        const json& sd = (*sk)["def"];
        Frame f;
        f.o = v3(sd["frame"]["origin"]);
        f.x = unit(v3(sd["frame"]["x"]));
        f.z = unit(v3(sd["frame"]["z"]));
        f.y = unit(cross(f.z, f.x));
        V3 e0 = ptMm(s.edge->startVertex()->geometry()), e1 = ptMm(s.edge->endVertex()->geometry());
        V3 dir = unit(sub(e1, e0));
        s.a = e0;
        s.b = e1;
        s.L = dist(e0, e1);
        s.tmin = 1e300;
        s.tmax = -1e300;
        for (auto& e : sd["entities"])
            for (const char* k2 : {"start", "end", "p"})
                if (e.contains(k2) && e[k2].is_array() && e[k2].size() >= 2)
                {
                    V3 p = f.toModel(e[k2][0].get<double>(), e[k2][1].get<double>());
                    V3 w = sub(p, e0);
                    double t = dot(w, dir);
                    s.tmin = std::min(s.tmin, t);
                    s.tmax = std::max(s.tmax, t);
                    s.h = std::max(s.h, norm(sub(w, mul(dir, t))));
                }
        if (s.h <= 0) return "flange profile has no height";
        specs.push_back(s);
    }
    for (auto& s : specs)
        if (std::fabs(s.h - specs[0].h) > 1e-6) return "flanges of different heights in one feature";
    // the profile sketches first, in the state they were drawn in (their vertex references still exist), on the
    // plane their frame gives; they carry the flange's height dimension
    std::vector<std::string> profIds;
    for (auto& pr : profRefs) profIds.push_back(pr.value("id", ""));
    replayDeferredSketchByFrame(fname);
    std::vector<json> before = solidFingerprints();
    // FORMAT-SPEC 4 `positionOffset`: the flange placed as `position` says, moved `distance` outward along the
    // face, with the sheet extended along the face to meet its bend. Fusion's edge flange has no offset, so the
    // functional equivalent is the same thing built in two steps: the sheet's edge face moved out by the distance,
    // then the flange (same position word) on the moved edge.
    Ptr<Feature> extension;
    std::string extensionNote;
    if (std::fabs(offsetMm) > 1e-9)
    {
        double t = cm2mm(comp_->activeSheetMetalRule()->thickness()->value());
        std::vector<Ptr<BRepFace>> faces;
        std::vector<V3> shift;
        for (auto& s : specs)
        {
            Ptr<BRepFace> tf = thicknessFaceAt(s.edge, t);
            if (!tf) return "position offset: no edge face of the sheet's thickness at the flange edge";
            faces.push_back(tf);
            shift.push_back(mul(outwardNormal(tf), offsetMm));
        }
        std::string why;
        extension = extendFaces(comp_, faces, offsetMm, why);
        if (!extension) return "position offset: the sheet could not be extended (" + why + ")";
        name(extension, fname + " (offset extension)");
        for (size_t k = 0; k < specs.size(); k++)
        {
            specs[k].edge = edgeByEnds(bodies(true), add(specs[k].a, shift[k]), add(specs[k].b, shift[k]));
            if (!specs[k].edge)
            {
                dropFeature(extension);
                return "position offset: the moved flange edge was not found after extending the sheet";
            }
        }
        char buf[200];
        std::snprintf(buf, sizeof buf, "position offset %.6g mm built as the sheet's edge face moved out by %.6g mm (Fusion %s) plus the flange on the moved edge",
                      offsetMm, offsetMm, Ptr<OffsetFacesFeature>(extension) ? "offset faces" : "extrusion of the face");
        extensionNote = std::string(buf) + " (Fusion's edge flange has no offset; FORMAT-SPEC 4 positionOffset)";
    }
    Ptr<FlangeFeatures> ff = comp_->features()->flangeFeatures();
    std::string pos = neutral::field(def, "position", "flangePosition", "materialInside");
    double angle = def.value("angle", 90.0);
    int tries = 0;
    std::string lastDetail;
    bool partial = false;
    for (auto& s : specs)
        if (s.tmin > 1e-3 || s.tmax < s.L - 1e-3) partial = true;
    // The variants, most likely first. FORMAT-SPEC 4 `edgeFlange.length` gives the flange's length and the line it
    // is measured from (outer / inner virtual sharp, bend tangent): Fusion's outer faces / inner faces / tangent to
    // bend datums at that length. Then the profile's height with every datum (files without `length`).
    // Not reproducible in Fusion (P34's second flange): where a flange meets an earlier one at an end of its edge,
    // Fusion treats the meeting as a two-bend corner and trims the earlier flange's bend there (the front flange's
    // bend lost its 4 mm at the left corner, 37.7 mm^3), where SolidWorks leaves it whole. Neither Fusion's corner
    // overrides (every two-bend relief shape), a near-zero rule (miter) gap, nor holding the flange back 0.0002 mm from that end
    // changed it; the carried result is used.
    struct Variant
    {
        BendPositionTypes bp;
        FlangeHeightDatumTypes datum;
        double h;
        bool flip, swapOffsets, miter;
    };
    std::vector<Variant> vs;
    std::vector<BendPositionTypes> bps = bendPositions(pos);
    const json len = def.value("length", json::object());
    FlangeHeightDatumTypes lenDatum = OuterFacesFlangeHeightDatumType;
    double lenH = 0;
    if (len.value("type", std::string("blind")) == "blind" && len.value("distance", 0.0) > 0)
    {
        lenH = len.value("distance", 0.0);
        std::string dw = len.value("datum", std::string("outerVirtualSharp"));
        lenDatum = dw == "innerVirtualSharp" ? InnerFacesFlangeHeightDatumType : dw == "bendTangent" ? TangentToBendFlangeHeightDatumType : OuterFacesFlangeHeightDatumType;
    }
    auto addV = [&](BendPositionTypes bp, FlangeHeightDatumTypes d, double h) {
        for (bool flip : {false, true})
            for (bool sw : {false, true})
                for (bool miter : {false, true})
                {
                    if (sw && !partial) continue;
                    vs.push_back({bp, d, h, flip, sw, miter});
                }
    };
    if (lenH > 0)
        for (BendPositionTypes bp : bps) addV(bp, lenDatum, lenH);
    for (BendPositionTypes bp : bps)
        for (FlangeHeightDatumTypes datum : {OuterFacesFlangeHeightDatumType, InnerFacesFlangeHeightDatumType, TangentToBendFlangeHeightDatumType})
            addV(bp, datum, specs[0].h);
    for (const Variant& v : vs)
    {
        Ptr<EdgeFlangeInput> in = ff->createEdgeFlangeInput({}, mmValue(v.h));
        if (!in) return "edge flange input: " + lastError();
        for (auto& s : specs)
        {
            if (s.tmin <= 1e-3 && s.tmax >= s.L - 1e-3) in->edgeDefinitions()->addFullLengthEdge(s.edge);
            else
            {
                double o1 = std::max(0.0, s.tmin), o2 = std::max(0.0, s.L - s.tmax);
                if (v.swapOffsets) std::swap(o1, o2);
                in->edgeDefinitions()->addTwoOffsetsEdge(s.edge, mmValue(o1), mmValue(o2));
            }
        }
        in->angle(ValueInput::createByString(std::to_string(angle) + " deg"));
        in->bendPositionType(v.bp);
        in->heightDatumType(v.datum);
        in->isFlipped(v.flip);
        // SolidWorks does not miter separately made flanges that meet: tried first without
        in->isMiteredCorners(v.miter);
        if (!def.value("useDefaultRadius", true) && def.contains("bendRadius"))
            in->bendRadiusOverride()->setOverride(mmText(def.value("bendRadius", 1.0)));
        tries++;
        Ptr<FlangeFeature> f = ff->add(in);
        if (!healthy(f))
        {
            dropFeature(f);
            continue;
        }
        if (!matchesCarriedResult(nf, before, &lastDetail))
        {
            double vol = 0;
            for (auto& fp : solidFingerprints()) vol += fp["volume"].get<double>();
            logLine(fname + " variant bp=" + std::to_string(v.bp) + " datum=" + std::to_string(v.datum) + " h=" + std::to_string(v.h) + " flip=" + std::to_string(v.flip) +
                    " miter=" + std::to_string(v.miter) + "" + " volume " + std::to_string(vol) + ": " + lastDetail);
            if (fs::exists(dataDir() / "debug_faces") && &v == &vs.front())
                for (auto& b : bodies(true))
                    for (size_t i = 0; i < b->faces()->count(); i++)
                    {
                        Ptr<BRepFace> fc = b->faces()->item(i);
                        auto bx = boxMm(fc);
                        char buf[300];
                        std::snprintf(buf, sizeof buf, "  face %zu %s area %.4f box %.3f %.3f %.3f  %.3f %.3f %.3f", i, fc->geometry()->objectType(), fc->area() * 100,
                                      bx[0], bx[1], bx[2], bx[3], bx[4], bx[5]);
                        logLine(buf);
                    }
            dropFeature(f);
            continue;
        }
        name(f, fname);
        features[nf.value("id", "")] = f;
        char hb[32];
        std::snprintf(hb, sizeof hb, "%.6g", v.h);
        nf["info"] = std::string("Fusion edge flange: bend position ") + bendPositionName(v.bp) + ", " + hb + " mm from the " +
                     (v.datum == OuterFacesFlangeHeightDatumType ? "outer faces" : v.datum == InnerFacesFlangeHeightDatumType ? "inner faces" : "bend tangent") +
                     (v.flip ? ", flipped" : "") + (v.miter ? ", mitered corners" : "") +
                     (lenH > 0 && v.h == lenH ? " (length and datum from the file; " : " (chosen by the carried result; ") + std::to_string(tries) + " tried)";
        // the flange's height follows the profile sketch's height dimension, as in the source
        linkFlangeHeight(f, profIds, specs[0].h, v.h);
        if (!extensionNote.empty()) approx(nf, extensionNote);
        return "";
    }
    if (extension) dropFeature(extension);
    return "no Fusion edge flange variant gives the source's geometry (" + std::to_string(tries) + " tried" +
           (extension ? ", on the sheet extended for its position offset" : "") + "; last " + lastDetail + ")";
}

// ---------------------------------------------------------------------------------------- hem
// SolidWorks' hems (open, closed, teardrop, rolled) map to Fusion's open (a closed hem is an open hem with the
// source's small gap), teardrop and rolled hems. The side and bend position are chosen by the carried result.
std::string Replayer::replayHem(json& nf, const json& def)
{
    if (!nf.contains("result")) return "hem without a carried result to check the rebuild against";
    std::vector<Ptr<Base>> edges;
    std::string why = resolveEntities(def.value("edges", json::array()), edges);
    if (!why.empty()) return why;
    if (edges.size() != 1) return "hem on " + std::to_string(edges.size()) + " edges: one Fusion hem per edge is not built yet";
    Ptr<BRepEdge> edge = edges[0];
    std::string type = neutral::field(def, "hemType", "hemType", "closed");
    double len = def.value("length", 0.0), gap = def.value("gap", 0.0), r = def.value("radius", 0.0), ang = def.value("angle", 180.0);
    std::vector<json> before = solidFingerprints();
    Ptr<HemFeatures> hf = comp_->features()->hemFeatures();
    std::string lastDetail;
    int tries = 0;
    for (BendPositionTypes bp : {InsideBendPositionType, OutsideBendPositionType, StartEdgeBendPositionType, TangentToSideBendPositionType})
        for (bool flip : {false, true})
            for (int lenMode = 0; lenMode < 3; lenMode++)
            {
                // the hem length as SolidWorks measures it, or Fusion's way (with the bend, or the thickness)
                double t = comp_->activeSheetMetalRule() ? cm2mm(comp_->activeSheetMetalRule()->thickness()->value()) : 0;
                double L = lenMode == 0 ? len : lenMode == 1 ? len - gap - t : len + gap + t;
                if (L <= 0) continue;
                Ptr<HemFeatureInput> in = hf->createHemFeatureInput();
                bool ok = false;
                if (type == "open" || type == "closed") ok = in->setOpenHem(edge, mmValue(L), mmValue(std::max(gap, 0.01)), flip, bp);
                else if (type == "teardrop") ok = in->setTeardropHem(edge, mmValue(r), mmValue(L), mmValue(gap), flip, bp);
                else if (type == "rolled") ok = in->setRolledHem(edge, mmValue(r), ValueInput::createByString(std::to_string(ang) + " deg"), flip, bp);
                else return "hem type " + type + " has no Fusion counterpart here";
                if (!ok)
                {
                    lastDetail = "input refused: " + lastError();
                    continue;   // not every bend position is valid for every hem
                }
                tries++;
                Ptr<HemFeature> f = hf->add(in);
                if (!healthy(f) || !matchesCarriedResult(nf, before, &lastDetail))
                {
                    double v = 0;
                    for (auto& fp : solidFingerprints()) v += fp["volume"].get<double>();
                    logLine(nf.value("name", "") + " hem variant bp=" + std::to_string(bp) + " flip=" + std::to_string(flip) + " len=" + std::to_string(L) +
                            " volume " + std::to_string(v) + ": " + (f ? lastDetail : "refused: " + lastError()));
                    dropFeature(f);
                    continue;
                }
                name(f, nf.value("name", ""));
                features[nf.value("id", "")] = f;
                nf["info"] = "Fusion " + std::string(type == "closed" ? "open" : type) + " hem, bend position " + bendPositionName(bp) + (flip ? ", flipped" : "") +
                             " (chosen by the carried result; " + std::to_string(tries) + " tried)";
                return "";
            }
    return "no Fusion hem variant gives the source's geometry (" + std::to_string(tries) + " tried; last " + lastDetail + ")";
}

// ---------------------------------------------------------------------------------------- unfold / fold
// SolidWorks' unfold flattens the listed bends about a fixed face; fold folds them back. Fusion's unfold takes the
// stationary face and the bend faces (all bends here: the payload names the bends by the source's feature, which
// Fusion has no names for); its refold refolds an unfold. Checked against the next carried result where there is one.
// The bends' identity, for pairing a fold with its unfold: the source's own bend ids (the face descriptors differ
// between the two, as the bend is bent for one and flat for the other).
static std::string bendsKey(const json& bends)
{
    std::string k;
    for (auto& b : bends) k += b.value("feature", "") + "#" + std::to_string(b.value("sub", 0)) + "#" + b.value("name", "") + ";";
    return k;
}

std::string Replayer::replayUnfold(json& nf, const json& def, bool fold)
{
    std::string why;
    std::string key = bendsKey(def.value("bends", json::array()));
    if (fold)
    {
        auto it = unfolds_.find(key);
        if (it == unfolds_.end() || !it->second) return "the unfold this fold refolds was not rebuilt";
        Ptr<UnfoldFeature> uf = it->second;
        // Fusion's refold takes no fixed face: it keeps the unfold's stationary face fixed, and when a cut in the
        // flat state has changed that face it may keep the other side fixed (P41). The face the source keeps fixed
        // must be where the source has it after refolding; if it is not, the unfold's stationary face is moved to
        // another face of the same flat region that the flat-state features left alone, and the refold retried.
        std::vector<std::string> tried;
        auto attempt = [&](std::string& w) -> Ptr<RefoldFeature> {
            Ptr<RefoldFeatureInput> in = comp_->features()->refoldFeatures()->createInput(uf);
            if (!in)
            {
                w = "refold input: " + lastError();
                return nullptr;
            }
            Ptr<RefoldFeature> f = comp_->features()->refoldFeatures()->add(in);
            if (!f)
            {
                w = "refold failed: " + lastError();
                return nullptr;
            }
            if (def.contains("fixedFace") && !refObject(def["fixedFace"], w))
            {
                w = "the source's fixed face is not where the source has it after refolding (" + w + ")";
                f->deleteMe();
                return nullptr;
            }
            return f;
        };
        std::string w;
        Ptr<RefoldFeature> f = attempt(w);
        tried.push_back(w);
        if (!f)
        {
            // candidates: the planar faces of the unfold's body, in the state just before the unfold, that are
            // parallel to its stationary face (the faces of the same flat region on either side of the sheet)
            Ptr<TimelineObject> uto = uf->timelineObject();
            Ptr<Timeline> tl = design_->timeline();
            int restore = tl ? tl->markerPosition() : -1;
            std::vector<V3> centres;   // candidate faces, by their centre (faces are found again after each roll)
            V3 n0{0, 0, 0};
            if (uto && uto->rollTo(true))
            {
                Ptr<BRepFace> sf = uf->stationaryFace();
                Ptr<Plane> pl = sf ? Ptr<Plane>(sf->geometry()) : nullptr;
                if (pl)
                {
                    n0 = unit(vecOf(pl->normal()));
                    Ptr<BRepBody> body = sf->body();
                    std::vector<std::pair<double, V3>> c;
                    for (size_t i = 0; body && i < body->faces()->count(); i++)
                    {
                        Ptr<BRepFace> fc = body->faces()->item(i);
                        Ptr<Plane> p = fc->geometry();
                        if (!p || fc == sf) continue;
                        if (std::fabs(std::fabs(dot(unit(vecOf(p->normal())), n0)) - 1) > 1e-9) continue;
                        c.push_back({-fc->area(), ptMm(fc->pointOnFace())});
                    }
                    std::sort(c.begin(), c.end(), [](auto& a, auto& b) { return a.first < b.first; });
                    for (auto& x : c) centres.push_back(x.second);
                }
            }
            if (tl && restore >= 0) tl->markerPosition(restore);
            for (size_t k = 0; k < centres.size() && !f; k++)
            {
                bool set = false;
                if (uto && uto->rollTo(true))
                {
                    Ptr<BRepFace> sf = uf->stationaryFace();
                    Ptr<BRepBody> body = sf ? sf->body() : nullptr;
                    for (size_t i = 0; body && i < body->faces()->count() && !set; i++)
                    {
                        Ptr<BRepFace> fc = body->faces()->item(i);
                        Ptr<Point3D> pt = fc->pointOnFace();
                        if (Ptr<Plane>(fc->geometry()) && dist(ptMm(pt), centres[k]) < 1e-6) set = uf->stationaryFace(fc);
                    }
                }
                if (tl && restore >= 0) tl->markerPosition(restore);
                if (!set) continue;
                f = attempt(w);
                tried.push_back(w);
            }
            if (f)
                nf["info"] = "the unfold's stationary face moved to another face of the same flat region, so that Fusion's refold (which takes no "
                             "fixed face) keeps the source's fixed face in place";
        }
        if (!f)
        {
            std::string all;
            for (auto& t : tried) all += (all.empty() ? "" : "; ") + t;
            return "Fusion's refold keeps another face fixed than the source's fold, and takes no fixed face of its own (" + std::to_string(tried.size()) +
                   " stationary faces tried: " + all + ")";
        }
        name(f, nf.value("name", ""));
        features[nf.value("id", "")] = f;
        return "";
    }
    Ptr<BRepFace> face = def.contains("fixedFace") ? Ptr<BRepFace>(refObject(def["fixedFace"], why)) : nullptr;
    if (!face) return "unfold fixed face: " + why;
    Ptr<UnfoldFeatureInput> in = comp_->features()->unfoldFeatures()->createInput(face);
    if (!in) return "unfold input: " + lastError();
    // the bends by their faces (FORMAT-SPEC 4: unfold `bends[].face`, the inner cylindrical face); all bends
    // when the file has no faces (older files name them by the source's bend features only)
    std::vector<Ptr<BRepFace>> bendFaces;
    std::string bendWhy;
    for (auto& b : def.value("bends", json::array()))
    {
        if (!b.contains("face"))
        {
            bendWhy = "a bend without its face";
            break;
        }
        std::string w;
        Ptr<BRepFace> bf = refObject(b["face"], w);
        if (!bf)
        {
            bendWhy = "bend face: " + w;
            break;
        }
        bendFaces.push_back(bf);
    }
    bool byFaces = bendWhy.empty() && !bendFaces.empty();
    if (byFaces)
    {
        in->isUnfoldAllBends(false);
        if (!in->bendFaces(bendFaces))
        {
            byFaces = false;
            bendWhy = "bend faces refused: " + lastError();
        }
    }
    if (!byFaces) in->isUnfoldAllBends(true);
    Ptr<UnfoldFeature> f = comp_->features()->unfoldFeatures()->add(in);
    if (!f) return "unfold failed: " + lastError();
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    unfolds_[key] = f;
    if (byFaces)
        nf["info"] = std::to_string(bendFaces.size()) + " bend(s) unfolded, found by their faces";
    else
        nf["info"] = "all bends unfolded (" + bendWhy + ")";
    return "";
}


// ---------------------------------------------------------------------------------------- sketched bend
// SolidWorks' sketched bend folds the sheet along a sketch line, keeping a fixed face. Fusion's fold takes the
// same: a stationary face and bend lines with an angle and a line position. The line position and the sense of
// the angle are chosen by the carried result.
std::string Replayer::replaySketchedBend(json& nf, const json& def)
{
    if (!nf.contains("result")) return "sketched bend without a carried result to check the rebuild against";
    std::string why;
    auto sit = sketches_.find(def["line"].value("id", ""));
    if (sit == sketches_.end()) return "bend line sketch not rebuilt";
    Ptr<SketchLine> line;
    for (size_t i = 0; i < sit->second.sketch->sketchCurves()->sketchLines()->count(); i++)
    {
        Ptr<SketchLine> l = sit->second.sketch->sketchCurves()->sketchLines()->item(i);
        if (l->isConstruction() || l->isReference()) continue;
        if (line) return "a bend sketch with several lines: one Fusion bend line per line is not built yet";
        line = l;
    }
    if (!line) return "the bend sketch has no line";
    Ptr<BRepFace> face = def.contains("fixedFace") ? Ptr<BRepFace>(refObject(def["fixedFace"], why)) : nullptr;
    if (!face) return "fixed face: " + why;
    double angle = def.value("angle", 90.0);
    std::vector<json> before = solidFingerprints();
    Ptr<FoldFeatures> ff = comp_->features()->foldFeatures();
    std::string lastDetail;
    int tries = 0;
    std::string pos = neutral::field(def, "position", "flangePosition", "bendCenterline");
    std::vector<FoldBendLinePositionTypes> order{CenterFoldBendLinePositionType, StartFoldBendLinePositionType, EndFoldBendLinePositionType};
    if (pos == "materialInside" || pos == "materialOutside") order = {StartFoldBendLinePositionType, EndFoldBendLinePositionType, CenterFoldBendLinePositionType};

    // Where the bend must be (FORMAT-SPEC 4: `bendGeometry`, measured in the source just after the bend; for
    // `materialInside` the source's line is the bend's outer virtual sharp). Fusion puts its bend line at the start,
    // centre or end of the bend, so the line Fusion is given is the source's line moved across the sheet: to the
    // bend's start (the axis projected onto the sheet's plane), or half / a whole flat bend length from it.
    Ptr<Sketch> srcSketch = sit->second.sketch;
    V3 l0 = modelPointMm(line->startSketchPoint()), l1 = modelPointMm(line->endSketchPoint());
    V3 ld = unit(sub(l1, l0));
    std::vector<double> offsets{0.0};   // across the sheet from the source's line, along `across` (mm)
    V3 across{0, 0, 0};
    std::string geomNote;
    const json bg = def.value("bendGeometry", json::array());
    Ptr<Plane> facePlane = face->geometry();
    if (bg.size() == 1 && facePlane)
    {
        V3 n = unit(vecOf(facePlane->normal()));
        V3 a = v3(bg[0]["axis"]["point"]);
        double ri = bg[0].value("innerRadius", 0.0), ro = bg[0].value("outerRadius", 0.0);
        // the axis projected onto the line's plane, then its offset from the line, across it in that plane
        V3 w = sub(a, l0);
        w = sub(w, mul(n, dot(w, n)));
        w = sub(w, mul(ld, dot(w, ld)));
        double d0 = norm(w);
        if (d0 > 1e-9) across = unit(w);
        else across = unit(cross(n, ld));
        double t = ro - ri, k = 0.5;
        if (Ptr<SheetMetalRule> rule = comp_->activeSheetMetalRule()) k = rule->kFactor();
        double ba = deg2rad(angle) * (ri + k * t);   // the bend's flat length
        offsets = {d0, d0 + ba / 2, d0 - ba / 2, d0 + ba, d0 - ba, 0.0};
        geomNote = "bend axis from bendGeometry";
    }
    Ptr<Sketch> helper;   // holds the moved line (the source's sketch keeps its own)
    for (double off : offsets)
    {
        Ptr<SketchLine> useLine = line;
        if (std::fabs(off) > 1e-9)
        {
            if (!helper)
            {
                Ptr<Base> pl = srcSketch->referencePlane();
                if (!pl) pl = face;
                helper = comp_->sketches()->add(pl);
                if (!helper)
                {
                    lastDetail = "helper sketch: " + lastError();
                    continue;
                }
                std::vector<Ptr<SketchCurve>> auto_;
                for (size_t i = 0; i < helper->sketchCurves()->count(); i++)
                    if (helper->sketchCurves()->item(i)->isReference()) auto_.push_back(helper->sketchCurves()->item(i));
                for (auto& c : auto_) c->deleteMe();
            }
            V3 s = add(l0, mul(across, off)), e = add(l1, mul(across, off));
            useLine = helper->sketchCurves()->sketchLines()->addByTwoPoints(helper->modelToSketchSpace(ptCm(s)), helper->modelToSketchSpace(ptCm(e)));
            if (!useLine)
            {
                lastDetail = "moved bend line: " + lastError();
                continue;
            }
        }
        for (FoldBendLinePositionTypes lp : order)
            for (double sgn : {1.0, -1.0})
            {
                Ptr<FoldFeatureInput> in = ff->createInput(face);
                if (!in) return "fold input: " + lastError();
                if (!in->bendLines()->add(useLine, ValueInput::createByString(std::to_string(sgn * angle) + " deg"), lp, true))
                {
                    lastDetail = "bend line refused: " + lastError();
                    continue;
                }
                if (!def.value("useDefaultRadius", true)) in->bendRadiusOverride()->setOverride(mmText(def.value("radius", 1.0)));
                tries++;
                Ptr<FoldFeature> f = ff->add(in);
                if (!healthy(f) || !matchesCarriedResult(nf, before, &lastDetail))
                {
                    if (!f) lastDetail = "refused: " + lastError();
                    dropFeature(f);
                    continue;
                }
                name(f, nf.value("name", ""));
                features[nf.value("id", "")] = f;
                std::string where = lp == CenterFoldBendLinePositionType ? "centre" : lp == StartFoldBendLinePositionType ? "start" : "end";
                if (useLine != line)
                {
                    // the moved line follows the source's line: parallel to its projection, at the measured offset
                    Ptr<ObjectCollection> pr = helper->project(line);
                    Ptr<SketchLine> ref = (pr && pr->count() == 1) ? Ptr<SketchLine>(pr->item(0)) : nullptr;
                    if (ref)
                    {
                        helper->geometricConstraints()->addParallel(ref, useLine);
                        Ptr<Point3D> mid = useLine->startSketchPoint()->geometry();
                        helper->sketchDimensions()->addOffsetDimension(ref, useLine, mid);
                    }
                    helper->name(sit->second.sketch->name() + " (Fusion bend line)");
                    helper->isVisible(false);
                    char buf[200];
                    std::snprintf(buf, sizeof buf, "Fusion fold on a line %.4g mm from %s (%s), at the %s of the bend%s", std::fabs(off),
                                  sit->second.sketch->name().c_str(), geomNote.c_str(), where.c_str(), sgn < 0 ? ", angle reversed" : "");
                    nf["info"] = std::string(buf) + " (Fusion places its bend relative to its line differently from the source's '" + pos +
                                 "'; checked against the carried result; " + std::to_string(tries) + " tried)";
                }
                else
                {
                    if (helper) helper->deleteMe();
                    nf["info"] = "Fusion fold, bend line at the " + where + " of the bend" + (sgn < 0 ? ", angle reversed" : "") +
                                 " (chosen by the carried result; " + std::to_string(tries) + " tried)";
                }
                return "";
            }
        if (useLine != line) useLine->deleteMe();
    }
    if (helper) helper->deleteMe();
    return "no Fusion fold variant gives the source's geometry (" + std::to_string(tries) + " tried; last " + lastDetail + ")";
}

// ---------------------------------------------------------------------------------------- break corner
// SolidWorks' break corner chamfers or rounds the sheet's corner edges (the thickness edges); Fusion's sheet-metal
// chamfer and fillet do the same.
std::string Replayer::replayBreakCorner(json& nf, const json& def)
{
    std::vector<Ptr<Base>> ents;
    std::string why = resolveEntities(def.value("entities", json::array()), ents);
    if (!why.empty()) return why;
    std::vector<Ptr<BRepEdge>> edges;
    for (auto& e : ents)
        if (Ptr<BRepEdge> ed = e) edges.push_back(ed);
    if (edges.empty() || edges.size() != ents.size()) return "break corner on something other than edges";
    std::string type = neutral::field(def, "breakType", "breakType", "chamfer");
    double d = def.value("distance", 0.0);
    // a real value here is taken in the design's units, not centimetres (measured: 0.5 gave a 0.5 mm chamfer)
    Ptr<ValueInput> dv = ValueInput::createByString(std::to_string(d) + " mm");
    std::vector<json> before = solidFingerprints();
    Ptr<Feature> f;
    if (type == "chamfer")
    {
        Ptr<SheetMetalChamferFeatureInput> in = comp_->features()->sheetMetalChamferFeatures()->createInput();
        if (!in || !in->edgeSetInputs()->add(edges, dv)) return "sheet-metal chamfer input: " + lastError();
        f = comp_->features()->sheetMetalChamferFeatures()->add(in);
    }
    else
    {
        Ptr<SheetMetalFilletFeatureInput> in = comp_->features()->sheetMetalFilletFeatures()->createInput();
        if (!in || !in->edgeSetInputs()->add(edges, dv)) return "sheet-metal fillet input: " + lastError();
        f = comp_->features()->sheetMetalFilletFeatures()->add(in);
    }
    if (!f) return "break corner failed: " + lastError();
    std::string detail;
    if (nf.contains("result") && !matchesCarriedResult(nf, before, &detail))
    {
        dropFeature(f);
        return "the Fusion sheet-metal " + type + " differs from the source's (" + detail + ")";
    }
    name(f, nf.value("name", ""));
    features[nf.value("id", "")] = f;
    return "";
}


// ---------------------------------------------------------------------------------------- lofted bend
// SolidWorks' lofted bend (two open or closed profiles, formed or bent) is Fusion's lofted flange (die formed or
// press-brake formed). The thickness side is chosen by the carried result.
std::string Replayer::replayLoftedBend(json& nf, const json& def)
{
    if (!comp_->activeSheetMetalRule()) return "no sheet-metal rule";
    if (!nf.contains("result")) return "lofted bend without a carried result to check the rebuild against";
    std::vector<Ptr<Profile>> profs;
    for (auto& pr : def.value("profiles", json::array()))
    {
        std::string why;
        Ptr<ObjectCollection> closed = profilesFor(json{{"profile", pr}}, why);
        if (closed && closed->count() == 1)
        {
            profs.push_back(closed->item(0));
            continue;
        }
        auto it = sketches_.find(pr.value("id", ""));
        if (it == sketches_.end()) return "lofted bend profile not rebuilt";
        Ptr<ObjectCollection> curves = ObjectCollection::create();
        Ptr<SketchCurves> cs = it->second.sketch->sketchCurves();
        for (size_t i = 0; i < cs->count(); i++)
            if (!cs->item(i)->isConstruction() && !cs->item(i)->isReference()) curves->add(cs->item(i));
        // one curve: passed on its own (a collection of one gives "Failed to resolve profile path" at add)
        Ptr<Profile> open = curves->count() == 1 ? comp_->createOpenProfile(curves->item(0), false)
                                                 : curves->count() ? comp_->createOpenProfile(curves, true) : nullptr;
        if (!open) return "lofted bend open profile: " + lastError();
        profs.push_back(open);
    }
    if (profs.size() != 2) return "a lofted bend needs two profiles";
    std::vector<json> before = solidFingerprints();
    Ptr<LoftedFlangeFeatures> lf = comp_->features()->loftedFlangeFeatures();
    std::string lastDetail;
    int tries = 0;
    bool formed = def.value("formed", true);
    for (FlangeOrientations o : {SideOneFlangeOrientation, SideTwoFlangeOrientation})
    {
        Ptr<LoftedFlangeFeatureInput> in = lf->createInput(profs);
        if (!in) return "lofted flange input: " + lastError();
        in->orientation(o);
        in->formingType(formed ? DieFormLoftedFlangeFormingType : BrakeFormLoftedFlangeFormingType);
        tries++;
        Ptr<LoftedFlangeFeature> f = lf->add(in);
        if (!f || !matchesCarriedResult(nf, before, &lastDetail))
        {
            if (!f) lastDetail = "refused: " + lastError();
            if (f) f->deleteMe();
            continue;
        }
        name(f, nf.value("name", ""));
        features[nf.value("id", "")] = f;
        nf["info"] = std::string("Fusion lofted flange, ") + (formed ? "die formed" : "press-brake formed") + " (thickness side chosen by the carried result)";
        return "";
    }
    return "no Fusion lofted flange variant gives the source's geometry (" + std::to_string(tries) + " tried; last " + lastDetail + ")";
}

// ---------------------------------------------------------------------------------------- closed corner
// SolidWorks' closed corner extends the listed flange end faces to meet the flange beside them (butt, overlap,
// underlap) with a gap. Fusion's corner closure takes a dominant and a submissive edge of the two flanges, an
// alignment (symmetric gap or overlap with a ratio, flipped or not) and a gap. Which edges, and which flange
// dominates, are chosen by the carried result: every pair of straight edges near the listed face that Fusion
// accepts as a two-bend corner is tried.
std::string Replayer::replayClosedCorner(json& nf, const json& def)
{
    if (!nf.contains("result")) return "closed corner without a carried result to check the rebuild against";
    std::vector<Ptr<Base>> faces;
    std::string why = resolveEntities(def.value("faces", json::array()), faces);
    if (!why.empty()) return why;
    if (faces.size() != 1) return std::to_string(faces.size()) + " corner faces: one Fusion corner closure per corner is not built yet";
    Ptr<BRepFace> f1 = faces[0];
    if (!f1) return "the closed corner's face is not a face";
    std::string type = neutral::field(def, "cornerType", "cornerType", "butt");
    double gap = def.value("gap", 0.0), ratio = def.value("overlapRatio", 1.0);
    V3 c1 = ptMm(f1->pointOnFace());
    // straight edges of the face, and straight edges of the same body near it that are not the face's
    std::vector<Ptr<BRepEdge>> mine, nearby;
    for (size_t i = 0; i < f1->edges()->count(); i++)
        if (Ptr<Line3D>(f1->edges()->item(i)->geometry())) mine.push_back(f1->edges()->item(i));
    Ptr<BRepBody> body = f1->body();
    for (size_t i = 0; body && i < body->edges()->count(); i++)
    {
        Ptr<BRepEdge> e = body->edges()->item(i);
        if (!Ptr<Line3D>(e->geometry())) continue;
        bool onF1 = false;
        for (auto& m : mine)
            if (m == e) onF1 = true;
        if (onF1) continue;
        V3 a = ptMm(e->startVertex()->geometry()), b = ptMm(e->endVertex()->geometry());
        if (std::min(dist(a, c1), dist(b, c1)) < 25.0) nearby.push_back(e);
    }
    std::vector<json> before = solidFingerprints();
    Ptr<CornerClosureFeatures> cf = comp_->features()->cornerClosureFeatures();
    std::string lastDetail;
    int tries = 0;
    // the first edge pair Fusion takes as a two-bend corner (every pair of the corner gives the same closure)
    Ptr<BRepEdge> pairDom, pairSub;
    for (auto& e1 : mine)
        for (auto& e2 : nearby)
        {
            if (pairDom) break;
            Ptr<CornerClosureFeatureInput> probe = cf->createInput(e1, e2);
            if (probe && probe->closureType() == TwoBendCornerClosureType)
            {
                pairDom = e1;
                pairSub = e2;
            }
        }
    if (!pairDom) return "no edge pair at the corner that Fusion takes as a two-bend corner";
    // the settings: alignment (overlap either way for overlap / underlap, symmetric gap for butt), the bend
    // transition, and whether the extension follows the flange edges. Measured on P34 (overlap, gap 0.2, on the
    // carried second flange): Fusion's overlap extends the end face by the thickness less the gap (+64.8 mm^3; the
    // overlap ratio, transition and width alignment change nothing), SolidWorks' adds 54.4 mm^3: the carried result
    // is kept.
    for (int order = 0; order < 2; order++)
        for (int align = 0; align < 3; align++)
            for (double g : {gap})
                for (BendTransitionTypes bt : {SmoothCornerBendTransitionType, StraightLineCornerBendTransitionType, TrimToBendCornerBendTransitionType})
                    for (bool widthAligned : {true, false})
                    {
                        if (type == "butt" && align > 0) continue;
                        if (type != "butt" && align == 0) continue;
                        Ptr<BRepEdge> dom = order ? pairSub : pairDom, sub = order ? pairDom : pairSub;
                        Ptr<CornerClosureFeatureInput> in = cf->createInput(dom, sub);
                        if (!in) continue;
                        bool ok = align == 0 ? in->setToSymmetricGapAlignmentType()
                                             : in->setToOverlapAlignmentType(ValueInput::createByReal(ratio), align == 2);
                        ok = ok && in->gap(ValueInput::createByString(std::to_string(g) + " mm")) && in->bendTransition(bt) && in->isWidthExtentAligned(widthAligned);
                        if (!ok)
                        {
                            lastDetail = "input refused: " + lastError();
                            continue;
                        }
                        tries++;
                        Ptr<CornerClosureFeature> f = cf->add(in);
                        if (!healthy(f) || !matchesCarriedResult(nf, before, &lastDetail))
                        {
                            if (!f) lastDetail = "refused: " + lastError();
                            double vol = 0;
                            for (auto& fp : solidFingerprints()) vol += fp["volume"].get<double>();
                            logLine(nf.value("name", "") + " corner variant order=" + std::to_string(order) + " align=" + std::to_string(align) + " gap=" +
                                    std::to_string(g) + " bt=" + std::to_string(bt) + " wa=" + std::to_string(widthAligned) + " volume " + std::to_string(vol) + ": " +
                                    lastDetail);
                            dropFeature(f);
                            continue;
                        }
                        name(f, nf.value("name", ""));
                        features[nf.value("id", "")] = f;
                        nf["info"] = std::string("Fusion corner closure, ") + (align == 0 ? "symmetric gap" : align == 1 ? "overlap" : "overlap flipped") +
                                     ", gap " + std::to_string(g) + " mm (edges, dominant flange and settings chosen by the carried result; " + std::to_string(tries) +
                                     " tried)";
                        return "";
                    }
    return "no Fusion corner closure gives the source's geometry (" + std::to_string(tries) + " tried" + (tries ? "; last " + lastDetail : std::string(": no edge pair Fusion takes as a corner")) + ")";
}

// ---------------------------------------------------------------------------------------- miter flange, square ends
// FORMAT-SPEC 4 `miterFlange.gap`: at a corner between two of its flanges each flange ends square (its end face
// normal to its own edge), symmetrically, with the flanges' inner end edges `gap` apart. For a right-angled corner
// each end therefore lies gap/sqrt(2) back from the other flange's inner face. Built as one Fusion edge flange on
// all the chain's edges, each edge with its two offsets (no mitring); the other flange's inner face is found from
// its bend (`bendGeometry`: the inner face is the inner radius from the axis). Only single-line profiles normal to
// the face and right-angled corners; the height datum, bend position and side are chosen by the carried result.
std::string Replayer::miterSquareEnds(json& nf, const json& def, double h, double t)
{
    if (def.value("startOffset", 0.0) != 0 || def.value("endOffset", 0.0) != 0) return "start / end offsets are not built";
    const json bg = def.value("bendGeometry", json::array());
    std::vector<Ptr<Base>> ents;
    std::string why = resolveEntities(def.value("edges", json::array()), ents);
    if (!why.empty()) return why;
    struct E
    {
        Ptr<BRepEdge> edge;
        V3 a, b, d;          // Fusion's start and end vertex, unit direction a -> b
        V3 axis{0, 0, 0};    // its bend's axis point
        double ri = 0;
        double offA = 0, offB = 0;   // flange held back from a, from b
    };
    std::vector<E> es;
    for (auto& e : ents)
    {
        Ptr<BRepEdge> ed = e;
        if (!ed || !Ptr<Line3D>(ed->geometry())) return "a miter flange along something other than straight edges";
        E x;
        x.edge = ed;
        x.a = ptMm(ed->startVertex()->geometry());
        x.b = ptMm(ed->endVertex()->geometry());
        x.d = unit(sub(x.b, x.a));
        // its bend: the bendGeometry entry whose axis runs along the edge, nearest to it
        double best = 1e300;
        for (auto& g : bg)
        {
            V3 p = v3(g["axis"]["point"]), dir = unit(v3(g["axis"]["direction"]));
            if (std::fabs(std::fabs(dot(dir, x.d)) - 1) > 1e-6) continue;
            V3 w = sub(p, x.a);
            double dd = norm(sub(w, mul(x.d, dot(w, x.d))));
            if (dd < best)
            {
                best = dd;
                x.axis = p;
                x.ri = g.value("innerRadius", 0.0);
            }
        }
        if (best > 1e200) return "an edge without its bend in bendGeometry";
        es.push_back(x);
    }
    if (es.size() < 2) return "a single edge (no corner)";
    double g = def.value("gap", 0.0);
    int corners = 0;
    for (size_t i = 0; i < es.size(); i++)
        for (size_t j = 0; j < es.size(); j++)
        {
            if (i == j) continue;
            // a corner: a vertex of edge i that is a vertex of edge j
            for (int endI = 0; endI < 2; endI++)
            {
                V3 v = endI ? es[i].b : es[i].a;
                if (dist(v, es[j].a) > 1e-6 && dist(v, es[j].b) > 1e-6) continue;
                if (std::fabs(dot(es[i].d, es[j].d)) > 1e-6) return "a corner that is not a right angle";
                // into the plate from edge j is along edge i, away from the corner
                V3 ui = endI ? mul(es[i].d, -1) : es[i].d;
                double qj = dot(sub(es[j].axis, v), ui) - es[j].ri;   // flange j's inner face, from the corner along edge i
                double back = qj + g / std::sqrt(2.0);
                (endI ? es[i].offB : es[i].offA) = back;
                corners++;
            }
        }
    if (!corners) return "the edges do not meet at a corner";
    std::vector<json> before = solidFingerprints();
    Ptr<FlangeFeatures> ff = comp_->features()->flangeFeatures();
    std::string pos = neutral::field(def, "position", "flangePosition", "materialInside");
    std::string lastDetail;
    int tries = 0;
    for (BendPositionTypes bp : bendPositions(pos))
        for (FlangeHeightDatumTypes datum : {InnerFacesFlangeHeightDatumType, OuterFacesFlangeHeightDatumType, TangentToBendFlangeHeightDatumType})
            for (double hh : {h, h + t})
                for (bool flip : {false, true})
                    for (bool swap : {false, true})
                    {
                        Ptr<EdgeFlangeInput> in = ff->createEdgeFlangeInput({}, mmValue(hh));
                        if (!in) return "edge flange input: " + lastError();
                        for (auto& x : es)
                        {
                            // Fusion's offsets run outward from the edge's ends (measured: +2.707 made the flange 2.707
                            // longer), so holding a flange back is a negative offset
                            double o1 = -x.offA, o2 = -x.offB;
                            if (swap) std::swap(o1, o2);
                            in->edgeDefinitions()->addTwoOffsetsEdge(x.edge, mmValue(o1), mmValue(o2));
                        }
                        in->angle(ValueInput::createByString("90 deg"));
                        in->bendPositionType(bp);
                        in->heightDatumType(datum);
                        in->isFlipped(flip);
                        in->isMiteredCorners(false);
                        if (!def.value("useDefaultRadius", true) && def.contains("bendRadius"))
                            in->bendRadiusOverride()->setOverride(mmText(def.value("bendRadius", 1.0)));
                        tries++;
                        Ptr<FlangeFeature> f = ff->add(in);
                        if (!healthy(f) || !matchesCarriedResult(nf, before, &lastDetail))
                        {
                            if (!f) lastDetail = "refused: " + lastError();
                            double vol = 0;
                            std::string fps;
                            for (auto& fp : solidFingerprints())
                            {
                                vol += fp["volume"].get<double>();
                                fps += " " + fp["centroid"].dump() + " " + fp["bbox"].dump();
                            }
                            logLine(nf.value("name", "") + " square-ends variant bp=" + std::to_string(bp) + " datum=" + std::to_string(datum) + " h=" + std::to_string(hh) +
                                    " flip=" + std::to_string(flip) + " swap=" + std::to_string(swap) + " volume " + std::to_string(vol) + fps + ": " + lastDetail);
                            dropFeature(f);
                            continue;
                        }
                        name(f, nf.value("name", ""));
                        features[nf.value("id", "")] = f;
                        char buf[200];
                        std::snprintf(buf, sizeof buf, "miter flange built as one Fusion edge flange on its %zu edges, each ending square %.6g mm back from the other flange's inner face",
                                      es.size(), g / std::sqrt(2.0));
                        approx(nf, std::string(buf) + " (FORMAT-SPEC 4 miterFlange.gap; Fusion's miter flange is not in its API); bend position " + bendPositionName(bp) +
                                       (flip ? ", flipped" : "") + " (chosen by the carried result; " + std::to_string(tries) + " tried)");
                        return "";
                    }
    return "no square-ended Fusion edge flange gives the source's geometry (" + std::to_string(tries) + " tried; last " + lastDetail + ")";
}

// ---------------------------------------------------------------------------------------- miter flange
// SolidWorks' miter flange sweeps an open profile (drawn on a plane normal to the first edge, at its start) along a
// chain of edges, mitering the corners with a gap. Built as its functional equivalent: one Fusion edge flange on all
// the chain's edges with mitered corners (Fusion's own miter / contour flange along edges is not in its API), the
// component's sheet-metal rule's miter gap standing for the flange's gap. For a single-line profile (an upstand)
// that is the same shape; the height datum, bend position, side and the gap's measure are chosen by the carried
// result. Start / end offsets and profiles of more than one line are not built.
std::string Replayer::replayMiterFlange(json& nf, const json& def)
{
    Ptr<SheetMetalRule> rule = comp_->activeSheetMetalRule();
    if (!rule || !rule->gap()) return "no sheet-metal rule";
    if (!nf.contains("result")) return "miter flange without a carried result to check the rebuild against";
    if (def.value("startOffset", 0.0) != 0 || def.value("endOffset", 0.0) != 0) return "miter flange start / end offsets are not built";
    // the profile: one straight line (the flange's height); its length is the height
    const json* sd = sketchDef(def["profile"].value("id", ""));
    if (!sd) return "miter flange profile not rebuilt";
    int lines = 0;
    double h = 0;
    for (auto& e : sd->value("entities", json::array()))
    {
        if (e.value("construction", false) || e.value("type", "") == "point") continue;
        if (e.value("type", "") != "line") return "a miter flange profile with arcs or other curves is not built (only a single line)";
        lines++;
        h = std::hypot(e["end"][0].get<double>() - e["start"][0].get<double>(), e["end"][1].get<double>() - e["start"][1].get<double>());
    }
    if (lines != 1) return "a miter flange profile of " + std::to_string(lines) + " lines is not built (only a single line)";
    double t = cm2mm(rule->thickness()->value());
    std::string squareWhy = miterSquareEnds(nf, def, h, t);
    if (squareWhy.empty()) return "";
    // the rule's gap is restored unless the flange is built
    struct GapGuard
    {
        Ptr<SheetMetalRule> rule;
        std::string old;
        bool keep = false;
        ~GapGuard()
        {
            if (!keep && rule && rule->gap() && !old.empty()) rule->gap()->expression(old);
        }
    } guard{rule, rule->gap()->expression()};
    std::vector<json> before = solidFingerprints();
    Ptr<FlangeFeatures> ff = comp_->features()->flangeFeatures();
    std::string pos = neutral::field(def, "position", "flangePosition", "materialInside");
    std::string lastDetail;
    int tries = 0;
    double g0 = def.value("gap", 1.0);
    // Measured on P35 (upstand along two edges at a right angle, gap 1): no Fusion variant reproduces SolidWorks'
    // corner. Fusion's volume is linear in its miter gap (1: -15.3 mm^3; 0.7071: -0.37; 0.7: the volume matches,
    // but area and centroid still differ): the two systems shape the mitered corner differently, so the carried
    // result is used. The loop over gaps stays for other sources whose miter gap means the same as Fusion's.
    for (double g : {g0})
    {
        char gb[64];
        std::snprintf(gb, sizeof gb, "%.10g mm", g);
        if (!rule->gap()->expression(gb)) return "the rule's miter gap refused " + std::string(gb) + ": " + lastError();
        // the edges are found after the rule changed (a rule change recomputes the body and invalidates its edges)
        std::vector<Ptr<Base>> ents;
        std::string why = resolveEntities(def.value("edges", json::array()), ents);
        if (!why.empty()) return why;
        std::vector<Ptr<BRepEdge>> edges;
        for (auto& e : ents)
            if (Ptr<BRepEdge> ed = e) edges.push_back(ed);
        if (edges.empty() || edges.size() != ents.size()) return "miter flange along something other than edges";
        for (BendPositionTypes bp : bendPositions(pos))
            for (FlangeHeightDatumTypes datum : {InnerFacesFlangeHeightDatumType, OuterFacesFlangeHeightDatumType, TangentToBendFlangeHeightDatumType})
                for (double hh : {h, h + t})
                    for (bool flip : {false, true})
                    {
                        Ptr<EdgeFlangeInput> in = ff->createEdgeFlangeInput(edges, mmValue(hh));
                        if (!in) return "edge flange input: " + lastError();
                        in->angle(ValueInput::createByString("90 deg"));
                        in->bendPositionType(bp);
                        in->heightDatumType(datum);
                        in->isFlipped(flip);
                        in->isMiteredCorners(true);
                        if (!def.value("useDefaultRadius", true) && def.contains("bendRadius"))
                            in->bendRadiusOverride()->setOverride(mmText(def.value("bendRadius", 1.0)));
                        tries++;
                        Ptr<FlangeFeature> f = ff->add(in);
                        if (!healthy(f) || !matchesCarriedResult(nf, before, &lastDetail))
                        {
                            if (!f) lastDetail = "refused: " + lastError();
                            double vol = 0;
                            for (auto& fp : solidFingerprints()) vol += fp["volume"].get<double>();
                            logLine(nf.value("name", "") + " miter variant gap=" + std::to_string(g) + " bp=" + std::to_string(bp) + " datum=" + std::to_string(datum) +
                                    " h=" + std::to_string(hh) + " flip=" + std::to_string(flip) + " volume " + std::to_string(vol) + ": " + lastDetail);
                            dropFeature(f);
                            continue;
                        }
                        guard.keep = true;
                        name(f, nf.value("name", ""));
                        features[nf.value("id", "")] = f;
                        char hb[32];
                        std::snprintf(hb, sizeof hb, "%.6g", hh);
                        approx(nf, std::string("miter flange built as a Fusion edge flange on its ") + std::to_string(edges.size()) +
                                       " edges with mitered corners (Fusion's miter flange is not in its API), the rule's miter gap " + gb + "; bend position " +
                                       bendPositionName(bp) + ", " + hb + " mm from the " +
                                       (datum == OuterFacesFlangeHeightDatumType ? "outer faces" : datum == InnerFacesFlangeHeightDatumType ? "inner faces" : "bend tangent") +
                                       (flip ? ", flipped" : "") + " (chosen by the carried result; " + std::to_string(tries) + " tried)");
                        return "";
                    }
    }
    return "square-ended flanges (FORMAT-SPEC 4 gap): " + squareWhy + "; no Fusion edge flange with mitered corners gives the source's geometry either (" +
           std::to_string(tries) + " tried; last " + lastDetail + ")";
}

// ---------------------------------------------------------------------------------------- jog
// SolidWorks' jog offsets the sheet beyond a sketch line by two opposite bends. Fusion has no jog; FORMAT-SPEC 4
// says a system without one builds it as two bends placed on `bendGeometry`. Built as its functional equivalent:
//  1. `fixProjectedLength: true` (the moved part keeps its far edge, the jog adds material): the sheet's edge
//     faces beyond the jog are first moved out by the flat length the two bends use beyond their projected
//     length (Fusion offset faces), so the far edge ends where the source's does;
//  2. one Fusion fold with two bend lines on the fixed face, each at the start of its bend: the first at the
//     first bend's start (its axis projected onto the sheet), the second a bend's flat length plus the straight
//     between the bends further on. The angles have opposite senses.
// The bend lines' senses and the second line's position word are chosen by the carried result.
std::string Replayer::replayJog(json& nf, const json& def)
{
    Ptr<SheetMetalRule> rule = comp_->activeSheetMetalRule();
    if (!rule) return "no sheet-metal rule";
    if (!nf.contains("result")) return "jog without a carried result to check the rebuild against";
    const json bg = def.value("bendGeometry", json::array());
    if (bg.size() != 2) return "a jog without the geometry of its two bends (bendGeometry) cannot be placed";
    auto sit = sketches_.find(def["line"].value("id", ""));
    if (sit == sketches_.end()) return "jog line sketch not rebuilt";
    Ptr<SketchLine> line;
    for (size_t i = 0; i < sit->second.sketch->sketchCurves()->sketchLines()->count(); i++)
    {
        Ptr<SketchLine> l = sit->second.sketch->sketchCurves()->sketchLines()->item(i);
        if (l->isConstruction() || l->isReference()) continue;
        if (line) return "a jog sketch with several lines";
        line = l;
    }
    if (!line) return "the jog sketch has no line";
    std::string why;
    Ptr<BRepFace> face = def.contains("fixedFace") ? Ptr<BRepFace>(refObject(def["fixedFace"], why)) : nullptr;
    if (!face) return "jog fixed face: " + why;
    Ptr<Plane> fpl = face->geometry();
    if (!fpl) return "the jog's fixed face is not planar";
    V3 n = unit(vecOf(fpl->normal()));
    // a jog is discontinuous at 90 degrees: the angle rounded (FORMAT-SPEC 4)
    double angle = std::round(def.value("angle", 90.0) * 1e6) / 1e6;
    double th = deg2rad(angle);
    double t = cm2mm(rule->thickness()->value()), k = rule->kFactor();
    V3 l0 = modelPointMm(line->startSketchPoint()), l1 = modelPointMm(line->endSketchPoint());
    V3 ld = unit(sub(l1, l0));
    V3 lm = mul(add(l0, l1), 0.5);
    // the bends: the first is the one nearer the fixed face's plane
    V3 ax[2] = {v3(bg[0]["axis"]["point"]), v3(bg[1]["axis"]["point"])};
    double ri[2] = {bg[0].value("innerRadius", 0.0), bg[1].value("innerRadius", 0.0)};
    V3 p0 = ptMm(fpl->origin());
    if (std::fabs(dot(sub(ax[1], p0), n)) < std::fabs(dot(sub(ax[0], p0), n)))
    {
        std::swap(ax[0], ax[1]);
        std::swap(ri[0], ri[1]);
    }
    // across the line in the sheet, from the fixed side to the moved side
    V3 d12 = sub(ax[1], ax[0]);
    V3 acr = sub(d12, add(mul(n, dot(d12, n)), mul(ld, dot(d12, ld))));
    if (norm(acr) < 1e-9) return "the jog's two bends are not side by side";
    acr = unit(acr);
    double s1 = dot(sub(ax[0], lm), acr);    // where the first bend starts (flat), from the source's line
    double exit2 = dot(sub(ax[1], lm), acr);  // where the moved sheet starts (folded)
    // the straight between the bends: the axes lie on either side of it, ri + t/2 from its mid-surface
    V3 d12p = sub(d12, mul(ld, dot(d12, ld)));
    double sep = ri[0] + ri[1] + t;
    double straight = std::sqrt(std::max(0.0, dot(d12p, d12p) - sep * sep));
    double ba1 = th * (ri[0] + k * t), ba2 = th * (ri[1] + k * t);
    double s2 = s1 + ba1 + straight;
    double ext = def.value("fixProjectedLength", false) ? s2 + ba2 - exit2 : 0.0;
    // the offset the spec's words give (FORMAT-SPEC 4 jog), against the one the bends give, as a check
    std::string dp = neutral::field(def, "dimensionPosition", "jogDimension", "outsideOffset");
    double off = def.value("offset", 0.0);
    double wordShift = dp == "insideOffset" ? off + t : dp == "overall" ? off - t : off;
    double geomShift = std::fabs(dot(d12, n)) + ri[0] + ri[1] + t;
    std::string shiftNote;
    if (std::fabs(angle - 90) < 1e-6 && std::fabs(wordShift - geomShift) > 1e-3)
        shiftNote = "; the bends move the sheet " + std::to_string(geomShift) + " mm where offset/dimensionPosition say " + std::to_string(wordShift);

    std::vector<json> before = solidFingerprints();
    std::vector<Ptr<Base>> made;   // removed again if no variant matches
    auto dropAll = [&]() {
        for (auto it = made.rbegin(); it != made.rend(); ++it)
        {
            if (Ptr<Feature> f = *it) f->deleteMe();
            else if (Ptr<Sketch> s = *it) s->deleteMe();
        }
        made.clear();
    };
    // 1. the sheet beyond the jog extended (fixProjectedLength)
    std::string extNote;
    if (ext > 1e-6)
    {
        std::vector<Ptr<BRepFace>> ends;
        Ptr<BRepBody> body = face->body();
        for (size_t i = 0; body && i < body->faces()->count(); i++)
        {
            Ptr<BRepFace> f = body->faces()->item(i);
            if (!Ptr<Plane>(f->geometry())) continue;
            if (dot(outwardNormal(f), acr) < 1 - 1e-9) continue;
            bool beyond = true;
            double lo = 1e300, hi = -1e300;
            for (size_t v = 0; v < f->vertices()->count(); v++)
            {
                V3 p = ptMm(f->vertices()->item(v)->geometry());
                if (dot(sub(p, lm), acr) <= s2 + ba2) beyond = false;
                lo = std::min(lo, dot(sub(p, lm), n));
                hi = std::max(hi, dot(sub(p, lm), n));
            }
            if (beyond && std::fabs(hi - lo - t) < 1e-4) ends.push_back(f);
        }
        if (ends.empty()) return "fixProjectedLength: no edge face of the sheet beyond the jog to extend";
        Ptr<Feature> e = extendFaces(comp_, ends, ext, why);
        if (!e) return "fixProjectedLength: the sheet beyond the jog could not be extended (" + why + ")";
        name(e, nf.value("name", "") + " (projected length)");
        made.push_back(e);
        char buf[160];
        std::snprintf(buf, sizeof buf, "the sheet beyond it first extended by %.6g mm (Fusion offset faces) to keep its projected length", ext);
        extNote = buf;
    }
    // 2. the stationary face (the extension recomputed the body), found by a point on the fixed side. Fusion's fold
    //    keeps the region of its face that holds the face's centre (measured: with both lines in one fold the strip
    //    between them stayed and both sides folded), so the jog is two folds, far line first: each keeps the fixed
    //    side where it is.
    V3 onFixed = add(lm, mul(acr, s1 - 1.0));
    auto fixedFaceNow = [&]() -> Ptr<BRepFace> {
        for (auto& b : bodies(true))
            for (size_t i = 0; b && i < b->faces()->count(); i++)
            {
                Ptr<BRepFace> f = b->faces()->item(i);
                Ptr<Plane> p = f->geometry();
                if (!p || std::fabs(std::fabs(dot(unit(vecOf(p->normal())), n)) - 1) > 1e-9) continue;
                if (distanceMm(f, onFixed) < 1e-6) return f;
            }
        return nullptr;
    };
    Ptr<BRepFace> stat = fixedFaceNow();
    if (!stat)
    {
        dropAll();
        return "the jog's fixed face was not found again on the fixed side";
    }
    Ptr<Sketch> helper = comp_->sketches()->add(stat);
    if (!helper)
    {
        dropAll();
        return "helper sketch: " + lastError();
    }
    made.push_back(helper);
    {
        std::vector<Ptr<SketchCurve>> auto_;
        for (size_t i = 0; i < helper->sketchCurves()->count(); i++)
            if (helper->sketchCurves()->item(i)->isReference()) auto_.push_back(helper->sketchCurves()->item(i));
        for (auto& c : auto_) c->deleteMe();
    }
    Ptr<SketchLine> bl[2];
    double at[2] = {s1, s2};
    for (int i = 0; i < 2; i++)
    {
        V3 s = add(l0, mul(acr, at[i] + dot(sub(lm, l0), acr))), e = add(l1, mul(acr, at[i] + dot(sub(lm, l1), acr)));
        bl[i] = helper->sketchCurves()->sketchLines()->addByTwoPoints(helper->modelToSketchSpace(ptCm(s)), helper->modelToSketchSpace(ptCm(e)));
        if (!bl[i])
        {
            dropAll();
            return "jog bend line: " + lastError();
        }
    }
    // the bend lines follow the source's line: parallel to it, at their measured offsets
    {
        Ptr<ObjectCollection> pr = helper->project(line);
        Ptr<SketchLine> ref = (pr && pr->count() == 1) ? Ptr<SketchLine>(pr->item(0)) : nullptr;
        if (ref)
        {
            ref->isConstruction(true);
            for (auto& b : bl)
            {
                helper->geometricConstraints()->addParallel(ref, b);
                helper->sketchDimensions()->addOffsetDimension(ref, b, b->startSketchPoint()->geometry());
            }
        }
    }
    Ptr<FoldFeatures> ff = comp_->features()->foldFeatures();
    std::string lastDetail;
    int tries = 0;
    auto fold = [&](const Ptr<SketchLine>& l, double deg, FoldBendLinePositionTypes lp) -> Ptr<FoldFeature> {
        Ptr<BRepFace> sf = fixedFaceNow();
        Ptr<FoldFeatureInput> in = sf ? ff->createInput(sf) : nullptr;
        if (!in || !in->bendLines()->add(l, ValueInput::createByString(std::to_string(deg) + " deg"), lp, true))
        {
            lastDetail = "fold input refused: " + lastError();
            return nullptr;
        }
        if (!def.value("useDefaultRadius", true) && def.contains("radius")) in->bendRadiusOverride()->setOverride(mmText(def.value("radius", 1.0)));
        Ptr<FoldFeature> f = ff->add(in);
        if (!healthy(f))
        {
            lastDetail = "fold failed: " + lastError();
            dropFeature(f);
            return nullptr;
        }
        return f;
    };
    for (FoldBendLinePositionTypes lp2 : {StartFoldBendLinePositionType, EndFoldBendLinePositionType, CenterFoldBendLinePositionType})
        for (double sgn : {1.0, -1.0})
        {
            tries++;
            Ptr<FoldFeature> f2 = fold(bl[1], -sgn * angle, lp2);
            if (!f2) continue;
            Ptr<FoldFeature> f1 = fold(bl[0], sgn * angle, StartFoldBendLinePositionType);
            if (!f1 || !matchesCarriedResult(nf, before, &lastDetail))
            {
                double vol = 0;
                std::string fps;
                for (auto& fp : solidFingerprints())
                {
                    vol += fp["volume"].get<double>();
                    fps += " " + fp["centroid"].dump() + " " + fp["bbox"].dump();
                }
                logLine(nf.value("name", "") + " jog variant lp2=" + std::to_string(lp2) + " sgn=" + std::to_string(sgn) + " volume " + std::to_string(vol) + fps + ": " + lastDetail);
                if (fs::exists(dataDir() / "debug_faces"))
                    for (auto& b : bodies(true))
                        for (size_t i = 0; i < b->faces()->count(); i++)
                        {
                            Ptr<BRepFace> fc = b->faces()->item(i);
                            auto bx = boxMm(fc);
                            char fb[300];
                            std::snprintf(fb, sizeof fb, "  face %zu %s area %.4f box %.3f %.3f %.3f  %.3f %.3f %.3f", i, fc->geometry()->objectType(), fc->area() * 100,
                                          bx[0], bx[1], bx[2], bx[3], bx[4], bx[5]);
                            logLine(fb);
                        }
                dropFeature(f1);
                dropFeature(f2);
                continue;
            }
            name(f2, nf.value("name", "") + " (bend 2)");
            name(f1, nf.value("name", ""));
            features[nf.value("id", "")] = f1;
            helper->name(sit->second.sketch->name() + " (Fusion jog lines)");
            helper->isVisible(false);
            char buf[300];
            std::snprintf(buf, sizeof buf, "jog built as two Fusion folds on lines %.6g and %.6g mm from %s (placed on bendGeometry, opposite senses, the far one first)",
                          s1, s2, sit->second.sketch->name().c_str());
            approx(nf, std::string(buf) + (extNote.empty() ? "" : "; " + extNote) + " (Fusion has no jog; FORMAT-SPEC 4 jog; " + std::to_string(tries) + " tried)" + shiftNote);
            return "";
        }
    dropAll();
    return "no pair of Fusion folds gives the source's jog (" + std::to_string(tries) + " tried; last " + lastDetail + ")" + shiftNote;
}

} // namespace ncxf
