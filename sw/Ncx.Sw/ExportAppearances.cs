// Exporter: appearances (FORMAT-SPEC 4.7) - colours and textured appearances on the document, a body, a feature,
// a face, or (in an assembly) a component. SolidWorks keeps every one as a render material: a plain colour is its
// `color.p2m`, a texture any other library appearance. Colours carry their RGB and optical values; textures carry
// their library path, colour and mapping, and are looked up in the importer's own library.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        List<object> ExportAppearances(IModelDoc2 d, Func<IComponent2, JObj> componentRef = null)
        {
            var res = new List<object>();
            // a material shows its own (textured) appearance on the whole part, listed like any other; the importer
            // gets it by setting the material, so it is not carried (it would override other configurations' materials)
            bool materialLook = false;
            var part = d as IPartDoc;
            if (part != null)
            {
                string db;
                string cfg = ((IConfiguration)d.GetActiveConfiguration()).Name;
                var vis = part.GetMaterialVisualProperties() as IMaterialVisualPropertiesData;
                materialLook = !string.IsNullOrEmpty(part.GetMaterialPropertyName2(cfg, out db)) && (vis == null || vis.ApplyAppearance);
            }
            var rms = (object[])d.Extension.GetRenderMaterials2(1, null) ?? new object[0];   // swThisDisplayState
            foreach (IRenderMaterial rm in rms)
            {
                string file = rm.FileName ?? "";
                bool plain = Path.GetFileName(file).Equals("color.p2m", StringComparison.OrdinalIgnoreCase);
                int c = rm.PrimaryColor;
                foreach (var e in (object[])rm.GetEntities() ?? new object[0])
                {
                    double[] mpv;
                    var target = AppearanceTarget(d, e, componentRef, out mpv);
                    if (target == null) continue;
                    if (materialLook && !plain && target.Str("kind") == "document") continue;   // the material's own
                    var a = new JObj { { "target", target }, { "kind", plain ? "color" : "appearance" } };
                    if (mpv != null && mpv.Length >= 9)
                    {
                        a["color"] = new[] { mpv[0], mpv[1], mpv[2] };
                        a["optical"] = new JObj { { "ambient", mpv[3] }, { "diffuse", mpv[4] }, { "specular", mpv[5] },
                                                  { "shininess", mpv[6] }, { "transparency", mpv[7] }, { "emission", mpv[8] } };
                    }
                    else a["color"] = new[] { (c & 255) / 255.0, (c >> 8 & 255) / 255.0, (c >> 16 & 255) / 255.0 };
                    if (!plain)
                    {
                        a["file"] = LibraryPath(file);
                        a["mapping"] = new JObj
                        {
                            { "type", rm.MappingType }, { "width", U.Mm(rm.Width) }, { "height", U.Mm(rm.Height) },
                            { "rotation", rm.RotationAngle }, { "x", U.Mm(rm.XPosition) }, { "y", U.Mm(rm.YPosition) },
                            { "fixedAspect", rm.FixedAspectRatio }
                        };
                    }
                    res.Add(a);
                }
            }
            return res;
        }

        void AddAppearances(IModelDoc2 d, string id, JObj json, Func<IComponent2, JObj> componentRef = null)
        {
            var apps = ExportAppearances(d, componentRef);
            if (apps.Count == 0) return;
            json["appearances"] = apps;
            int tex = apps.Cast<JObj>().Count(a => a.Str("kind") == "appearance");
            Report.Add(id, "appearances", "mapped", (apps.Count - tex) + " colours, " + tex + " textured appearances");
        }

        /// <summary>"metal/aluminum/brushed aluminum.p2m": the path inside the appearance library, as every
        /// SolidWorks install has it (the install folder differs).</summary>
        static string LibraryPath(string file)
        {
            int i = file.IndexOf(@"\materials\", StringComparison.OrdinalIgnoreCase);
            return (i >= 0 ? file.Substring(i + 11) : Path.GetFileName(file)).Replace('\\', '/');
        }

        /// <summary>The neutral reference to what an appearance is on, and that entity's own colour values.</summary>
        JObj AppearanceTarget(IModelDoc2 d, object e, Func<IComponent2, JObj> componentRef, out double[] mpv)
        {
            mpv = null;
            if (e is IFace2)
            {
                mpv = ((IFace2)e).MaterialPropertyValues as double[];
                return Host.Descriptor(e);
            }
            if (e is IBody2)
            {
                mpv = ((IBody2)e).MaterialPropertyValues2 as double[];
                return BodyRef(e);
            }
            if (e is IComponent2)
            {
                mpv = ((IComponent2)e).GetMaterialPropertyValues2(1, null) as double[];
                return componentRef?.Invoke((IComponent2)e);
            }
            if (e is IFeature)
            {
                var f = (IFeature)e;
                if (!featureIds.ContainsKey(f.Name)) return null;   // not an exported feature
                mpv = f.GetMaterialPropertyValues2(1, null) as double[];
                return FeatureRef(f);
            }
            if (e is IModelDoc2)
            {
                mpv = d.MaterialPropertyValues as double[];
                return new JObj { { "kind", "document" }, { "by", "id" } };
            }
            return null;
        }
    }
}
