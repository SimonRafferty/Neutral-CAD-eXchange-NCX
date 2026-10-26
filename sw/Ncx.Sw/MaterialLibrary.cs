// The SolidWorks material databases, for a material written under another system's name (FORMAT-SPEC 4,
// Materials: "an importer looks the name up in its own material library"). Fusion writes "Stainless Steel AISI 304"
// for SolidWorks' "AISI 304", or its generic "Steel"; the name alone does not say which SolidWorks material it is.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    static class MaterialLibrary
    {
        // (database, classification, material), read once from the installed databases
        static List<Tuple<string, string, string>> all;

        // other systems' generic names -> the SolidWorks material that stands for them
        static readonly Dictionary<string, string> Generic = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "Steel", "Plain Carbon Steel" }, { "Aluminum", "1060 Alloy" }, { "Aluminium", "1060 Alloy" },
            { "Stainless Steel", "AISI 304" }, { "Copper", "Copper" }, { "Brass", "Brass" }, { "Iron", "Gray Cast Iron" },
            { "Titanium", "Titanium" }, { "ABS Plastic", "ABS" }, { "Nylon", "Nylon 6/10" },
            { "Polycarbonate", "PC High Viscosity" }, { "Acrylic", "Acrylic (Medium-high impact)" }, { "Rubber", "Rubber" },
        };

        static List<Tuple<string, string, string>> Load(ISldWorks sw)
        {
            if (all != null) return all;
            all = new List<Tuple<string, string, string>>();
            string exe = sw.GetExecutablePath();
            string dir = Path.Combine(Directory.Exists(exe) ? exe : Path.GetDirectoryName(exe), "lang", "english", "sldmaterials");
            if (!Directory.Exists(dir)) return all;
            foreach (var file in Directory.EnumerateFiles(dir, "*.sldmat"))
            {
                string text;
                try { text = File.ReadAllText(file); } catch (Exception) { continue; }   // UTF-16 with a BOM
                string db = Path.GetFileNameWithoutExtension(file), cls = "";
                foreach (Match m in Regex.Matches(text, "<(classification|material) name=\"([^\"]*)\""))
                {
                    string name = System.Net.WebUtility.HtmlDecode(m.Groups[2].Value);
                    if (m.Groups[1].Value == "classification") cls = name;
                    else all.Add(Tuple.Create(db, cls, name));
                }
            }
            return all;
        }

        static List<string> Words(string s)
        {
            return Regex.Matches((s ?? "").ToLowerInvariant().Replace("aluminium", "aluminum"), "[a-z0-9]+").Cast<Match>().Select(m => m.Value).ToList();
        }

        /// <summary>The (database, name) of this SolidWorks' material nearest to another system's name, or null:
        /// a generic name by the table above; else a material whose name holds every word of the given name that
        /// has a digit (a grade: "304", "6061-T6"), the one sharing most other words; else one holding every word.</summary>
        public static Tuple<string, string> Nearest(ISldWorks sw, string name)
        {
            var mats = Load(sw);
            if (mats.Count == 0 || string.IsNullOrWhiteSpace(name)) return null;
            Func<string, Tuple<string, string>> byName = n =>
            {
                var hit = mats.FirstOrDefault(t => string.Equals(t.Item3, n, StringComparison.OrdinalIgnoreCase));
                return hit == null ? null : Tuple.Create(hit.Item1, hit.Item3);
            };
            string generic;
            if (Generic.TryGetValue(name.Trim(), out generic) && byName(generic) != null) return byName(generic);
            var words = Words(name);
            var grade = words.Where(w => w.Any(char.IsDigit)).ToList();
            Func<Tuple<string, string, string>, int> shared = t =>
            {
                var mw = Words(t.Item3 + " " + t.Item2);
                return words.Count(w => mw.Contains(w));
            };
            IEnumerable<Tuple<string, string, string>> pool;
            if (grade.Count > 0)
                pool = mats.Where(t => grade.All(g => Words(t.Item3).Contains(g)));
            else
                pool = mats.Where(t => Words(t.Item3).All(w => words.Contains(w)) && Words(t.Item3).Count > 0);
            var best = pool.OrderByDescending(shared).ThenBy(t => t.Item3.Length).FirstOrDefault();
            if (best != null) return Tuple.Create(best.Item1, best.Item3);
            // a grade SolidWorks does not have ("Aluminum AlSi10Mg"): the family the name says, by the table above
            foreach (var kv in Generic.OrderByDescending(k => k.Key.Length))
                if (Words(kv.Key).All(words.Contains) && byName(kv.Value) != null) return byName(kv.Value);
            return null;
        }
    }
}
