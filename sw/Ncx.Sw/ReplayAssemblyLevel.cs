// Replayer: assembly-level sketches and extrude cuts (FORMAT-SPEC 4, "Assembly features"), rebuilt with the part
// machinery pointed at the assembly document. SolidWorks' API cannot limit an assembly cut to chosen components
// (selection marks, the scope flags and the cut's data were all tried): a cut is made on every component it passes
// through, and kept only if those are the components the source cut.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        // assembly sketches rebuilt so far (their entities), by payload id
        readonly Dictionary<string, SketchMap> assemblySketchMaps = new Dictionary<string, SketchMap>();

        string ReplayAssemblyLevel(IModelDoc2 adoc, Dictionary<string, IComponent2> comps, JObj nf, ref string note)
        {
            var saveDoc = doc;
            var savePart = part;
            var saveOrigin = originPoint;
            var savePlanes = originPlanes.ToList();
            var saveFeatures = features.ToDictionary(k => k.Key, k => k.Value);
            var saveSketches = sketchMaps.ToDictionary(k => k.Key, k => k.Value);
            try
            {
                doc = adoc;
                part = null;
                originPlanes.Clear();
                originPoint = null;
                for (var f = (IFeature)adoc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                {
                    string t = f.GetTypeName2();
                    if (t == "RefPlane" && originPlanes.Count < 3) originPlanes.Add(f);
                    if (t == "OriginProfileFeature" && originPoint == null)
                    {
                        var pts = (object[])((ISketch)f.GetSpecificFeature2())?.GetSketchPoints2();
                        if (pts != null && pts.Length > 0) originPoint = (ISketchPoint)pts[0];
                    }
                }
                features.Clear();
                foreach (var kv in assemblyFeatures) features[kv.Key] = kv.Value;
                sketchMaps.Clear();
                foreach (var kv in assemblySketchMaps) sketchMaps[kv.Key] = kv.Value;
                var def = nf.Obj("def") ?? new JObj();
                string why;
                if (nf.Str("type") == "sketch")
                {
                    why = ReplaySketch(nf, def);
                    if (why == null && sketchMaps.ContainsKey(nf.Str("id"))) assemblySketchMaps[nf.Str("id")] = sketchMaps[nf.Str("id")];
                }
                else
                {
                    Func<Dictionary<string, double>> volumes = () => comps.ToDictionary(kv => kv.Key,
                        kv => ((object[])kv.Value.GetBodies2(0) ?? new object[0]).Cast<IBody2>().Sum(b => ((double[])b.GetMassProperties(1.0))[3]));
                    var before = volumes();
                    why = ReplayExtrude(nf, def, true);
                    if (why == null)
                    {
                        adoc.EditRebuild3();
                        var after = volumes();
                        var cut = new HashSet<string>(before.Keys.Where(k => Math.Abs(before[k] - after[k]) > 1e-12 * Math.Max(1, before[k])));
                        var want = new HashSet<string>(def.Arr("components").Cast<string>());
                        if (!cut.SetEquals(want))
                        {
                            // a wrong cut is worse than none: take it out again and say why
                            features[nf.Str("id")].Select2(false, 0);
                            adoc.Extension.DeleteSelection2(0);
                            adoc.ClearSelection2(true);
                            features.Remove(nf.Str("id"));
                            why = "the cut would change " + string.Join(", ", cut.OrderBy(x => x)) + " but the source cut only " +
                                  string.Join(", ", want.OrderBy(x => x)) + " (SolidWorks' API cannot limit an assembly cut's scope)";
                        }
                    }
                }
                if (why == null && features.ContainsKey(nf.Str("id"))) assemblyFeatures[nf.Str("id")] = features[nf.Str("id")];
                return why;
            }
            finally
            {
                doc = saveDoc;
                part = savePart;
                originPoint = saveOrigin;
                originPlanes.Clear();
                originPlanes.AddRange(savePlanes);
                features.Clear();
                foreach (var kv in saveFeatures) features[kv.Key] = kv.Value;
                sketchMaps.Clear();
                foreach (var kv in saveSketches) sketchMaps[kv.Key] = kv.Value;
            }
        }
    }
}
