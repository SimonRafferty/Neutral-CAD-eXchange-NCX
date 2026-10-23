// Exporter: assembly-level sketches and extrude cuts (FORMAT-SPEC 4, "Assembly features"). They are exported with
// the part machinery pointed at the assembly document: its first three planes are its origin planes and its features
// have "a:" ids. A cut's scope is the set of components it changes, found by comparing their bodies on either side
// of it.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        // assembly sketches exported so far (their entity ids), per assembly document
        readonly Dictionary<string, SketchIds> assemblySketchMaps = new Dictionary<string, SketchIds>();

        JObj ExportAssemblyLevel(IModelDoc2 asmDoc, IFeature f, string type, List<IComponent2> comps,
                                 Dictionary<string, string> compIds, string asmId)
        {
            var nf = new JObj { { "id", "a:" + f.Name }, { "type", "opaque" }, { "name", f.Name },
                                { "native", new JObj { { "app", "SOLIDWORKS" }, { "type", type } } } };
            var saveDoc = doc;
            var savePart = part;
            string saveId = docId;
            var saveIds = featureIds.ToDictionary(k => k.Key, k => k.Value);
            var savePlanes = originPlanes.ToDictionary(k => k.Key, k => k.Value);
            var saveSketches = sketchMaps.ToDictionary(k => k.Key, k => k.Value);
            var fm = (IFeatureManager)asmDoc.FeatureManager;
            try
            {
                doc = asmDoc;
                part = null;
                docId = asmId;
                featureIds.Clear();
                originPlanes.Clear();
                sketchMaps.Clear();
                foreach (var kv in assemblySketchMaps) sketchMaps[kv.Key] = kv.Value;
                for (var g = (IFeature)asmDoc.FirstFeature(); g != null; g = (IFeature)g.GetNextFeature())
                {
                    if (g.GetTypeName2() == "RefPlane" && originPlanes.Count < 3) originPlanes[g.Name] = new[] { "front", "top", "right" }[originPlanes.Count];
                    else featureIds[g.Name] = "a:" + g.Name;
                }
                fm.EditRollback(3, f.Name);   // references in the state the feature saw
                if (type == "ProfileFeature")
                {
                    nf["type"] = "sketch";
                    nf["def"] = ExportSketch(f, nf);
                    assemblySketchMaps[f.Name] = sketchMaps[f.Name];
                }
                else
                {
                    ExportExtrude(f, nf);
                    if (nf.Str("status") != "opaque") AssemblyCutScope(asmDoc, f, nf, comps, compIds);
                }
            }
            catch (Exception e)
            {
                Opaque(nf, "export failed: " + e.Message);
            }
            finally
            {
                fm.EditRollback(1, "");
                doc = saveDoc;
                part = savePart;
                docId = saveId;
                featureIds.Clear();
                foreach (var kv in saveIds) featureIds[kv.Key] = kv.Value;
                originPlanes.Clear();
                foreach (var kv in savePlanes) originPlanes[kv.Key] = kv.Value;
                sketchMaps.Clear();
                foreach (var kv in saveSketches) sketchMaps[kv.Key] = kv.Value;
            }
            if (!nf.Has("status")) nf["status"] = "mapped";
            Report.Add(asmId, f.Name, nf.Str("status"), nf.Str("reason") ?? nf.Str("note"));
            return nf;
        }

        /// <summary>An assembly cut: only plain end conditions (an up-to face would be a component face, which the
        /// importer resolves in the part, not the assembly), and the components it changes.</summary>
        void AssemblyCutScope(IModelDoc2 asmDoc, IFeature f, JObj nf, List<IComponent2> comps, Dictionary<string, string> compIds)
        {
            var def = nf.Obj("def");
            if (nf.Str("type") != "cut") { Opaque(nf, "assembly-level boss extrudes do not exist"); return; }
            foreach (var key in new[] { "end", "end2" })
            {
                var e = def.Obj(key);
                if (e != null && e.Obj("target") != null) { Opaque(nf, "assembly cut up to a component face is not mapped yet"); return; }
            }
            if (def.Obj("start")?.Obj("target") != null) { Opaque(nf, "assembly cut from a component face is not mapped yet"); return; }
            var fm = (IFeatureManager)asmDoc.FeatureManager;
            Func<Dictionary<string, double>> volumes = () => comps.Where(c => compIds.ContainsKey(c.Name2)).ToDictionary(
                c => compIds[c.Name2], c => ((object[])c.GetBodies2(0) ?? new object[0]).Cast<IBody2>().Sum(b => ((double[])b.GetMassProperties(1.0))[3]));
            fm.EditRollback(3, f.Name);
            var before = volumes();
            fm.EditRollback(4, f.Name);   // swMoveRollbackBarToAfterFeature
            var after = volumes();
            var cut = before.Keys.Where(k => after.ContainsKey(k) && Math.Abs(before[k] - after[k]) > 1e-12 * Math.Max(1, before[k]))
                            .Select(k => (object)k).ToList();
            var d = (IExtrudeFeatureData2)f.GetDefinition();
            def["components"] = cut;   // the components it cuts
            def["autoSelect"] = d.AutoSelectComponents;
            def["propagate"] = d.PropagateFeatureToParts;
            if (d.PropagateFeatureToParts) Approx(nf, "cut propagated to the parts: carried as an assembly cut");
        }
    }
}
