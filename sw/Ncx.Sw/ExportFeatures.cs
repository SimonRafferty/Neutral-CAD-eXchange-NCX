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

        /// <summary>A region's boundary (FORMAT-SPEC 4, `regions`): per loop of its face, in loop order, the sketch segment
        /// each edge lies on (its id; consecutive edges on one segment once), or, for an edge on no sketch segment, its
        /// curve in sketch coordinates (`{line: [start, end]}` or `{arc: [centre, radius]}`, mm). Null if a loop's edges
        /// cannot be read.</summary>
        static List<object> RegionBoundary(ISketchRegion rg, SketchIds ids, SketchFrame frame)
        {
            try
            {
                // matched against the entities as written, so a sketch block's segments count too
                var segs = (ids.Entities ?? new List<object>()).OfType<JObj>()
                    .Where(e => !e.Bool("construction")
                                && (Equals(e["type"], "line") || Equals(e["type"], "arc") || Equals(e["type"], "circle")))
                    .ToList();
                var loops = new List<object>();
                for (var loop = rg.GetFirstLoop() as ILoop2; loop != null; loop = loop.GetNext() as ILoop2)
                {
                    var items = new List<object>();
                    foreach (ICoEdge ce in (object[])loop.GetCoEdges() ?? new object[0])
                    {
                        var e = (IEdge)ce.GetEdge();
                        var cp = (double[])e.GetCurveParams2() ?? new double[0];
                        var curve = (ICurve)e.GetCurve();
                        if (cp.Length < 8 || curve == null) return null;
                        // the edge's ends and middle, in sketch coordinates (mm). The ends come from GetCurveParams2
                        // itself: a line curve's Evaluate2 does not take these start/end parameters (the points came
                        // out displaced along the line), so the middle of a line is its ends' average, and of a circle
                        // the circle point nearest that average (the arc test below needs only the circle).
                        Func<double, double, double, double[]> sk = (x, y, z) =>
                        {
                            var q = frame.ToSketch(new[] { x, y, z });
                            return new[] { U.Mm(q[0]), U.Mm(q[1]) };
                        };
                        var a = sk(cp[0], cp[1], cp[2]);
                        var b = sk(cp[3], cp[4], cp[5]);
                        if (!ce.GetSense()) { var t = a; a = b; b = t; }   // run along the loop, so its pieces join end to end
                        var mid = new[] { (a[0] + b[0]) / 2, (a[1] + b[1]) / 2 };
                        if (curve.IsCircle())
                        {
                            var near = (double[])curve.GetClosestPointOn((cp[0] + cp[3]) / 2, (cp[1] + cp[4]) / 2, (cp[2] + cp[5]) / 2);
                            if (near != null) mid = sk(near[0], near[1], near[2]);
                        }
                        var seg = segs.FirstOrDefault(s => OnEntity(s, mid));
                        if (seg != null)
                        {
                            string id = seg.Str("id");
                            if (items.Count == 0 || !Equals(items[items.Count - 1], id)) items.Add(id);
                            continue;
                        }
                        if (curve.IsCircle())
                        {
                            var cpar = (double[])curve.CircleParams;
                            var c = sk(cpar[0], cpar[1], cpar[2]);
                            items.Add(new JObj { { "arc", new object[] { new[] { U.R(c[0]), U.R(c[1]) }, U.R(U.Mm(cpar[6])) } } });
                        }
                        else
                            items.Add(new JObj { { "line", new object[] { new[] { U.R(a[0]), U.R(a[1]) }, new[] { U.R(b[0]), U.R(b[1]) } } } });
                    }
                    loops.Add(items);
                }
                return loops.Count > 0 ? loops : null;
            }
            catch (Exception) { return null; }   // the boundary is optional: the point and area still select the region
        }

        /// <summary>Does a point (sketch mm) lie on this written sketch line, or on this arc's or circle's circle
        /// (within 1e-4 mm)?</summary>
        static bool OnEntity(JObj e, double[] p)
        {
            const double tol = 1e-4;
            Func<object, double[]> pt = o => ((System.Collections.IEnumerable)o).Cast<object>().Select(Convert.ToDouble).ToArray();
            if (Equals(e["type"], "line"))
            {
                var a = pt(e["start"]);
                var b = pt(e["end"]);
                double dx = b[0] - a[0], dy = b[1] - a[1], len2 = dx * dx + dy * dy;
                if (len2 < 1e-12) return false;
                double t = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / len2;
                if (t < -1e-6 || t > 1 + 1e-6) return false;
                return Math.Abs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / Math.Sqrt(len2) < tol;
            }
            var c = pt(e["center"]);
            double r = Convert.ToDouble(e["radius"]);
            return Math.Abs(Math.Sqrt((p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1])) - r) < tol;
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
                    var region = new JObj { { "point", new[] { U.R(U.Mm(q[0])), U.R(U.Mm(q[1])) } }, { "area", face.GetArea() * 1e6 },
                                            { "clearance", clearance } };
                    var boundary = RegionBoundary(rg, ids, frame);
                    if (boundary != null) region["boundary"] = boundary;
                    regions.Add(region);
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

        /// <summary>Is a selected region bounded partly by something other than the sketch's own entities (a model
        /// edge, the edge of the face the sketch lies on)? Another system may not find that region the same way, so
        /// the feature's result goes with it.</summary>
        static bool RegionsLeaveSketch(JObj def)
        {
            foreach (JObj rg in def.Arr("regions"))
                foreach (var loop in rg.Arr("boundary"))
                    if (((System.Collections.IList)loop).Cast<object>().Any(piece => !(piece is string))) return true;
            return false;
        }

        /// <summary>Does a normal cut cross a bend? Its profile reaches past the face of the sheet it is drawn on,
        /// or its walls are not all square to the sketch (a flat plane along the cut direction, or a cylinder about
        /// it): some follow the bent sheet.</summary>
        bool NormalCutCrossesBend(IFeature f)
        {
            IFeature skf = null;
            for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                if (sub.GetTypeName2() == "ProfileFeature") { skf = sub; break; }
            if (skf == null) return false;
            var sk = (ISketch)skf.GetSpecificFeature2();
            var fr = new SketchFrame(sw, sk);
            int refType = 0;
            var onFace = sk.GetReferenceEntity(ref refType) as IFace2;
            if (onFace != null)
                foreach (ISketchPoint pt in (object[])sk.GetSketchPoints2() ?? new object[0])
                {
                    var m = fr.ToModel(pt.X, pt.Y);
                    var c = (double[])onFace.GetClosestPointOn(m[0], m[1], m[2]);
                    if (c != null && V.Dist(V.P3(c), m) > 1e-6) return true;   // a profile point off the face: over a bend
                }
            var z = fr.Z;
            foreach (IFace2 face in (object[])f.GetFaces() ?? new object[0])
            {
                var srf = (ISurface)face.GetSurface();
                if (srf.IsPlane())
                {
                    var pp = (double[])srf.PlaneParams;
                    double c = Math.Abs(V.Dot(V.Unit(new[] { pp[0], pp[1], pp[2] }), z));
                    if (c > 1e-6 && c < 1 - 1e-6) return true;   // neither a wall along the cut nor a floor
                }
                else if (srf.IsCylinder())
                {
                    var cp = (double[])srf.CylinderParams;
                    if (Math.Abs(V.Dot(V.Unit(new[] { cp[3], cp[4], cp[5] }), z)) < 1 - 1e-6) return true;
                }
                else return true;
            }
            return false;
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
                    if (cwhy == null && RegionsLeaveSketch(def)) needsResult.Add(nf);
                }
                def["reverse"] = d.ReverseDirection;
                def["end"] = End(d, true, nf);
                if (d.BothDirections) def["end2"] = End(d, false, nf);
                if (((JObj)def["end"]).Str("type") == "throughAllBoth")
                {
                    // SolidWorks' "Through All - Both" is one end condition; neutrally it is through all both ways
                    def["end"] = new JObj { { "type", "throughAll" } };
                    def["end2"] = new JObj { { "type", "throughAll" } };
                }
                if (!boss && d.FlipSideToCut) def["flipSide"] = true;
                // sheet metal: a cut normal to the sheet across bends; a cut's or boss's depth tied to the thickness
                if (!boss && d.NormalCut)
                {
                    def["normalCut"] = true;
                    // across a bend it removes material square to the bent sheet, which a system without normal cuts
                    // cannot make: its result goes with it
                    if (NormalCutCrossesBend(f)) needsResult.Add(nf);
                }
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
