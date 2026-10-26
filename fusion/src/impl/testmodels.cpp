// Developer: models made in Fusion itself, for testing the export of Fusion's own features (the samples all come
// from SolidWorks, so features that only a Fusion user would make are tested here).
//
//   job {"cmd": "testModel", "name": "<model>"}  -> a new unsaved design holding the model (closeDocs closes it)
//
// Models: sm_flanges (edge flanges at 90 and 120 degrees, part width, other bend positions and datums), sm_hems (open,
// rolled, teardrop, flat), sm_fold (a fold; "position": start|center|end), sm_convert (Fusion's convert to sheet metal
// of a plate, then a flange), patterns_axes (patterns along the origin axes, a partial circular pattern), sm_unfold
// (a flange, unfolded, a slot cut across the bend in the flat, refolded), sm_corner (a break corner chamfer on a
// plate's corner), offset_faces (a block's top face offset).
#include "host.h"
#include "replay.h"

namespace ncxf {

namespace {

Ptr<ValueInput> mm(double v) { return ValueInput::createByReal(mm2cm(v)); }
Ptr<ValueInput> deg(double v) { return ValueInput::createByString(std::to_string(v) + " deg"); }

// a rectangle (mm) on the component's XY plane, its sketch
Ptr<Sketch> rectSketch(const Ptr<Component>& c, double x0, double y0, double x1, double y1, const std::string& name)
{
    Ptr<Sketch> sk = c->sketches()->add(c->xYConstructionPlane());
    sk->name(name);
    sk->sketchCurves()->sketchLines()->addTwoPointRectangle(Point3D::create(mm2cm(x0), mm2cm(y0), 0), Point3D::create(mm2cm(x1), mm2cm(y1), 0));
    return sk;
}

// the component's sheet-metal rule: a copy of the first one Fusion has, t thick, bend radius r
bool sheetRule(const Ptr<Design>& des, const Ptr<Component>& c, double t, double r)
{
    Ptr<SheetMetalRules> lib = des->librarySheetMetalRules(), mine = des->designSheetMetalRules();
    Ptr<SheetMetalRule> base = (mine && mine->count() > 0) ? mine->item(0) : (lib && lib->count() > 0 ? lib->item(0) : nullptr);
    if (!base) return false;
    Ptr<SheetMetalRule> rule = mine->addByCopy(base, "Test rule");
    if (!rule) return false;
    rule->thickness()->expression(std::to_string(t) + " mm");
    rule->bendRadius()->expression(std::to_string(r) + " mm");
    return c->activeSheetMetalRule(rule);
}

// a base flange plate from a rectangle
Ptr<FlangeFeature> plate(const Ptr<Component>& c, double w, double h)
{
    Ptr<Sketch> sk = rectSketch(c, 0, 0, w, h, "SK_Plate");
    Ptr<BaseFlangeInput> in = c->features()->flangeFeatures()->createBaseFlangeInput({sk->profiles()->item(0)});
    if (!in) return nullptr;
    in->orientation(SideOneFlangeOrientation);
    Ptr<FlangeFeature> f = c->features()->flangeFeatures()->add(in);
    if (f) f->name("Base-Plate");
    return f;
}

// the straight edge of the plate's face at height z (mm) whose middle is nearest (x, y)
Ptr<BRepEdge> edgeNear(const Ptr<Component>& c, double x, double y, double z)
{
    Ptr<BRepEdge> best;
    double bd = 1e300;
    for (size_t b = 0; b < c->bRepBodies()->count(); b++)
    {
        Ptr<BRepEdges> es = c->bRepBodies()->item(b)->edges();
        for (size_t i = 0; i < es->count(); i++)
        {
            Ptr<BRepEdge> e = es->item(i);
            if (!Ptr<Line3D>(e->geometry())) continue;
            V3 a = ptMm(e->startVertex()->geometry()), q = ptMm(e->endVertex()->geometry());
            if (std::fabs(a[2] - z) > 1e-6 || std::fabs(q[2] - z) > 1e-6) continue;
            V3 m = mul(add(a, q), 0.5);
            double d = std::hypot(m[0] - x, m[1] - y);
            if (d < bd)
            {
                bd = d;
                best = e;
            }
        }
    }
    return best;
}

// the top face's height of the plate (its sheet lies at z 0..t, or -t..0)
double topZ(const Ptr<Component>& c)
{
    double z = -1e300;
    for (size_t b = 0; b < c->bRepBodies()->count(); b++)
        z = std::max(z, ptMm(c->bRepBodies()->item(b)->boundingBox()->maxPoint())[2]);
    return z;
}

// the planar face of the component's bodies whose normal is n and whose centre is nearest p (mm)
Ptr<BRepFace> faceNear(const Ptr<Component>& c, V3 n, V3 p)
{
    Ptr<BRepFace> best;
    double bd = 1e300;
    for (size_t b = 0; b < c->bRepBodies()->count(); b++)
    {
        Ptr<BRepFaces> fs = c->bRepBodies()->item(b)->faces();
        for (size_t i = 0; i < fs->count(); i++)
        {
            Ptr<BRepFace> f = fs->item(i);
            Ptr<Plane> g = f->geometry();
            if (!g) continue;
            V3 nn = unit(vecOf(g->normal()));
            if (f->isParamReversed()) nn = mul(nn, -1);
            if (dot(nn, n) < 0.999) continue;
            double d = dist(ptMm(f->centroid()), p);
            if (d < bd) bd = d, best = f;
        }
    }
    return best;
}

Ptr<FlangeFeature> edgeFlange(const Ptr<Component>& c, const Ptr<BRepEdge>& e, double height, double angle, BendPositionTypes bp,
                              FlangeHeightDatumTypes datum, const std::string& name, double off1 = -1, double off2 = -1)
{
    Ptr<EdgeFlangeInput> in = c->features()->flangeFeatures()->createEdgeFlangeInput({}, mm(height));
    if (!in || !e) return nullptr;
    if (off1 >= 0) in->edgeDefinitions()->addTwoOffsetsEdge(e, mm(-off1), mm(-off2));   // offsets run outward: held back
    else in->edgeDefinitions()->addFullLengthEdge(e);
    in->angle(deg(angle));
    in->bendPositionType(bp);
    in->heightDatumType(datum);
    in->isMiteredCorners(false);
    Ptr<FlangeFeature> f = c->features()->flangeFeatures()->add(in);
    if (f) f->name(name);
    return f;
}

} // namespace

json testModel(const json& req)
{
    std::string name = req.value("name", "");
    Ptr<Document> doc;
    Ptr<Design> des = newDesign(doc);
    if (!des) return json{{"ok", false}, {"error", "could not make a design"}};
    Ptr<Component> c = des->rootComponent();
    json notes = json::array();
    auto need = [&](bool ok, const std::string& what) {
        if (!ok) notes.push_back(what + " failed: " + lastError());
        return ok;
    };
    if (name.rfind("sm_", 0) == 0)
    {
        if (!need(sheetRule(des, c, 2, 2), "sheet-metal rule")) return json{{"ok", false}, {"notes", notes}};
    }
    if (name == "sm_flanges")
    {
        need((bool)plate(c, 100, 60), "plate");
        double z = topZ(c);
        need((bool)edgeFlange(c, edgeNear(c, 50, 0, z), 20, 90, InsideBendPositionType, OuterFacesFlangeHeightDatumType, "Flange-90"), "flange 90");
        z = topZ(c) >= 2 ? 2 : z;
        need((bool)edgeFlange(c, edgeNear(c, 50, 60, 2), 15, 120, InsideBendPositionType, OuterFacesFlangeHeightDatumType, "Flange-120"), "flange 120");
        need((bool)edgeFlange(c, edgeNear(c, 0, 30, 2), 12, 90, OutsideBendPositionType, InnerFacesFlangeHeightDatumType, "Flange-Part", 5, 10), "flange part width");
        need((bool)edgeFlange(c, edgeNear(c, 100, 30, 2), 10, 90, TangentToSideBendPositionType, TangentToBendFlangeHeightDatumType, "Flange-Tangent"), "flange tangent");
    }
    else if (name == "sm_hems")
    {
        need((bool)plate(c, 120, 80), "plate");
        Ptr<HemFeatures> hf = c->features()->hemFeatures();
        struct H
        {
            double x, y;
            int kind;
            const char* n;
        };
        // "position": all (default) puts the four on the four edges, so the short edges' hems meet the long edges'
        // at the corners; "pair" puts flat and teardrop alone on the long edges
        std::vector<H> hems = {H{60, 0, 0, "Hem-Open"}, H{60, 80, 1, "Hem-Rolled"}, H{0, 40, 2, "Hem-Teardrop"}, H{120, 40, 3, "Hem-Flat"}};
        if (req.value("position", "all") == "pair") hems = {H{60, 0, 3, "Hem-Flat"}, H{60, 80, 2, "Hem-Teardrop"}};
        for (H h : hems)
        {
            Ptr<BRepEdge> e = edgeNear(c, h.x, h.y, 2);
            Ptr<HemFeatureInput> in = hf->createHemFeatureInput();
            bool ok = false;
            if (h.kind == 0) ok = in->setOpenHem(e, mm(10), mm(0.5), false, StartEdgeBendPositionType);
            if (h.kind == 1) ok = in->setRolledHem(e, mm(3), deg(180), false, StartEdgeBendPositionType);
            if (h.kind == 2) ok = in->setTeardropHem(e, mm(2), mm(10), mm(0.5), false, StartEdgeBendPositionType);
            if (h.kind == 3) ok = in->setFlatHem(e, mm(8), false, StartEdgeBendPositionType);
            Ptr<HemFeature> f = ok ? hf->add(in) : nullptr;
            if (f) f->name(h.n);
            need((bool)f, h.n);
        }
    }
    else if (name == "sm_fold")
    {
        need((bool)plate(c, 120, 80), "plate");
        std::string pos = req.value("position", "center");
        Ptr<Sketch> sk = c->sketches()->add(c->xYConstructionPlane());
        sk->name("SK_FoldLine");
        Ptr<SketchLine> l = sk->sketchCurves()->sketchLines()->addByTwoPoints(Point3D::create(mm2cm(70), mm2cm(-5), 0), Point3D::create(mm2cm(70), mm2cm(85), 0));
        Ptr<BRepFace> fixed;
        for (size_t i = 0; i < c->bRepBodies()->item(0)->faces()->count(); i++)
        {
            Ptr<BRepFace> f = c->bRepBodies()->item(0)->faces()->item(i);
            if (Ptr<Plane>(f->geometry()) && f->area() > 50) fixed = f;
        }
        Ptr<FoldFeatureInput> in = c->features()->foldFeatures()->createInput(fixed);
        FoldBendLinePositionTypes lp = pos == "start" ? StartFoldBendLinePositionType : pos == "end" ? EndFoldBendLinePositionType : CenterFoldBendLinePositionType;
        need(in && in->bendLines()->add(l, deg(60), lp, true), "bend line");
        Ptr<FoldFeature> f = in ? c->features()->foldFeatures()->add(in) : nullptr;
        if (f) f->name("Fold-" + pos);
        need((bool)f, "fold");
    }
    else if (name == "sm_convert")
    {
        Ptr<Sketch> sk = rectSketch(c, 0, 0, 100, 50, "SK_Plate");
        Ptr<ExtrudeFeatureInput> ein = c->features()->extrudeFeatures()->createInput(sk->profiles()->item(0), NewBodyFeatureOperation);
        ein->setDistanceExtent(false, mm(2));
        Ptr<ExtrudeFeature> ex = c->features()->extrudeFeatures()->add(ein);
        if (ex) ex->name("Plate");
        Ptr<BRepBody> b = ex ? ex->bodies()->item(0) : nullptr;
        Ptr<BRepFace> big;
        for (size_t i = 0; b && i < b->faces()->count(); i++)
            if (!big || b->faces()->item(i)->area() > big->area()) big = b->faces()->item(i);
        need(b && big && b->convertToSheetMetal(big, c->activeSheetMetalRule()), "convert to sheet metal");
        need((bool)edgeFlange(c, edgeNear(c, 50, 0, 2), 15, 90, InsideBendPositionType, OuterFacesFlangeHeightDatumType, "Flange-After"), "flange after convert");
    }
    else if (name == "sm_unfold")
    {
        need((bool)plate(c, 100, 60), "plate");
        need((bool)edgeFlange(c, edgeNear(c, 50, 0, 2), 20, 90, InsideBendPositionType, OuterFacesFlangeHeightDatumType, "Flange-Front"), "flange");
        Ptr<BRepFace> top = faceNear(c, {0, 0, 1}, {50, 30, 2});
        Ptr<UnfoldFeatureInput> ui = top ? c->features()->unfoldFeatures()->createInput(top) : nullptr;
        if (ui) ui->isUnfoldAllBends(true);
        Ptr<UnfoldFeature> uf = ui ? c->features()->unfoldFeatures()->add(ui) : nullptr;
        if (uf) uf->name("Unfold1");
        need((bool)uf, "unfold");
        // a slot across the bend zone in the flat (the flange lies flat beyond y 0 now)
        Ptr<Sketch> ss = rectSketch(c, 40, -15, 60, 10, "SK_Slot");
        Ptr<ExtrudeFeatureInput> cin = c->features()->extrudeFeatures()->createInput(ss->profiles()->item(0), CutFeatureOperation);
        cin->setAllExtent(SymmetricExtentDirection);
        Ptr<ExtrudeFeature> cut = uf ? c->features()->extrudeFeatures()->add(cin) : nullptr;
        if (cut) cut->name("Cut-Slot");
        need((bool)cut, "slot");
        Ptr<RefoldFeatureInput> ri = uf ? c->features()->refoldFeatures()->createInput(uf) : nullptr;
        Ptr<RefoldFeature> rf = ri ? c->features()->refoldFeatures()->add(ri) : nullptr;
        if (rf) rf->name("Refold1");
        need((bool)rf, "refold");
    }
    else if (name == "sm_corner")
    {
        need((bool)plate(c, 100, 60), "plate");
        // the plate's corner edge at (100, 60), across its thickness
        Ptr<BRepEdge> corner;
        for (size_t i = 0; c->bRepBodies()->count() > 0 && i < c->bRepBodies()->item(0)->edges()->count(); i++)
        {
            Ptr<BRepEdge> e = c->bRepBodies()->item(0)->edges()->item(i);
            V3 a = ptMm(e->startVertex()->geometry()), b = ptMm(e->endVertex()->geometry());
            if (std::fabs(a[0] - 100) < 1e-6 && std::fabs(b[0] - 100) < 1e-6 && std::fabs(a[1] - 60) < 1e-6 && std::fabs(b[1] - 60) < 1e-6) corner = e;
        }
        Ptr<SheetMetalChamferFeatureInput> ci = corner ? c->features()->sheetMetalChamferFeatures()->createInput() : nullptr;
        if (ci) ci->edgeSetInputs()->add({corner}, mm(8));
        Ptr<SheetMetalChamferFeature> ch = ci ? c->features()->sheetMetalChamferFeatures()->add(ci) : nullptr;
        if (ch) ch->name("Break-Corner");
        need((bool)ch, "break corner");
    }
    else if (name == "offset_faces")
    {
        Ptr<Sketch> sk = rectSketch(c, 0, 0, 100, 60, "SK_Block");
        Ptr<ExtrudeFeatureInput> ein = c->features()->extrudeFeatures()->createInput(sk->profiles()->item(0), NewBodyFeatureOperation);
        ein->setDistanceExtent(false, mm(10));
        Ptr<ExtrudeFeature> blk = c->features()->extrudeFeatures()->add(ein);
        if (blk) blk->name("Block");
        Ptr<BRepFace> top = faceNear(c, {0, 0, 1}, {50, 30, 10});
        Ptr<OffsetFacesFeatureInput> oi = top ? c->features()->offsetFacesFeatures()->createInput({top}, mm(3)) : nullptr;
        Ptr<OffsetFacesFeature> of = oi ? c->features()->offsetFacesFeatures()->add(oi) : nullptr;
        if (of) of->name("Offset-Top");
        need((bool)of, "offset faces");
    }
    else if (name == "patterns_axes")
    {
        Ptr<Sketch> sk = rectSketch(c, 0, 0, 100, 60, "SK_Block");
        Ptr<ExtrudeFeatureInput> ein = c->features()->extrudeFeatures()->createInput(sk->profiles()->item(0), NewBodyFeatureOperation);
        ein->setDistanceExtent(false, mm(10));
        Ptr<ExtrudeFeature> blk = c->features()->extrudeFeatures()->add(ein);
        if (blk) blk->name("Block");
        Ptr<Sketch> hs = c->sketches()->add(c->xYConstructionPlane());
        hs->name("SK_Hole");
        hs->sketchCurves()->sketchCircles()->addByCenterRadius(Point3D::create(mm2cm(10), mm2cm(10), 0), mm2cm(3));
        Ptr<ExtrudeFeatureInput> cin = c->features()->extrudeFeatures()->createInput(hs->profiles()->item(0), CutFeatureOperation);
        cin->setAllExtent(PositiveExtentDirection);
        Ptr<ExtrudeFeature> hole = c->features()->extrudeFeatures()->add(cin);
        if (hole) hole->name("Cut-Hole");
        Ptr<ObjectCollection> seeds = ObjectCollection::create();
        if (hole) seeds->add(hole);
        Ptr<RectangularPatternFeatureInput> rin = c->features()->rectangularPatternFeatures()->createInput(seeds, c->xConstructionAxis(),
                                                                                                         ValueInput::createByReal(4), mm(20), SpacingPatternDistanceType);
        if (rin)
        {
            rin->setDirectionTwo(c->yConstructionAxis(), ValueInput::createByReal(2), mm(30));
            Ptr<RectangularPatternFeature> rp = c->features()->rectangularPatternFeatures()->add(rin);
            if (rp) rp->name("Pattern-Axes");
            need((bool)rp, "rectangular pattern");
        }
        Ptr<Sketch> ps = c->sketches()->add(c->xYConstructionPlane());
        ps->name("SK_Pin");
        ps->sketchCurves()->sketchCircles()->addByCenterRadius(Point3D::create(mm2cm(60), mm2cm(10), 0), mm2cm(4));   // its copies stay on the block
        Ptr<ExtrudeFeatureInput> pin = c->features()->extrudeFeatures()->createInput(ps->profiles()->item(0), JoinFeatureOperation);
        pin->setDistanceExtent(false, mm(20));
        Ptr<ExtrudeFeature> pinF = c->features()->extrudeFeatures()->add(pin);
        if (pinF) pinF->name("Boss-Pin");
        Ptr<ObjectCollection> ps2 = ObjectCollection::create();
        if (pinF) ps2->add(pinF);
        Ptr<CircularPatternFeatureInput> cpi = c->features()->circularPatternFeatures()->createInput(ps2, c->zConstructionAxis());
        if (cpi)
        {
            cpi->quantity(ValueInput::createByReal(3));
            cpi->totalAngle(deg(40));
            cpi->isSymmetric(false);
            Ptr<CircularPatternFeature> cp = c->features()->circularPatternFeatures()->add(cpi);
            if (cp) cp->name("Pattern-Circular");
            need((bool)cp, "circular pattern");
        }
    }
    else
        return json{{"ok", false}, {"error", "no test model " + name}};
    return json{{"ok", true}, {"document", doc->name()}, {"notes", notes}};
}

} // namespace ncxf
