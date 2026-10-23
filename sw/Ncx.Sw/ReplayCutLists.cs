// Replayer: weldment cut lists (FORMAT-SPEC 4, "Cut lists"). SolidWorks makes the cut-list items itself; each is
// matched to the payload's by the bodies in it, renamed (linked properties name their item) and given its
// properties.
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        void ReplayCutLists(IModelDoc2 d, JObj nd)
        {
            var lists = nd.Arr("cutLists").Cast<JObj>().ToList();
            if (lists.Count == 0) return;
            var folders = Exporter.CutListFolders(d);
            foreach (var f in folders) ((IBodyFolder)f.GetSpecificFeature2()).UpdateCutList();
            folders = Exporter.CutListFolders(d);
            var problems = new List<string>();
            var used = new HashSet<IFeature>();
            foreach (var cl in lists)
            {
                // the rebuilt item holding the first of its bodies
                var first = FindBody(cl.Arr("bodies").Cast<JObj>().FirstOrDefault());
                var folder = first == null ? null : folders.FirstOrDefault(f => !used.Contains(f) &&
                    ((object[])((IBodyFolder)f.GetSpecificFeature2()).GetBodies() ?? new object[0]).Cast<IBody2>().Any(b => b.Name == first.Name));
                if (folder == null)
                {
                    problems.Add("cut-list item " + cl.Str("name") + " not found (" + (first == null ? "its body is not in the part" :
                                 "no item holds " + first.Name + "; " + folders.Count + " items") + ")");
                    continue;
                }
                used.Add(folder);
                try { Name(folder, cl.Str("name")); }
                catch (System.Runtime.InteropServices.COMException) { problems.Add("cut-list item " + cl.Str("name") + " could not be renamed"); }
                var cpm = folder.CustomPropertyManager;
                foreach (JObj p in cl.Arr("properties"))
                {
                    int type = (int)(p.Obj("native")?.Num("type") ?? 30);
                    if (cpm.Add3(p.Str("name"), type, p.Str("value") ?? "", 2) < 0)   // swCustomPropertyReplaceValue
                        problems.Add("cut-list property " + p.Str("name") + " could not be set");
                }
            }
            Report.Add(docId, "cut lists", problems.Count == 0 ? "rebuilt" : "approximate",
                       problems.Count == 0 ? lists.Count + " cut-list items" : string.Join("; ", problems));
        }
    }
}
