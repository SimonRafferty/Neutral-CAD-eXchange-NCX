// Replayer: custom properties and materials (FORMAT-SPEC 4.6), applied once the configurations exist, and checked
// as part of the structure verification.
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        void ReplayPropertiesAndMaterials(IModelDoc2 d, JObj nd)
        {
            var props = nd.Arr("customProperties").Cast<JObj>().ToList();
            var mats = nd.Arr("materials").Cast<JObj>().ToList();
            if (props.Count + mats.Count == 0) return;
            var problems = new List<string>();
            foreach (var p in props)
            {
                var cpm = d.Extension.get_CustomPropertyManager(p.Str("configuration") ?? "");
                int type = (int)(p.Obj("native")?.Num("type") ?? 0);
                if (type == 0) type = p.Str("type") == "number" ? 3 : p.Str("type") == "yesno" ? 11 : p.Str("type") == "date" ? 64 : 30;
                if (cpm.Add3(p.Str("name"), type, p.Str("value") ?? "", 2) < 0)   // swCustomPropertyReplaceValue
                    problems.Add("property " + p.Str("name") + " could not be set");
            }
            var part = d as IPartDoc;
            var substituted = new List<string>();
            foreach (var m in mats)
            {
                if (part == null) break;
                string cfg = m.Str("configuration"), name = m.Str("name");
                part.SetMaterialPropertyName2(cfg, m.Str("database") ?? "", name);   // a void call
                string db;
                if (part.GetMaterialPropertyName2(cfg, out db) == name) continue;
                // another system's name for it ("Stainless Steel AISI 304", "Steel"): this SolidWorks' nearest material
                var near = MaterialLibrary.Nearest(sw, name);
                if (near != null)
                {
                    part.SetMaterialPropertyName2(cfg, near.Item1, near.Item2);
                    if (part.GetMaterialPropertyName2(cfg, out db) == near.Item2)
                    {
                        materialAs[name] = near.Item2;
                        substituted.Add(name + " as SolidWorks' \"" + near.Item2 + "\"");
                        continue;
                    }
                }
                problems.Add("material " + name + " (" + m.Str("database") + ") is not in this SolidWorks' material databases");
            }
            var names = mats.Select(m => m.Str("name")).Distinct().ToList();
            if (substituted.Count > 0) problems.Add("material " + string.Join(", ", substituted.Distinct()));
            Report.Add(docId, "properties", problems.Count == 0 ? "rebuilt" : "approximate",
                       problems.Count == 0 ? props.Count + " custom properties" + (names.Count == 0 ? "" : "; material " + string.Join(", ", names))
                                           : string.Join("; ", problems));
        }

        // a material written under another system's name -> the SolidWorks material it was set as
        readonly Dictionary<string, string> materialAs = new Dictionary<string, string>(System.StringComparer.OrdinalIgnoreCase);

        /// <summary>A linked value names the document's own file ("SW-Mass@@Default@P1.SLDPRT"); SolidWorks keeps that
        /// name in step with the document (an unsaved rebuild shows "Part53"), so compare it as "this document".</summary>
        static string SelfLinks(string expr)
        {
            return System.Text.RegularExpressions.Regex.Replace(expr ?? "", @"@[^@""]+\.(SLDPRT|SLDASM)", "@<this>",
                                                                System.Text.RegularExpressions.RegexOptions.IgnoreCase);
        }

        /// <summary>Properties and materials as the payload has them? (part of the structure check)</summary>
        List<object> PropertyDifferences(IModelDoc2 d, JObj nd)
        {
            var diffs = new List<object>();
            foreach (JObj p in nd.Arr("customProperties"))
            {
                string value, resolved;
                d.Extension.get_CustomPropertyManager(p.Str("configuration") ?? "").Get4(p.Str("name"), false, out value, out resolved);
                if (SelfLinks(value) != SelfLinks(p.Str("value")))
                    diffs.Add(new JObj { { "property", p.Str("name") }, { "configuration", p.Str("configuration") }, { "expected", p.Str("value") }, { "actual", value } });
            }
            var part = d as IPartDoc;
            if (part != null)
                foreach (JObj m in nd.Arr("materials"))
                {
                    string db, now = part.GetMaterialPropertyName2(m.Str("configuration"), out db), want;
                    if (now != m.Str("name") && !(materialAs.TryGetValue(m.Str("name") ?? "", out want) && now == want))
                        diffs.Add(new JObj { { "material", m.Str("configuration") }, { "expected", m.Str("name") }, { "actual", now } });
                }
            return diffs;
        }
    }
}
