// Replayer: multibody features. Bodies are found by the geometry recorded at export (volume, centroid).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        /// <summary>The solid body matching a body reference: volume within 1e-5 relative and centroid within
        /// 0.01 mm. Null if none or more than one match.</summary>
        IBody2 FindBody(JObj r, ICollection<IBody2> taken = null)
        {
            var g = r?.Obj("geom");
            if (g == null) return null;
            double v = g.Num("volume");
            var c = g.Vec("centroid");
            var all = ((object[])part.GetBodies2(-1, true) ?? new object[0]).Cast<IBody2>()   // solids and surfaces
                .Select(b => new { b, fp = Host.BodyFingerprint(b) }).ToList();
            Func<double, double, IList<IBody2>> within = (rel, mm) => all.Where(x =>
                Math.Abs(x.fp.Num("volume") - v) <= rel * Math.Max(Math.Abs(v), 1e-9) && V.Norm(V.Sub(x.fp.Vec("centroid"), c)) <= mm).Select(x => x.b).ToList();
            // bodies another reference of the same feature already took are not candidates (two identical bodies in
            // one place, such as a cope's tool and the member it copes: either is the right one, but not both)
            if (taken != null) all = all.Where(x => !taken.Contains(x.b)).ToList();
            var hits = within(1e-5, 0.01);
            if (hits.Count == 1) return hits[0];
            if (hits.Count > 1)
            {
                // several bodies the same to the descriptor: indistinguishable, any of them is it
                var f0 = Host.BodyFingerprint(hits[0]);
                bool same = hits.All(h => { var f = Host.BodyFingerprint(h); return Math.Abs(f.Num("area") - f0.Num("area")) <= 1e-6 * Math.Max(1, f0.Num("area")); });
                return same && taken != null ? hits[0] : null;
            }
            if (!Foreign) return null;
            // another kernel's measurement of the same body (a drawn section a little off SolidWorks' own: P46's mitred
            // tubes, 1.5e-5 in volume): the nearest body within exchange bounds, when it is the only one there
            var loose = within(2e-3, 0.1);
            return loose.Count == 1 ? loose[0] : null;
        }

        string ReplayCombine(JObj nf, JObj def)
        {
            var bodies = new List<IBody2>();
            var main = FindBody(def.Obj("main"), bodies);
            if (main == null) return "main body not found";
            var taken = new List<IBody2> { main };
            foreach (JObj r in def.Arr("bodies"))
            {
                var b = FindBody(r, taken);
                if (b == null) return "combined body not found";
                bodies.Add(b);
                taken.Add(b);
            }
            string opName = def.Str("operation");
            int op = opName == "subtract" ? 15902 : opName == "common" ? 15901 : 15903;   // swBodyOperationType_e
            if (bodies.Count == 0) return "no bodies to combine";
            // add/common take every body as a tool and no main body (given a main body, add makes a feature in
            // error and combines nothing - measured); subtract takes the main body plus the tools
            Body2 mainArg = null;
            if (opName == "subtract") mainArg = (Body2)main;
            else bodies.Insert(0, main);
            doc.ClearSelection2(true);
            int n0 = FeatureCount();
            var feat = FM.InsertCombineFeature(op, mainArg, bodies.Select(b => new DispatchWrapper(b)).ToArray());
            return Created(n0, feat as IFeature, nf, "combine");
        }
    }
}
