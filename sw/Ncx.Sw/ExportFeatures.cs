// Exporter: solid features (extrude/cut, fillet, chamfer for now).
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        // swEndConditions_e -> neutral end condition
        static readonly Dictionary<int, string> EndTypes = new Dictionary<int, string>
        {
            { 0, "blind" }, { 1, "throughAll" }, { 2, "upToNext" }, { 3, "upToVertex" }, { 4, "upToSurface" },
            { 5, "offsetFromSurface" }, { 6, "midPlane" }, { 7, "upToBody" }, { 9, "throughAllBoth" }, { 11, "upToNext" }
        };

        JObj ProfileRef(IFeature f, JObj nf)
        {
            var sub = (IFeature)f.GetFirstSubFeature();
            if (sub == null) { Opaque(nf, "no profile sketch"); return null; }
            var r = FeatureRef(sub);
            if (r == null) Opaque(nf, "profile " + sub.Name + " was not exported");
            return r;
        }

        JObj End(IExtrudeFeatureData2 d, bool forward, JObj nf)
        {
            int t = d.GetEndCondition(forward);
            string name;
            if (!EndTypes.TryGetValue(t, out name)) { Approx(nf, "end condition " + t + " replaced by blind"); name = "blind"; }
            var e = new JObj { { "type", name } };
            if (name == "blind" || name == "midPlane" || name == "offsetFromSurface") e["depth"] = U.Mm(d.GetDepth(forward));
            if (name == "upToSurface" || name == "offsetFromSurface")
            {
                var face = d.GetFace(forward);
                var r = EntityRef(face);
                if (r == null) Approx(nf, "up-to surface not recognised");
                else e["target"] = r;
            }
            if (name == "upToVertex")
            {
                var r = EntityRef(d.GetVertex(forward));   // the vertex (or sketch point) it runs up to
                if (r == null) Approx(nf, "up-to vertex not recognised");
                else e["target"] = r;
            }
            if (d.GetDraftWhileExtruding(forward))
            {
                e["draft"] = U.Deg(d.GetDraftAngle(forward));
                e["draftOutward"] = d.GetDraftOutward(forward);
            }
            if (d.GetReverseOffset(forward)) e["offsetReverse"] = true;
            return e;
        }

        // swThinWallType_e
        static readonly string[] ThinTypes = { "oneDirection", "oppositeDirection", "midPlane", "twoDirection" };

        /// <summary>Where the extrusion starts when not at the sketch plane (swStartConditions_e).</summary>
        JObj Start(IExtrudeFeatureData2 d, JObj nf)
        {
            switch (d.FromType)
            {
                case 0: return null;   // the sketch plane
                case 3: return new JObj { { "type", "offset" }, { "distance", U.Mm(d.FromOffsetDistance) }, { "reverse", d.FromOffsetReverse } };
                case 1:
                case 2:
                {
                    object ent;
                    int entType;
                    d.GetFromEntity(out ent, out entType);
                    var r = EntityRef(ent);
                    if (r == null) { Approx(nf, "start " + (d.FromType == 1 ? "surface" : "vertex") + " not recognised; sketch plane used"); return null; }
                    return new JObj { { "type", d.FromType == 1 ? "surface" : "vertex" }, { "target", r } };
                }
            }
            Approx(nf, "start condition " + d.FromType + " replaced by sketch plane");
            return null;
        }

        /// <summary>The selected part of the profile sketch (FORMAT-SPEC 4, extrude): closed contours as the ids of the
        /// sketch segments around each (`contours`), and regions (an area between crossing or nested segments, picked
        /// by clicking in it) as a point well inside each with its area (`regions`). Returns why it could not be
        /// carried, or null.</summary>
        /// <summary>The point (mm) well inside a sketch region's face, and its distance from the region's edges.
        /// From the face's triangulation where SolidWorks gives one (as face samples are chosen); a region's face
        /// often has none, so else from a grid over its parameter range, keeping the inside point furthest from
        /// the edges.</summary>
        static List<double[]> RegionPoint(IFace2 face, out double clearance)
        {
            clearance = 0;
            try
            {
                var pts = Host.FaceSamples(face, 1, out clearance);
                if (pts.Count > 0) return pts;
            }
            catch (Exception) { }   // no triangulation (a DBNull from GetTessTriangles, through a dynamic cast)
            var uv = face.GetUVBounds() as double[];
            var surf = face.GetSurface() as ISurface;
            var edges = ((object[])face.GetEdges() ?? new object[0]).Cast<IEdge>().ToList();
            if (uv == null || surf == null) return new List<double[]>();
            const int N = 24;
            double[] best = null;
            for (int i = 1; i < N; i++)
                for (int j = 1; j < N; j++)
                {
                    double u = uv[0] + (uv[1] - uv[0]) * i / N, v = uv[2] + (uv[3] - uv[2]) * j / N;
                    var ev = surf.Evaluate(u, v, 0, 0) as double[];
                    if (ev == null) continue;
                    var pMm = U.Mm(new[] { ev[0], ev[1], ev[2] });
                    if (Host.DistanceMm(face, pMm) > 1e-6) continue;   // outside the region (its parameter box is larger)
                    double c = edges.Count == 0 ? 1.0 : edges.Min(e => Host.DistanceMm(e, pMm));
                    if (c > clearance) { clearance = c; best = pMm; }
                }
            return best == null ? new List<double[]>() : new List<double[]> { best };
        }

        int unreadableContours;   // set by SelectedProfile: selections the API returned as nothing

        string SelectedProfile(JObj def, object[] items, IFeature sketchFeature)
        {
            unreadableContours = 0;
            SketchIds ids;
            // a sketch several features use is listed under each as "<name><2>"...
            string sketchName = System.Text.RegularExpressions.Regex.Replace(sketchFeature.Name, @"<\d+>$", "");
            if (items == null) return "no contour list";
            if (!sketchMaps.TryGetValue(sketchName, out ids)) return "sketch " + sketchName + " not exported";
            var contours = new List<object>();
            var regions = new List<object>();
            SketchFrame frame = null;
            foreach (var c in items)
            {
                var rg = c as ISketchRegion;
                if (rg != null)
                {
                    // the region's own face: its most interior point (as face samples are chosen, FORMAT-SPEC 5) in
                    // sketch coordinates, its area, and how far that point is from the region's edges
                    var face = (rg.GetFirstLoop() as ILoop2)?.GetFace() as IFace2;
                    if (face == null) return "a sketch region has no face";
                    double clearance;
                    var pts = RegionPoint(face, out clearance);
                    if (pts.Count == 0) return "no point found inside a sketch region";
                    if (frame == null) frame = new SketchFrame(sw, (ISketch)sketchFeature.GetSpecificFeature2());
                    var q = frame.ToSketch(pts[0].Select(U.M).ToArray());
                    regions.Add(new JObj { { "point", new[] { U.R(U.Mm(q[0])), U.R(U.Mm(q[1])) } }, { "area", face.GetArea() * 1e6 },
                                           { "clearance", clearance } });
                    continue;
                }
                if (c == null) { unreadableContours++; continue; }   // SolidWorks lists it but returns nothing for it
                var ct = c as ISketchContour;
                if (ct == null) return "an unknown contour object is not carried";
                var segs = new List<object>();
                foreach (var s in (object[])ct.GetSketchSegments() ?? new object[0])   // GetEdges gives model edges
                {
                    string id = ids.Of(s);
                    if (id == null)
                        return "a contour item (" + (s is ISketchSegment ? "segment " + SketchIds.Key(s) : s is IEdge ? "model edge" :
                               s is IEntity ? "entity" : s == null ? "null" : "unknown") + ") is not in the exported sketch";
                    segs.Add(id);
                }
                contours.Add(segs);
            }
            if (contours.Count > 0) def["contours"] = contours;
            if (regions.Count > 0) def["regions"] = regions;
            return null;
        }

        void ExportExtrude(IFeature f, JObj nf)
        {
            var d = (IExtrudeFeatureData2)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                // GetTypeName2 is "ICE" for both boss and cut extrudes and IsBossFeature reads false for both;
                // the legacy GetTypeName says "Cut"/"CutThin" for cuts
                bool boss = !f.GetTypeName().StartsWith("Cut");
                nf["type"] = boss ? "extrude" : "cut";
                var def = new JObj { { "profile", ProfileRef(f, nf) } };
                if (nf.Str("status") == "opaque") return;
                if (d.GetContoursCount() > 0)
                {
                    string cwhy = SelectedProfile(def, (object[])d.Contours, (IFeature)f.GetFirstSubFeature());
                    if (cwhy != null) Approx(nf, "selected contours not carried (" + cwhy + "); whole sketch used");
                    else if (unreadableContours > 0)
                    {
                        // built from the rest; the result it carries shows whether that is the same
                        Approx(nf, unreadableContours + " selected contour(s) not readable through SolidWorks' API: built from the others");
                        needsResult.Add(nf);
                    }
                }
                def["reverse"] = d.ReverseDirection;
                def["end"] = End(d, true, nf);
                if (d.BothDirections) def["end2"] = End(d, false, nf);
                if (!boss && d.FlipSideToCut) def["flipSide"] = true;
                // sheet metal: a cut normal to the sheet across bends; a cut's or boss's depth tied to the thickness
                if (!boss && d.NormalCut) def["normalCut"] = true;
                if (d.LinkToThickness) def["linkToThickness"] = true;
                if (boss) def["merge"] = d.Merge;
                var start = Start(d, nf);
                if (start != null) def["start"] = start;
                if (d.IsThinFeature())
                    def["thin"] = new JObj
                    {
                        { "type", ThinTypes[Math.Max(0, Math.Min(3, d.ThinWallType))] },
                        { "thickness", U.Mm(d.GetWallThickness(true)) }, { "thickness2", U.Mm(d.GetWallThickness(false)) },
                        { "capEnds", d.CapEnds }, { "capThickness", U.Mm(d.CapThickness) }
                    };
                // FeatureScope/AutoSelect read back as "explicit" even on single-body parts; only a multibody
                // part with named scope bodies loses something
                int nBodies = part == null ? 0 : ((object[])part.GetBodies2(0, false) ?? new object[0]).Length;   // (none in an assembly)
                if (d.FeatureScope && !d.AutoSelect && nBodies > 1 && d.GetFeatureScopeBodiesCount() > 0)
                    Approx(nf, "explicit feature scope (selected bodies) not carried");
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportFillet(IFeature f, JObj nf)
        {
            var data = f.GetDefinition();
            var d = data as ISimpleFilletFeatureData2;
            if (d == null) { Opaque(nf, "fillet kind not mapped yet"); return; }
            // AccessSelections rolls the model back, so the descriptors below are in the fillet's input state
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "fillet";
                int type = d.Type;   // swSimpleFilletType_e: 0 constant radius, 2 face, 3 full round
                var def = new JObj { { "radius", U.Mm(d.DefaultRadius) },
                                     { "filletType", type == 2 ? "face" : type == 3 ? "fullRound" : "constant" } };
                if (type == 2 || type == 3)
                {
                    // face sets: face fillet 1, 2; full round side 1, centre, side 2. The lists are numbered from 1
                    // (asking for 0 or one past the end quietly returns list 1 again)
                    for (int i = 0; i < (type == 2 ? 2 : 3); i++)
                    {
                        var refs = new List<object>();
                        foreach (var item in (object[])d.GetFaces(i + 1) ?? new object[0])
                        {
                            var r = EntityRef(item);
                            if (r == null) { Opaque(nf, "fillet face not recognised"); return; }
                            refs.Add(r);
                        }
                        def["faces" + (i + 1)] = refs;
                    }
                }
                else
                {
                    var edges = new List<object>();
                    var radii = new List<object>();
                    foreach (var item in (object[])d.Edges ?? new object[0])
                    {
                        var r = EntityRef(item);
                        if (r == null) { Opaque(nf, "fillet edge not recognised"); return; }
                        edges.Add(r);
                        radii.Add(U.Mm(d.GetRadius(item)));
                    }
                    if (edges.Count > 0) def["edges"] = edges;
                    if (d.IsMultipleRadius) def["radii"] = radii;   // one per edge, in edge order
                }
                if (d.GetFeatureCount() > 0) Approx(nf, "fillet of whole features not carried");
                // the edge list is what the user picked; propagation along tangent edges happens at replay too
                if (d.PropagateToTangentFaces) def["propagate"] = true;
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>A variable-radius fillet: its edges and the radius at each of their vertices.</summary>
        void ExportVariableFillet(IFeature f, JObj nf)
        {
            var d = (IVariableFilletFeatureData2)f.GetDefinition();
            d.AccessSelections(doc, null);   // the descriptors are in the fillet's input state
            try
            {
                nf["type"] = "fillet";
                var edges = new List<object>();
                var vertexRadii = new List<object>();
                var seen = new List<double[]>();
                for (int i = 0; i < d.FilletEdgeCount; i++)
                {
                    var e = (IEdge)d.GetFilletEdgeAtIndex(i);
                    var r = EntityRef(e);
                    if (r == null) { Opaque(nf, "fillet edge not recognised"); return; }
                    edges.Add(r);
                    foreach (var v in new[] { (IVertex)e.GetStartVertex(), (IVertex)e.GetEndVertex() })
                    {
                        if (v == null) continue;
                        var p = (double[])v.GetPoint();
                        if (seen.Any(q => V.Dist(q, p) < 1e-9)) continue;
                        seen.Add(p);
                        vertexRadii.Add(new JObj { { "vertex", Host.VertexDescriptor(v) }, { "radius", U.Mm(d.GetRadius(v)) } });
                    }
                }
                nf["def"] = new JObj
                {
                    { "filletType", "variable" }, { "radius", U.Mm(d.DefaultRadius) }, { "edges", edges },
                    { "vertexRadii", vertexRadii }, { "propagate", d.PropagateToTangentFaces },
                    { "transition", Neutral.Word("filletTransition", d.TransitionType) }
                };
                if (d.GetControlPointsCount() > 0) Approx(nf, d.GetControlPointsCount() + " control points along the edges not carried (vertex radii are)");
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>Each leg of a vertex chamfer: {direction (unit, from the vertex along its edge), distance}, from
        /// the corners of the chamfer's triangular face; null when that face is not found.</summary>
        static List<object> VertexChamferLegs(IFeature f, double[] vertexMm)
        {
            if (vertexMm == null) return null;
            foreach (IFace2 face in (object[])f.GetFaces() ?? new object[0])
            {
                if (!((ISurface)face.GetSurface()).IsPlane()) continue;
                var corners = new List<double[]>();
                foreach (IEdge e in (object[])face.GetEdges() ?? new object[0])
                    foreach (var vx in new[] { (IVertex)e.GetStartVertex(), (IVertex)e.GetEndVertex() })   // edges need not run head to tail
                    {
                        if (vx == null) continue;
                        var s = U.Mm((double[])vx.GetPoint());
                        if (!corners.Any(c => V.Norm(V.Sub(c, s)) < 1e-6)) corners.Add(s);
                    }
                if (corners.Count != 3) continue;
                return corners.Select(c => (object)new JObj
                {
                    { "direction", V.Unit(V.Sub(c, vertexMm)) }, { "distance", V.Norm(V.Sub(c, vertexMm)) }
                }).ToList();
            }
            return null;
        }

        void ExportChamfer(IFeature f, JObj nf)
        {
            var d = (IChamferFeatureData2)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "chamfer";
                int t = d.Type;   // swChamferType_e: 1 angle-distance, 2 distance-distance, 16 equal distance, 3 vertex
                if (t == 3)
                {
                    var v = EntityRef(d.Vertex);
                    if (v == null) { Opaque(nf, "chamfer vertex not recognised"); return; }
                    nf["def"] = new JObj
                    {
                        { "chamferType", "vertex" }, { "vertex", v },
                        { "distances", new[] { U.Mm(d.GetVertexChamferDistance(0)), U.Mm(d.GetVertexChamferDistance(1)), U.Mm(d.GetVertexChamferDistance(2)) } }
                    };
                    // which distance goes along which edge, in neutral form: the chamfer's triangular face has one
                    // corner on each edge, at that edge's distance from the vertex
                    // (measured once the model is rolled forward: the face does not exist in the state the chamfer saw)
                    var vp = v.Obj("geom")?.Obj("params")?.Vec("point");
                    afterExport.Add(() =>
                    {
                        var legs = VertexChamferLegs(f, vp);
                        if (legs != null) ((JObj)nf["def"])["legs"] = legs;
                        else
                        {
                            const string why = "vertex chamfer: which distance runs along which edge not measured";
                            Approx(nf, why);
                            Report.Add(docId, nf.Str("name"), "approximate", why);   // the feature was reported before this ran
                        }
                    });
                    return;
                }
                var edges = ((object[])d.Edges ?? new object[0]).ToList();
                var refs = new List<object>();
                var flipped = new List<object>();
                foreach (var e in edges)
                {
                    var r = EntityRef(e);
                    if (r == null) { Opaque(nf, "chamfer edge not recognised"); return; }
                    refs.Add(r);
                    flipped.Add(d.GetIsFlipped(e));
                }
                var def = new JObj
                {
                    { "chamferType", t == 1 ? "angleDistance" : t == 2 ? "distanceDistance" : "equalDistance" },
                    { "distance", U.Mm(d.GetEdgeChamferDistance(0)) }, { "edges", refs }, { "flipped", flipped }
                };
                if (t == 1) def["angle"] = U.Deg(d.EdgeChamferAngle);
                if (d.TangentPropagation) def["propagate"] = true;
                if (t == 2) def["distance2"] = U.Mm(d.GetEdgeChamferDistance(1));
                if (((object[])d.Faces ?? new object[0]).Length > 0) Approx(nf, "face chamfer items not carried");
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }
    }
}
