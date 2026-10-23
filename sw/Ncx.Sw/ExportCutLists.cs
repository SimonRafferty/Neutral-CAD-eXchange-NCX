// Exporter: weldment cut lists (FORMAT-SPEC 4, "Cut lists") - each cut-list item's name, the bodies in it and its
// custom properties, as the user typed them and as they resolve (what a weldment BOM is built from).
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        /// <summary>The cut-list folders of a part, wherever they sit in the tree.</summary>
        internal static List<IFeature> CutListFolders(IModelDoc2 d)
        {
            var res = new List<IFeature>();
            for (var f = (IFeature)d.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                Collect(f, res);
            return res;
        }

        static void Collect(IFeature f, List<IFeature> res)
        {
            // a folder is reached from the tree and again as a sub-feature of the solid-bodies folder: once each
            if (f.GetTypeName2() == "CutListFolder" && !res.Any(x => x.Name == f.Name)) res.Add(f);
            for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                Collect(sub, res);
        }

        List<object> ExportCutLists(IModelDoc2 d)
        {
            var res = new List<object>();
            foreach (var f in CutListFolders(d))
            {
                var bodies = ((object[])((IBodyFolder)f.GetSpecificFeature2()).GetBodies() ?? new object[0])
                    .Select(b => (object)BodyRef(b)).Where(r => r != null).ToList();
                if (bodies.Count == 0) continue;
                var props = new List<object>();
                var cpm = f.CustomPropertyManager;
                foreach (string name in (object[])cpm?.GetNames() ?? new object[0])
                {
                    string value, resolved;
                    cpm.Get4(name, false, out value, out resolved);
                    int type = cpm.GetType2(name);
                    string neutral;
                    props.Add(new JObj
                    {
                        { "name", name }, { "type", PropTypes.TryGetValue(type, out neutral) ? neutral : "text" },
                        { "value", value }, { "resolved", resolved }, { "native", new JObj { { "app", "SOLIDWORKS" }, { "type", type } } }
                    });
                }
                res.Add(new JObj { { "name", f.Name }, { "bodies", bodies }, { "properties", props } });
            }
            return res;
        }

        void AddCutLists(IModelDoc2 d, string id, JObj json)
        {
            var lists = ExportCutLists(d);
            if (lists.Count == 0) return;
            json["cutLists"] = lists;
            Report.Add(id, "cut lists", "mapped", lists.Count + " cut-list items, " +
                       lists.Cast<JObj>().Sum(l => l.Arr("properties").Count) + " properties");
        }
    }
}
