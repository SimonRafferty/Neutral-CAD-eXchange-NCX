// Exporter: custom properties and materials (FORMAT-SPEC 4.6) - what a BOM is built from. Properties are carried
// as the user typed them (a linked value keeps its expression, e.g. "SW-Mass@P1.SLDPRT") together with the value
// they resolve to, so an importer that cannot evaluate the expression still has the value. Configuration-specific
// properties and materials carry their configuration.
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        // swCustomInfoType_e -> neutral type
        static readonly Dictionary<int, string> PropTypes = new Dictionary<int, string>
        {
            { 30, "text" }, { 3, "number" }, { 5, "number" }, { 11, "yesno" }, { 64, "date" }
        };

        List<object> ExportCustomProperties(IModelDoc2 d)
        {
            var res = new List<object>();
            var cfgs = new List<string> { null };   // null = the whole file
            cfgs.AddRange((string[])d.GetConfigurationNames() ?? new string[0]);
            foreach (var cfg in cfgs)
            {
                var cpm = d.Extension.get_CustomPropertyManager(cfg ?? "");
                foreach (string name in (object[])cpm.GetNames() ?? new object[0])
                {
                    string value, resolved;
                    cpm.Get4(name, false, out value, out resolved);
                    int type = cpm.GetType2(name);
                    string neutral;
                    var p = new JObj
                    {
                        { "name", name }, { "type", PropTypes.TryGetValue(type, out neutral) ? neutral : "text" },
                        { "value", value }, { "resolved", resolved }, { "native", new JObj { { "app", "SOLIDWORKS" }, { "type", type } } }
                    };
                    if (cfg != null) p["configuration"] = cfg;
                    res.Add(p);
                }
            }
            return res;
        }

        List<object> ExportMaterials(IModelDoc2 d)
        {
            var res = new List<object>();
            var part = d as IPartDoc;
            if (part == null) return res;
            foreach (string cfg in (string[])d.GetConfigurationNames() ?? new string[0])
            {
                string db;
                string name = part.GetMaterialPropertyName2(cfg, out db);
                if (!string.IsNullOrEmpty(name)) res.Add(new JObj { { "configuration", cfg }, { "name", name }, { "database", db } });
            }
            return res;
        }

        /// <summary>Properties and material, added to a document's JSON with a report line.</summary>
        void AddPropertiesAndMaterials(IModelDoc2 d, string id, JObj json)
        {
            var props = ExportCustomProperties(d);
            var mats = ExportMaterials(d);
            if (props.Count > 0) json["customProperties"] = props;
            if (mats.Count > 0) json["materials"] = mats;
            if (props.Count + mats.Count == 0) return;
            var names = mats.Cast<JObj>().Select(m => m.Str("name")).Distinct().ToList();
            Report.Add(id, "properties", "mapped", props.Count + " custom properties" +
                       (names.Count == 0 ? "" : "; material " + string.Join(", ", names)));
        }
    }
}
