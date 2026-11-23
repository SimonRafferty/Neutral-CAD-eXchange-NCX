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
            string active = ((IConfiguration)d.ConfigurationManager.ActiveConfiguration)?.Name;
            foreach (string cfg in (string[])d.GetConfigurationNames() ?? new string[0])
            {
                string db;
                string name = part.GetMaterialPropertyName2(cfg, out db);
                if (string.IsNullOrEmpty(name)) continue;
                var m = new JObj { { "configuration", cfg }, { "name", name }, { "database", db } };
                // what the material is, so a system without it can recreate it (FORMAT-SPEC 4.6): from this
                // computer's libraries, else what the part shows of it (measurable in the active configuration only)
                var def = MaterialDefinitions.FromLibrary(sw, db, name) ?? (cfg == active ? MeasuredMaterial(d, part) : null);
                if (def != null) m["definition"] = def;
                res.Add(m);
            }
            return res;
        }

        /// <summary>A material embedded in the part but in no library on this computer: the API cannot read its
        /// definition, so its density (mass properties) and its colour (when the part shows the material's
        /// appearance) are measured. Marked incomplete: its other properties are not known here.</summary>
        static JObj MeasuredMaterial(IModelDoc2 d, IPartDoc part)
        {
            var def = new JObj { { "complete", false } };
            try
            {
                var mp = (IMassProperty)d.Extension.CreateMassProperty();
                if (mp != null && mp.Density > 0) def["density"] = mp.Density;   // kg/m^3
            }
            catch (System.Runtime.InteropServices.COMException) { }
            var vis = part.GetMaterialVisualProperties() as IMaterialVisualPropertiesData;
            var mpv = d.MaterialPropertyValues as double[];
            if ((vis == null || vis.ApplyAppearance) && mpv != null && mpv.Length >= 3)
                def["color"] = new[] { mpv[0], mpv[1], mpv[2] };
            return def.Has("density") || def.Has("color") ? def : null;
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
