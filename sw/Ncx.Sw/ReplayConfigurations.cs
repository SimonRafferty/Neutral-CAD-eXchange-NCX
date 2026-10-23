// Replayer: configurations - create them, apply per-configuration dimension values and suppression,
// verify each against its own fingerprint, and leave the source's active configuration showing.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        void ReplayConfigurations(JObj d)
        {
            var cfgs = d.Arr("configurations").Cast<JObj>().ToList();
            if (cfgs.Count == 0) return;
            string activeId = d.Str("activeConfiguration");
            var byId = cfgs.ToDictionary(c => c.Str("id"), c => c.Str("name"));
            // the new document's only configuration becomes the source's active one; the rest are added
            // (features can create configurations themselves - a weldment adds <As Machined>/<As Welded>
            // derived ones - so only missing configurations are added)
            string activeName = byId.ContainsKey(activeId) ? byId[activeId] : cfgs[0].Str("name");
            var existing = new HashSet<string>((string[])doc.GetConfigurationNames() ?? new string[0]);
            if (!existing.Contains(activeName) && existing.Count == 1)
            {
                var first = (IConfiguration)doc.GetActiveConfiguration();
                first.Name = activeName;
                existing = new HashSet<string> { activeName };
            }
            foreach (var c in cfgs.Where(c => !existing.Contains(c.Str("name"))))
                if (doc.AddConfiguration3(c.Str("name"), "", "", 0) == null)
                    Report.Add(docId, "configuration " + c.Str("name"), "failed", "could not be created");
            doc.ShowConfiguration2(activeName);
            var names = cfgs.Select(c => c.Str("name")).ToArray();

            foreach (var c in cfgs)
                foreach (var kv in c.Obj("values") ?? new JObj())
                {
                    var dim = doc.Parameter(kv.Key) as IDimension;
                    if (dim == null) { Report.Add(docId, kv.Key, "failed", "dimension not found for configuration " + c.Str("name")); continue; }
                    bool angle = false;
                    try { angle = ((IDimension)dim).GetType() == 2; } catch { }   // swDimensionParamTypeDoubleAngular
                    double v = Convert.ToDouble(kv.Value);
                    dim.SetSystemValue3(angle ? U.Rad(v) : U.M(v), 3, new[] { c.Str("name") });   // swSetValue_InSpecificConfigurations
                }

            foreach (JObj nf in d.Arr("features"))
            {
                var s = nf.Obj("suppressed");
                IFeature f;
                if (s == null || !features.TryGetValue(nf.Str("id"), out f)) continue;
                foreach (var kv in s)
                {
                    string name;
                    if (!byId.TryGetValue(kv.Key, out name)) continue;
                    f.SetSuppression2((bool)kv.Value ? 0 : 1, 3, new[] { name });   // suppress / unsuppress, specified configuration
                }
            }

            // verify every configuration that carries its own fingerprint, the active one last
            foreach (var c in cfgs.Where(c => c.Obj("fingerprint") != null).OrderBy(c => c.Str("name") == activeName))
            {
                doc.ShowConfiguration2(c.Str("name"));
                doc.ForceRebuild3(false);
                var rep = (JObj)Json.Parse(NcxCore.FingerprintCompare(Json.Write(c.Obj("fingerprint"), true),
                                                                       Json.Write(Host.PartFingerprint(part), true)).ReportJson);
                Verification.Add(new JObj { { "document", docId + "#" + c.Str("name") }, { "result", rep.Str("result") },
                                            { "differences", rep["differences"] } });
            }
            doc.ShowConfiguration2(activeName);
        }
    }
}
