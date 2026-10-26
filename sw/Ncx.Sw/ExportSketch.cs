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
        // a sketch block's own relations that keep its exploded geometry joined up whatever way the block is turned
        // (horizontal/vertical are in its own axes; parallel, perpendicular and equal around its closed shapes are
        // often redundant once exploded, and over-define the sketch)
        static readonly HashSet<string> BlockRelations = new HashSet<string> { "coincident", "tangent", "concentric", "coradial" };
        static readonly HashSet<int> DimensionRelations = new HashSet<int> { 1, 2, 3, 15, 41, 43, 44, 84 };
        static readonly Dictionary<int, string> DimKinds = new Dictionary<int, string>
        {
            { 2, "distance" }, { 11, "horizontal" }, { 12, "vertical" }, { 3, "angle" }, { 5, "radius" }, { 6, "diameter" }
        };

        class SketchIds
        {
            public readonly Dictionary<string, string> ById = new Dictionary<string, string>();   // native key -> local id
            public string Sketch;   // the sketch these ids belong to
            public List<object> Entities;   // the sketch's entities as written (sketch mm), sketch blocks' included

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

        /// <summary>A slot (Straight Slot) holds its shape by relations SolidWorks does not list: its end caps equal
        /// and its sides tangent to them. They are carried as those plain relations. The slot's segments are found
        /// by geometry: the caps are the arcs of half the slot's width about its two end centres, the sides the
        /// lines from one cap to the other.</summary>
        void SlotRelations(ISketch sk, SketchIds ids, List<object> cons, JObj nf)
        {
            dynamic dsk = sk;   // late bound: slots are not in every interop the add-in is built against
            object[] slots;
            try { slots = (object[])dsk.GetSketchSlots(); }
            catch (Exception) { return; }
            if (slots == null || slots.Length == 0) return;
            var segs = ((object[])sk.GetSketchSegments() ?? new object[0]).Cast<ISketchSegment>().ToList();
            const double tol = 1e-7;   // m
            Func<double, double, double, double, bool> same = (x1, y1, x2, y2) => Math.Abs(x1 - x2) < tol && Math.Abs(y1 - y2) < tol;
            foreach (dynamic slot in slots)
            {
                int kind = slot.CreationType;
                List<ISketchPoint> pts = ((object[])slot.GetSlotPoints() ?? new object[0]).Cast<ISketchPoint>().ToList();
                if (kind > 1 || pts.Count < 2)
                {
                    // arc slots: their caps and sides follow a centre arc, not carried yet
                    Approx(nf, "arc slot: its own (hidden) relations not carried (its shape is kept as drawn)");
                    continue;
                }
                double r = (double)slot.Width / 2;
                var caps = new List<ISketchArc>();
                foreach (var c in pts.Take(2))
                {
                    var cap = segs.OfType<ISketchArc>().FirstOrDefault(a =>
                    {
                        var cp = (ISketchPoint)a.GetCenterPoint2();
                        return !((ISketchSegment)a).ConstructionGeometry && Math.Abs(a.GetRadius() - r) < tol && same(cp.X, cp.Y, c.X, c.Y);
                    });
                    if (cap != null) caps.Add(cap);
                }
                if (caps.Count != 2) { Approx(nf, "slot: its end caps not found, its own (hidden) relations not carried"); continue; }
                Func<ISketchArc, ISketchPoint[]> ends = a => new[] { (ISketchPoint)a.GetStartPoint2(), (ISketchPoint)a.GetEndPoint2() };
                Func<ISketchPoint, ISketchArc, bool> onEnd = (p, a) => ends(a).Any(q => same(p.X, p.Y, q.X, q.Y));
                var sides = segs.OfType<ISketchLine>().Where(l => !((ISketchSegment)l).ConstructionGeometry).Where(l =>
                {
                    var a = (ISketchPoint)l.GetStartPoint2();
                    var b = (ISketchPoint)l.GetEndPoint2();
                    return (onEnd(a, caps[0]) && onEnd(b, caps[1])) || (onEnd(a, caps[1]) && onEnd(b, caps[0]));
                }).ToList();
                string c0 = ids.Of(caps[0]), c1 = ids.Of(caps[1]);
                var sideIds = sides.Select(l => ids.Of(l)).ToList();
                if (c0 == null || c1 == null || sides.Count != 2 || sideIds.Any(x => x == null))
                {
                    Approx(nf, "slot: its sides not found, its own (hidden) relations not carried");
                    continue;
                }
                cons.Add(new JObj { { "type", "equal" }, { "entities", new List<object> { c0, c1 } } });
                foreach (var s in sideIds)
                    foreach (var c in new[] { c0, c1 })
                        cons.Add(new JObj { { "type", "tangent" }, { "entities", new List<object> { c, s } } });
                // a slot drawn from its centre has a centre point, held at the middle of its centreline (the
                // construction line between the caps' centres). The slot does not return it (its third point is a
                // placeholder at 0,0): it is the sketch's point midway between the caps' centres that no segment ends on
                double mx = (pts[0].X + pts[1].X) / 2, my = (pts[0].Y + pts[1].Y) / 2;
                var segEnds = new HashSet<string>();
                foreach (var sg in segs)
                {
                    var l = sg as ISketchLine;
                    var a = sg as ISketchArc;
                    if (l != null) { segEnds.Add(SketchIds.Key(l.GetStartPoint2())); segEnds.Add(SketchIds.Key(l.GetEndPoint2())); }
                    if (a != null) { segEnds.Add(SketchIds.Key(a.GetStartPoint2())); segEnds.Add(SketchIds.Key(a.GetEndPoint2())); segEnds.Add(SketchIds.Key(a.GetCenterPoint2())); }
                }
                var centre = ((object[])sk.GetSketchPoints2() ?? new object[0]).Cast<ISketchPoint>()
                    .FirstOrDefault(p => same(p.X, p.Y, mx, my) && !segEnds.Contains(SketchIds.Key(p)));
                string mid = centre == null ? null : ids.Of(centre);
                if (mid != null)
                {
                    var line = segs.OfType<ISketchLine>().FirstOrDefault(l =>
                    {
                        var a = (ISketchPoint)l.GetStartPoint2();
                        var b = (ISketchPoint)l.GetEndPoint2();
                        return ((ISketchSegment)l).ConstructionGeometry &&
                               ((same(a.X, a.Y, pts[0].X, pts[0].Y) && same(b.X, b.Y, pts[1].X, pts[1].Y)) ||
                                (same(a.X, a.Y, pts[1].X, pts[1].Y) && same(b.X, b.Y, pts[0].X, pts[0].Y)));
                    });
                    string lid = line == null ? null : ids.Of(line);
                    if (lid != null) cons.Add(new JObj { { "type", "midpoint" }, { "entities", new List<object> { lid, mid } } });
                    else Approx(nf, "slot: its centre point not tied to its centreline (no centreline found)");
                }
            }
        }

        /// <summary>A polygon (the Polygon tool) keeps its sides as turned copies of each other by relations listed only
        /// as "patterned" side to side: carried as equal sides (the patterned relation), which alone lets the polygon
        /// go out of shape. Its corners on the circle through them (the polygon's construction circle) make it
        /// regular again: added where that circle is in the sketch, for corners not already on it.</summary>
        void PolygonRelations(ISketch sk, SketchIds ids, List<object> cons, JObj nf)
        {
            const double tol = 1e-7;   // m
            // the lines tied by patterned relations
            var sides = new Dictionary<string, ISketchLine>();
            foreach (ISketchRelation r in ((object[])((ISketchRelationManager)sk.RelationManager).GetRelations(0) ?? new object[0]))
            {
                if (r.GetRelationType() != 57) continue;
                foreach (var o in (object[])r.GetEntities() ?? new object[0])
                {
                    var l = o as ISketchLine;
                    string id = l == null ? null : ids.Of(l);
                    if (id != null) sides[id] = l;
                }
            }
            if (sides.Count < 3) return;
            // corners: the sides' ends; a closed loop has every corner shared by exactly two sides
            var corners = new Dictionary<string, ISketchPoint>();
            var uses = new Dictionary<string, int>();
            foreach (var l in sides.Values)
                foreach (ISketchPoint p in new[] { l.GetStartPoint2(), l.GetEndPoint2() })
                {
                    string pid = ids.Of(p);
                    if (pid == null) return;
                    corners[pid] = p;
                    uses[pid] = (uses.ContainsKey(pid) ? uses[pid] : 0) + 1;
                }
            if (uses.Values.Any(n => n != 2) || corners.Count != sides.Count) return;   // not one closed polygon
            double cx = corners.Values.Average(p => p.X), cy = corners.Values.Average(p => p.Y);
            double rad = corners.Values.Average(p => Math.Sqrt((p.X - cx) * (p.X - cx) + (p.Y - cy) * (p.Y - cy)));
            if (corners.Values.Any(p => Math.Abs(Math.Sqrt((p.X - cx) * (p.X - cx) + (p.Y - cy) * (p.Y - cy)) - rad) > tol)) return;
            var circle = ((object[])sk.GetSketchSegments() ?? new object[0]).OfType<ISketchArc>().FirstOrDefault(a =>
            {
                var c = (ISketchPoint)a.GetCenterPoint2();
                return Math.Abs(c.X - cx) < tol && Math.Abs(c.Y - cy) < tol && Math.Abs(a.GetRadius() - rad) < tol;
            });
            string cid = circle == null ? null : ids.Of(circle);
            if (cid == null)
            {
                Approx(nf, "polygon: no circle through its corners, its shape (regular) is kept as drawn but not held");
                return;
            }
            // corners already on the circle by a relation of their own
            var onCircle = new HashSet<string>(cons.Cast<JObj>().Where(c => c.Str("type") == "coincident")
                .Select(c => c.Arr("entities")).Where(e => e.Count == 2 && e.Contains(cid)).SelectMany(e => e.OfType<string>()));
            foreach (var pid in corners.Keys.Where(k => !onCircle.Contains(k)))
                cons.Add(new JObj { { "type", "coincident" }, { "entities", new List<object> { pid, cid } } });
        }

        /// <summary>Slots mirror images of each other about a line (a symmetric relation between two slots: SolidWorks
        /// returns the slots as nothing, only the line). Each pair: the cap centres of one paired with the other's at
        /// their mirror images (sketch ids), then the two slots' first caps (equal: the slots are equally wide); and
        /// whether all four centres lie along the line's own length (projected), which settles the choice when more
        /// slots pair than the line has relations.</summary>
        static List<Tuple<List<string[]>, bool>> MirrorSlotPairs(ISketch sk, ISketchLine line, SketchIds ids)
        {
            var found = new List<Tuple<List<string[]>, bool>>();
            try
            {
                var a = (ISketchPoint)line.GetStartPoint2();
                var b = (ISketchPoint)line.GetEndPoint2();
                double dx = b.X - a.X, dy = b.Y - a.Y, len2 = dx * dx + dy * dy;
                if (len2 < 1e-18) return found;
                dynamic dsk = sk;
                var slots = ((object[])dsk.GetSketchSlots() ?? new object[0])
                    .Select(o => ((object[])((dynamic)o).GetSlotPoints() ?? new object[0]).Cast<ISketchPoint>().Take(2).ToList())
                    .Where(c => c.Count == 2).ToList();
                Func<ISketchPoint, double> along = p => ((p.X - a.X) * dx + (p.Y - a.Y) * dy) / len2;
                // the other slot's centre at the mirror image of p, or null
                Func<ISketchPoint, List<ISketchPoint>, ISketchPoint> image = (p, other) =>
                {
                    double t = along(p);
                    double mx = 2 * (a.X + t * dx) - p.X, my = 2 * (a.Y + t * dy) - p.Y;
                    return other.FirstOrDefault(c => Math.Abs(c.X - mx) < 1e-7 && Math.Abs(c.Y - my) < 1e-7);
                };
                var arcs = ((object[])sk.GetSketchSegments() ?? new object[0]).OfType<ISketchArc>()
                    .Where(c => !((ISketchSegment)c).ConstructionGeometry).ToList();
                Func<string, string> capAt = pid => arcs.Select(c => new { c, p = ids.Of(c.GetCenterPoint2()) })
                    .Where(x => x.p == pid).Select(x => ids.Of(x.c)).FirstOrDefault();
                for (int i = 0; i < slots.Count; i++)
                    for (int j = i + 1; j < slots.Count; j++)
                    {
                        var qs = slots[i].Select(p => image(p, slots[j])).ToList();
                        if (qs.Any(q => q == null)) continue;
                        var pr = slots[i].Select((p, k) => new[] { ids.Of(p), ids.Of(qs[k]) }).ToList();
                        pr.Add(new[] { capAt(pr[0][0]), capAt(pr[0][1]) });
                        if (pr.Any(x => x[0] == null || x[1] == null)) continue;
                        bool inSpan = slots[i].Concat(slots[j]).All(p => along(p) > -1e-6 && along(p) < 1 + 1e-6);
                        found.Add(Tuple.Create(pr, inSpan));
                    }
            }
            catch (Exception) { }
            return found;
        }

        static double[] ExportSketchPoint2(ISketchPoint p) { return new[] { U.R(U.Mm(p.X)), U.R(U.Mm(p.Y)) }; }

        static double[] P3(ISketchPoint p) { return new[] { U.R(U.Mm(p.X)), U.R(U.Mm(p.Y)), U.R(U.Mm(p.Z)) }; }   // 3D sketch: model mm

        JObj ExportSketch(IFeature f, JObj nf)
        {
            var sk = (ISketch)f.GetSpecificFeature2();
            exportingSketch = sk;
            // a 3D sketch's points are in model coordinates: its frame is the model's, and it has no plane
            bool is3d = sk.Is3D();
            var frame = is3d ? new SketchFrame(new double[3], new[] { 1.0, 0, 0 }, new[] { 0.0, 0, 1 }) : new SketchFrame(sw, sk);
            Func<ISketchPoint, double[]> P2 = is3d ? (Func<ISketchPoint, double[]>)P3 : ExportSketchPoint2;
            var def = new JObj();
            // fully defined or not: the importer checks a fully defined sketch arrives fully defined (FORMAT-SPEC 6.2)
            def["constrained"] = Host.ConstrainedStatus(sk);
            // shown or hidden in the source (layout and construction sketches usually hidden)
            if (f.Visible == 1) def["hidden"] = true;   // swVisibilityStateHide (2: shown)

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
            int np = 0, ns = 0;
            // one sketch's points and segments: the sketch's own (P = P2), or a sketch block's, mapped into this
            // sketch by the block instance's transform (block: its own ids, radii scaled, splines by fit points)
            void AddEntities(ISketch src, Func<ISketchPoint, double[]> P, SketchIds map, double scale, bool block)
            {
            var pts = ((object[])src.GetSketchPoints2() ?? new object[0]).Cast<ISketchPoint>().ToList();
            foreach (var p in pts)
            {
                string id = (block ? "b" : "") + "p" + (++np);
                map.ById[SketchIds.Key(p)] = id;
                ents.Add(new JObj { { "id", id }, { "type", "point" }, { "p", P(p) } });
            }
            var segs = ((object[])src.GetSketchSegments() ?? new object[0]).Cast<ISketchSegment>().ToList();
            foreach (var s in segs)
            {
                string id = (block ? "b" : "") + "s" + (++ns);
                map.ById[SketchIds.Key(s)] = id;
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
                        e["start"] = P(p1);
                        e["end"] = P(p2);
                        e["startPoint"] = map.Of(p1);
                        e["endPoint"] = map.Of(p2);
                        break;
                    }
                    case 1:   // arc or circle
                    {
                        var c = (ISketchArc)s;
                        var pc = (ISketchPoint)c.GetCenterPoint2();
                        e["center"] = P(pc);
                        e["centerPoint"] = map.Of(pc);
                        e["radius"] = U.R(U.Mm(c.GetRadius() * scale));
                        if (c.IsCircle() == 1)
                            e["type"] = "circle";
                        else
                        {
                            var p1 = (ISketchPoint)c.GetStartPoint2();
                            var p2 = (ISketchPoint)c.GetEndPoint2();
                            e["type"] = "arc";
                            e["start"] = P(p1);
                            e["end"] = P(p2);
                            e["startPoint"] = map.Of(p1);
                            e["endPoint"] = map.Of(p2);
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
                        bool closed = p1 == null || p2 == null || V.Dist(P(p1).Concat(new[] { 0.0 }).ToArray(), P(p2).Concat(new[] { 0.0 }).ToArray()) < 1e-9;
                        e["type"] = closed ? "ellipse" : "ellipticalArc";
                        e["center"] = P(pc);
                        e["major"] = P(pa);
                        e["minor"] = P(pb);
                        e["centerPoint"] = map.Of(pc);
                        e["majorPoint"] = map.Of(pa);
                        e["minorPoint"] = map.Of(pb);
                        if (!closed)
                        {
                            e["start"] = P(p1);
                            e["end"] = P(p2);
                            e["startPoint"] = map.Of(p1);
                            e["endPoint"] = map.Of(p2);
                            e["ccw"] = el.GetRotationDir() == 1;
                        }
                        break;
                    }
                    case 3:   // spline: through its fit points, natural ends, plus any handle the user set
                    {
                        var sp = (ISketchSpline)s;
                        var spts = ((object[])sp.GetPoints2() ?? new object[0]).Cast<ISketchPoint>().ToList();
                        e["type"] = "spline";
                        e["points"] = spts.Select(q => (object)P(q)).ToList();
                        e["pointIds"] = spts.Select(q => (object)map.Of(q)).ToList();
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
                        // the exact curve, so a system that fits splines differently reproduces it (the fit points
                        // stay the editable intent)
                        var nurbs = block ? null : SplineNurbs(s, spts, frame, is3d);
                        if (nurbs != null) e["nurbs"] = nurbs;
                        else if (!block) Approx(nf, "spline " + e["id"] + ": exact curve not readable (fit points carried)");
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
            }
            AddEntities(sk, P2, ids, 1.0, false);
            // sketch blocks (a logo): carried as their geometry, placed as the instance shows it
            sketchBlocks.Clear();
            foreach (var bo in (object[])sk.GetSketchBlockInstances() ?? new object[0])
            {
                var bi = bo as ISketchBlockInstance;
                var bsk = (bi?.Definition as ISketchBlockDefinition)?.GetSketch() as ISketch;
                var t = (bi?.BlockToSketchTransform as IMathTransform)?.ArrayData as double[];
                if (bsk == null || t == null) { Approx(nf, "sketch block " + bi?.Name + " not readable"); continue; }
                // p' = p R s + t (row vectors), in this sketch's plane
                Func<ISketchPoint, double[]> PB = q => new[] { U.R(U.Mm((q.X * t[0] + q.Y * t[3]) * t[12] + t[9])), U.R(U.Mm((q.X * t[1] + q.Y * t[4]) * t[12] + t[10])) };
                var bids = new SketchIds { Sketch = bi.Name };
                AddEntities(bsk, PB, bids, t[12], true);
                sketchBlocks.Add(Tuple.Create(bsk, bids));
                Approx(nf, "sketch block " + bi.Name + " carried as its geometry (the block is exploded)");
            }
            // drop the points owned by segments from the standalone list? No: keep every point, since
            // relations and dimensions refer to them; replay knows which ones segments create.
            def["entities"] = ents;
            ids.Entities = ents;

            // relations
            var cons = new List<object>();
            var rm = (ISketchRelationManager)sk.RelationManager;
            var slotSymmetry = new Dictionary<string, Tuple<ISketchLine, int>>();   // line id -> (line, relations)
            foreach (ISketchRelation r in ((object[])rm.GetRelations(0) ?? new object[0]))   // swAll
            {
                int rt = r.GetRelationType();
                if (DimensionRelations.Contains(rt)) continue;
                string name;
                var refs = ((object[])r.GetEntities() ?? new object[0]).Select(o => LocalOrExternal(ids, o, frame)).ToList();
                // a relation between SolidWorks' hidden inference geometry and the model (none of the sketch's own
                // entities) constrains nothing in the sketch: not carried
                if (refs.Count > 0 && refs.All(x => x != null && !(x is string))) continue;
                if (refs.Count == 0) continue;   // a relation SolidWorks keeps with no entities constrains nothing
                if ((rt == 16 || rt == 47) && refs.Count == 2 && refs.All(x => x != null))
                {
                    // Offset Entities (16) or a sketch offset within the sketch (47): the offset segment and its
                    // source; the distance is one of the sketch's dimensions
                    cons.Add(new JObj { { "type", "offset" }, { "entities", refs } });
                    continue;
                }
                if (rt == 57 && refs.Count >= 2 && refs.All(x => x != null))
                {
                    // a sketch pattern's copies (swConstraintType_PATTERNED): they follow the seed
                    cons.Add(new JObj { { "type", "patterned" }, { "entities", refs } });
                    continue;
                }
                if (rt == 32 && refs.Count == 2 && refs.Count(x => x is string) == 1 && refs.All(x => x != null))
                {
                    // Convert Entities: the sketch's own entity and the one it was converted from (another sketch's,
                    // the layout's, or a model edge), FORMAT-SPEC 4 "converted"
                    var own = refs.First(x => x is string);
                    cons.Add(new JObj { { "type", "converted" }, { "entities", new List<object> { own, refs.First(x => !(x is string)) } } });
                    continue;
                }
                if (rt == 11 && refs.Count == 3 && refs[2] is string && refs[0] == null && refs[1] == null
                    && ((object[])r.GetEntities())[2] is ISketchLine)
                {
                    // two slots symmetric about a line: which two is settled after all the sketch's relations are read
                    string lid = (string)refs[2];
                    if (!slotSymmetry.ContainsKey(lid)) slotSymmetry[lid] = Tuple.Create((ISketchLine)((object[])r.GetEntities())[2], 0);
                    slotSymmetry[lid] = Tuple.Create(slotSymmetry[lid].Item1, slotSymmetry[lid].Item2 + 1);
                    continue;
                }
                if (!Relations.TryGetValue(rt, out name) || refs.Any(x => x == null))
                {
                    cons.Add(new JObj { { "type", "native:" + rt }, { "entities", new List<object>() }, { "supported", false } });
                    var missing = ((object[])r.GetEntities() ?? new object[0]).Where((o, i) => refs[i] == null).Select(KindOf);
                    string known;
                    Approx(nf, (Relations.TryGetValue(rt, out known) ? known + " relation" : "relation type " + rt) + " not carried" +
                               (missing.Any() ? " (refers to " + string.Join(", ", missing) + " outside the sketch)" : ""));
                    continue;
                }
                cons.Add(new JObj { { "type", name }, { "entities", refs } });
            }
            // slots symmetric about a line: carried as their caps' centres symmetric about it and their caps equal
            // (with the slots' own relations the rest of each slot follows). The pairs are the slots that are mirror
            // images about the line; when more pair than the line has relations, those along the line's own length
            foreach (var kv in slotSymmetry)
            {
                var cand = MirrorSlotPairs(sk, kv.Value.Item1, ids);
                if (cand.Count > kv.Value.Item2) cand = cand.Where(c => c.Item2).ToList();
                if (cand.Count != kv.Value.Item2)
                {
                    Approx(nf, kv.Value.Item2 + " symmetric relation(s) between slots not carried (which slots could not be told)");
                    continue;
                }
                foreach (var pr in cand.Select(c => c.Item1))
                {
                    for (int k = 0; k < pr.Count - 1; k++)
                        cons.Add(new JObj { { "type", "symmetric" }, { "entities", new List<object> { pr[k][0], pr[k][1], kv.Key } } });
                    cons.Add(new JObj { { "type", "equal" }, { "entities", new List<object> { pr[pr.Count - 1][0], pr[pr.Count - 1][1] } } });
                }
            }
            // a block's own relations keep its exploded geometry in shape. Those that do not depend on the block's
            // axes are carried; horizontal and vertical are in the block's axes, which the instance may turn
            foreach (var blk in sketchBlocks)
            {
                int axisBound = 0;
                foreach (ISketchRelation r in ((object[])((ISketchRelationManager)blk.Item1.RelationManager).GetRelations(0) ?? new object[0]))
                {
                    int rt = r.GetRelationType();
                    string name;
                    if (DimensionRelations.Contains(rt) || !Relations.TryGetValue(rt, out name) || !BlockRelations.Contains(name))
                    {
                        if (!DimensionRelations.Contains(rt)) axisBound++;
                        continue;
                    }
                    var refs = ((object[])r.GetEntities() ?? new object[0]).Select(o => (object)blk.Item2.Of(o)).ToList();
                    if (refs.Count == 0 || refs.Any(x => x == null)) continue;
                    cons.Add(new JObj { { "type", name }, { "entities", refs } });
                }
                if (axisBound > 0)
                    Approx(nf, "sketch block " + blk.Item2.Sketch + ": " + axisBound + " of its own relations not carried (its shape is kept as drawn)");
            }
            if (!is3d) SlotRelations(sk, ids, cons, nf);
            if (!is3d) PolygonRelations(sk, ids, cons, nf);
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
        /// <summary>A spline's exact B-spline in sketch coordinates (model coordinates for a 3D sketch), mm:
        /// {degree, knots, controlPoints, weights?, periodic}. Null when the curve cannot be read or placed.</summary>
        static JObj SplineNurbs(ISketchSegment seg, List<ISketchPoint> fit, SketchFrame frame, bool is3d)
        {
            try
            {
                var curve = (ICurve)seg.GetCurve();
                var pd = (ISplineParamData)curve.GetBCurveParams5(false, false, true, false);
                if (pd == null || fit.Count == 0) return null;
                object cpo, kno;
                pd.GetControlPoints(out cpo);
                pd.GetKnotPoints(out kno);
                var cp = (double[])cpo;
                var kn = (double[])kno;
                int dim = pd.Dimension, n = pd.ControlPointsCount;   // dim 3: x y z; 4: x y z w (rational)
                if (cp == null || kn == null || cp.Length < n * dim) return null;
                // the curve is in sketch space or in model space depending on the version: a clamped spline's first
                // control point is its first fit point, which tells them apart
                var f0 = new[] { fit[0].X, fit[0].Y, fit[0].Z };
                var c0 = new[] { cp[0], cp[1], cp[2] };
                bool sketchSpace = is3d || V.Norm(V.Sub(c0, f0)) < 1e-9;
                if (!sketchSpace && V.Norm(V.Sub(frame.ToSketch(c0), f0)) > 1e-9) return null;
                var pts = new List<object>();
                var weights = new List<object>();
                for (int i = 0; i < n; i++)
                {
                    var q = new[] { cp[i * dim], cp[i * dim + 1], cp[i * dim + 2] };
                    if (!sketchSpace) q = frame.ToSketch(q);
                    pts.Add(is3d ? new[] { U.Mm(q[0]), U.Mm(q[1]), U.Mm(q[2]) } : new[] { U.Mm(q[0]), U.Mm(q[1]) });
                    if (dim == 4) weights.Add(cp[i * dim + 3]);
                }
                var o = new JObj
                {
                    { "degree", pd.Order - 1 }, { "knots", kn.Cast<object>().ToList() }, { "controlPoints", pts },
                    { "periodic", pd.Periodic != 0 }
                };
                if (weights.Count > 0) o["weights"] = weights;
                return o;
            }
            catch (Exception) { return null; }   // not a B-spline curve, or not readable in this version
        }

        // the sketch blocks of the sketch being exported: each instance's definition sketch and the ids its exploded
        // entities have
        readonly List<Tuple<ISketch, SketchIds>> sketchBlocks = new List<Tuple<ISketch, SketchIds>>();

        ISketch exportingSketch;   // the sketch whose relations are being exported

        object LocalOrExternal(SketchIds ids, object o, SketchFrame frame)
        {
            string local = ids.Of(o);
            // an entity of a block placed in this sketch (a relation to the block's geometry) is reported as the
            // definition's: the exploded entity with its id and the same definition coordinates. Ids are numbered per
            // sketch, so a block's entity can carry the id of one of the sketch's own: told apart by coordinates. A
            // block placed more than once cannot say which instance is meant (not carried)
            string inBlock = null;
            var mine = Coords(o);
            if (mine != null && sketchBlocks.Count > 0)
            {
                Func<ISketch, bool> twinIn = sk2 => sk2 != null &&
                    ((object[])sk2.GetSketchSegments() ?? new object[0]).Concat((object[])sk2.GetSketchPoints2() ?? new object[0])
                        .Any(x => SketchIds.Key(x) == SketchIds.Key(o) && SameCoords(Coords(x), mine));
                var inBlocks = sketchBlocks.Where(b => b.Item2.Of(o) != null && twinIn(b.Item1)).ToList();
                if (inBlocks.Count == 1 && (local == null || !twinIn(exportingSketch))) inBlock = inBlocks[0].Item2.Of(o);
            }
            if (inBlock != null) return inBlock;
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
            // an entity of a sketch an Insert Part brought in (the layout's)
            if (r == null && (o is ISketchSegment || sp != null)) r = TransferredEntityRef(o);
            // or an entity of another of this part's sketches that SolidWorks reports as this sketch's
            if (r == null && (o is ISketchSegment || sp != null)) r = OwnSketchEntityRef(o, ids.Sketch);
            // or neither by id: such an entity can carry an id of its own and its coordinates in the sketch being
            // exported (a relation to a layout point SolidWorks keeps through a projected copy): found by where it is
            // in the model, among the inserted layout sketches' entities, then the part's other exported sketches
            if (r == null && (o is ISketchLine || o is ISketchArc || sp != null)) r = ByModelPosition(o, ids.Sketch, frame);
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
            var arc = o as ISketchArc;
            if (r == null && arc != null && local == null)
            {
                // a circular model edge's projection (the source of a converted or offset arc): the body edge with
                // that centre and radius, its axis along the sketch normal
                var c = ModelPoint((ISketchPoint)arc.GetCenterPoint2());
                var edge = c == null ? null : CircleEdgeAt(c, arc.GetRadius(), frame.Z);
                if (edge != null) r = Host.Descriptor(edge);
            }
            return r;
        }

        /// <summary>A straight body edge on the line through a and b (both ends on it), or null.</summary>
        /// <summary>A circular body edge whose centre, projected along `normal`, is at `c` (model m) with radius `r`.</summary>
        IEdge CircleEdgeAt(double[] c, double r, double[] normal)
        {
            if (part == null) return null;
            var n = V.Unit(normal);
            foreach (IBody2 body in (object[])part.GetBodies2(-1, true) ?? new object[0])
                foreach (IEdge e in (object[])body.GetEdges() ?? new object[0])
                {
                    var cv = (ICurve)e.GetCurve();
                    if (cv == null || !cv.IsCircle()) continue;
                    var p = (double[])cv.CircleParams;   // centre xyz, axis xyz, radius
                    if (Math.Abs(p[6] - r) > 1e-8) continue;
                    var axis = V.Unit(new[] { p[3], p[4], p[5] });
                    if (Math.Abs(Math.Abs(V.Dot(axis, n)) - 1) > 1e-9) continue;
                    var d = V.Sub(new[] { p[0], p[1], p[2] }, c);
                    double along = V.Dot(d, n);
                    if (V.Norm(new[] { d[0] - along * n[0], d[1] - along * n[1], d[2] - along * n[2] }) < 1e-8) return e;
                }
            return null;
        }

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
        /// <summary>An entity of a sketch an Insert Part brought into this part (the layout sketch a derived part is
        /// drawn on), named as the source document's sketch entity (FORMAT-SPEC 5, Rule 1, `document`). The inserted
        /// copy keeps the source sketch's entity ids and coordinates, so the entity is found in the source's ids by
        /// its own id, checked against the copy's entity with that id (same coordinates). SolidWorks does not always
        /// say which sketch such an entity belongs to (a converted entity's source reports the converting sketch).
        /// Null if it is in no inserted sketch.</summary>
        JObj TransferredEntityRef(object o)
        {
            string key = SketchIds.Key(o);
            if (key == null) return null;
            var mine = Coords(o);
            foreach (var tr in transferred.Values)
            {
                Dictionary<string, SketchIds> maps;
                SketchIds srcIds;
                if (tr.Str("nativeType") != "ProfileFeature" || tr.Str("feature") == null ||
                    !sketchIdsByDoc.TryGetValue(tr.Str("document"), out maps) || !maps.TryGetValue(tr.Str("sourceName") ?? "", out srcIds)) continue;
                string local = srcIds.Of(o);
                if (local == null) continue;
                var copy = ((doc as IPartDoc)?.FeatureByName(tr.Str("name")) as IFeature)?.GetSpecificFeature2() as ISketch;
                if (copy == null) continue;
                var twin = ((object[])copy.GetSketchSegments() ?? new object[0]).Concat((object[])copy.GetSketchPoints2() ?? new object[0])
                    .FirstOrDefault(x => SketchIds.Key(x) == key);
                var theirs = twin == null ? null : Coords(twin);
                if (mine == null || theirs == null || mine.Length != theirs.Length || mine.Where((v, i) => Math.Abs(v - theirs[i]) > 1e-9).Any()) continue;
                return new JObj { { "kind", "sketchEntity" }, { "by", "id" }, { "document", tr.Str("document") }, { "feature", tr.Str("feature") }, { "id", local } };
            }
            return null;
        }

        /// <summary>An entity of another sketch of this part, already exported, that SolidWorks reports as belonging
        /// to the sketch being exported (a converted or offset entity's source, a relation to it): found in the
        /// other sketches by its id and coordinates (sketch space, so only in sketches on the same plane).</summary>
        JObj OwnSketchEntityRef(object o, string current)
        {
            string key = SketchIds.Key(o);
            var mine = Coords(o);
            if (key == null || mine == null) return null;
            foreach (var kv in sketchMaps)
            {
                string fid;
                if (kv.Key == current || !featureIds.TryGetValue(kv.Key, out fid)) continue;
                string local = kv.Value.Of(o);
                if (local == null) continue;
                var other = ((doc as IPartDoc)?.FeatureByName(kv.Key) as IFeature)?.GetSpecificFeature2() as ISketch;
                if (other == null) continue;
                var twin = ((object[])other.GetSketchSegments() ?? new object[0]).Concat((object[])other.GetSketchPoints2() ?? new object[0])
                    .FirstOrDefault(x => SketchIds.Key(x) == key);
                var theirs = twin == null ? null : Coords(twin);
                if (theirs == null || theirs.Length != mine.Length || mine.Where((v, i) => Math.Abs(v - theirs[i]) > 1e-9).Any()) continue;
                return new JObj { { "kind", "sketchEntity" }, { "by", "id" }, { "feature", fid }, { "id", local } };
            }
            return null;
        }

        // sketch name -> its points, lines and arcs in model coordinates (m), and its normal, for ByModelPosition (per part)
        readonly Dictionary<string, List<Tuple<object, double[]>>> modelPoints = new Dictionary<string, List<Tuple<object, double[]>>>();
        readonly Dictionary<string, double[]> modelNormals = new Dictionary<string, double[]>();

        List<Tuple<object, double[]>> ModelEntities(string sketchName)
        {
            List<Tuple<object, double[]>> list;
            if (modelPoints.TryGetValue(sketchName, out list)) return list;
            list = new List<Tuple<object, double[]>>();
            var sk = ((doc as IPartDoc)?.FeatureByName(sketchName) as IFeature)?.GetSpecificFeature2() as ISketch;
            if (sk != null && !sk.Is3D())
            {
                var fr = new SketchFrame(sw, sk);
                modelNormals[sketchName] = V.Unit(fr.Z);
                foreach (ISketchPoint q in (object[])sk.GetSketchPoints2() ?? new object[0])
                    list.Add(Tuple.Create((object)q, fr.ToModel(q.X, q.Y)));
                foreach (var s in ((object[])sk.GetSketchSegments() ?? new object[0]).OfType<ISketchLine>())
                {
                    var a = (ISketchPoint)s.GetStartPoint2(); var b = (ISketchPoint)s.GetEndPoint2();
                    var ma = fr.ToModel(a.X, a.Y); var mb = fr.ToModel(b.X, b.Y);
                    list.Add(Tuple.Create((object)s, new[] { ma[0], ma[1], ma[2], mb[0], mb[1], mb[2] }));
                }
                foreach (var s in ((object[])sk.GetSketchSegments() ?? new object[0]).OfType<ISketchArc>())
                {
                    var c = (ISketchPoint)s.GetCenterPoint2();
                    var mc = fr.ToModel(c.X, c.Y);
                    list.Add(Tuple.Create((object)s, new[] { mc[0], mc[1], mc[2], s.GetRadius() }));
                }
            }
            modelPoints[sketchName] = list;
            return list;
        }

        /// <summary>A point, line or arc outside the sketch found by its model position (1e-9 m: a line by its ends, an
        /// arc by its centre and radius): an inserted layout sketch's entity (as the source document's), else another
        /// exported sketch's of this part. A relation to another sketch's entity holds its projection onto this
        /// sketch's plane (a layout point on the mid plane, the sketch on the sheet's face): positions are compared
        /// projected onto it, arcs only between parallel sketches. Null if none.</summary>
        JObj ByModelPosition(object o, string current, SketchFrame frame)
        {
            var nz = V.Unit(frame.Z);
            Func<double[], double[]> flat = q => V.Sub(q, V.Scale(nz, V.Dot(V.Sub(q, frame.Origin), nz)));   // onto this sketch's plane
            Func<double[], double[]> project = m => m.Length == 3 ? flat(m)
                : m.Length == 4 ? flat(m.Take(3).ToArray()).Concat(new[] { m[3] }).ToArray()
                : flat(m.Take(3).ToArray()).Concat(flat(m.Skip(3).ToArray())).ToArray();
            Func<string, bool> parallel = name => { double[] n; return modelNormals.TryGetValue(name, out n) && Math.Abs(Math.Abs(V.Dot(n, nz)) - 1) < 1e-9; };
            double[] want;
            var sp = o as ISketchPoint;
            var arc = o as ISketchArc;
            if (sp != null) want = ModelPoint(sp);
            else if (arc != null)
            {
                var c = ModelPoint((ISketchPoint)arc.GetCenterPoint2());
                want = c == null ? null : new[] { c[0], c[1], c[2], arc.GetRadius() };
            }
            else
            {
                var l = (ISketchLine)o;
                var a = ModelPoint((ISketchPoint)l.GetStartPoint2()); var b = ModelPoint((ISketchPoint)l.GetEndPoint2());
                want = a == null || b == null ? null : new[] { a[0], a[1], a[2], b[0], b[1], b[2] };
            }
            if (want == null) return null;
            want = project(want);
            Func<double[], bool> same0 = m => m.Length == want.Length &&
                (want.Length == 3 ? V.Dist(m, want) < 1e-9
                 : want.Length == 4 ? V.Dist(m.Take(3).ToArray(), want.Take(3).ToArray()) < 1e-9 && Math.Abs(m[3] - want[3]) < 1e-9
                 : (V.Dist(m.Take(3).ToArray(), want.Take(3).ToArray()) < 1e-9 && V.Dist(m.Skip(3).ToArray(), want.Skip(3).ToArray()) < 1e-9) ||
                   (V.Dist(m.Take(3).ToArray(), want.Skip(3).ToArray()) < 1e-9 && V.Dist(m.Skip(3).ToArray(), want.Take(3).ToArray()) < 1e-9));
            Func<double[], bool> same = m => same0(project(m));
            foreach (var tr in transferred.Values)
            {
                Dictionary<string, SketchIds> maps;
                SketchIds srcIds;
                if (tr.Str("nativeType") != "ProfileFeature" || tr.Str("feature") == null ||
                    !sketchIdsByDoc.TryGetValue(tr.Str("document"), out maps) || !maps.TryGetValue(tr.Str("sourceName") ?? "", out srcIds)) continue;
                // (a point, an arc and a line describe themselves with 3, 4 and 6 numbers: `same` tells them apart)
                var hits = ModelEntities(tr.Str("name")).Where(x => same(x.Item2) && (x.Item2.Length != 4 || parallel(tr.Str("name")))).ToList();
                foreach (var h in hits)
                {
                    string local = srcIds.Of(h.Item1);   // the copy keeps the source sketch's ids
                    if (local != null)
                        return new JObj { { "kind", "sketchEntity" }, { "by", "id" }, { "document", tr.Str("document") }, { "feature", tr.Str("feature") }, { "id", local } };
                }
            }
            foreach (var kv in sketchMaps)
            {
                string fid;
                if (kv.Key == current || !featureIds.TryGetValue(kv.Key, out fid)) continue;
                foreach (var h in ModelEntities(kv.Key).Where(x => same(x.Item2) && (x.Item2.Length != 4 || parallel(kv.Key))))
                {
                    string local = kv.Value.Of(h.Item1);
                    if (local != null) return new JObj { { "kind", "sketchEntity" }, { "by", "id" }, { "feature", fid }, { "id", local } };
                }
            }
            return null;
        }

        /// <summary>A sketch entity's own coordinates (sketch space, m): a point's x, y; a line's ends; an arc's
        /// centre, radius and ends. Null for other entities.</summary>
        static bool SameCoords(double[] a, double[] b)
        {
            return a != null && b != null && a.Length == b.Length && !a.Where((v, i) => Math.Abs(v - b[i]) > 1e-9).Any();
        }

        static double[] Coords(object o)
        {
            var p = o as ISketchPoint;
            if (p != null) return new[] { p.X, p.Y };
            var l = o as ISketchLine;
            if (l != null)
            {
                var a = (ISketchPoint)l.GetStartPoint2(); var b = (ISketchPoint)l.GetEndPoint2();
                return new[] { a.X, a.Y, b.X, b.Y };
            }
            var c = o as ISketchArc;
            if (c != null)
            {
                var m = (ISketchPoint)c.GetCenterPoint2();
                return new[] { m.X, m.Y, c.GetRadius() };
            }
            return null;
        }

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
