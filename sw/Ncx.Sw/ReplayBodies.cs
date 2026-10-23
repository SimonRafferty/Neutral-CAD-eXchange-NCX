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
        IBody2 FindBody(JObj r)
        {
            var g = r?.Obj("geom");
            if (g == null) return null;
            double v = g.Num("volume");
            var c = g.Vec("centroid");
            var hits = ((object[])part.GetBodies2(-1, true) ?? new object[0]).Cast<IBody2>().Where(b =>   // solids and surfaces
            {
                var fp = Host.BodyFingerprint(b);
                double bv = fp.Num("volume");
                return Math.Abs(bv - v) <= 1e-5 * Math.Max(Math.Abs(v), 1e-9) && V.Norm(V.Sub(fp.Vec("centroid"), c)) <= 0.01;
            }).ToList();
            return hits.Count == 1 ? hits[0] : null;
        }

        string ReplayCombine(JObj nf, JObj def)
        {
            var bodies = new List<IBody2>();
            foreach (JObj r in def.Arr("bodies"))
            {
                var b = FindBody(r);
                if (b == null) return "combined body not found";
                bodies.Add(b);
            }
            string opName = def.Str("operation");
            int op = opName == "subtract" ? 15902 : opName == "common" ? 15901 : 15903;   // swBodyOperationType_e
            var main = FindBody(def.Obj("main"));
            if (main == null) return "main body not found";
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
