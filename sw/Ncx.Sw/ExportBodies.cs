// Exporter: multibody features. A body is referenced by geometry, as measured just before the feature
// that uses it (volume, centroid, box): replay rebuilds the same bodies in the same place, so the match
// is unambiguous unless two bodies coincide.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        static JObj BodyRef(object o)
        {
            var b = o as IBody2;
            if (b == null) return null;
            var fp = Host.BodyFingerprint(b);
            return new JObj
            {
                { "kind", "body" }, { "by", "geom" },
                { "geom", new JObj { { "volume", fp["volume"] }, { "centroid", fp["centroid"] }, { "bbox", fp["bbox"] } } }
            };
        }

        void ExportCombine(IFeature f, JObj nf)
        {
            var d = (ICombineBodiesFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);
            try
            {
                nf["type"] = "combine";
                int op = d.OperationType;   // swCombineBodiesOperationType_e
                var opName = op == 0 ? "add" : op == 1 ? "subtract" : op == 2 ? "common" : null;
                if (opName == null) { Opaque(nf, "combine operation " + op + " not mapped"); return; }
                var tools = new List<object>();
                foreach (var b in (object[])d.BodiesToCombine ?? new object[0])
                {
                    var r = BodyRef(b);
                    if (r == null) { Opaque(nf, "combined body not recognised"); return; }
                    tools.Add(r);
                }
                // the main body is separate when there is one; an add made without one lists every body as a tool
                var main = BodyRef(d.MainBody);
                if (main == null && op != 1 && tools.Count > 1) { main = (JObj)tools[0]; tools.RemoveAt(0); }
                if (main == null) { Opaque(nf, "main body not recognised"); return; }
                nf["def"] = new JObj { { "operation", opName }, { "main", main }, { "bodies", tools } };
            }
            finally { d.ReleaseSelectionAccess(); }
        }
    }
}
