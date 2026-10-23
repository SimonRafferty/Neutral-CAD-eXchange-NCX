// Exporter: geometry measured just after a feature, so another CAD system can place its own version of the feature
// and check it without SolidWorks' conventions (FORMAT-SPEC 4: sheet-metal `bendGeometry`, loft `connectors`).
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        /// <summary>Measurements a feature's export defers until the rollback bar is just after that feature.</summary>
        readonly List<Action> afterFeature = new List<Action>();

        /// <summary>Run the feature's deferred measurements with the model rolled to just after it.</summary>
        void MeasureAfterFeature(IFeature f, IFeatureManager fm, JObj nf)
        {
            if (afterFeature.Count == 0) return;
            try
            {
                fm.EditRollback(4, f.Name);   // swMoveRollbackBarToAfterFeature
                foreach (var measure in afterFeature) measure();
            }
            catch (Exception e)
            {
                Report.Add(docId, f.Name, "note", "geometry after the feature not measured: " + e.Message);
            }
            finally { afterFeature.Clear(); }
        }

        /// <summary>The faces a feature made, its own and its sub-features' (sheet-metal bends are sub-features).</summary>
        static List<IFace2> FeatureFaces(IFeature f)
        {
            var faces = ((object[])f.GetFaces() ?? new object[0]).Cast<IFace2>().ToList();
            for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                if (sub.GetTypeName2() != "ProfileFeature")
                    faces.AddRange(((object[])sub.GetFaces() ?? new object[0]).Cast<IFace2>());
            return faces;
        }

        /// <summary>Each bend a sheet-metal feature made, from its cylindrical faces (model frame, mm): the bend's
        /// axis (a point at the middle of the bend along the axis, and the axis direction) and its inner and outer
        /// radii. Coaxial faces of one bend are taken together.</summary>
        static List<object> BendGeometry(IFeature f)
        {
            var bends = new List<Tuple<double[], double[], List<double>, List<double[]>>>();   // axis point, dir, radii, face centres
            foreach (var face in FeatureFaces(f))
            {
                var s = (ISurface)face.GetSurface();
                if (!s.IsCylinder()) continue;
                var p = (double[])s.CylinderParams;   // origin, axis, radius (m)
                var o = new[] { p[0], p[1], p[2] };
                var dir = V.Unit(new[] { p[3], p[4], p[5] });
                var box = (double[])face.GetBox();
                var centre = new[] { (box[0] + box[3]) / 2, (box[1] + box[4]) / 2, (box[2] + box[5]) / 2 };
                var same = bends.FirstOrDefault(b => Math.Abs(Math.Abs(V.Dot(b.Item2, dir)) - 1) < 1e-9 &&
                                                     V.Norm(V.Cross(V.Sub(o, b.Item1), b.Item2)) < 1e-7);
                if (same == null) bends.Add(same = Tuple.Create(o, dir, new List<double>(), new List<double[]>()));
                if (!same.Item3.Any(r => Math.Abs(r - p[6]) < 1e-9)) same.Item3.Add(p[6]);
                same.Item4.Add(centre);
            }
            return bends.Select(b =>
            {
                // the point on the axis level with the middle of the bend's faces
                var mid = new double[3];
                foreach (var c in b.Item4) for (int k = 0; k < 3; k++) mid[k] += c[k] / b.Item4.Count;
                double along = V.Dot(V.Sub(mid, b.Item1), b.Item2);
                var onAxis = Enumerable.Range(0, 3).Select(k => b.Item1[k] + along * b.Item2[k]).ToArray();
                return (object)new JObj
                {
                    { "axis", new JObj { { "point", U.Mm(onAxis) }, { "direction", b.Item2 } } },
                    { "innerRadius", U.Mm(b.Item3.Min()) }, { "outerRadius", U.Mm(b.Item3.Max()) }
                };
            }).ToList();
        }

        /// <summary>Record the bends a sheet-metal feature made, measured just after it.</summary>
        void DeferBendGeometry(IFeature f, JObj nf)
        {
            afterFeature.Add(() =>
            {
                var def = nf.Obj("def");
                if (def == null) return;
                var bends = BendGeometry(f);
                if (bends.Count > 0) def["bendGeometry"] = bends;
            });
        }

        /// <summary>A loft's connectors: each edge of the loft that runs from one section to another, as the two
        /// points it joins (model frame, mm) and the sections' indices. They say which point of each section the loft
        /// joins to which point of the next (its alignment).</summary>
        void DeferLoftConnectors(IFeature f, JObj nf, List<ISketch> sections)
        {
            var frames = sections.Select(s => s == null ? null : new SketchFrame(sw, s)).ToList();
            afterFeature.Add(() =>
            {
                var def = nf.Obj("def");
                if (def == null || frames.Any(fr => fr == null)) return;
                Func<double[], int> sectionOf = pt =>
                {
                    for (int i = 0; i < frames.Count; i++)
                        if (Math.Abs(V.Dot(V.Sub(pt, frames[i].Origin), frames[i].Z)) < 1e-7) return i;
                    return -1;
                };
                var connectors = new List<object>();
                var seen = new List<double[]>();
                foreach (var face in ((object[])f.GetFaces() ?? new object[0]).Cast<IFace2>())
                    foreach (IEdge e in (object[])face.GetEdges() ?? new object[0])
                    {
                        var v0 = (IVertex)e.GetStartVertex();
                        var v1 = (IVertex)e.GetEndVertex();
                        if (v0 == null || v1 == null) continue;
                        var a = (double[])v0.GetPoint();
                        var b = (double[])v1.GetPoint();
                        int sa = sectionOf(a), sb = sectionOf(b);
                        if (sa < 0 || sb < 0 || sa == sb) continue;
                        if (sa > sb) { var t = a; a = b; b = t; var ts = sa; sa = sb; sb = ts; }
                        var key = a.Concat(b).ToArray();
                        if (seen.Any(k => Enumerable.Range(0, 6).All(i => Math.Abs(k[i] - key[i]) < 1e-9))) continue;
                        seen.Add(key);
                        connectors.Add(new JObj { { "from", sa }, { "to", sb }, { "points", new List<object> { U.Mm(a), U.Mm(b) } } });
                    }
                if (connectors.Count > 0) def["connectors"] = connectors;
            });
        }
    }
}
