// Exporter: configurations. The features are exported in the active configuration; each configuration
// then carries the values of every dimension that differs between configurations, every feature's
// suppression state, and its own fingerprint so the replay can be verified configuration by
// configuration. A design table is carried as the configurations it produces (noted).
//
// Values are read by activating each configuration in turn: the per-configuration query
// (GetSystemValue3 with swInSpecificConfigurations) returns 0 for dimensions that are not configured
// separately, and IsSuppressed2 with several names is unreliable (both measured).
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        static string CfgId(string name) { return "cfg:" + name; }

        /// <summary>"D1@SK_Base@P10.Part" -> "D1@SK_Base" (what IModelDoc2.Parameter accepts).</summary>
        static string ParamName(string fullName)
        {
            var parts = fullName.Split('@');
            return parts.Length >= 2 ? parts[0] + "@" + parts[1] : fullName;
        }

        /// <summary>The feature, or - when a configuration change made SolidWorks replace it (weldment structural
        /// members; the old object is disconnected) - its replacement: by name, else the same-type feature at the
        /// same position among features of that type.</summary>
        IFeature Live(IModelDoc2 d, IFeature f, JObj nf)
        {
            try { var _ = f.Name; return f; }
            catch (System.Runtime.InteropServices.COMException) { }
            var byName = (d as IPartDoc)?.FeatureByName(nf.Str("name")) as IFeature;
            if (byName != null) return byName;
            string type = nf.Obj("native")?.Str("type");
            int k = exportedFeatures.Where(x => x.Value.Obj("native")?.Str("type") == type).Select(x => x.Value).ToList().IndexOf(nf);
            var same = new List<IFeature>();
            for (var g = (IFeature)d.FirstFeature(); g != null; g = (IFeature)g.GetNextFeature())
                if (g.GetTypeName2() == type) same.Add(g);
            return k >= 0 && k < same.Count ? same[k] : null;
        }

        List<object> ExportConfigurations(IModelDoc2 d, out string activeId)
        {
            var names = (string[])d.GetConfigurationNames() ?? new string[0];
            string active = ((IConfiguration)d.GetActiveConfiguration()).Name;
            activeId = CfgId(active);
            var cfgs = names.Select(n => new JObj { { "id", CfgId(n) }, { "name", n }, { "values", new JObj() } }).ToList();

            // every dimension of the exported features: parameter name -> is angle
            var dims = new List<KeyValuePair<string, bool>>();
            foreach (var kv in exportedFeatures)
                for (var dd = (IDisplayDimension)kv.Key.GetFirstDisplayDimension(); dd != null; dd = (IDisplayDimension)kv.Key.GetNextDisplayDimension(dd))
                {
                    var dim = (IDimension)dd.GetDimension2(0);
                    if (dim.FullName.Contains("@" + kv.Key.Name + "@"))   // not a plane's dimension listed under its sketch
                        dims.Add(new KeyValuePair<string, bool>(ParamName(dim.FullName), dd.Type2 == 3));
                }

            var values = new Dictionary<string, double[]>();
            var supp = new Dictionary<JObj, bool[]>();
            foreach (var kv in exportedFeatures) supp[kv.Value] = new bool[names.Length];
            foreach (var p in dims) values[p.Key] = new double[names.Length];
            // the active configuration last, so the document ends as it was
            var order = names.Select((n, i) => i).OrderBy(i => names[i] == active).ToList();
            foreach (int i in order)
            {
                if (names.Length > 1) d.ShowConfiguration2(names[i]);
                foreach (var kv in exportedFeatures)
                {
                    var s = Live(d, kv.Key, kv.Value)?.IsSuppressed2(1, null) as bool[];   // this (now active) configuration
                    supp[kv.Value][i] = s != null && s.Length > 0 && s[0];
                }
                foreach (var p in dims)
                {
                    var dim = d.Parameter(p.Key) as IDimension;
                    values[p.Key][i] = dim == null ? double.NaN : dim.SystemValue;
                }
                if (names.Length > 1) cfgs[i]["fingerprint"] = Host.PartFingerprint((IPartDoc)d);
            }

            foreach (var kv in exportedFeatures)
            {
                var s = supp[kv.Value];
                if (!s.Any(x => x)) continue;
                var m = new JObj();
                for (int i = 0; i < names.Length; i++) m[CfgId(names[i])] = s[i];
                kv.Value["suppressed"] = m;
            }
            foreach (var p in dims)
            {
                var v = values[p.Key];
                if (v.Any(double.IsNaN) || v.All(x => Math.Abs(x - v[0]) < 1e-12)) continue;   // same everywhere
                for (int i = 0; i < names.Length; i++)
                    cfgs[i].Obj("values")[p.Key] = p.Value ? U.Deg(v[i]) : U.Mm(v[i]);
            }
            // GetDesignTable returns an (empty) object on parts that have none: only a table with rows counts
            var dt = d.GetDesignTable() as IDesignTable;
            int rows = 0;
            try { if (dt != null) rows = dt.GetRowCount(); } catch { }
            if (rows > 0)
                Report.Add(docId, "Design Table", "approximate", "design table carried as the configurations it produces");
            return cfgs.Cast<object>().ToList();
        }
    }
}
