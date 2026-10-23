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
            public bool Mirrored;   // the replayed sketch's normal is opposite the source's: directions flip
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
                string why = SelectRef(plane, false, 0);
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
            var sm2 = new SketchMap { Mirrored = mirrored };
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

                foreach (JObj c in def.Arr("constraints"))
                {
                    step = "relation " + c.Str("type");
                    int t;
                    if (c.Bool("supported", true) == false || !RelationTypes.TryGetValue(c.Str("type"), out t))
                    {
                        notes.Add("relation " + c.Str("type") + " not rebuilt");
                        continue;
                    }
                    var objs = c.Arr("entities").Select(x => Resolve(sm2, x)).ToArray();
                    if (objs.Any(o => o == null)) { notes.Add("relation " + c.Str("type") + ": entity missing"); continue; }
                    // AddRelation faults (RPC_E_SERVERFAULT) when given entity arrays from C# out of process and
                    // when given the origin's point; selecting the entities and adding the constraint, as a user
                    // would, works everywhere
                    int before = rm.GetRelationsCount(0);
                    if (!AddConstraintBySelection(objs, c.Str("type")) || rm.GetRelationsCount(0) <= before)
                        notes.Add("relation " + c.Str("type") + " rejected");
                }

                foreach (JObj dm in def.Arr("dimensions"))
                {
                    step = "dimension " + dm.Str("name");
                    string w = AddDimension(sm2, dm, src, tgt);
                    if (w != null) notes.Add(w);
                }
            }
            finally
            {
                if (!step.StartsWith("close")) step = "close (after " + step + ")";
                dsm.AddToDB = addToDb;
                dsm.DisplayWhenAdded = display;
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
            doc.SketchAddConstraints(id);
            doc.ClearSelection2(true);
            return true;
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
            foreach (var kv in m.Target)
            {
                var hit = pts.FirstOrDefault(p => Math.Abs(p.X - kv.Value[0]) < 1e-9 && Math.Abs(p.Y - kv.Value[1]) < 1e-9);
                if (hit != null) m.Points[kv.Key] = hit;
            }
        }

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
            if (dd == null) dd = (IDisplayDimension)doc.AddDimension2(at[0], at[1], at[2]);
            doc.ClearSelection2(true);
            if (dd == null) return "dimension " + dm.Str("name") + " could not be added";
            var dim = (IDimension)dd.GetDimension2(0);
            // a reference dimension - but never one an equation drives (files exported before 0.1.1 marked those
            // as driven: SolidWorks reports an equation's target as driven)
            if (!dm.Bool("driving", true) && !EquationTarget(dm.Str("fullName"))) dim.DrivenState = 1;   // swDimensionDriven
            else
            {
                double v = dm.Str("kind") == "angle" ? U.Rad(dm.Num("value")) : U.M(dm.Num("value"));
                int st = dim.SetSystemValue3(v, 2, null);   // swSetValue_InAllConfigurations
                if (st != 0) return "dimension " + dm.Str("name") + ": value not accepted (status " + st + ")";
            }
            if (!string.IsNullOrEmpty(dm.Str("name")) && dim.Name != dm.Str("name")) dim.Name = dm.Str("name");
            return null;
        }
    }
}
