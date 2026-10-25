// Replayer: sketches. Entities are created at their seed coordinates with inference off (AddToDB),
// shared points are merged, then relations and dimensions are added. Coordinates go source sketch ->
// model -> target sketch, so a target sketch whose axes differ from the source's still lands right.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        static readonly Dictionary<string, int> RelationTypes = new Dictionary<string, int>
        {
            { "horizontal", 4 }, { "vertical", 5 }, { "tangent", 6 }, { "parallel", 7 }, { "perpendicular", 8 },
            { "coincident", 9 }, { "concentric", 10 }, { "symmetric", 11 }, { "midpoint", 12 }, { "intersection", 13 },
            { "equal", 14 }, { "fix", 17 }, { "horizontalPoints", 25 }, { "verticalPoints", 26 }, { "collinear", 27 },
            { "coradial", 28 }, { "pierce", 40 }
        };

        class SketchMap
        {
            public readonly Dictionary<string, ISketchSegment> Segs = new Dictionary<string, ISketchSegment>();
            public readonly Dictionary<string, ISketchPoint> Points = new Dictionary<string, ISketchPoint>();
            public readonly Dictionary<string, double[]> Target = new Dictionary<string, double[]>();   // point id -> target sketch xy (m)
            // point id -> the segment it is an end of and which end (0 start, 1 end, 2 centre): found again through
            // the segment, as another point may lie at the same spot
            public readonly Dictionary<string, Tuple<string, int>> EndOf = new Dictionary<string, Tuple<string, int>>();
            public bool Mirrored;   // the replayed sketch's normal is opposite the source's: directions flip
            public SketchFrame Source;   // the source sketch's frame (model m): where its regions' points lie
        }

        string step;   // the call in progress, so a COM fault can be reported precisely

        // sketches on a plane another feature generates (edge flange profiles): sketch id -> its feature,
        // replayed by that feature through ReplaySketch with its own opener
        readonly Dictionary<string, JObj> deferredSketches = new Dictionary<string, JObj>();

        /// <summary>Replay a sketch. `open` (optional) opens the sketch for editing in place of selecting
        /// the recorded plane; it returns null or why it could not.</summary>
        string ReplaySketch(JObj nf, JObj def, Func<string> open = null)
        {
            if (open == null && def.Obj("plane")?.Str("kind") == "generated")
            {
                deferredSketches[nf.Str("id")] = nf;
                return null;
            }
            step = "start";
            try { return ReplaySketchSteps(nf, def, open); }
            catch (Exception e) { throw new InvalidOperationException("sketch replay failed at '" + step + "': " + e.Message, e); }
        }

        string ReplaySketchSteps(JObj nf, JObj def, Func<string> open)
        {
            step = "sketch manager";
            var sm = (ISketchManager)doc.SketchManager;
            step = "clear selection";
            doc.ClearSelection2(true);
            if (open != null)
            {
                step = "open generated sketch";
                string w = open();
                if (w != null) return w;
            }
            else
            {
                var plane = def.Obj("plane");
                step = "select plane " + plane.Str("kind");
                string why;
                if (plane.Str("kind") == "frame")
                {
                    // the frame alone places the sketch (FORMAT-SPEC 4, Sketch): a plane at the frame
                    var fr0 = def.Obj("frame");
                    var fp = fr0 == null ? null : PlaneAt(fr0.Vec("origin"), fr0.Vec("z"), "Plane-" + nf.Str("name"));
                    if (fp == null) return "sketch plane: a plane at the sketch's frame could not be made";
                    why = fp.Select2(false, 0) ? null : "could not select the plane at the sketch's frame";
                    if (nf.Str("status") == "mapped") nf["status"] = "approximate";
                }
                else why = SelectRef(plane, false, 0);
                if (why != null) return "sketch plane: " + why;
                step = "open sketch";
                sm.InsertSketch(true);
            }
            step = "active sketch";
            var sk = sm.ActiveSketch;
            if (sk == null) return "could not open a sketch on the plane";
            step = "source frame";
            var fr = def.Obj("frame");
            var src = new SketchFrame(fr.Vec("origin").Select(U.M).ToArray(), fr.Vec("x"), fr.Vec("z"));
            step = "target frame";
            var tgt = new SketchFrame(sw, sk);
            step = "sketch setup";
            bool mirrored = V.Dot(src.Z, tgt.Z) < 0;
            Func<double[], double[]> map = p => tgt.ToSketch(src.ToModel(U.M(p[0]), U.M(p[1])));

            var notes = new List<string>();
            var sm2 = new SketchMap { Mirrored = mirrored, Source = src };
            // AddToDB/DisplayWhenAdded fault when called through the typed interface out of process
            // (RPC_E_SERVERFAULT, SolidWorks 2018 and 2026); late binding works in and out of process
            dynamic dsm = sm;
            bool addToDb = dsm.AddToDB, display = dsm.DisplayWhenAdded;
            dsm.AddToDB = true;           // no inference: no automatic relations, no snapping
            dsm.DisplayWhenAdded = false;
            try
            {
                var ents = def.Arr("entities").Cast<JObj>().ToList();
                var pointPos = ents.Where(e => e.Str("type") == "point").ToDictionary(e => e.Str("id"), e => map(e.Vec("p")));
                foreach (var kv in pointPos) sm2.Target[kv.Key] = kv.Value;
                var owned = new HashSet<string>();
                var copies = new Dictionary<string, List<ISketchPoint>>();
                Action<string, object> own = (pid, pt) =>
                {
                    if (pid == null || pt == null) return;
                    owned.Add(pid);
                    if (!copies.ContainsKey(pid)) copies[pid] = new List<ISketchPoint>();
                    copies[pid].Add((ISketchPoint)pt);
                };
                foreach (var e in ents)
                {
                    step = "create " + e.Str("id");
                    ISketchSegment seg = null;
                    switch (e.Str("type"))
                    {
                        case "line":
                        {
                            var a = map(e.Vec("start"));
                            var b = map(e.Vec("end"));
                            seg = sm.CreateLine(a[0], a[1], 0, b[0], b[1], 0);
                            if (seg != null)
                            {
                                own(e.Str("startPoint"), ((ISketchLine)seg).GetStartPoint2());
                                own(e.Str("endPoint"), ((ISketchLine)seg).GetEndPoint2());
                            }
                            break;
                        }
                        case "circle":
                        {
                            var c = map(e.Vec("center"));
                            seg = sm.CreateCircleByRadius(c[0], c[1], 0, U.M(e.Num("radius")));
                            if (seg != null) own(e.Str("centerPoint"), ((ISketchArc)seg).GetCenterPoint2());
                            break;
                        }
                        case "arc":
                        {
                            var c = map(e.Vec("center"));
                            var a = map(e.Vec("start"));
                            var b = map(e.Vec("end"));
                            bool ccw = e.Bool("ccw") != mirrored;
                            seg = sm.CreateArc(c[0], c[1], 0, a[0], a[1], 0, b[0], b[1], 0, (short)(ccw ? 1 : -1));
                            if (seg != null)
                            {
                                own(e.Str("centerPoint"), ((ISketchArc)seg).GetCenterPoint2());
                                own(e.Str("startPoint"), ((ISketchArc)seg).GetStartPoint2());
                                own(e.Str("endPoint"), ((ISketchArc)seg).GetEndPoint2());
                            }
                            break;
                        }
                        case "ellipse":
                        case "ellipticalArc":
                        {
                            var c = map(e.Vec("center"));
                            var a = map(e.Vec("major"));
                            var b = map(e.Vec("minor"));
                            if (e.Str("type") == "ellipse")
                                seg = sm.CreateEllipse(c[0], c[1], 0, a[0], a[1], 0, b[0], b[1], 0);
                            else
                            {
                                var s1 = map(e.Vec("start"));
                                var s2 = map(e.Vec("end"));
                                bool ccw = e.Bool("ccw") != mirrored;
                                seg = sm.CreateEllipticalArc(c[0], c[1], 0, a[0], a[1], 0, b[0], b[1], 0, s1[0], s1[1], 0, s2[0], s2[1], 0, (short)(ccw ? 1 : -1));
                            }
                            if (seg != null)
                            {
                                var el = (ISketchEllipse)seg;
                                own(e.Str("centerPoint"), el.GetCenterPoint2());
                                own(e.Str("majorPoint"), el.GetMajorPoint2());
                                own(e.Str("minorPoint"), el.GetMinorPoint2());
                                // SolidWorks also gives an ellipse points at the far ends of both axes: the exported
                                // points there are those, not separate points
                                var made = ((object[])sk.GetSketchPoints2() ?? new object[0]).Cast<ISketchPoint>().ToList();
                                foreach (var far in new[] { new[] { 2 * c[0] - a[0], 2 * c[1] - a[1] }, new[] { 2 * c[0] - b[0], 2 * c[1] - b[1] } })
                                {
                                    Func<double, double, bool> at = (x, y) => Math.Abs(x - far[0]) < 1e-8 && Math.Abs(y - far[1]) < 1e-8;
                                    string pid = pointPos.Where(kv => !owned.Contains(kv.Key) && at(kv.Value[0], kv.Value[1])).Select(kv => kv.Key).FirstOrDefault();
                                    var pt = made.FirstOrDefault(q => at(q.X, q.Y));
                                    if (pid != null && pt != null) own(pid, pt);
                                }
                                if (e.Str("type") == "ellipticalArc")
                                {
                                    own(e.Str("startPoint"), el.GetStartPoint2());
                                    own(e.Str("endPoint"), el.GetEndPoint2());
                                }
                            }
                            break;
                        }
                        case "spline":
                        {
                            var fit = e.Arr("points").Select(q => map(((List<object>)q).Select(Convert.ToDouble).ToArray())).ToList();
                            var data = fit.SelectMany(q => new[] { q[0], q[1], 0.0 }).ToArray();
                            seg = (ISketchSegment)sm.CreateSpline2(data, true);   // natural ends, as the UI makes them
                            if (seg != null)
                            {
                                var sp = (ISketchSpline)seg;
                                var made = ((object[])sp.GetPoints2() ?? new object[0]);
                                var pids = e.Arr("pointIds");
                                for (int i = 0; i < made.Length && i < pids.Count; i++) own(pids[i] as string, made[i]);
                                string hw = ApplySplineHandles(sp, e, mirrored, map);
                                if (hw != null) notes.Add(hw);

                            }
                            break;
                        }
                        case "point":
                            continue;
                        default:
                            notes.Add("entity " + e.Str("id") + " (" + e.Str("type") + ") not rebuilt");
                            continue;
                    }
                    if (seg == null) { notes.Add("could not create " + e.Str("id")); continue; }
                    if (e.Bool("construction")) seg.ConstructionGeometry = true;
                    sm2.Segs[e.Str("id")] = seg;
                    string[] roles = { "startPoint", "endPoint", "centerPoint" };
                    if (e.Str("type") == "line" || e.Str("type") == "arc" || e.Str("type") == "circle")
                        for (int r = 0; r < roles.Length; r++)
                            if (e.Str(roles[r]) != null && !sm2.EndOf.ContainsKey(e.Str(roles[r])))
                                sm2.EndOf[e.Str(roles[r])] = Tuple.Create(e.Str("id"), r);
                }
                foreach (var kv in pointPos.Where(kv => !owned.Contains(kv.Key)))
                {
                    var p = sm.CreatePoint(kv.Value[0], kv.Value[1], 0);
                    if (p != null) sm2.Points[kv.Key] = p;
                }
                // shared points: segments created separately each got their own copy -> merge them
                step = "relation manager";
                var rm = (ISketchRelationManager)sk.RelationManager;
                // SolidWorks often joins endpoints created at the same spot by itself: then nothing to merge
                int distinct = pointPos.Count;
                bool joined = ((object[])sk.GetSketchPoints2() ?? new object[0]).Length <= distinct;
                foreach (var kv in copies.Where(kv => !joined))
                {
                    step = "merge " + kv.Key;
                    sm2.Points[kv.Key] = kv.Value[0];
                    for (int i = 1; i < kv.Value.Count; i++)
                    {
                        // AddRelation returns null even when the merge happens: judge by the point count
                        // a coincident relation between two endpoints merges them (as in the UI);
                        // judged by the point count, since the call reports nothing useful
                        // (SolidWorks may keep both points with a coincident relation instead of merging:
                        // equivalent for solving, profiles and dimensions)
                        int n = ((object[])sk.GetSketchPoints2() ?? new object[0]).Length;
                        int r0 = rm.GetRelationsCount(0);
                        AddConstraintBySelection(new object[] { kv.Value[0], kv.Value[i] }, "coincident");
                        if (((object[])sk.GetSketchPoints2() ?? new object[0]).Length >= n && rm.GetRelationsCount(0) <= r0)
                            notes.Add("could not join copies of point " + kv.Key);
                    }
                }
                step = "refresh points";
                RefreshPoints(sk, sm2);

                Trace(nf.Str("name") + " relations");
                // geometry wins: a relation that would move the drawn geometry from where the source has it is undone
                guard = sm2;
                guardMoved.Clear();   // relations that moved the geometry (reported)
                foreach (JObj c in def.Arr("constraints"))
                {
                    step = "relation " + c.Str("type");
                    if (c.Str("type") == "offset")
                    {
                        // Offset Entities: the offset follows its source - concentric arcs, parallel lines; the
                        // offset distance is one of the sketch's dimensions
                        var oo = c.Arr("entities").Select(x => Resolve(sm2, x)).ToArray();
                        string kindOf = oo.Length == 2 && oo.All(o => o is ISketchArc) ? "concentric" : oo.Length == 2 && oo.All(o => o is ISketchLine) ? "parallel" : null;
                        int b0 = rm.GetRelationsCount(0);
                        if (kindOf == null || oo.Any(o => o == null)) notes.Add("offset entity: its source was not found");
                        else if (!AddConstraintBySelection(oo, kindOf) || rm.GetRelationsCount(0) <= b0) notes.Add("offset entity: " + kindOf + " with its source rejected");
                        continue;
                    }
                    if (c.Str("type") == "patterned")
                    {
                        // a sketch pattern's copies follow the seed: equal (and parallel for lines)
                        var pp = c.Arr("entities").Select(x => Resolve(sm2, x)).ToArray();
                        if (pp.Any(o => o == null)) { notes.Add("patterned entity missing"); continue; }
                        // parallel only where the copies are (a linear pattern); a circular one turns them
                        bool lines = pp.All(o => o is ISketchLine) && AllParallel(pp.Cast<ISketchLine>().ToList());
                        foreach (var rel in lines ? new[] { "equal", "parallel" } : new[] { "equal" })
                        {
                            int b1 = rm.GetRelationsCount(0);
                            if (!AddConstraintBySelection(pp, rel) || rm.GetRelationsCount(0) <= b1) notes.Add("patterned entities: " + rel + " rejected");
                        }
                        continue;
                    }
                    if (c.Str("type") == "converted")
                    {
                        var co = c.Arr("entities").Select(x => Resolve(sm2, x)).ToArray();
                        if (co.Length != 2 || co.Any(o => o == null)) { notes.Add("converted entity: its source was not found"); continue; }
                        notes.AddRange(ReplayConverted(co[0], co[1]));
                        continue;
                    }
                    int t;
                    if (c.Bool("supported", true) == false || !RelationTypes.TryGetValue(c.Str("type"), out t))
                    {
                        notes.Add("relation " + c.Str("type") + " not rebuilt");
                        continue;
                    }
                    var objs = c.Arr("entities").Select(x => Resolve(sm2, x)).ToArray();
                    if (objs.Any(o => o == null)) { notes.Add("relation " + c.Str("type") + ": entity missing"); continue; }
                    // two of the sketch's own points SolidWorks already joined into one (drawn at the same spot):
                    // coincident as they are. Only the sketch's own: ids are unique within a sketch, so a point of
                    // another sketch (the origin, a layout point) can have the same id
                    if (c.Str("type") == "coincident" && c.Arr("entities").All(x => x is string) && objs.All(o => o is ISketchPoint)
                        && objs.Select(o => PointKey((ISketchPoint)o)).Distinct().Count() == 1)
                        continue;
                    // AddRelation faults (RPC_E_SERVERFAULT) when given entity arrays from C# out of process and
                    // when given the origin's point; selecting the entities and adding the constraint, as a user
                    // would, works everywhere
                    int before = rm.GetRelationsCount(0);
                    // which entities, for the report (an id of this sketch, or the kind of outside reference)
                    guardLabel = c.Str("type") + " (" + string.Join(", ", c.Arr("entities").Select(x => x as string ?? ((JObj)x).Str("kind"))) + ")";
                    if (!AddConstraintBySelection(objs, c.Str("type")) || rm.GetRelationsCount(0) <= before)
                        notes.Add("relation " + guardLabel + " rejected");
                    guardLabel = null;
                }

                guard = null;
                if (guardMoved.Count > 0)
                    notes.Add("relation(s) " + string.Join(", ", guardMoved) + " moved the drawn geometry away from the source's");
                Trace(nf.Str("name") + " dimensions");
                foreach (JObj dm in def.Arr("dimensions"))
                {
                    step = "dimension " + dm.Str("name");
                    string w = AddDimension(sm2, dm, src, tgt);
                    if (w != null) notes.Add(w);
                }
            }
            finally
            {
                guard = null;
                if (!step.StartsWith("close")) step = "close (after " + step + ")";
                dsm.AddToDB = addToDb;
                dsm.DisplayWhenAdded = display;
                Trace(nf.Str("name") + " closing");
                sm.InsertSketch(true);
            }
            var feat = (IFeature)sk;
            // SolidWorks 2018 leaves a spline made with CreateSpline2 unsolved until its sketch is solved again (it
            // keeps a bulged curve until the file is reopened): open and close the sketch once
            if (def.Arr("entities").Cast<JObj>().Any(e => e.Str("type") == "spline"))
            {
                doc.ClearSelection2(true);
                feat.Select2(false, 0);
                doc.EditSketch();
                sm.InsertSketch(true);
                doc.ClearSelection2(true);
            }
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            sketchMaps[nf.Str("id")] = sm2;
            if (def.Bool("hidden")) Hide(feat);
            if (notes.Count > 0) nf["note"] = (nf.Str("note") == null ? "" : nf.Str("note") + "; ") + string.Join("; ", notes);
            if (notes.Count > 0 && nf.Str("status") == "mapped") nf["status"] = "approximate";
            return null;
        }

        static readonly Dictionary<string, string> ConstraintIds = new Dictionary<string, string>
        {
            { "horizontal", "sgHORIZONTAL2D" }, { "vertical", "sgVERTICAL2D" }, { "tangent", "sgTANGENT" },
            { "parallel", "sgPARALLEL" }, { "perpendicular", "sgPERPENDICULAR" }, { "coincident", "sgCOINCIDENT" },
            { "concentric", "sgCONCENTRIC" }, { "symmetric", "sgSYMMETRIC" }, { "midpoint", "sgATMIDDLE" },
            { "intersection", "sgATINTERSECT" }, { "equal", "sgSAMELENGTH" }, { "fix", "sgFIXED" },
            { "horizontalPoints", "sgHORIZONTALPOINTS2D" }, { "verticalPoints", "sgVERTICALPOINTS2D" },
            { "collinear", "sgCOLINEAR" }, { "coradial", "sgCORADIAL" }, { "pierce", "sgATPIERCE" },
            // 3D sketches
            { "alongX", "sgALONGX3D" }, { "alongY", "sgALONGY3D" }, { "alongZ", "sgALONGZ3D" }
        };

        bool AddConstraintBySelection(object[] objs, string type)
        {
            string id;
            if (!ConstraintIds.TryGetValue(type, out id)) return false;
            doc.ClearSelection2(true);
            bool append = false;
            foreach (var o in objs)
            {
                if (!SelectSketchObject(o, append)) { doc.ClearSelection2(true); return false; }
                append = true;
            }
            double drift0 = guard == null ? 0 : Drift(guard);
            doc.SketchAddConstraints(id);
            doc.ClearSelection2(true);
            // a relation that moves the drawn geometry from where the source has it is reported (undoing it from
            // inside an edited sketch stalled SolidWorks 2026)
            double drift = guard == null ? 0 : Drift(guard);
            if (drift > drift0 + 1e-6) guardMoved.Add((guardLabel ?? type) + " by " + U.R(U.Mm(drift - drift0)) + " mm");
            return true;
        }

        SketchMap guard;   // while relations are added: the sketch whose drawn geometry must stay put
        string guardLabel;   // the relation being added, with its entities (for the report)
        readonly List<string> guardMoved = new List<string>();   // relations that moved it, with how far

        /// <summary>How far the sketch's points are from where the source has them (m, the largest).</summary>
        static double Drift(SketchMap m)
        {
            double worst = 0;
            foreach (var kv in m.Points)
            {
                double[] t;
                if (!m.Target.TryGetValue(kv.Key, out t)) continue;
                try { worst = Math.Max(worst, Math.Sqrt(Math.Pow(kv.Value.X - t[0], 2) + Math.Pow(kv.Value.Y - t[1], 2))); }
                catch (System.Runtime.InteropServices.COMException) { }
            }
            return worst;
        }

        /// <summary>Select a sketch segment/point, or a model edge/face/vertex (an external reference).</summary>
        static bool SelectSketchObject(object o, bool append)
        {
            if (o is ISketchSegment) return ((ISketchSegment)o).Select4(append, null);
            if (o is ISketchPoint) return ((ISketchPoint)o).Select4(append, null);
            if (o is IEntity) return ((IEntity)o).Select4(append, null);
            if (o is IFeature) return ((IFeature)o).Select2(append, 0);   // a datum plane
            return false;
        }

        /// <summary>After merges, re-find every point by its target position.</summary>
        static void RefreshPoints(ISketch sk, SketchMap m)
        {
            var pts = ((object[])sk.GetSketchPoints2() ?? new object[0]).Cast<ISketchPoint>().ToList();
            // a segment's ends are found through the segment: a separate point (a standalone point on a line's end,
            // only tied to it by a relation) may lie at the same spot and must stay a different point
            var ends = new HashSet<string>();
            foreach (var kv in m.EndOf)
            {
                var p = SegmentEnd(m, kv.Value);
                if (p == null) continue;
                m.Points[kv.Key] = p;
                ends.Add(PointKey(p));
            }
            foreach (var kv in m.Target)
            {
                if (m.EndOf.ContainsKey(kv.Key) && m.Points.ContainsKey(kv.Key)) continue;
                var at = pts.Where(p => Math.Abs(p.X - kv.Value[0]) < 1e-9 && Math.Abs(p.Y - kv.Value[1]) < 1e-9).ToList();
                var hit = at.FirstOrDefault(p => !ends.Contains(PointKey(p))) ?? at.FirstOrDefault();
                if (hit != null) m.Points[kv.Key] = hit;
            }
        }

        static ISketchPoint SegmentEnd(SketchMap m, Tuple<string, int> end)
        {
            ISketchSegment seg;
            if (!m.Segs.TryGetValue(end.Item1, out seg)) return null;
            try
            {
                var arc = seg as ISketchArc;
                if (arc != null)
                    return (ISketchPoint)(end.Item2 == 0 ? arc.GetStartPoint2() : end.Item2 == 1 ? arc.GetEndPoint2() : arc.GetCenterPoint2());
                var line = seg as ISketchLine;
                if (line != null && end.Item2 < 2)
                    return (ISketchPoint)(end.Item2 == 0 ? line.GetStartPoint2() : line.GetEndPoint2());
            }
            catch (System.Runtime.InteropServices.COMException) { }
            return null;
        }

        static string PointKey(ISketchPoint p) => string.Join(",", (int[])p.GetID());

        /// <summary>Spline handles the user set (tangent direction/magnitude, curvature), on the rebuilt spline. The
        /// direction is an angle in the sketch's axes, so it is mapped through the same source-to-target transform
        /// as the points (the target sketch's axes may be rotated or mirrored).</summary>
        string ApplySplineHandles(ISketchSpline sp, JObj e, bool mirrored, Func<double[], double[]> map)
        {
            var want = e.Arr("handles").Cast<JObj>().ToList();
            if (want.Count == 0) return null;
            var have = ((object[])sp.GetSplineHandles() ?? new object[0]).Cast<ISplineHandle>().ToList();
            var pts = e.Arr("points");
            foreach (var w in want)
            {
                int i = (int)w.Num("point");
                var h = have.FirstOrDefault(x => x.SplinePointNumber == i);
                if (h == null || i >= pts.Count) return "spline handle at point " + i + " not found";
                if (w.Bool("tangentDriving"))
                {
                    var p = ((List<object>)pts[i]).Select(Convert.ToDouble).ToArray();
                    double r = w.Num("radial");
                    var a = map(p);
                    var b = map(new[] { p[0] + Math.Cos(r), p[1] + Math.Sin(r) });   // mm in, m out
                    h.TangentDriving = true;
                    h.TangentRadialDirection = Math.Atan2(b[1] - a[1], b[0] - a[0]);
                    h.set_TangentMagnitude(1, w.Num("magnitude"));   // the two sides of the point
                    if (w.Has("magnitude2")) h.set_TangentMagnitude(2, w.Num("magnitude2"));
                }
                if (w.Bool("curvatureControlled"))
                {
                    h.CurvatureControlled = true;
                    h.Curvature = mirrored ? -w.Num("curvature") : w.Num("curvature");
                }
            }
            return null;
        }

        /// <summary>An entity of a sketch an Insert Part brought in (FORMAT-SPEC 5: a sketch entity of another
        /// `document`): found in this part's inserted copy of that sketch by geometry, the source entity's own
        /// coordinates taken through the source sketch's frame into the copy's. Null if not found.</summary>
        object ResolveTransferred(JObj r)
        {
            IFeature tf;
            if (!transferredItems.TryGetValue(r.Str("document") + "|" + r.Str("feature"), out tf)) return null;
            var copy = tf.GetSpecificFeature2() as ISketch;
            var srcDoc = Model.Arr("documents").Cast<JObj>().FirstOrDefault(d => d.Str("id") == r.Str("document"));
            var def = srcDoc?.Arr("features").Cast<JObj>().FirstOrDefault(f => f.Str("id") == r.Str("feature"))?.Obj("def");
            var ent = def?.Arr("entities").Cast<JObj>().FirstOrDefault(e => e.Str("id") == r.Str("id"));
            var fr = def?.Obj("frame");
            if (copy == null || ent == null || fr == null) return null;
            var src = new SketchFrame(fr.Vec("origin").Select(U.M).ToArray(), fr.Vec("x"), fr.Vec("z"));
            var tgt = new SketchFrame(sw, copy);
            Func<string, double[]> at = k => tgt.ToSketch(src.ToModel(U.M(ent.Vec(k)[0]), U.M(ent.Vec(k)[1])));
            Func<ISketchPoint, double[], bool> near = (sp, q) => sp != null && Math.Abs(sp.X - q[0]) < 1e-7 && Math.Abs(sp.Y - q[1]) < 1e-7;
            var segs = ((object[])copy.GetSketchSegments() ?? new object[0]).OfType<ISketchSegment>().ToList();
            switch (ent.Str("type"))
            {
                case "point":
                {
                    var q = at("p");
                    return ((object[])copy.GetSketchPoints2() ?? new object[0]).OfType<ISketchPoint>().FirstOrDefault(sp => near(sp, q));
                }
                case "line":
                {
                    var a = at("start");
                    var b = at("end");
                    return segs.OfType<ISketchLine>().FirstOrDefault(l =>
                    {
                        var p1 = (ISketchPoint)l.GetStartPoint2();
                        var p2 = (ISketchPoint)l.GetEndPoint2();
                        return (near(p1, a) && near(p2, b)) || (near(p1, b) && near(p2, a));
                    });
                }
                case "arc":
                case "circle":
                {
                    var c = at("center");
                    double rad = U.M(ent.Num("radius"));
                    bool circle = ent.Str("type") == "circle";
                    var s0 = circle ? null : at("start");
                    return segs.OfType<ISketchArc>().FirstOrDefault(x => near((ISketchPoint)x.GetCenterPoint2(), c) && Math.Abs(x.GetRadius() - rad) < 1e-7 &&
                        (circle || near((ISketchPoint)x.GetStartPoint2(), s0) || near((ISketchPoint)x.GetEndPoint2(), s0)));
                }
            }
            return null;
        }

        /// <summary>Convert Entities (FORMAT-SPEC 4, relation "converted"): the own entity follows its source. Rebuilt as
        /// the relations that keep it on the source: collinear (a line) or coradial (an arc or circle), and its end
        /// points coincident with the source's nearest ones. Returns notes for what could not be added.</summary>
        List<string> ReplayConverted(object own, object source)
        {
            var notes = new List<string>();
            var rm = (ISketchRelationManager)((ISketch)((ISketchSegment)own).GetSketch()).RelationManager;
            Func<string, object[], bool> add = (type, o) =>
            {
                int before = rm.GetRelationsCount(0);
                return AddConstraintBySelection(o, type) && rm.GetRelationsCount(0) > before;
            };
            bool isLine = own is ISketchLine;
            // the ends (a circle has none): each own end on the source end nearest to it, in model space - only where
            // the drawn end is there. The source is the edge as the finished part has it, which a later feature may
            // have split (an edge flange's own profile, converted from the edge it stands on, keeps the flange's
            // stretch while the recorded edge is what is left beside it): geometry wins
            var ownEnds = Ends(own);
            var srcEnds = Ends(source);
            var ends = new List<object[]>();
            if (ownEnds.Count == 2 && srcEnds.Count == 2)
            {
                bool swap = V.Dist(Model3(ownEnds[0]), Model3(srcEnds[0])) + V.Dist(Model3(ownEnds[1]), Model3(srcEnds[1])) >
                            V.Dist(Model3(ownEnds[0]), Model3(srcEnds[1])) + V.Dist(Model3(ownEnds[1]), Model3(srcEnds[0]));
                for (int i = 0; i < 2; i++)
                {
                    var src = srcEnds[swap ? 1 - i : i];
                    if (V.Dist(Model3(ownEnds[i]), Model3(src)) < 1e-6) ends.Add(new[] { ownEnds[i], src });
                }
            }
            if (!add(isLine ? "collinear" : "coradial", new[] { own, source }))
                notes.Add("converted entity: " + (isLine ? "collinear" : "coradial") + " with its source rejected");
            foreach (var pair in ends)
                if (!add("coincident", pair))
                    notes.Add("converted entity: an end on its source rejected");
            return notes;
        }

        /// <summary>A line's or arc's two end points (sketch points, or an edge's vertices); empty for a circle.</summary>
        static List<object> Ends(object o)
        {
            if (o is ISketchLine) return new List<object> { ((ISketchLine)o).GetStartPoint2(), ((ISketchLine)o).GetEndPoint2() };
            var arc = o as ISketchArc;
            if (arc != null && arc.IsCircle() == 0) return new List<object> { arc.GetStartPoint2(), arc.GetEndPoint2() };
            var e = o as IEdge;
            if (e != null && e.GetStartVertex() != null && e.GetEndVertex() != null && !ReferenceEquals(e.GetStartVertex(), e.GetEndVertex()))
                return new List<object> { e.GetStartVertex(), e.GetEndVertex() };
            return new List<object>();
        }

        /// <summary>A sketch point or vertex in model coordinates (m).</summary>
        double[] Model3(object o)
        {
            var v = o as IVertex;
            if (v != null) return V.P3(v.GetPoint());
            var sp = (ISketchPoint)o;
            return new SketchFrame(sw, (ISketch)sp.GetSketch()).ToModel(sp.X, sp.Y);
        }

        /// <summary>Are the lines parallel as drawn?</summary>
        static bool AllParallel(List<ISketchLine> lines)
        {
            Func<ISketchLine, double[]> dir = l =>
            {
                var a = (ISketchPoint)l.GetStartPoint2();
                var b = (ISketchPoint)l.GetEndPoint2();
                return V.Unit(new[] { b.X - a.X, b.Y - a.Y, 0 });
            };
            var d0 = dir(lines[0]);
            return lines.All(l => Math.Abs(Math.Abs(V.Dot(dir(l), d0)) - 1) < 1e-9);
        }

        /// <summary>Hide a sketch (as it was in the source).</summary>
        void Hide(IFeature sketch)
        {
            doc.ClearSelection2(true);
            if (sketch.Select2(false, 0)) doc.BlankSketch();
            doc.ClearSelection2(true);
        }

        object Resolve(SketchMap m, object entity)
        {
            var s = entity as string;
            if (s != null)
            {
                ISketchSegment seg;
                ISketchPoint pt;
                if (m.Segs.TryGetValue(s, out seg)) return seg;
                if (m.Points.TryGetValue(s, out pt)) return pt;
                return null;
            }
            var r = entity as JObj;
            if (r == null) return null;
            switch (r.Str("kind"))
            {
                case "origin":
                    return originPoint;
                case "originPlane":   // a datum plane (a point kept on where the sketch plane meets it)
                {
                    int i = Array.IndexOf(new[] { "front", "top", "right" }, r.Str("plane"));
                    return i >= 0 && i < originPlanes.Count ? originPlanes[i] : null;
                }
                case "feature":
                {
                    IFeature f;
                    return features.TryGetValue(r.Str("id"), out f) ? f : null;
                }
                case "sketchEntity":
                {
                    if (r.Str("document") != null && r.Str("document") != docId) return ResolveTransferred(r);
                    SketchMap other;
                    return sketchMaps.TryGetValue(r.Str("feature"), out other) ? Resolve(other, r.Str("id")) : null;
                }
                case "face":
                case "edge":
                case "vertex":
                {
                    List<object> ents;
                    var res = ResolveRef(r, out ents);
                    return res.Str("result") == "resolved" && ents.Count == 1 ? ents[0] : null;
                }
            }
            return null;
        }

        string AddDimension(SketchMap m, JObj dm, SketchFrame src, SketchFrame tgt)
        {
            doc.ClearSelection2(true);
            bool append = false;
            foreach (var x in dm.Arr("entities"))
            {
                var o = Resolve(m, x);
                if (o == null) return "dimension " + dm.Str("name") + ": entity missing";
                if (!SelectSketchObject(o, append)) return "dimension " + dm.Str("name") + ": could not select its entities";
                append = true;
            }
            var tp = dm.Vec("textPosition") ?? new[] { 0.0, 0.0 };
            var at = src.ToModel(U.M(tp[0]), U.M(tp[1]));
            // AddDimension2 guesses horizontal / vertical from where the text sits, and can guess wrong (a Hole Wizard
            // position sketch got two horizontal dimensions on one point): those kinds are made explicitly, along the
            // target sketch axis nearest the source's (the two frames may be turned against each other)
            IDisplayDimension dd = null;
            string kind = dm.Str("kind");
            if (kind == "horizontal" || kind == "vertical")
            {
                var dir = kind == "horizontal" ? src.X : src.Y;
                bool alongX = Math.Abs(V.Dot(dir, tgt.X)) >= Math.Abs(V.Dot(dir, tgt.Y));
                dd = (IDisplayDimension)(alongX ? doc.AddHorizontalDimension2(at[0], at[1], at[2])
                                                : doc.AddVerticalDimension2(at[0], at[1], at[2]));
            }
            // diameter and radius explicitly too: AddDimension2 makes a radius on an arc and a diameter on a circle,
            // whatever the source had (a diameter on an arc replayed as a radius of the diameter's value doubles it)
            if (kind == "diameter") dd = (IDisplayDimension)doc.AddDiameterDimension2(at[0], at[1], at[2]);
            else if (kind == "radius") dd = (IDisplayDimension)doc.AddRadialDimension2(at[0], at[1], at[2]);
            if (dd == null)
            {
                // a refused explicit kind drops the selection: select the entities again for the general call
                doc.ClearSelection2(true);
                bool again = false;
                foreach (var x in dm.Arr("entities"))
                {
                    var o = Resolve(m, x);
                    if (o == null || !SelectSketchObject(o, again)) break;
                    again = true;
                }
                dd = (IDisplayDimension)doc.AddDimension2(at[0], at[1], at[2]);
            }
            doc.ClearSelection2(true);
            if (dd == null) return "dimension " + dm.Str("name") + " could not be added";
            var dim = (IDimension)dd.GetDimension2(0);
            if (!string.IsNullOrEmpty(dm.Str("name")) && dim.Name != dm.Str("name")) dim.Name = dm.Str("name");
            // a reference dimension - but never one an equation drives (files exported before 0.1.1 marked those
            // as driven: SolidWorks reports an equation's target as driven)
            if (!dm.Bool("driving", true) && !EquationTarget(dm.Str("fullName"))) { dim.DrivenState = 1; return null; }   // swDimensionDriven
            // Geometry wins: every entity already sits where the source solved it, so the new dimension measures the
            // recorded value. If it does not, SolidWorks measured something else (the other side of an angle, a radius
            // for a diameter): setting the value would move the geometry, so it is left as measured
            bool angle = dm.Str("kind") == "angle";
            double want = angle ? U.Rad(dm.Num("value")) : U.M(dm.Num("value"));
            double have = dim.SystemValue;
            if (Math.Abs(have - want) <= Math.Max(1e-9, 1e-7 * Math.Abs(want)))
            {
                int st = dim.SetSystemValue3(want, 2, null);   // swSetValue_InAllConfigurations (removes rounding)
                return st == 0 ? null : "dimension " + dm.Str("name") + ": value not accepted (status " + st + ")";
            }
            if (Math.Abs(2 * have - want) <= 1e-9 || Math.Abs(have - 2 * want) <= 1e-9)
                return "dimension " + dm.Str("name") + " made as a " + (kind == "diameter" ? "radius" : "diameter") + " (same geometry)";
            return "dimension " + dm.Str("name") + " measures " + (angle ? U.Deg(have).ToString("G6") + " deg" : U.Mm(have).ToString("G6") + " mm") +
                   " where the source had " + dm.Num("value").ToString("G6") + ": left as measured, geometry kept";
        }
    }
}
