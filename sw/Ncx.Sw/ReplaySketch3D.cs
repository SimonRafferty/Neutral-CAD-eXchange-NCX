// Replayer: a 3D sketch (FORMAT-SPEC 4, "sketch3d"): entities at their model positions (seeds, as for 2D sketches),
// then relations and dimensions. An arc is made through three points (its start, a point half-way round about its
// normal, its end), which fixes its plane whatever plane the 3D sketch has active.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        string ReplaySketch3D(JObj nf, JObj def)
        {
            var sm = (ISketchManager)doc.SketchManager;
            doc.ClearSelection2(true);
            sm.Insert3DSketch(true);
            var sk = sm.ActiveSketch;
            if (sk == null) return "could not open a 3D sketch";
            var notes = new List<string>();
            var map = new SketchMap();
            Func<object, double[]> M = q => ((List<object>)q).Select(Convert.ToDouble).Select(U.M).ToArray();   // mm -> m
            dynamic dsm = sm;
            bool addToDb = dsm.AddToDB, display = dsm.DisplayWhenAdded;
            dsm.AddToDB = true;   // no inference: no automatic relations, no snapping
            dsm.DisplayWhenAdded = false;
            try
            {
                var ents = def.Arr("entities").Cast<JObj>().ToList();
                var pointPos = ents.Where(e => e.Str("type") == "point").ToDictionary(e => e.Str("id"), e => M(e["p"]));
                foreach (var kv in pointPos) map.Target[kv.Key] = kv.Value;
                var owned = new HashSet<string>();
                Action<string> own = pid => { if (pid != null) owned.Add(pid); };
                foreach (var e in ents)
                {
                    ISketchSegment seg = null;
                    switch (e.Str("type"))
                    {
                        case "line":
                        {
                            var a = M(e["start"]);
                            var b = M(e["end"]);
                            seg = sm.CreateLine(a[0], a[1], a[2], b[0], b[1], b[2]);
                            own(e.Str("startPoint"));
                            own(e.Str("endPoint"));
                            break;
                        }
                        case "arc":
                        {
                            var c = M(e["center"]);
                            var a = M(e["start"]);
                            var b = M(e["end"]);
                            var n = V.Unit(e.Vec("normal") ?? new[] { 0.0, 0, 1 });
                            var mid = ArcMidPoint(c, a, b, n, e.Bool("ccw"));
                            seg = sm.Create3PointArc(a[0], a[1], a[2], b[0], b[1], b[2], mid[0], mid[1], mid[2]);
                            own(e.Str("startPoint"));
                            own(e.Str("endPoint"));
                            own(e.Str("centerPoint"));
                            break;
                        }
                        case "spline":
                        {
                            var data = e.Arr("points").SelectMany(q => M(q)).ToArray();
                            seg = (ISketchSegment)sm.CreateSpline2(data, true);
                            foreach (var pid in e.Arr("pointIds")) own(pid as string);
                            break;
                        }
                        case "point":
                            continue;
                        default:
                            notes.Add("3D sketch entity " + e.Str("id") + " (" + e.Str("type") + ") not rebuilt");
                            continue;
                    }
                    if (seg == null) { notes.Add("could not create " + e.Str("id")); continue; }
                    if (e.Bool("construction")) seg.ConstructionGeometry = true;
                    map.Segs[e.Str("id")] = seg;
                }
                foreach (var kv in pointPos.Where(kv => !owned.Contains(kv.Key)))
                {
                    var p = sm.CreatePoint(kv.Value[0], kv.Value[1], kv.Value[2]);
                    if (p != null) map.Points[kv.Key] = p;
                }
                // every point by its position (segments made separately at a shared corner are joined by SolidWorks,
                // or kept apart: then a coincident relation joins them, as in 2D)
                var all = ((object[])sk.GetSketchPoints2() ?? new object[0]).Cast<ISketchPoint>().ToList();
                foreach (var kv in map.Target)
                {
                    var at = all.Where(p => Math.Abs(p.X - kv.Value[0]) < 1e-9 && Math.Abs(p.Y - kv.Value[1]) < 1e-9 && Math.Abs(p.Z - kv.Value[2]) < 1e-9).ToList();
                    if (at.Count == 0) continue;
                    map.Points[kv.Key] = at[0];
                    for (int i = 1; i < at.Count; i++) AddConstraintBySelection(new object[] { at[0], at[i] }, "coincident");
                }
                var rm = (ISketchRelationManager)sk.RelationManager;
                foreach (JObj c in def.Arr("constraints"))
                {
                    if (c.Bool("supported", true) == false || !ConstraintIds.ContainsKey(c.Str("type") ?? ""))
                    {
                        notes.Add("relation " + c.Str("type") + " not rebuilt");
                        continue;
                    }
                    var objs = c.Arr("entities").Select(x => Resolve(map, x)).ToArray();
                    if (objs.Any(o => o == null)) { notes.Add("relation " + c.Str("type") + ": entity missing"); continue; }
                    int before = rm.GetRelationsCount(0);
                    if (!AddConstraintBySelection(objs, c.Str("type")) || rm.GetRelationsCount(0) <= before)
                        notes.Add("relation " + c.Str("type") + " rejected");
                }
                foreach (JObj dm in def.Arr("dimensions"))
                {
                    doc.ClearSelection2(true);
                    bool append = false, ok = true;
                    foreach (var x in dm.Arr("entities"))
                    {
                        var o = Resolve(map, x);
                        if (o == null || !SelectSketchObject(o, append)) { ok = false; break; }
                        append = true;
                    }
                    var tp = dm.Vec("textPosition") ?? new double[3];
                    var dd = ok ? (IDisplayDimension)doc.AddDimension2(U.M(tp[0]), U.M(tp[1]), U.M(tp.Length > 2 ? tp[2] : 0)) : null;
                    doc.ClearSelection2(true);
                    if (dd == null) { notes.Add("dimension " + dm.Str("name") + " could not be added"); continue; }
                    var dim = (IDimension)dd.GetDimension2(0);
                    if (!dm.Bool("driving", true) && !EquationTarget(dm.Str("fullName"))) dim.DrivenState = 1;
                    else
                    {
                        double v = dm.Str("kind") == "angle" ? U.Rad(dm.Num("value")) : U.M(dm.Num("value"));
                        if (dim.SetSystemValue3(v, 2, null) != 0) notes.Add("dimension " + dm.Str("name") + ": value not accepted");
                    }
                    if (!string.IsNullOrEmpty(dm.Str("name")) && dim.Name != dm.Str("name")) dim.Name = dm.Str("name");
                }
            }
            finally
            {
                dsm.AddToDB = addToDb;
                dsm.DisplayWhenAdded = display;
                sm.Insert3DSketch(true);
            }
            var feat = (IFeature)sk;
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            sketchMaps[nf.Str("id")] = map;
            if (notes.Count > 0) nf["note"] = (nf.Str("note") == null ? "" : nf.Str("note") + "; ") + string.Join("; ", notes);
            if (notes.Count > 0 && nf.Str("status") == "mapped") nf["status"] = "approximate";
            return null;
        }

        /// <summary>The point half-way round an arc from a to b about centre c and unit normal n (counter-clockwise
        /// about n when ccw).</summary>
        static double[] ArcMidPoint(double[] c, double[] a, double[] b, double[] n, bool ccw)
        {
            var u = V.Sub(a, c);
            var w = V.Sub(b, c);
            double sweep = Math.Atan2(V.Dot(n, V.Cross(u, w)), V.Dot(u, w));
            if (ccw && sweep <= 0) sweep += 2 * Math.PI;
            if (!ccw && sweep >= 0) sweep -= 2 * Math.PI;
            // Rodrigues: u turned by sweep/2 about n
            double t = sweep / 2, cos = Math.Cos(t), sin = Math.Sin(t);
            var k = V.Cross(n, u);
            double kd = V.Dot(n, u);
            return new[]
            {
                c[0] + u[0] * cos + k[0] * sin + n[0] * kd * (1 - cos),
                c[1] + u[1] * cos + k[1] * sin + n[1] * kd * (1 - cos),
                c[2] + u[2] * cos + k[2] * sin + n[2] * kd * (1 - cos)
            };
        }
    }
}
