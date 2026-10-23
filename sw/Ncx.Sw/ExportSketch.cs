// Exporter: sketches. Entities carry solved coordinates (sketch frame, mm) as replay seeds; relations
// and dimensions refer to entities by local id; the sketch plane is a reference (origin plane, plane
// feature or face descriptor) plus the explicit frame.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        // swConstraintType_e -> neutral constraint type. Dimension-like relations are exported as dimensions.
        static readonly Dictionary<int, string> Relations = new Dictionary<int, string>
        {
            { 4, "horizontal" }, { 5, "vertical" }, { 6, "tangent" }, { 7, "parallel" }, { 8, "perpendicular" },
            { 9, "coincident" }, { 10, "concentric" }, { 11, "symmetric" }, { 12, "midpoint" }, { 13, "intersection" },
            { 14, "equal" }, { 17, "fix" }, { 25, "horizontalPoints" }, { 26, "verticalPoints" }, { 27, "collinear" },
            { 28, "coradial" }, { 40, "pierce" },
            // 3D sketches: along a model axis (SolidWorks makes 64/65 for x/y and 50 for z through its 3D constraint ids)
            { 48, "alongX" }, { 49, "alongY" }, { 50, "alongZ" }, { 64, "alongX" }, { 65, "alongY" }
        };
        static readonly HashSet<int> DimensionRelations = new HashSet<int> { 1, 2, 3, 15, 41, 43, 44, 84 };
        static readonly Dictionary<int, string> DimKinds = new Dictionary<int, string>
        {
            { 2, "distance" }, { 11, "horizontal" }, { 12, "vertical" }, { 3, "angle" }, { 5, "radius" }, { 6, "diameter" }
        };

        class SketchIds
        {
            public readonly Dictionary<string, string> ById = new Dictionary<string, string>();   // native key -> local id
            public string Sketch;   // the sketch these ids belong to

            /// <summary>Segment ids are unique only within a segment type (a sketch's first line and first
            /// arc are both (0, 1)), so the type is part of the key.</summary>
            public static string Key(object o)
            {
                var p = o as ISketchPoint;
                if (p != null) { var a = (int[])p.GetID(); return "p" + a[0] + "_" + a[1]; }
                var s = o as ISketchSegment;
                if (s != null) { var a = (int[])s.GetID(); return "s" + s.GetType() + "_" + a[0] + "_" + a[1]; }
                return null;
            }

            public string Of(object o)
            {
                string key = Key(o), id;
                return key != null && ById.TryGetValue(key, out id) ? id : null;
            }
        }

        static double[] ExportSketchPoint2(ISketchPoint p) { return new[] { U.R(U.Mm(p.X)), U.R(U.Mm(p.Y)) }; }

        static double[] P3(ISketchPoint p) { return new[] { U.R(U.Mm(p.X)), U.R(U.Mm(p.Y)), U.R(U.Mm(p.Z)) }; }   // 3D sketch: model mm

        JObj ExportSketch(IFeature f, JObj nf)
        {
            var sk = (ISketch)f.GetSpecificFeature2();
            // a 3D sketch's points are in model coordinates: its frame is the model's, and it has no plane
            bool is3d = sk.Is3D();
            var frame = is3d ? new SketchFrame(new double[3], new[] { 1.0, 0, 0 }, new[] { 0.0, 0, 1 }) : new SketchFrame(sw, sk);
            Func<ISketchPoint, double[]> P2 = is3d ? (Func<ISketchPoint, double[]>)P3 : ExportSketchPoint2;
            var def = new JObj();
            // fully defined or not: the importer checks a fully defined sketch arrives fully defined (FORMAT-SPEC 6.2)
            def["constrained"] = Host.ConstrainedStatus(sk);

            // plane
            string generator;
            if (is3d)
                def["space"] = "3d";
            else if (generatedSketches.TryGetValue(f.Name, out generator))
                def["plane"] = new JObj { { "kind", "generated" }, { "by", generator } };   // the owner replays it
            else
            {
                int refType = 0;
                object planeEnt = sk.GetReferenceEntity(ref refType);
                var planeRef = EntityRef(planeEnt);
                if (planeRef == null) Approx(nf, "sketch plane reference not recognised; frame used");
                def["plane"] = planeRef ?? new JObj { { "kind", "frame" }, { "by", "id" } };
            }
            if (!is3d) def["frame"] = new JObj { { "origin", U.Mm(frame.Origin) }, { "x", frame.X }, { "z", frame.Z } };

            // entities: points first (every point, including segment ends and centres), then segments
            var ids = new SketchIds { Sketch = f.Name };
            sketchMaps[f.Name] = ids;
            var ents = new List<object>();
            var pts = ((object[])sk.GetSketchPoints2() ?? new object[0]).Cast<ISketchPoint>().ToList();
            int np = 0, ns = 0;
            foreach (var p in pts)
            {
                string id = "p" + (++np);
                ids.ById[SketchIds.Key(p)] = id;
                ents.Add(new JObj { { "id", id }, { "type", "point" }, { "p", P2(p) } });
            }
            var segs = ((object[])sk.GetSketchSegments() ?? new object[0]).Cast<ISketchSegment>().ToList();
            foreach (var s in segs)
            {
                string id = "s" + (++ns);
                ids.ById[SketchIds.Key(s)] = id;
                var e = new JObj { { "id", id } };
                if (s.ConstructionGeometry) e["construction"] = true;
                switch (s.GetType())
                {
                    case 0:   // line
                    {
                        var l = (ISketchLine)s;
                        var p1 = (ISketchPoint)l.GetStartPoint2();
                        var p2 = (ISketchPoint)l.GetEndPoint2();
                        e["type"] = "line";
                        e["start"] = P2(p1);
                        e["end"] = P2(p2);
                        e["startPoint"] = ids.Of(p1);
                        e["endPoint"] = ids.Of(p2);
                        break;
                    }
                    case 1:   // arc or circle
                    {
                        var c = (ISketchArc)s;
                        var pc = (ISketchPoint)c.GetCenterPoint2();
                        e["center"] = P2(pc);
                        e["centerPoint"] = ids.Of(pc);
                        e["radius"] = U.R(U.Mm(c.GetRadius()));
                        if (c.IsCircle() == 1)
                            e["type"] = "circle";
                        else
                        {
                            var p1 = (ISketchPoint)c.GetStartPoint2();
                            var p2 = (ISketchPoint)c.GetEndPoint2();
                            e["type"] = "arc";
                            e["start"] = P2(p1);
                            e["end"] = P2(p2);
                            e["startPoint"] = ids.Of(p1);
                            e["endPoint"] = ids.Of(p2);
                            e["ccw"] = c.GetRotationDir() == 1;
                        }
                        if (is3d) e["normal"] = (double[])c.GetNormalVector();   // the arc's plane in a 3D sketch
                        break;
                    }
                    case 2:   // ellipse or elliptical arc
                    {
                        var el = (ISketchEllipse)s;
                        var pc = (ISketchPoint)el.GetCenterPoint2();
                        var pa = (ISketchPoint)el.GetMajorPoint2();
                        var pb = (ISketchPoint)el.GetMinorPoint2();
                        var p1 = (ISketchPoint)el.GetStartPoint2();
                        var p2 = (ISketchPoint)el.GetEndPoint2();
                        bool closed = p1 == null || p2 == null || V.Dist(P2(p1).Concat(new[] { 0.0 }).ToArray(), P2(p2).Concat(new[] { 0.0 }).ToArray()) < 1e-9;
                        e["type"] = closed ? "ellipse" : "ellipticalArc";
                        e["center"] = P2(pc);
                        e["major"] = P2(pa);
                        e["minor"] = P2(pb);
                        e["centerPoint"] = ids.Of(pc);
                        e["majorPoint"] = ids.Of(pa);
                        e["minorPoint"] = ids.Of(pb);
                        if (!closed)
                        {
                            e["start"] = P2(p1);
                            e["end"] = P2(p2);
                            e["startPoint"] = ids.Of(p1);
                            e["endPoint"] = ids.Of(p2);
                            e["ccw"] = el.GetRotationDir() == 1;
                        }
                        break;
                    }
                    case 3:   // spline: through its fit points, natural ends, plus any handle the user set
                    {
                        var sp = (ISketchSpline)s;
                        var spts = ((object[])sp.GetPoints2() ?? new object[0]).Cast<ISketchPoint>().ToList();
                        e["type"] = "spline";
                        e["points"] = spts.Select(q => (object)P2(q)).ToList();
                        e["pointIds"] = spts.Select(q => (object)ids.Of(q)).ToList();
                        var handles = new List<object>();
                        // handles are only listed while the sketch is being edited, so outside it none are carried
                        // (a user-driven tangent then shows as a shape difference at import)
                        foreach (ISplineHandle h in (object[])sp.GetSplineHandles() ?? new object[0])
                        {
                            if (!h.TangentDriving && !h.CurvatureControlled) continue;
                            handles.Add(new JObj
                            {
                                { "point", h.SplinePointNumber }, { "tangentDriving", h.TangentDriving },
                                { "radial", h.TangentRadialDirection }, { "polar", h.TangentPolarDirection },
                                { "magnitude", h.get_TangentMagnitude(1) }, { "magnitude2", h.get_TangentMagnitude(2) },
                                { "curvatureControlled", h.CurvatureControlled },
                                { "curvature", h.Curvature }
                            });
                        }
                        if (handles.Count > 0) e["handles"] = handles;
                        break;
                    }
                    default:
                        e["type"] = "unsupported";
                        e["native"] = s.GetType();
                        Approx(nf, "sketch segment type " + s.GetType() + " not carried");
                        break;
                }
                ents.Add(e);
            }
            // drop the points owned by segments from the standalone list? No: keep every point, since
            // relations and dimensions refer to them; replay knows which ones segments create.
            def["entities"] = ents;

            // relations
            var cons = new List<object>();
            var rm = (ISketchRelationManager)sk.RelationManager;
            foreach (ISketchRelation r in ((object[])rm.GetRelations(0) ?? new object[0]))   // swAll
            {
                int rt = r.GetRelationType();
                if (DimensionRelations.Contains(rt)) continue;
                string name;
                var refs = ((object[])r.GetEntities() ?? new object[0]).Select(o => LocalOrExternal(ids, o, frame)).ToList();
                // a relation between SolidWorks' hidden inference geometry and the model (none of the sketch's own
                // entities) constrains nothing in the sketch: not carried
                if (refs.Count > 0 && refs.All(x => x != null && !(x is string))) continue;
                if (!Relations.TryGetValue(rt, out name) || refs.Any(x => x == null))
                {
                    cons.Add(new JObj { { "type", "native:" + rt }, { "entities", new List<object>() }, { "supported", false } });
                    var missing = ((object[])r.GetEntities() ?? new object[0]).Where((o, i) => refs[i] == null).Select(KindOf);
                    Approx(nf, "relation type " + rt + " not carried" +
                               (missing.Any() ? " (refers to " + string.Join(", ", missing) + ")" : ""));
                    continue;
                }
                cons.Add(new JObj { { "type", name }, { "entities", refs } });
            }
            def["constraints"] = cons;

            // dimensions
            var dims = new List<object>();
            int nd = 0;
            for (var dd = (IDisplayDimension)f.GetFirstDisplayDimension(); dd != null; dd = (IDisplayDimension)f.GetNextDisplayDimension(dd))
            {
                var dim = (IDimension)dd.GetDimension2(0);
                // a sketch also lists dimensions of the plane it sits on (D1@Plane-Top): keep only its own
                if (!dim.FullName.Contains("@" + f.Name + "@")) continue;
                var ann = (IAnnotation)dd.GetAnnotation();
                string kind;
                if (!DimKinds.TryGetValue(dd.Type2, out kind))
                {
                    Approx(nf, "dimension " + dim.Name + " of type " + dd.Type2 + " not carried");
                    continue;
                }
                var attached = ((object[])ann.GetAttachedEntities3() ?? new object[0]).Select(o => LocalOrExternal(ids, o, frame)).ToList();
                if (attached.Any(x => x == null))
                {
                    Approx(nf, "dimension " + dim.Name + " refers to geometry outside the sketch");
                    continue;
                }
                double sys = dim.SystemValue;
                var pos = ann.GetPosition() as double[];
                var jd = new JObj
                {
                    { "id", "d" + (++nd) }, { "name", dim.Name }, { "fullName", dim.FullName }, { "kind", kind },
                    { "entities", attached }, { "value", kind == "angle" ? U.Deg(sys) : U.Mm(sys) },
                    { "driving", dim.DrivenState != 1 || equationTargets.Contains(dim.Name + "@" + f.Name) }   // SolidWorks reports an equation's target as driven
                };
                if (pos != null && is3d)
                    jd["textPosition"] = new[] { U.R(U.Mm(pos[0])), U.R(U.Mm(pos[1])), U.R(U.Mm(pos[2])) };
                else if (pos != null)
                {
                    var sp = frame.ToSketch(new[] { pos[0], pos[1], pos[2] });
                    jd["textPosition"] = new[] { U.R(U.Mm(sp[0])), U.R(U.Mm(sp[1])) };
                }
                dims.Add(jd);
            }
            def["dimensions"] = dims;
            return def;
        }

        /// <summary>A sketch entity as a local id, the origin as an external reference, else null.</summary>
        object LocalOrExternal(SketchIds ids, object o, SketchFrame frame)
        {
            string local = ids.Of(o);
            if (local != null && !IsForeign(o, ids.Sketch)) return local;
            var sp = o as ISketchPoint;
            if (sp != null)
            {
                // a sketch's own origin point (the model origin projected onto the sketch plane) is not
                // among its sketch points
                if (local == null && Math.Abs(sp.X) < 1e-12 && Math.Abs(sp.Y) < 1e-12 && Math.Abs(sp.Z) < 1e-12)
                    return new JObj { { "kind", "origin" }, { "by", "id" } };
                var m = ModelPoint(sp);
                if (m != null && V.Norm(m) < 1e-9) return new JObj { { "kind", "origin" }, { "by", "id" } };
            }
            // anything else outside the sketch: another sketch's entity by id, model edges/faces by geometry
            var r = EntityRef(o);
            if (r == null && sp != null && local == null)
            {
                // an inference point SolidWorks keeps inside the sketch but not among its points (made when an
                // entity snapped to a model corner): carry it as the body vertex at the same place, if one is
                var m = ModelPoint(sp);
                var v = m == null || part == null ? null : ((object[])part.GetBodies2(-1, true)   // all bodies: surfaces too (none in an assembly)
                    ?? new object[0]).Cast<IBody2>()
                    .SelectMany(b => ((object[])b.GetVertices() ?? new object[0]).Cast<IVertex>())
                    .FirstOrDefault(x => V.Norm(V.Sub((double[])x.GetPoint(), m)) < 1e-8);
                if (v != null) r = Host.VertexDescriptor(v);
            }
            var line = o as ISketchLine;
            if (r == null && line != null && local == null)
            {
                // likewise an inference line (the model edge a point snapped to): carry the body edge along it
                var a = ModelPoint((ISketchPoint)line.GetStartPoint2());
                var b = ModelPoint((ISketchPoint)line.GetEndPoint2());
                var edge = a == null || b == null ? null : EdgeAlong(a, b);
                if (edge != null) r = Host.Descriptor(edge);
                // or where the sketch plane meets a datum plane: the relation keeps the point on that plane
                else if (a != null && b != null) r = PlaneThrough(a, b, frame);
            }
            return r;
        }

        /// <summary>A straight body edge on the line through a and b (both ends on it), or null.</summary>
        IEdge EdgeAlong(double[] a, double[] b)
        {
            var dir = V.Sub(b, a);
            double len = V.Norm(dir);
            if (len < 1e-9 || part == null) return null;   // (an assembly sketch has no bodies of its own)
            dir = V.Unit(dir);
            System.Func<double[], double> off = x => V.Norm(V.Cross(V.Sub(x, a), dir));   // distance from the line
            foreach (IBody2 body in (object[])part.GetBodies2(-1, true) ?? new object[0])
                foreach (IEdge e in (object[])body.GetEdges() ?? new object[0])
                {
                    var c = (ICurve)e.GetCurve();
                    if (c == null || !c.IsLine()) continue;
                    var sv = (IVertex)e.GetStartVertex();
                    var ev = (IVertex)e.GetEndVertex();
                    if (sv == null || ev == null) continue;
                    if (off((double[])sv.GetPoint()) < 1e-8 && off((double[])ev.GetPoint()) < 1e-8) return e;
                }
            return null;
        }

        /// <summary>A datum plane (origin or reference plane, not parallel to the sketch) containing the line a-b.</summary>
        JObj PlaneThrough(double[] a, double[] b, SketchFrame frame)
        {
            for (var f = (IFeature)doc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
            {
                if (f.GetTypeName2() != "RefPlane") continue;
                var m = (double[])((IMathTransform)((IRefPlane)f.GetSpecificFeature2()).Transform).ArrayData;
                var n = V.Unit(new[] { m[6], m[7], m[8] });
                var o = new[] { m[9], m[10], m[11] };
                if (1 - Math.Abs(V.Dot(n, V.Unit(frame.Z))) < 1e-9) continue;   // the sketch's own plane, or parallel to it
                if (Math.Abs(V.Dot(V.Sub(a, o), n)) < 1e-8 && Math.Abs(V.Dot(V.Sub(b, o), n)) < 1e-8)
                {
                    var r = FeatureRef(f);
                    if (r != null) return r;
                }
            }
            return null;
        }

        /// <summary>What kind of object a relation refers to, for the report.</summary>
        static string KindOf(object o)
        {
            if (o is ISketchPoint) return "sketch point";
            if (o is ISketchSegment)
            {
                var owner = ((ISketchSegment)o).GetSketch() as IFeature;
                return "sketch segment of " + (owner == null ? "no sketch" : owner.Name) + ", type " + ((ISketchSegment)o).GetType();
            }
            if (o is IEdge) return "model edge";
            if (o is ISilhouetteEdge) return "silhouette edge";
            if (o is IFace2) return "face";
            if (o is IVertex) return "vertex";
            if (o is IRefAxis) return "reference axis";
            if (o is IRefPlane) return "reference plane";
            if (o is IFeature) return "feature";
            return o == null ? "nothing" : "unknown object";
        }

        /// <summary>True if an entity carries a local-looking id but belongs to another sketch (ids are only
        /// unique within a sketch, so a foreign point can collide with a local one).</summary>
        static bool IsForeign(object o, string sketch)
        {
            object owner = o is ISketchPoint ? ((ISketchPoint)o).GetSketch() : o is ISketchSegment ? ((ISketchSegment)o).GetSketch() : null;
            var f = owner as IFeature;
            return f != null && f.Name != sketch;
        }

        double[] ModelPoint(ISketchPoint sp)
        {
            var owner = (ISketch)sp.GetSketch();
            if (owner == null) return null;
            var fr = new SketchFrame(sw, owner);
            return fr.ToModel(sp.X, sp.Y);
        }
    }
}
