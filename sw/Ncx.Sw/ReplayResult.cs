// Replayer: a feature's carried result (FORMAT-SPEC 4, "Feature results"). When a feature cannot be rebuilt - opaque, or its replay
// failed - and it carries its result, the bodies it changed are found by fingerprint and replaced by the bodies in
// the result's STEP data. The rest of the history then rebuilds on that geometry; only this step is fixed.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        bool resultsApplied;   // in the part being rebuilt (its geometry went through STEP: exchange tolerance)
        readonly HashSet<string> fixedGeometry = new HashSet<string>();   // documents holding a carried result

        /// <summary>Put the feature's result in place of the bodies it changed. Returns null or why not.</summary>
        string ApplyResult(JObj nf)
        {
            var res = nf.Obj("result");
            if (res == null) return "it carries no result";
            if (res.Str("format") != "step") return "result format " + res.Str("format") + " not supported";

            // the bodies the feature replaced: the nearest current body to each recorded fingerprint
            var current = ((object[])part.GetBodies2(-1, false) ?? new object[0]).Cast<IBody2>().ToList();
            var currentFp = current.Select(Host.BodyFingerprint).ToList();
            var doomed = new List<IBody2>();
            foreach (JObj fp in res.Arr("replaces"))
            {
                int best = -1;
                double bestScore = double.MaxValue;
                for (int i = 0; i < current.Count; i++)
                {
                    if (doomed.Contains(current[i])) continue;
                    double s = BodyDistance(fp, currentFp[i]);
                    if (s < bestScore) { bestScore = s; best = i; }
                }
                if (best < 0 || bestScore > 1e-3)
                    return "a body it changed is not in the rebuilt part (an earlier step differs)";
                doomed.Add(current[best]);
            }

            IModelDoc2 imp = null;
            string tmp = null;
            try
            {
                var made = new List<IBody2>();
                if (!string.IsNullOrEmpty(res.Str("data")))
                {
                    tmp = Path.Combine(Path.GetTempPath(), "ncx_result_" + Guid.NewGuid().ToString("N") + ".step");
                    File.WriteAllText(tmp, res.Str("data"));
                    imp = ImportStepQuietly(tmp);
                    if (imp == null) return "its result's STEP data could not be imported";
                    made = ((object[])((IPartDoc)imp).GetBodies2(-1, false) ?? new object[0]).Cast<IBody2>().ToList();
                    if (made.Count != res.Arr("bodies").Count)
                    {
                        // a STEP reader may return a surface body in pieces (SolidWorks 2026 splits a knitted surface):
                        // the same geometry if the areas add up
                        double want = res.Arr("bodies").Cast<JObj>().Sum(b => b.Obj("fingerprint")?.Num("area") ?? 0);
                        double got = made.Sum(b => Host.BodyFingerprint(b).Num("area"));
                        if (System.Math.Abs(want - got) > 1e-4 * System.Math.Max(1, want))
                            return "its result's STEP data holds " + made.Count + " bodies, not " + res.Arr("bodies").Count;
                    }
                }
                if (doomed.Count > 0)
                {
                    doc.ClearSelection2(true);
                    bool sel = false;
                    foreach (var b in doomed) sel |= b.Select2(sel, null);
                    int n0 = FeatureCount();
                    FM.InsertDeleteBody2(false);   // delete the selected bodies
                    doc.ClearSelection2(true);
                    if (FeatureCount() == n0) return "the bodies it changed could not be removed";
                    Name(LastFeature(), nf.Str("name") + " (replaced bodies)");
                }
                IFeature last = null;
                foreach (var b in made)
                {
                    last = Host.AddBody(doc, b);
                    if (last == null) return "a body of its result could not be added";
                }
                if (last != null)
                {
                    Name(last, nf.Str("name"));
                    features[nf.Str("id")] = last;
                }
                NameResultBodies(res);
                resultsApplied = true;
                return null;
            }
            finally
            {
                if (imp != null) sw.CloseDoc(imp.GetTitle());
                if (tmp != null && File.Exists(tmp)) File.Delete(tmp);
            }
        }

        /// <summary>Give the result's bodies their source names (each found by fingerprint).</summary>
        void NameResultBodies(JObj res)
        {
            var bodies = ((object[])part.GetBodies2(-1, false) ?? new object[0]).Cast<IBody2>().ToList();
            var fps = bodies.Select(Host.BodyFingerprint).ToList();
            foreach (JObj rb in res.Arr("bodies"))
            {
                int best = -1;
                double bestScore = 1e-3;
                for (int i = 0; i < bodies.Count; i++)
                {
                    double s = BodyDistance(rb.Obj("fingerprint"), fps[i]);
                    if (s < bestScore) { bestScore = s; best = i; }
                }
                if (best >= 0 && !string.IsNullOrEmpty(rb.Str("name")) && bodies[best].Name != rb.Str("name"))
                    try { bodies[best].Name = rb.Str("name"); } catch (System.Runtime.InteropServices.COMException) { }
            }
        }

        /// <summary>How far apart two body fingerprints are: relative volume and area, and centroid distance per
        /// 100 mm of body size (0 for the same body).</summary>
        static double BodyDistance(JObj a, JObj b)
        {
            if (a == null || b == null) return double.MaxValue;
            Func<double, double, double> rel = (x, y) => Math.Abs(x - y) / Math.Max(1e-9, Math.Max(Math.Abs(x), Math.Abs(y)));
            var ca = a.Vec("centroid");
            var cb = b.Vec("centroid");
            var box = b.Vec("bbox");
            double size = box == null ? 100 : Math.Max(1, Math.Sqrt(Enumerable.Range(0, 3).Sum(k => Math.Pow(box[k + 3] - box[k], 2))));
            double dc = Math.Sqrt(Enumerable.Range(0, 3).Sum(k => Math.Pow(ca[k] - cb[k], 2))) / size;
            double dv = a.Num("volume") == 0 && b.Num("volume") == 0 ? 0 : rel(a.Num("volume"), b.Num("volume"));
            return Math.Max(dv, Math.Max(rel(a.Num("area"), b.Num("area")), dc));
        }

        /// <summary>A STEP file as real bodies (not a 3D Interconnect link), with the user's setting restored.</summary>
        IModelDoc2 ImportStepQuietly(string path)
        {
            bool ic = sw.GetUserPreferenceToggle(T_INTERCONNECT);
            sw.SetUserPreferenceToggle(T_INTERCONNECT, false);
            try
            {
                int err = 0;
                object data = sw.GetImportFileData(path);
                return (IModelDoc2)sw.LoadFile4(path, "r", data, ref err);
            }
            finally { sw.SetUserPreferenceToggle(T_INTERCONNECT, ic); }
        }
    }
}
