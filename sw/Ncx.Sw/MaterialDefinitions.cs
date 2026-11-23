// Material definitions (FORMAT-SPEC 4.6, `definition`): what a material is, so a system that does not have it in
// its libraries can recreate it, as SolidWorks itself shows a material a part file carries even when no library on
// this computer has it.
//
// Export: the definition comes from this computer's material libraries (*.sldmat, the ones SolidWorks lists). A
// material that is only embedded in the part (made on another computer) cannot be read through the API: its density
// (mass properties) and its colour are measured instead, and the definition says it is not complete.
//
// Import: SolidWorks applies a material only from a library in a folder it lists (tested 2018 and 2026: a .sldmat
// passed by full path elsewhere is ignored), and the part then keeps the material without the library. So the
// importer writes the carried definitions into one library in such a folder, applies them from it, and deletes it
// when the import ends.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    static class MaterialDefinitions
    {
        static readonly XNamespace Mstns = "http://www.solidworks.com/sldmaterials";
        static readonly XNamespace Swatch = "http://www.solidworks.com/sldcolorswatch";

        /// <summary>SolidWorks' physical property keys and the neutral names (SI units on both sides).</summary>
        static readonly Dictionary<string, string> Keys = new Dictionary<string, string>
        {
            { "EX", "elasticModulus" }, { "NUXY", "poissonRatio" }, { "GXY", "shearModulus" },
            { "ALPX", "thermalExpansion" }, { "KX", "thermalConductivity" }, { "C", "specificHeat" },
            { "SIGXT", "tensileStrength" }, { "SIGXC", "compressiveStrength" }, { "SIGYLD", "yieldStrength" },
        };

        static readonly Dictionary<string, string> DisplayNames = new Dictionary<string, string>
        {
            { "EX", "Elastic modulus" }, { "NUXY", "Poisson's ratio" }, { "GXY", "Shear modulus" },
            { "ALPX", "Thermal expansion coefficient" }, { "KX", "Thermal conductivity" }, { "C", "Specific heat" },
            { "SIGXT", "Tensile strength" }, { "SIGXC", "Compressive strength" }, { "SIGYLD", "Yield strength" },
            { "DENS", "Mass density" },
        };

        static List<Tuple<string, XDocument>> Libraries(ISldWorks sw)
        {
            var res = new List<Tuple<string, XDocument>>();
            foreach (string path in (sw.GetMaterialDatabases() as object[] ?? new object[0]).Cast<string>())
            {
                try { res.Add(Tuple.Create(path, XDocument.Load(path))); }   // UTF-16 with a BOM, read by its declaration
                catch (Exception) { }   // unreadable library: skip it
            }
            return res;
        }

        static double? Num(XElement e, string attr)
        {
            double v;
            var a = e?.Attribute(attr);
            return a != null && double.TryParse(a.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out v) ? v : (double?)null;
        }

        /// <summary>The definition of a material from this computer's libraries, or null if none has it. The library
        /// named <paramref name="database"/> is searched first, then the others.</summary>
        public static JObj FromLibrary(ISldWorks sw, string database, string name)
        {
            if (string.IsNullOrEmpty(name)) return null;
            var libs = Libraries(sw)
                .OrderByDescending(l => string.Equals(Path.GetFileNameWithoutExtension(l.Item1), database ?? "", StringComparison.OrdinalIgnoreCase))
                .ToList();
            foreach (var lib in libs)
            {
                var mat = lib.Item2.Descendants("material").FirstOrDefault(m => (string)m.Attribute("name") == name);
                if (mat == null) continue;
                var def = new JObj { { "complete", true } };
                string cls = (string)mat.Parent?.Attribute("name");
                if (!string.IsNullOrEmpty(cls)) def["classification"] = cls;
                string desc = (string)mat.Attribute("description");
                if (!string.IsNullOrEmpty(desc) && desc != "-") def["description"] = desc;
                var phys = mat.Element("physicalproperties");
                var dens = Num(phys?.Element("DENS"), "value");
                if (dens != null) def["density"] = dens.Value;
                var props = new JObj();
                foreach (var kv in Keys)
                {
                    var v = Num(phys?.Element(kv.Key), "value");
                    if (v != null) props[kv.Value] = v.Value;
                }
                if (props.Any()) def["properties"] = props;
                string rgb = (string)mat.Element("swatchcolor")?.Attribute("RGB");
                int c;
                if (rgb != null && rgb.Length == 6 && int.TryParse(rgb, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out c))
                    def["color"] = new[] { (c >> 16 & 255) / 255.0, (c >> 8 & 255) / 255.0, (c & 255) / 255.0 };
                return def;
            }
            return null;
        }

        /// <summary>Temporary libraries holding carried definitions, for one import. A library holds one material of
        /// a name, and parts can carry the same material with different definitions (each part keeps its own copy:
        /// two magnets of one grade, coloured red and blue), so a definition whose name is taken goes into the next
        /// library: "NCX import materials.sldmat", "NCX import materials 2.sldmat", ...</summary>
        public sealed class ImportLibrary : IDisposable
        {
            readonly List<string> files = new List<string>();
            readonly Dictionary<string, string> pathOf = new Dictionary<string, string>();   // name + definition -> library
            public string Problem { get; private set; }

            static string Key(string name, JObj def) { return name + "|" + Json.Write(def); }

            /// <summary>Write the definitions (material name, definition) into libraries in the first folder SolidWorks
            /// lists material libraries from that can be written (folders outside Program Files first).</summary>
            public ImportLibrary(ISldWorks sw, IEnumerable<KeyValuePair<string, JObj>> definitions)
            {
                var groups = new List<Dictionary<string, JObj>>();
                var keys = new List<Dictionary<string, string>>();
                foreach (var kv in definitions)
                {
                    string key = Key(kv.Key, kv.Value);
                    if (keys.Any(k => k.ContainsKey(key))) continue;   // the same definition again
                    int i = groups.FindIndex(g => !g.ContainsKey(kv.Key));
                    if (i < 0) { groups.Add(new Dictionary<string, JObj>()); keys.Add(new Dictionary<string, string>()); i = groups.Count - 1; }
                    groups[i][kv.Key] = kv.Value;
                    keys[i][key] = kv.Key;
                }
                var dirs = (sw.GetMaterialDatabases() as object[] ?? new object[0]).Cast<string>()
                    .Select(System.IO.Path.GetDirectoryName).Distinct(StringComparer.OrdinalIgnoreCase)
                    .OrderBy(d => d.IndexOf("Program Files", StringComparison.OrdinalIgnoreCase) >= 0)
                    .ToList();
                foreach (string dir in dirs)
                {
                    try
                    {
                        for (int i = 0; i < groups.Count; i++)
                        {
                            string p = System.IO.Path.Combine(dir, "NCX import materials" + (i == 0 ? "" : " " + (i + 1)) + ".sldmat");
                            Document(groups[i]).Save(p);   // UTF-16, as SolidWorks writes them
                            files.Add(p);
                            foreach (var k in keys[i].Keys) pathOf[k] = p;
                        }
                        return;
                    }
                    catch (Exception)
                    {
                        Dispose();   // read-only folder: try the next
                    }
                }
                Problem = "no material library folder of this SolidWorks can be written";
            }

            /// <summary>The library holding this definition of the material, or null.</summary>
            public string PathFor(string name, JObj def)
            {
                string p;
                return pathOf.TryGetValue(Key(name, def), out p) ? p : null;
            }

            public void Dispose()
            {
                foreach (var p in files)
                    try { File.Delete(p); } catch (Exception) { }   // the parts keep their materials without them
                files.Clear();
                pathOf.Clear();
            }
        }

        static XDocument Document(IDictionary<string, JObj> definitions)
        {
            var root = new XElement(Mstns + "materials",
                new XAttribute(XNamespace.Xmlns + "mstns", Mstns.NamespaceName),
                new XAttribute(XNamespace.Xmlns + "sldcolorswatch", Swatch.NamespaceName),
                new XAttribute("version", "2008.03"),
                new XElement("curves", new XAttribute("id", "curve0"),
                    new XElement("point", new XAttribute("x", "1.0"), new XAttribute("y", "1.0")),
                    new XElement("point", new XAttribute("x", "2.0"), new XAttribute("y", "1.0")),
                    new XElement("point", new XAttribute("x", "3.0"), new XAttribute("y", "1.0"))));
            foreach (var group in definitions.GroupBy(kv => kv.Value.Str("classification") ?? "NCX"))
            {
                var cls = new XElement("classification", new XAttribute("name", group.Key));
                foreach (var kv in group)
                {
                    var def = kv.Value;
                    var props = def.Obj("properties") ?? new JObj();
                    // the schema's order (sldmaterials.xsd, a sequence): SolidWorks 2018 rejects the whole library
                    // when it is not kept (2026 does not mind); density sits between ALPX and KX
                    var phys = new XElement("physicalproperties");
                    foreach (string key in SchemaOrder)
                    {
                        if (key == "DENS") { if (def.Has("density")) phys.Add(Property("DENS", def.Num("density"))); }
                        else if (props.Has(Keys[key])) phys.Add(Property(key, props.Num(Keys[key])));
                    }
                    var mat = new XElement("material", new XAttribute("name", kv.Key),
                        new XAttribute("description", def.Str("description") ?? "carried by an NCX file"), phys,
                        new XElement("shaders", new XElement("cgshader2", new XAttribute("name", "defaultplastic"))));
                    var color = def.Vec("color");
                    if (color != null && color.Length >= 3)
                    {
                        int rgb = (Byte(color[0]) << 16) | (Byte(color[1]) << 8) | Byte(color[2]);
                        mat.Add(new XElement("swatchcolor", new XAttribute("RGB", rgb.ToString("x6")),
                            new XElement(Swatch + "Optical", new XAttribute("Ambient", "1.0"), new XAttribute("Transparency", "0.0"),
                                new XAttribute("Diffuse", "1.0"), new XAttribute("Specularity", "1.0"),
                                new XAttribute("Shininess", "0.3"), new XAttribute("Emission", "0.0"))));
                    }
                    cls.Add(mat);
                }
                root.Add(cls);
            }
            return new XDocument(new XDeclaration("1.0", "utf-16", null), root);
        }

        static readonly string[] SchemaOrder = { "EX", "NUXY", "GXY", "ALPX", "DENS", "KX", "C", "SIGXT", "SIGXC", "SIGYLD" };

        static XElement Property(string key, double value)
        {
            return new XElement(key, new XAttribute("displayname", DisplayNames[key]),
                                new XAttribute("value", value.ToString("R", CultureInfo.InvariantCulture)));
        }

        static int Byte(double v) { return Math.Max(0, Math.Min(255, (int)Math.Round(v * 255))); }
    }
}
