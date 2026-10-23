// Exporter: parameter-flex probes (FORMAT-SPEC 6.4). A few driving parameters per document - sketch
// dimensions and global variables - are changed in the source, every part of the model is rebuilt in
// dependency order and measured, and the value is put back. A probe records the fingerprint of every part
// it changed, so a layout's dimension is tested through the derived parts that use it. Any importer can
// apply the same change to its rebuild and check the parameters still drive the model the same way.
//
// Automatic, only where a structural check cannot prove behaviour (FORMAT-SPEC 6.4): a document whose global
// variables drive it through equations, and a document other parts are derived from (a layout). Plain parts
// get none - the fully-defined check covers them - so most exports never touch the model. Only dimensions of
// fully defined sketches are probed: how an under-defined sketch moves when edited is up to each CAD system's
// solver, not a translation fault. The model is changed temporarily; afterwards every part is checked to be
// back exactly as it was, and feature names SolidWorks reset on rebuild are restored.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        /// <summary>Probes per document: -1 = automatic (only where needed, at most AutoProbes), 0 = none,
        /// N = N on every part (the harness).</summary>
        public int FlexProbes = -1;
        const int AutoProbes = 2;

        const double FlexFraction = 0.05;          // the change: 5% of the value...
        const double FlexMinLength = 0.5e-3;        // ...but at least 0.5 mm
        const double FlexMinAngle = Math.PI / 180;  // ...or 1 degree

        readonly Dictionary<string, IModelDoc2> docsById = new Dictionary<string, IModelDoc2>();

        /// <summary>One flexible parameter of a document: how to read and set it, in system units.</summary>
        internal class FlexParam
        {
            public string Name, Kind;   // kind: length | angle | variable
            public Func<double> Get;
            public Action<double> Set;
        }

        /// <summary>Add flex probes to every document of the model (after all documents are exported).</summary>
        void ExportFlex()
        {
            if (FlexProbes == 0) return;
            var want = documents.Cast<JObj>().ToDictionary(d => d.Str("id"), ProbesWanted);
            if (want.Values.All(n => n == 0)) return;   // nothing needs probing: the model is never touched
            var parts = PartDocs();
            var baseline = Flex.Measure(parts);
            var names = parts.ToDictionary(p => p.Key, p => Flex.TreeNames(p.Value));
            try
            {
                foreach (JObj d in documents)
                {
                    IModelDoc2 md;
                    if (want[d.Str("id")] == 0 || !docsById.TryGetValue(d.Str("id"), out md) || md.GetType() != 1) continue;
                    var skipped = new List<string>();
                    var probes = Probes(md, d, parts, baseline, want[d.Str("id")], skipped);
                    if (probes.Count > 0) d["flex"] = probes;
                    // never silently: a document that needed probing and got none is reported with the reasons
                    if (probes.Count == 0 && FlexProbes < 0)
                        Report.Add(d.Str("id"), "parameter flex", "note", "no probe recorded" +
                                   (skipped.Count == 0 ? ": no flexible parameter (dimensions of fully defined sketches, plain global variables)"
                                                       : ": " + string.Join("; ", skipped)));
                }
            }
            finally
            {
                foreach (var p in parts) Flex.RestoreNames(p.Value, names[p.Key]);
                var after = Flex.Measure(parts);
                foreach (var p in parts)
                    if (Flex.Compare(baseline[p.Key], after[p.Key]) != "match")
                        Report.Add(p.Key, "parameter flex", "failed", "the part did not return exactly to its own values after the flex probes");
            }
        }

        List<KeyValuePair<string, IModelDoc2>> PartDocs()
        {
            // documents are in dependency order: a layout before the parts that insert it
            return documents.Cast<JObj>().Where(d => d.Str("kind") == "part" && docsById.ContainsKey(d.Str("id")))
                            .Select(d => new KeyValuePair<string, IModelDoc2>(d.Str("id"), docsById[d.Str("id")])).ToList();
        }

        /// <summary>How many probes a document needs. Automatic: a part driven by equations or with parts derived
        /// from it (its behaviour is not proven by its own structure); otherwise none.</summary>
        int ProbesWanted(JObj d)
        {
            if (d.Str("kind") != "part") return 0;
            if (FlexProbes > 0) return FlexProbes;
            bool drivesOthers = documents.Cast<JObj>().Any(o => o.Arr("features").Cast<JObj>()
                .Any(f => f.Str("type") == "insertPart" && f.Obj("def")?.Str("source") == d.Str("id")));
            return drivesOthers || d.Arr("equations").Count > 0 ? AutoProbes : 0;
        }

        List<object> Probes(IModelDoc2 md, JObj d, List<KeyValuePair<string, IModelDoc2>> parts, Dictionary<string, JObj> baseline, int want, List<string> skipped)
        {
            var probes = new List<object>();
            var cands = Flex.Parameters(sw, md, d);
            if (cands.Count == 0) return probes;
            // automatic: global variables first (what drives an equation-based part or a layout)
            if (FlexProbes < 0) cands = cands.OrderBy(c => c.Kind == "variable" ? 0 : 1).ToList();
            string cfg = ((IConfiguration)md.GetActiveConfiguration()).Name;
            int wrong0 = Flex.WhatsWrong(parts);
            // spread the probes over the document rather than flexing the first sketch only
            var order = Enumerable.Range(0, want).Select(k => k * cands.Count / want)
                                  .Concat(Enumerable.Range(0, cands.Count)).Distinct();
            int tried = 0;
            foreach (int i in order)
            {
                if (probes.Count >= want || tried >= want * 4) break;
                var p = cands[i];
                tried++;
                double orig = p.Get();
                foreach (double sign in new[] { 1.0, -1.0 })   // grow, else shrink
                {
                    double min = p.Kind == "angle" ? FlexMinAngle : p.Kind == "length" ? FlexMinLength : 0;
                    double step = Math.Max(Math.Abs(orig) * FlexFraction, min);
                    if (step == 0) step = 1;   // a variable at 0
                    double value = orig + sign * step;
                    if (value <= 0 && p.Kind != "variable") continue;
                    p.Set(value);
                    Flex.Rebuild(parts);
                    bool rebuilt = Flex.WhatsWrong(parts) <= wrong0;
                    var fps = rebuilt ? Flex.Measure(parts) : null;
                    p.Set(orig);
                    Flex.Rebuild(parts);
                    if (!rebuilt) { skipped.Add(p.Name + " " + (sign > 0 ? "+" : "-") + "5% breaks the rebuild"); continue; }
                    // the parts this change moved; none (e.g. a construction sketch) proves nothing
                    var changed = new JObj();
                    foreach (var kv in fps)
                        if (Flex.Compare(baseline[kv.Key], kv.Value) != "match") changed[kv.Key] = kv.Value;
                    if (!changed.Any()) { skipped.Add(p.Name + " moved no part"); break; }
                    probes.Add(new JObj
                    {
                        { "parameter", p.Name }, { "kind", p.Kind },
                        { "original", Flex.ToUser(p.Kind, orig) }, { "value", Flex.ToUser(p.Kind, value) },
                        { "configuration", cfg }, { "fingerprints", changed }
                    });
                    break;
                }
            }
            return probes;
        }
    }

    /// <summary>Parameter flex, shared by the exporter (recording probes) and the replayer (checking them).</summary>
    public static class Flex
    {
        /// <summary>The flexible parameters of a part: driving sketch dimensions not driven by an equation,
        /// and global variables whose value is a plain number.</summary>
        internal static List<Exporter.FlexParam> Parameters(ISldWorks sw, IModelDoc2 md, JObj d)
        {
            var res = new List<Exporter.FlexParam>();
            var driven = new HashSet<string>(d.Arr("equations").Cast<JObj>().Select(e => e.Str("target")), StringComparer.OrdinalIgnoreCase);
            foreach (JObj nf in d.Arr("features"))
            {
                // only fully defined sketches: an under-defined one moves as each solver chooses
                if (nf.Str("type") != "sketch" || nf.Str("status") == "opaque" || nf.Obj("def")?.Str("constrained") != "full") continue;
                foreach (JObj dm in (nf.Obj("def")?.Arr("dimensions") ?? new List<object>()))
                {
                    string name = dm.Str("name") + "@" + nf.Str("name");
                    if (!dm.Bool("driving", true) || driven.Any(t => t.Equals(name, StringComparison.OrdinalIgnoreCase) ||
                                                                    t.StartsWith(name + "@", StringComparison.OrdinalIgnoreCase)))
                        continue;
                    var p = Dimension(md, name, dm.Str("kind") == "angle" ? "angle" : "length");
                    if (p != null) res.Add(p);
                }
            }
            foreach (JObj v in d.Arr("parameters"))
            {
                double x;
                if (!double.TryParse(v.Str("expression") ?? "", NumberStyles.Float, CultureInfo.InvariantCulture, out x)) continue;
                var p = Variable(sw, md, v.Str("name"));
                if (p != null) res.Add(p);
            }
            return res;
        }

        internal static Exporter.FlexParam Dimension(IModelDoc2 md, string name, string kind)
        {
            var dim = md.Parameter(name) as IDimension;
            if (dim == null) return null;
            return new Exporter.FlexParam
            {
                Name = name, Kind = kind,
                Get = () => ((double[])dim.GetSystemValue3(1, null))[0],   // swThisConfiguration
                Set = x => dim.SetSystemValue3(x, 1, null)
            };
        }

        /// <summary>A global variable, set through its equation ("name" = value). SolidWorks refuses an equation
        /// edit (silently: Status -1) unless the document is the active one, so it is activated for the edit.</summary>
        internal static Exporter.FlexParam Variable(ISldWorks sw, IModelDoc2 md, string name)
        {
            var em = (IEquationMgr)md.GetEquationMgr();
            int index = -1;
            for (int i = 0; i < em.GetCount(); i++)
                if (em.get_GlobalVariable(i) && em.get_Equation(i).Split('=')[0].Trim().Trim('"') == name) index = i;
            if (index < 0) return null;
            return new Exporter.FlexParam
            {
                Name = name, Kind = "variable",
                Get = () => em.get_Value(index),
                Set = x =>
                {
                    var previous = sw.ActiveDoc as IModelDoc2;
                    int err = 0;
                    if (previous == null || previous.GetTitle() != md.GetTitle()) sw.ActivateDoc3(md.GetTitle(), false, 0, ref err);
                    try
                    {
                        em.set_Equation(index, "\"" + name + "\" = " + x.ToString("R", CultureInfo.InvariantCulture));
                        if (em.Status != 0) throw new InvalidOperationException("SolidWorks refused to change global variable " + name);
                        em.EvaluateAll();
                    }
                    finally
                    {
                        if (previous != null && previous.GetTitle() != md.GetTitle()) sw.ActivateDoc3(previous.GetTitle(), false, 0, ref err);
                    }
                }
            };
        }

        public static double ToUser(string kind, double sys) { return kind == "angle" ? U.Deg(sys) : kind == "length" ? U.Mm(sys) : sys; }
        public static double ToSystem(string kind, double user) { return kind == "angle" ? U.Rad(user) : kind == "length" ? U.M(user) : user; }

        /// <summary>Rebuild every part in dependency order (derived parts pick up their source's change).</summary>
        public static void Rebuild(IEnumerable<KeyValuePair<string, IModelDoc2>> parts)
        {
            foreach (var p in parts) p.Value.ForceRebuild3(false);
        }

        public static int WhatsWrong(IEnumerable<KeyValuePair<string, IModelDoc2>> parts)
        {
            return parts.Sum(p => p.Value.Extension.GetWhatsWrongCount());
        }

        public static Dictionary<string, JObj> Measure(IEnumerable<KeyValuePair<string, IModelDoc2>> parts)
        {
            return parts.ToDictionary(p => p.Key, p => Host.PartFingerprint((IPartDoc)p.Value));
        }

        public static string Compare(JObj expected, JObj actual)
        {
            var rep = (JObj)Json.Parse(NcxCore.FingerprintCompare(Json.Write(expected, true), Json.Write(actual, true)).ReportJson);
            return rep.Str("result");
        }

        /// <summary>Top-level feature (type, name) in tree order.</summary>
        public static List<KeyValuePair<string, string>> TreeNames(IModelDoc2 md)
        {
            var res = new List<KeyValuePair<string, string>>();
            for (var f = (IFeature)md.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                res.Add(new KeyValuePair<string, string>(f.GetTypeName2(), f.Name));
            return res;
        }

        /// <summary>SolidWorks replaces some feature objects on a rebuild (weldment structural members) and
        /// the new one has its default name: put the names back where the tree is otherwise unchanged.</summary>
        public static void RestoreNames(IModelDoc2 md, List<KeyValuePair<string, string>> before)
        {
            var now = new List<IFeature>();
            for (var f = (IFeature)md.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature()) now.Add(f);
            if (now.Count != before.Count) return;
            for (int i = 0; i < now.Count; i++)
                if (now[i].GetTypeName2() == before[i].Key && now[i].Name != before[i].Value)
                    try { now[i].Name = before[i].Value; } catch (System.Runtime.InteropServices.COMException) { }
        }
    }
}
