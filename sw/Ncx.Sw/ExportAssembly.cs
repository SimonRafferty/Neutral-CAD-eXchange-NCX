// Exporter: Insert Part (derived parts) and assemblies - the user's layout method: a layout part,
// parts made by inserting it, and origin-to-origin assemblies with mates only for moving parts.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        // swMateType_e -> neutral mate type
        static readonly Dictionary<int, string> MateTypes = new Dictionary<int, string>
        {
            { 0, "coincident" }, { 1, "concentric" }, { 2, "perpendicular" }, { 3, "parallel" }, { 4, "tangent" },
            { 5, "distance" }, { 6, "angle" }, { 16, "lock" }, { 10, "gear" }, { 11, "width" }, { 17, "screw" }, { 22, "hinge" },
            { 8, "symmetric" }, { 21, "slot" }, { 9, "cam" }, { 13, "rackPinion" }
        };

        /// <summary>Items an Insert Part brought into this part: named "&lt;source item&gt;-&lt;source title&gt;".</summary>
        void CollectTransferred(List<IFeature> all)
        {
            foreach (var stock in all.Where(f => f.GetTypeName2() == "Stock"))
            {
                var data = stock.GetDefinition() as IDerivedPartFeatureData;
                string srcId;
                if (data == null || !docIdByPath.TryGetValue(data.PathName, out srcId)) continue;
                string suffix = "-" + Path.GetFileNameWithoutExtension(data.PathName);
                var srcFeatures = featureIdsByDoc[srcId];
                var srcPlanes = originPlanesByDoc[srcId];
                foreach (var f in all)
                {
                    if (!f.Name.EndsWith(suffix)) continue;
                    string srcName = f.Name.Substring(0, f.Name.Length - suffix.Length);
                    var r = new JObj { { "kind", "transferred" }, { "by", "id" }, { "document", srcId }, { "name", f.Name } };
                    string fid, plane;
                    if (srcFeatures.TryGetValue(srcName, out fid)) r["feature"] = fid;
                    else if (srcPlanes.TryGetValue(srcName, out plane)) r["plane"] = plane;
                    else continue;
                    r["nativeType"] = f.GetTypeName2();
                    r["stock"] = stock.Name;
                    transferred[f.Name] = r;
                }
            }
        }

        void ExportInsertPart(IFeature f, JObj nf)
        {
            var data = f.GetDefinition() as IDerivedPartFeatureData;
            string srcId;
            if (data == null || !docIdByPath.TryGetValue(data.PathName, out srcId))
            {
                Opaque(nf, "inserted part " + data?.PathName + " was not exported");
                return;
            }
            nf["type"] = "insertPart";
            var items = transferred.Values.Where(r => r.Str("stock") == f.Name).ToList();
            var transfer = new List<object>();
            if (items.Any(r => r.Str("nativeType") == "ProfileFeature")) { transfer.Add("sketches"); transfer.Add("unabsorbedSketches"); }
            if (items.Any(r => r.Str("nativeType") == "RefPlane")) transfer.Add("planes");
            if (items.Any(r => r.Str("nativeType") == "RefAxis")) transfer.Add("axes");
            var src = (IModelDoc2)data.GetModelDoc();
            if (src is IPartDoc && (((object[])((IPartDoc)src).GetBodies2(0, false)) ?? new object[0]).Length > 0) transfer.Add("solidBodies");
            nf["def"] = new JObj
            {
                { "source", srcId }, { "transfer", transfer },
                { "transform", new double[] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } },
                { "items", items.Select(r => (object)new JObj { { "source", r.Str("feature") ?? "plane:" + r.Str("plane") }, { "name", r.Str("name") } }).ToList() }
            };
            if (data.GetMoveFeature() != null) Approx(nf, "insert-part move/copy transform not carried");
        }

        /// <summary>A mirrored part: the source document mirrored about a plane, given as one of the source's origin
        /// planes (found from the mirror transform). Other planes are not carried yet.</summary>
        void ExportMirrorPart(IFeature f, JObj nf)
        {
            var data = f.GetDefinition() as IMirrorPartFeatureData;
            if (data == null) { Opaque(nf, "mirror part data not readable"); return; }
            data.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            try { ExportMirrorPartData(f, data, nf); }
            finally { data.ReleaseSelectionAccess(); }
        }

        void ExportMirrorPartData(IFeature f, IMirrorPartFeatureData data, JObj nf)
        {
            string srcId;
            var srcDoc = data.GetModelDoc() as IModelDoc2;
            string srcPath = srcDoc?.GetPathName();
            if (string.IsNullOrEmpty(srcPath)) srcPath = data.PathName;
            if (string.IsNullOrEmpty(srcPath)) srcPath = ExternalPart(f);
            IModelDoc2 scanned;
            if (string.IsNullOrEmpty(srcPath) && mirrorSources.TryGetValue(doc.GetPathName(), out scanned)) srcPath = scanned.GetPathName();
            if (srcPath == null || !docIdByPath.TryGetValue(srcPath, out srcId))
            {
                Opaque(nf, "mirrored part " + srcPath + " was not exported");
                return;
            }
            nf["type"] = "mirrorPart";
            // the transform is a reflection p' = p R + t: the plane's normal is R's -1 eigenvector, through t / 2
            var a = (double[])((IMathTransform)data.GetTransform()).ArrayData;
            var axes = new[] { "right", "top", "front" };   // normals x, y, z
            string plane = null;
            for (int i = 0; i < 3 && plane == null; i++)
            {
                bool flips = Math.Abs(a[i * 4] + 1) < 1e-9;   // diagonal element of R is -1 for the normal's axis
                bool others = Enumerable.Range(0, 3).All(j => j == i || Math.Abs(a[j * 4] - 1) < 1e-9);
                if (flips && others && Math.Abs(a[9 + i]) < 1e-9) plane = axes[i];
            }
            if (plane == null) { Opaque(nf, "mirror plane is not an origin plane of the source"); return; }
            var transfer = new List<object>();
            if (data.SolidBodies) transfer.Add("solidBodies");
            if (data.SurfaceBodies) transfer.Add("surfaceBodies");
            if (data.Planes) transfer.Add("planes");
            if (data.Axes) transfer.Add("axes");
            if (data.AbsorbedSketches) transfer.Add("sketches");
            if (data.UnabsorbedSketches) transfer.Add("unabsorbedSketches");
            if (data.CustomProperties) transfer.Add("customProperties");
            if (data.CoordinateSystems) transfer.Add("coordinateSystems");
            nf["def"] = new JObj { { "source", srcId }, { "plane", new JObj { { "kind", "originPlane" }, { "by", "id" }, { "plane", plane } } },
                                   { "transfer", transfer } };
        }

        // --------------------------------------------------------------------------- assembly
        /// <summary>SolidWorks MathTransform array (3x3 rotation, translation in m, scale) -> neutral row-major
        /// 4x4 in mm. SolidWorks applies the rotation as p' = p R + t (row vectors), so M = R^T.</summary>
        public static double[] ToMatrix(double[] a)
        {
            return new[]
            {
                a[0], a[3], a[6], U.Mm(a[9]),
                a[1], a[4], a[7], U.Mm(a[10]),
                a[2], a[5], a[8], U.Mm(a[11]),
                0, 0, 0, 1
            };
        }

        JObj ExportAssembly(IModelDoc2 d, string id)
        {
            var asm = (IAssemblyDoc)d;
            asm.ResolveAllLightWeightComponents(false);
            var comps = ((object[])asm.GetComponents(true) ?? new object[0]).Cast<IComponent2>().ToList();
            CollectMirrorSources(d, comps);
            var components = new List<object>();
            var compIds = new Dictionary<string, string>();
            foreach (var c in comps)
            {
                string cid = "c:" + c.Name2;
                compIds[c.Name2] = cid;
                var md = (IModelDoc2)c.GetModelDoc2();
                if (md == null)
                {
                    Report.Add(id, c.Name2, "opaque", "component document not loaded");
                    continue;
                }
                string docRef = ExportDoc(md);
                var jc = new JObj
                {
                    { "id", cid }, { "document", docRef }, { "name", c.Name2 },
                    { "configuration", c.ReferencedConfiguration },
                    { "transform", ToMatrix((double[])((IMathTransform)c.Transform2).ArrayData) },
                    { "fixed", c.IsFixed() }
                };
                if (c.IsSuppressed()) jc["suppressed"] = new JObj { { "cfg:active", true } };
                components.Add(jc);
            }

            var mates = new List<object>();
            var features = new List<object>();
            int planes = 0;
            for (var f = (IFeature)d.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
            {
                string type = f.GetTypeName2();
                if (type == "MateGroup")
                {
                    for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                        mates.Add(ExportMate(d, sub, compIds, id));
                }
                else if (type == "LocalLPattern" || type == "LocalCirPattern")
                    features.Add(ExportComponentPattern(d, f, type == "LocalLPattern", comps, compIds, components, id));
                else if (type == "MirrorCompFeat")
                    features.Add(ExportMirrorComponents(d, f, comps, compIds, components, id));
                else if (type == "RefPlane" && ++planes <= 3) { }   // the origin planes
                else if (type == "RefPlane")
                    features.Add(ExportAssemblyPlane(d, f, compIds, id));
                else if (type == "ProfileFeature" || type == "ICE" || type == "Cut")   // assembly sketches and cuts
                    features.Add(ExportAssemblyLevel(d, f, type, comps, compIds, id));
                else if (!AssemblyTreeItems.Contains(type))
                {
                    // nothing at assembly level may disappear silently
                    var nf = new JObj { { "id", "a:" + f.Name }, { "type", "opaque" }, { "name", f.Name },
                                        { "native", new JObj { { "app", "SOLIDWORKS" }, { "type", type } } } };
                    features.Add(AssemblyOpaque(nf, "assembly feature type " + type + " is not mapped yet", id));
                }
            }
            Report.Add(id, Path.GetFileName(d.GetPathName()), "mapped", components.Count + " components, " + mates.Count + " mates");
            var json = new JObj
            {
                { "id", id }, { "kind", "assembly" }, { "name", Path.GetFileNameWithoutExtension(d.GetPathName()) },
                { "sourceFile", Path.GetFileName(d.GetPathName()) },
                { "components", components }, { "mates", mates }, { "features", features }
            };
            AddPropertiesAndMaterials(d, id, json);
            // appearances on components (top level, by component id); nested ones are not carried yet
            AddAppearances(d, id, json, c => compIds.TryGetValue(c.Name2, out var cid) ? new JObj { { "kind", "component" }, { "id", cid } } : null);
            return json;
        }

        /// <summary>A mated datum plane as a reference into its component's document: an origin plane or a
        /// plane feature. The mate gives the plane only as geometry (point + normal in root metres), so the
        /// component's plane features are matched against it; origin planes win a tie.</summary>
        JObj ComponentPlaneRef(IComponent2 comp, double[] p)
        {
            if (p == null || p.Length < 6) return null;
            var md = (IModelDoc2)comp.GetModelDoc2();
            string docRef;
            if (md == null || !docIdByPath.TryGetValue(md.GetPathName(), out docRef)) return null;
            // into the component's own frame
            var inv = (IMathTransform)((IMathTransform)comp.Transform2).Inverse();
            var mu = (IMathUtility)sw.GetMathUtility();
            var pt = (double[])((IMathPoint)((IMathPoint)mu.CreatePoint(new[] { p[0], p[1], p[2] })).MultiplyTransform(inv)).ArrayData;
            var nv = (double[])((IMathVector)((IMathVector)mu.CreateVector(new[] { p[3], p[4], p[5] })).MultiplyTransform(inv)).ArrayData;
            var rootPt = new[] { p[0], p[1], p[2] };
            var rootN = new[] { p[3], p[4], p[5] };
            Func<double[], double[], double[], double[], bool> same = (n, o, q, m) =>
                1 - Math.Abs(V.Dot(V.Unit(n), V.Unit(m))) < 1e-9 && Math.Abs(V.Dot(V.Sub(q, o), V.Unit(n))) < 1e-7;
            // a component's plane should report its part frame; the assembly frame is the fallback
            for (int pass = 0; pass < 2; pass++)
            {
                int planes = 0;
                for (var f = (IFeature)comp.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                {
                    if (f.GetTypeName2() != "RefPlane") continue;
                    planes++;
                    var a = (double[])((IMathTransform)((IRefPlane)f.GetSpecificFeature2()).Transform).ArrayData;
                    var n = new[] { a[6], a[7], a[8] };
                    var o = new[] { a[9], a[10], a[11] };
                    if (!(pass == 0 ? same(n, o, pt, nv) : same(n, o, rootPt, rootN))) continue;
                    if (planes <= 3) return new JObj { { "kind", "originPlane" }, { "by", "id" }, { "plane", new[] { "front", "top", "right" }[planes - 1] } };
                    Dictionary<string, string> ids;
                    string fid;
                    if (featureIdsByDoc.TryGetValue(docRef, out ids) && ids.TryGetValue(f.Name, out fid))
                        return new JObj { { "kind", "feature" }, { "by", "id" }, { "document", docRef }, { "id", fid } };
                    return null;
                }
            }
            return null;
        }

        /// <summary>A mated reference axis or point: by its feature in the component's document (the mate entity is
        /// the component's own feature); the component's origin point is its own kind.</summary>
        JObj ComponentDatumRef(IComponent2 comp, IFeature datum)
        {
            if (datum == null) return null;
            var md = (IModelDoc2)comp.GetModelDoc2();
            string docRef;
            if (md == null || !docIdByPath.TryGetValue(md.GetPathName(), out docRef)) return null;
            if (datum.GetTypeName2() == "OriginProfileFeature") return new JObj { { "kind", "origin" }, { "by", "id" } };
            Dictionary<string, string> ids;
            string fid;
            if (featureIdsByDoc.TryGetValue(docRef, out ids) && ids.TryGetValue(datum.Name, out fid))
                return new JObj { { "kind", "feature" }, { "by", "id" }, { "document", docRef }, { "id", fid } };
            return null;
        }

        // assembly tree items that carry no geometry or intent of their own (components are listed separately)
        static readonly HashSet<string> AssemblyTreeItems = new HashSet<string>
        {
            "CommentsFolder", "FavoriteFolder", "HistoryFolder", "SelectionSetFolder", "SensorFolder", "LiveSectionFolder",
            "DocsFolder", "DetailCabinet", "EnvFolder", "InkMarkupFolder", "EqnFolder", "OriginProfileFeature",
            "Reference", "ReferencePattern", "MaterialFolder", "SolidBodyFolder", "SurfaceBodyFolder", "BlockFolder",
            "MateReferenceGroupFolder", "MarkupCommentFolder", "FtrFolder", "GridFeature", "AnalysisFolder"
        };

        /// <summary>A linear or circular component pattern. Its instances are marked on their components
        /// (`pattern`), so an importer that rebuilds the pattern does not add them itself; one that cannot just
        /// places them as listed.</summary>
        JObj ExportComponentPattern(IModelDoc2 asmDoc, IFeature f, bool linear, List<IComponent2> comps,
                                    Dictionary<string, string> compIds, List<object> components, string asmId)
        {
            string fid = "a:" + f.Name;
            var nf = new JObj { { "id", fid }, { "type", "componentPattern" }, { "name", f.Name },
                                { "native", new JObj { { "app", "SOLIDWORKS" }, { "type", f.GetTypeName2() } } } };
            var data = f.GetDefinition();
            var lin = data as ILocalLinearPatternFeatureData;
            var cir = data as ILocalCircularPatternFeatureData;
            if (linear) lin.AccessSelections(asmDoc, null); else cir.AccessSelections(asmDoc, null);
            try
            {
                var seedObjs = (object[])(linear ? lin.SeedComponentArray : cir.SeedComponentArray) ?? new object[0];
                var seeds = new List<object>();
                foreach (var s in seedObjs)
                {
                    // a seed may come as the component or as its tree item
                    var c = s as IComponent2 ?? (s as IFeature)?.GetSpecificFeature2() as IComponent2;
                    string cid;
                    if (c == null || !compIds.TryGetValue(c.Name2, out cid))
                        return AssemblyOpaque(nf, "pattern seed " + (c == null ? "of an unexpected kind" : c.Name2) + " not recognised", asmId);
                    seeds.Add(cid);
                }
                var def = new JObj { { "kind", linear ? "linear" : "circular" }, { "seeds", seeds } };
                if (linear)
                {
                    var d1 = AssemblyEntityRef(asmDoc, lin.D1Axis, compIds);
                    if (d1 == null) return AssemblyOpaque(nf, "pattern direction not recognised", asmId);
                    def["direction"] = new JObj { { "ref", d1 }, { "count", lin.D1TotalInstances }, { "spacing", U.Mm(lin.D1Spacing) }, { "reverse", lin.D1ReverseDirection } };
                    if (lin.D2TotalInstances > 1)
                    {
                        var d2 = AssemblyEntityRef(asmDoc, lin.D2Axis, compIds);
                        if (d2 == null) return AssemblyOpaque(nf, "second pattern direction not recognised", asmId);
                        def["direction2"] = new JObj { { "ref", d2 }, { "count", lin.D2TotalInstances }, { "spacing", U.Mm(lin.D2Spacing) }, { "reverse", lin.D2ReverseDirection } };
                    }
                }
                else
                {
                    var ax = AssemblyEntityRef(asmDoc, cir.Axis, compIds)
                             // a cylindrical face as the axis reads back as nothing: find it from where the pattern put
                             // its first instance
                             ?? (cir.Axis == null ? CoaxialFace(f, seedObjs, cir.Spacing, comps, compIds) : null);
                    if (ax == null) return AssemblyOpaque(nf, "pattern axis not recognised (axis type " + cir.GetAxisType() + ", " + KindOf(cir.Axis) + ")", asmId);
                    def["axis"] = ax;
                    def["count"] = cir.TotalInstances;
                    def["angle"] = U.Deg(cir.Spacing);   // between neighbouring instances
                    def["reverse"] = cir.ReverseDirection;
                }
                var skipped = (int[])(linear ? lin.SkippedItemArray : cir.SkippedItemArray);
                if (skipped != null && skipped.Length > 0) def["skipped"] = skipped.Cast<object>().ToList();
                nf["def"] = def;
                nf["status"] = "mapped";
            }
            finally { if (linear) lin.ReleaseSelectionAccess(); else cir.ReleaseSelectionAccess(); }

            int n = MarkMadeComponents(f, fid, components, null);
            Report.Add(asmId, f.Name, "mapped", null, n + " instances");
            return nf;
        }

        /// <summary>Mark the components a pattern or mirror made (its sub-features in the tree) with `pattern`; returns
        /// how many. `only`: just these component names.</summary>
        static int MarkMadeComponents(IFeature f, string fid, List<object> components, ICollection<string> only)
        {
            var made = new HashSet<string>();
            for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                if (sub.GetTypeName2() == "ReferencePattern" && (only == null || only.Contains(sub.Name))) made.Add("c:" + sub.Name);
            int n = 0;
            foreach (JObj jc in components)
                if (made.Contains(jc.Str("id")) && !jc.Has("pattern")) { jc["pattern"] = fid; n++; }
            return n;
        }

        // mirrored part path -> its source part (a mirrored part made by Mirror Components reports no source itself)
        readonly Dictionary<string, IModelDoc2> mirrorSources = new Dictionary<string, IModelDoc2>(StringComparer.OrdinalIgnoreCase);

        /// <summary>Pair the opposite-hand parts each Mirror Components feature made with their sources (by volume),
        /// from its feature data (SolidWorks 2019 on).</summary>
        void CollectMirrorSources(IModelDoc2 asmDoc, List<IComponent2> comps)
        {
            for (var f = (IFeature)asmDoc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
            {
                if (f.GetTypeName2() != "MirrorCompFeat") continue;
                object data = f.GetDefinition();
                if (data == null) continue;   // SolidWorks 2018 has no mirror components data
                List<IComponent2> sources;
                try { sources = ((object[])Late(data, "OppositeHandComponents") ?? new object[0]).Cast<IComponent2>().ToList(); }
                catch (Exception e) when (e is System.Reflection.TargetInvocationException || e is MissingMethodException ||
                                          e is System.Runtime.InteropServices.COMException) { continue; }
                for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                {
                    var made = comps.FirstOrDefault(c => c.Name2 == sub.Name);
                    var md = made?.GetModelDoc2() as IModelDoc2;
                    if (md == null || !IsMirroredPart(md)) continue;
                    // measured through the components: the mirrored part's own document may not have its bodies loaded
                    double v = Volume(made);
                    var src = sources.FirstOrDefault(s => Math.Abs(Volume(s) - v) <= 1e-6 * Math.Max(v, 1e-12));
                    if (src != null) { mirrorSources[md.GetPathName()] = (IModelDoc2)src.GetModelDoc2(); sources.Remove(src); }
                }
            }
        }

        static bool IsMirroredPart(IModelDoc2 d)
        {
            for (var f = (IFeature)d.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                if (f.GetTypeName2() == "MirrorStock") return true;
            return false;
        }

        static double Volume(IComponent2 c)
        {
            return ((object[])c.GetBodies2(0) ?? new object[0]).Cast<IBody2>().Sum(b => ((double[])b.GetMassProperties(1))[3]);
        }

        /// <summary>Mirror Components: instances of the same part (re-mirrored at import) and opposite-hand copies (their
        /// mirrored parts are documents of their own; at import they are placed, not re-mirrored).</summary>
        JObj ExportMirrorComponents(IModelDoc2 asmDoc, IFeature f, List<IComponent2> comps, Dictionary<string, string> compIds,
                                    List<object> components, string asmId)
        {
            string fid = "a:" + f.Name;
            var nf = new JObj { { "id", fid }, { "type", "mirrorComponents" }, { "name", f.Name },
                                { "native", new JObj { { "app", "SOLIDWORKS" }, { "type", "MirrorCompFeat" } } } };
            object data = f.GetDefinition();
            if (data == null) return AssemblyOpaque(nf, "mirror components data not readable in this SolidWorks version", asmId);
            try
            {
                var plane = AssemblyEntityRef(asmDoc, Late(data, "MirrorPlane"), compIds);
                if (plane == null) return AssemblyOpaque(nf, "mirror plane not recognised", asmId);
                if (Convert.ToInt32(Late(data, "MirrorType")) != 0)
                    return AssemblyOpaque(nf, "mirror aligned to a selection (type " + Late(data, "MirrorType") + ") not mapped yet", asmId);
                var seeds = ((object[])Late(data, "ComponentsToInstanceAlignToComponentOrigin") ?? new object[0]).Cast<IComponent2>().ToList();
                var orients = (int[])Late(data, "ComponentOrientationsAlignToComponentOrigin") ?? new int[0];
                var instances = new List<object>();
                for (int i = 0; i < seeds.Count; i++)
                {
                    string cid;
                    if (!compIds.TryGetValue(seeds[i].Name2, out cid)) return AssemblyOpaque(nf, "mirror seed " + seeds[i].Name2 + " not recognised", asmId);
                    instances.Add(new JObj { { "seed", cid }, { "orientation", i < orients.Length ? orients[i] : 0 } });
                }
                var opposite = ((object[])Late(data, "OppositeHandComponents") ?? new object[0]).Cast<IComponent2>()
                    .Select(c => (object)(compIds.TryGetValue(c.Name2, out var cid) ? cid : c.Name2)).ToList();
                nf["def"] = new JObj { { "plane", plane }, { "instances", instances }, { "oppositeHand", opposite } };
                nf["status"] = "mapped";
                if (opposite.Count > 0)
                {
                    nf["status"] = "approximate";
                    nf["note"] = "opposite-hand components are placed as their mirrored parts (rebuilt from their sources), not re-mirrored";
                }
            }
            catch (Exception e) when (e is System.Reflection.TargetInvocationException || e is MissingMethodException ||
                                      e is System.Runtime.InteropServices.COMException || e is InvalidCastException)
            {
                return AssemblyOpaque(nf, "mirror components data not readable in this SolidWorks version", asmId);
            }
            // the instances it made are re-mirrored at import; the opposite-hand copies stay plain components
            var instanceNames = new HashSet<string>();
            for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
            {
                var c = comps.FirstOrDefault(x => x.Name2 == sub.Name);
                var md = c?.GetModelDoc2() as IModelDoc2;
                if (md != null && !IsMirroredPart(md)) instanceNames.Add(sub.Name);
            }
            int n = MarkMadeComponents(f, fid, components, instanceNames);
            Report.Add(asmId, f.Name, nf.Str("status"), nf.Str("note"), n + " instances");
            return nf;
        }

        /// <summary>Is feature f of component c the reference axis `axis` (the same line, read in the component's frame or
        /// in the assembly's)? Used when SolidWorks hands over the axis object without its feature.</summary>
        bool SameAxis(IComponent2 c, IFeature f, IRefAxis axis)
        {
            if (axis == null || f.GetTypeName2() != "RefAxis") return false;
            try
            {
                var a = (double[])axis.GetRefAxisParams();
                var b = (double[])((IRefAxis)f.GetSpecificFeature2()).GetRefAxisParams();
                if (a == null || b == null) return false;
                Func<double[], double[], bool> collinear = (p, q) =>
                {
                    var d = V.Unit(V.Sub(new[] { p[3], p[4], p[5] }, new[] { p[0], p[1], p[2] }));
                    System.Func<double[], double> off = x => V.Norm(V.Cross(V.Sub(x, new[] { p[0], p[1], p[2] }), d));
                    return off(new[] { q[0], q[1], q[2] }) < 1e-8 && off(new[] { q[3], q[4], q[5] }) < 1e-8;
                };
                if (collinear(a, b)) return true;
                // b in the assembly frame
                var mu = (IMathUtility)sw.GetMathUtility();
                var tr = (IMathTransform)c.Transform2;
                Func<double, double, double, double[]> up = (x, y, z) =>
                    (double[])((IMathPoint)((IMathPoint)mu.CreatePoint(new[] { x, y, z })).MultiplyTransform(tr)).ArrayData;
                var b0 = up(b[0], b[1], b[2]);
                var b1 = up(b[3], b[4], b[5]);
                return collinear(a, new[] { b0[0], b0[1], b0[2], b1[0], b1[1], b1[2] });
            }
            catch (System.Runtime.InteropServices.COMException) { return false; }   // some axes fault when read
        }

        /// <summary>COM identity: two wrappers of one object share their IUnknown (ReferenceEquals compares wrappers).</summary>
        static bool SameComObject(object a, object b)
        {
            if (a == null || b == null) return false;
            if (ReferenceEquals(a, b)) return true;
            IntPtr pa = IntPtr.Zero, pb = IntPtr.Zero;
            try
            {
                pa = System.Runtime.InteropServices.Marshal.GetIUnknownForObject(a);
                pb = System.Runtime.InteropServices.Marshal.GetIUnknownForObject(b);
                return pa == pb;
            }
            finally
            {
                if (pa != IntPtr.Zero) System.Runtime.InteropServices.Marshal.Release(pa);
                if (pb != IntPtr.Zero) System.Runtime.InteropServices.Marshal.Release(pb);
            }
        }

        /// <summary>The cylindrical component face about whose axis a circular pattern turned its seed onto its first
        /// instance (by the pattern angle, either way), as an assembly reference; null if there is none.</summary>
        JObj CoaxialFace(IFeature pattern, object[] seedObjs, double angle, List<IComponent2> comps, Dictionary<string, string> compIds)
        {
            var seed = seedObjs.Select(s => s as IComponent2 ?? (s as IFeature)?.GetSpecificFeature2() as IComponent2).FirstOrDefault(s => s != null);
            string firstName = null;
            for (var sub = (IFeature)pattern.GetFirstSubFeature(); sub != null && firstName == null; sub = (IFeature)sub.GetNextSubFeature())
                if (sub.GetTypeName2() == "ReferencePattern") firstName = sub.Name;
            var first = comps.FirstOrDefault(c => c.Name2 == firstName);
            if (seed == null || first == null) return null;
            var c0 = ComponentCentroid(seed);
            var c1 = ComponentCentroid(first);
            if (c0 == null || c1 == null) return null;
            var mu = (IMathUtility)sw.GetMathUtility();
            foreach (var comp in comps)
            {
                var tr = (IMathTransform)comp.Transform2;
                foreach (IBody2 body in (object[])comp.GetBodies2(0) ?? new object[0])
                    foreach (IFace2 face in (object[])body.GetFaces() ?? new object[0])
                    {
                        var s = (ISurface)face.GetSurface();
                        if (s == null || !s.IsCylinder()) continue;
                        var cp = (double[])s.CylinderParams;   // origin, axis, radius (component frame)
                        var o = (double[])((IMathPoint)((IMathPoint)mu.CreatePoint(new[] { cp[0], cp[1], cp[2] })).MultiplyTransform(tr)).ArrayData;
                        var e = V.Unit((double[])((IMathVector)((IMathVector)mu.CreateVector(new[] { cp[3], cp[4], cp[5] })).MultiplyTransform(tr)).ArrayData);
                        foreach (double a in new[] { angle, -angle })
                            if (V.Dist(Rotate(c0, o, e, a), c1) < 1e-6)
                            {
                                string cid;
                                var path = comp.Name2.Split('/');
                                if (!compIds.TryGetValue(path[0], out cid)) continue;
                                var r = Host.ComponentLocalDescriptor(sw, comp, face);
                                if (r == null) continue;
                                var jr = new JObj { { "component", cid }, { "ref", r } };
                                if (path.Length > 1) jr["path"] = path.Skip(1).Select(x => (object)("c:" + x)).ToList();
                                return jr;
                            }
                    }
            }
            return null;
        }

        /// <summary>A component's centroid in the assembly frame.</summary>
        double[] ComponentCentroid(IComponent2 c)
        {
            var body = ((object[])c.GetBodies2(0) ?? new object[0]).Cast<IBody2>().FirstOrDefault();
            if (body == null) return null;
            var mp = (double[])body.GetMassProperties(1);
            var mu = (IMathUtility)sw.GetMathUtility();
            return (double[])((IMathPoint)((IMathPoint)mu.CreatePoint(new[] { mp[0], mp[1], mp[2] })).MultiplyTransform((IMathTransform)c.Transform2)).ArrayData;
        }

        /// <summary>p turned by angle (rad) about the line through o along the unit vector e (Rodrigues).</summary>
        static double[] Rotate(double[] p, double[] o, double[] e, double angle)
        {
            var v = V.Sub(p, o);
            double c = Math.Cos(angle), s = Math.Sin(angle), k = V.Dot(e, v) * (1 - c);
            var x = V.Cross(e, v);
            return new[] { o[0] + v[0] * c + x[0] * s + e[0] * k, o[1] + v[1] * c + x[1] * s + e[1] * k, o[2] + v[2] * c + x[2] * s + e[2] * k };
        }

        /// <summary>A reference plane of the assembly: its references (assembly references: the assembly's own planes or
        /// component faces, edges, vertices and datums) with their constraints, and the resulting plane.</summary>
        JObj ExportAssemblyPlane(IModelDoc2 asmDoc, IFeature f, Dictionary<string, string> compIds, string asmId)
        {
            var nf = new JObj { { "id", "a:" + f.Name }, { "type", "plane" }, { "name", f.Name },
                                { "native", new JObj { { "app", "SOLIDWORKS" }, { "type", "RefPlane" } } } };
            var d = (IRefPlaneFeatureData)f.GetDefinition();
            d.AccessSelections(asmDoc, null);
            try
            {
                var refs = new List<object>();
                for (int i = 0; i < d.GetSelectionsCount(); i++)
                {
                    var r = AssemblyEntityRef(asmDoc, d.get_Reference(i), compIds);
                    if (r == null) return AssemblyOpaque(nf, "plane reference " + i + " not recognised", asmId);
                    int c = d.get_Constraint(i);   // swRefPlaneReferenceConstraints_e flags
                    double v = d.get_AngleOrDistance(i);
                    var pr = new JObj { { "ref", r } };
                    Neutral.PlaneConstraint(pr, c);
                    pr["value"] = (c & 16) != 0 ? U.Deg(v) : U.Mm(v);
                    pr["flip"] = (c & 256) != 0;
                    refs.Add(pr);
                }
                var a = (double[])((IMathTransform)((IRefPlane)f.GetSpecificFeature2()).Transform).ArrayData;
                nf["def"] = new JObj { { "references", refs }, { "origin", U.Mm(new[] { a[9], a[10], a[11] }) }, { "normal", new[] { a[6], a[7], a[8] } } };
                nf["status"] = "mapped";
            }
            finally { d.ReleaseSelectionAccess(); }
            Report.Add(asmId, f.Name, "mapped");
            return nf;
        }

        JObj AssemblyOpaque(JObj nf, string reason, string asmId)
        {
            nf["status"] = "opaque";
            nf["reason"] = reason;
            Report.Add(asmId, nf.Str("name"), "opaque", reason);
            return nf;
        }

        /// <summary>An entity a pattern or other assembly feature refers to: a component's face, edge, vertex or
        /// datum as {component, path?, ref}, or the assembly's own origin plane as {ref}.</summary>
        JObj AssemblyEntityRef(IModelDoc2 asmDoc, object ent, Dictionary<string, string> compIds)
        {
            if (ent == null) return null;
            var feat = ent as IFeature;
            if (feat != null && feat.GetTypeName2() == "RefPlane")
            {
                int planes = 0;
                for (var f = (IFeature)asmDoc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                {
                    if (f.GetTypeName2() != "RefPlane") continue;
                    planes++;
                    if (f.Name != feat.Name) continue;
                    if (planes <= 3)
                        return new JObj { { "ref", new JObj { { "kind", "originPlane" }, { "by", "id" }, { "plane", new[] { "front", "top", "right" }[planes - 1] } } } };
                    return new JObj { { "ref", new JObj { { "kind", "feature" }, { "by", "id" }, { "id", "a:" + f.Name } } } };   // an assembly plane
                }
            }
            IComponent2 comp;
            if (ent is IRefAxis || ent is IRefPlane || ent is IRefPoint)
            {
                // a datum comes as its specific object: find the component feature that owns it
                feat = null;
                comp = null;
                foreach (var c in ((object[])((IAssemblyDoc)asmDoc).GetComponents(false) ?? new object[0]).Cast<IComponent2>())
                {
                    for (var f = (IFeature)c.FirstFeature(); f != null && feat == null; f = (IFeature)f.GetNextFeature())
                        if (SameComObject(f.GetSpecificFeature2(), ent) || SameAxis(c, f, ent as IRefAxis)) feat = f;
                    if (feat != null) { comp = c; break; }
                }
            }
            else comp = (ent as IEntity)?.GetComponent() as IComponent2;
            if (comp == null) return null;
            var path = comp.Name2.Split('/');
            string cid;
            if (!compIds.TryGetValue(path[0], out cid)) return null;
            JObj r = ent is IFace2 || ent is IEdge || ent is IVertex ? Host.ComponentLocalDescriptor(sw, comp, ent)
                   : feat != null ? ComponentDatumRef(comp, feat) : null;
            if (r == null) return null;
            var jr = new JObj { { "component", cid }, { "ref", r } };
            if (path.Length > 1) jr["path"] = path.Skip(1).Select(x => (object)("c:" + x)).ToList();
            return jr;
        }

        /// <summary>Is this assembly reference a plane feature of a component's document?</summary>
        bool IsPlaneFeatureRef(JObj r)
        {
            var rf = r.Obj("ref");
            if (rf?.Str("kind") != "feature") return false;
            IModelDoc2 d;
            if (!docsById.TryGetValue(rf.Str("document") ?? "", out d)) return false;
            var ids = featureIdsByDoc.TryGetValue(rf.Str("document"), out var map) ? map : null;
            string name = ids?.FirstOrDefault(kv => kv.Value == rf.Str("id")).Key;
            return name != null && ((IFeature)((IPartDoc)d).FeatureByName(name))?.GetTypeName2() == "RefPlane";
        }

        /// <summary>Read a property of a COM object through IDispatch (for interfaces newer than the 2018 API the add-in
        /// is compiled against, and indexed properties).</summary>
        static object Late(object o, string name, params object[] args)
        {
            return o.GetType().InvokeMember(name, System.Reflection.BindingFlags.GetProperty, null, o, args);
        }

        /// <summary>A gear mate's ratio where its mate data is not available (SolidWorks 2018): the mate shows the two
        /// ratio values as its display dimensions.</summary>
        static string GearFromDimensions(IFeature f, JObj jm)
        {
            var vals = new List<double>();
            for (var dd = (IDisplayDimension)f.GetFirstDisplayDimension(); dd != null; dd = (IDisplayDimension)f.GetNextDisplayDimension(dd))
                vals.Add(((IDimension)dd.GetDimension2(0)).SystemValue);
            if (vals.Count != 2) return "gear ratio not readable (" + vals.Count + " dimensions)";
            jm["params"] = new JObj { { "ratio", vals.ToArray() } };
            // (the mate's Flipped flag is not the gear's direction)
            jm["status"] = "approximate";
            jm["note"] = "gear direction not readable in this SolidWorks version";
            return null;
        }

        /// <summary>A rack-and-pinion mate's pinion where its mate data is not available (SolidWorks 2018): the mate
        /// shows the pitch diameter (or the rack travel per turn) as its display dimension.</summary>
        static string RackFromDimensions(IFeature f, JObj jm)
        {
            var dd = (IDisplayDimension)f.GetFirstDisplayDimension();
            if (dd == null) return "rack-and-pinion diameter not readable";
            double v = U.Mm(((IDimension)dd.GetDimension2(0)).SystemValue);
            jm["params"] = new JObj { { "pitchDiameter", v }, { "travelPerTurn", v * Math.PI }, { "byTravel", false } };
            // the mate's own entities, in the order SolidWorks lists them: the rack, then the pinion
            var refs = jm.Arr("refs").Cast<JObj>().ToList();
            if (refs.Count == 2) { refs[0]["role"] = "rack"; refs[1]["role"] = "pinion"; }
            jm["status"] = "approximate";
            jm["note"] = "rack-and-pinion direction and value type not readable in this SolidWorks version (taken as the pitch diameter)";
            return null;
        }

        /// <summary>The parameters (and, for width and hinge mates, the role of each reference) of a mechanical mate,
        /// from its feature data. Returns why not, or null.</summary>
        string MechanicalMate(IModelDoc2 asmDoc, IFeature f, string type, JObj jm, Dictionary<string, string> compIds)
        {
            object data = f.GetDefinition();
            if (data == null) return type + " mate data not readable";
            try
            {
                switch (type)
                {
                    case "gear":   // IGearMateFeatureData (SolidWorks 2019 on)
                        jm["params"] = new JObj
                        {
                            { "ratio", new[] { Convert.ToDouble(Late(data, "GearRatioNumerator")), Convert.ToDouble(Late(data, "GearRatioDenominator")) } },
                            { "reverse", Convert.ToBoolean(Late(data, "Reverse")) }
                        };
                        return null;
                    case "screw":   // IScrewMateFeatureData: type 0 revolutions per mm, type 1 distance (m) per revolution
                    {
                        int rtype = Convert.ToInt32(Late(data, "RevolutionType"));
                        double v = Convert.ToDouble(Late(data, "RevolutionVal"));
                        jm["params"] = new JObj
                        {
                            { "pitch", rtype == 0 ? (v > 0 ? 1 / v : 0) : U.Mm(v) },   // mm per turn
                            { "pitchAsTurnsPerMm", rtype == 0 },
                            { "reverse", Convert.ToBoolean(Late(data, "Reverse")) }
                        };
                        return null;
                    }
                    case "width":   // IWidthMateFeatureData (in the 2018 API too)
                    {
                        var w = (IWidthMateFeatureData)data;
                        var refs = new List<object>();
                        foreach (var pair in new[] { new { role = "width", ents = w.WidthSelection }, new { role = "tab", ents = w.TabSelection } })
                            foreach (var e in (object[])pair.ents ?? new object[0])
                            {
                                var r = AssemblyEntityRef(asmDoc, e, compIds);
                                if (r == null) return "width mate " + pair.role + " face not recognised";
                                r["role"] = pair.role;
                                refs.Add(r);
                            }
                        jm["refs"] = refs;
                        jm["params"] = new JObj { { "constraint", Neutral.Word("widthConstraint", w.ConstraintType) }, { "distance", U.Mm(w.DistanceFromEnd) },
                                                  { "percent", w.PercentDistanceFromEnd }, { "flip", w.FlipDimension } };
                        return null;
                    }
                    case "symmetric":   // ISymmetricMateFeatureData (in the 2018 API too)
                    {
                        var s = (ISymmetricMateFeatureData)data;
                        var refs = new List<object>();
                        foreach (var e in (object[])s.EntitiesToMate ?? new object[0])
                        {
                            var r = AssemblyEntityRef(asmDoc, e, compIds);
                            if (r == null) return "symmetric mate entity not recognised";
                            r["role"] = "entity";
                            refs.Add(r);
                        }
                        var plane = AssemblyEntityRef(asmDoc, s.SymmetryPlane, compIds);
                        if (plane == null) return "symmetry plane not recognised";
                        plane["role"] = "plane";
                        refs.Add(plane);
                        jm["refs"] = refs;
                        jm["params"] = new JObj { { "alignment", s.MateAlignment } };
                        return null;
                    }
                    case "slot":   // ISlotMateFeatureData (2019 on)
                    {
                        var refs = new List<object>();
                        foreach (var e in (object[])Late(data, "EntitiesToMate") ?? new object[0])
                        {
                            var r = AssemblyEntityRef(asmDoc, e, compIds);
                            if (r == null) return "slot mate entity not recognised";
                            refs.Add(r);
                        }
                        jm["refs"] = refs;
                        jm["params"] = new JObj
                        {
                            { "constraint", Neutral.Word("slotConstraint", Convert.ToInt32(Late(data, "Constraint"))) },
                            { "distance", U.Mm(Convert.ToDouble(Late(data, "Distance"))) },
                            { "percent", Convert.ToDouble(Late(data, "Percent")) },
                            { "flip", Convert.ToBoolean(Late(data, "FlipDirection")) }
                        };
                        return null;
                    }
                    case "rackPinion":   // IRackPinionMateFeatureData (2019 on): type 0 pinion pitch diameter, 1 rack travel per turn
                    {
                        var refs = new List<object>();
                        foreach (var pair in new[] { new { role = "rack", which = 0 }, new { role = "pinion", which = 1 } })
                        {
                            var r = AssemblyEntityRef(asmDoc, Late(data, "EntitiesToMate", pair.which), compIds);
                            if (r == null) return "rack-and-pinion " + pair.role + " reference not recognised";
                            r["role"] = pair.role;
                            refs.Add(r);
                        }
                        jm["refs"] = refs;
                        int dtype = Convert.ToInt32(Late(data, "DiameterType"));
                        double v = U.Mm(Convert.ToDouble(Late(data, "DiameterVal")));
                        jm["params"] = new JObj
                        {
                            { "pitchDiameter", dtype == 0 ? v : v / Math.PI },   // mm
                            { "travelPerTurn", dtype == 0 ? v * Math.PI : v },   // mm of rack per pinion turn
                            { "byTravel", dtype == 1 },
                            { "reverse", Convert.ToBoolean(Late(data, "Reverse")) }
                        };
                        return null;
                    }
                    case "cam":   // the cam faces and the follower are the mate's own entities; nothing else to read
                        return null;
                    case "hinge":   // IHingeMateFeatureData (2019 on): a concentric pair and a coincident pair
                    {
                        var refs = new List<object>();
                        foreach (var pair in new[] { new { role = "concentric", which = 0 }, new { role = "coincident", which = 1 } })
                            foreach (var e in (object[])Late(data, "EntitiesToMate", pair.which) ?? new object[0])
                            {
                                var r = AssemblyEntityRef(asmDoc, e, compIds);
                                if (r == null) return "hinge " + pair.role + " reference not recognised";
                                r["role"] = pair.role;
                                refs.Add(r);
                            }
                        if (refs.Count != 4) return "hinge with " + refs.Count + " references";
                        jm["refs"] = refs;
                        if (Convert.ToBoolean(Late(data, "AngleSelection")))
                            jm["params"] = new JObj { { "angle", U.Deg(Convert.ToDouble(Late(data, "Angle"))) },
                                                      { "max", U.Deg(Convert.ToDouble(Late(data, "MaxVal"))) },
                                                      { "min", U.Deg(Convert.ToDouble(Late(data, "MinVal"))) } };
                        return null;
                    }
                }
            }
            catch (Exception e) when (e is System.Reflection.TargetInvocationException || e is MissingMethodException ||
                                      e is System.Runtime.InteropServices.COMException || e is InvalidCastException)
            {
                return type + " mate data not readable in this SolidWorks version";
            }
            return null;
        }

        /// <summary>A mated plane or origin of the assembly itself: one of its planes (matched by geometry; the first
        /// three are the origin planes) or its origin point. Other assembly-level datums are not carried yet.</summary>
        static JObj AssemblyDatumRef(IModelDoc2 asmDoc, int rt, object ent, double[] p)
        {
            if (rt == 4 && p != null && p.Length >= 6)   // swSelDATUMPLANES: point + normal, assembly metres
            {
                int planes = 0;
                for (var f = (IFeature)asmDoc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                {
                    if (f.GetTypeName2() != "RefPlane") continue;
                    planes++;
                    var a = (double[])((IMathTransform)((IRefPlane)f.GetSpecificFeature2()).Transform).ArrayData;
                    var n = V.Unit(new[] { a[6], a[7], a[8] });
                    var o = new[] { a[9], a[10], a[11] };
                    var pn = V.Unit(new[] { p[3], p[4], p[5] });
                    var pt = new[] { p[0], p[1], p[2] };
                    if (1 - Math.Abs(V.Dot(n, pn)) < 1e-9 && Math.Abs(V.Dot(V.Sub(pt, o), n)) < 1e-7)
                        return planes <= 3
                            ? new JObj { { "kind", "originPlane" }, { "by", "id" }, { "plane", new[] { "front", "top", "right" }[planes - 1] } }
                            : new JObj { { "kind", "feature" }, { "by", "id" }, { "id", "a:" + f.Name } };   // an assembly plane
                }
                return null;
            }
            var sp = ent as ISketchPoint;
            var owner = (sp?.GetSketch() as IFeature) ?? (ent as IFeature);
            if (owner?.GetTypeName2() == "OriginProfileFeature") return new JObj { { "kind", "origin" }, { "by", "id" } };
            return null;
        }

        /// <summary>A mated sketch entity of a component: its sketch and local id in the component's document.</summary>
        JObj ComponentSketchRef(IComponent2 comp, object ent)
        {
            var md = (IModelDoc2)comp.GetModelDoc2();
            string docRef;
            if (md == null || !docIdByPath.TryGetValue(md.GetPathName(), out docRef)) return null;
            var sk = ent is ISketchSegment ? ((ISketchSegment)ent).GetSketch() as ISketch : ((ISketchPoint)ent).GetSketch() as ISketch;
            if ((sk as IFeature)?.GetTypeName2() == "OriginProfileFeature") return new JObj { { "kind", "origin" }, { "by", "id" } };
            string skName = (sk as IFeature)?.Name;
            Dictionary<string, SketchIds> maps;
            SketchIds ids;
            Dictionary<string, string> fids;
            string fid;
            if (skName == null || !sketchIdsByDoc.TryGetValue(docRef, out maps) || !maps.TryGetValue(skName, out ids) ||
                !featureIdsByDoc.TryGetValue(docRef, out fids) || !fids.TryGetValue(skName, out fid)) return null;
            string local = ids.Of(ent);
            return local == null ? null : new JObj { { "kind", "sketchEntity" }, { "by", "id" }, { "document", docRef }, { "feature", fid }, { "id", local } };
        }

        JObj ExportMate(IModelDoc2 asmDoc, IFeature f, Dictionary<string, string> compIds, string asmId)
        {
            var jm = new JObj { { "id", "m:" + f.Name }, { "name", f.Name }, { "type", "other" }, { "refs", new List<object>() } };
            var mate = f.GetSpecificFeature2() as IMate2;
            string status = "mapped", reason = null;
            if (mate == null) { status = "opaque"; reason = "not a mate (" + f.GetTypeName2() + ")"; }
            else
            {
                string t;
                if (!MateTypes.TryGetValue(mate.Type, out t)) { t = "other"; status = "opaque"; reason = "mate type " + mate.Type + " not mapped yet"; }
                jm["type"] = t;
                jm["alignment"] = mate.Alignment == 0 ? "aligned" : mate.Alignment == 1 ? "antiAligned" : "closest";
                if (mate.Flipped) jm["flipped"] = true;
                var dd = (IDisplayDimension)f.GetFirstDisplayDimension();
                if (dd != null)
                {
                    double v = ((IDimension)dd.GetDimension2(0)).SystemValue;
                    var prm = new JObj { { t == "angle" ? "angle" : "distance", t == "angle" ? U.Deg(v) : U.Mm(v) } };
                    // a limit mate: the range it may move in. SolidWorks reports the limits as variations from the
                    // nominal value (+5 / -5 around 10); the payload carries absolute limits (15 / 5)
                    double hi = mate.MaximumVariation, lo = mate.MinimumVariation;
                    if (Math.Abs(hi - lo) > 1e-12)
                    {
                        prm["max"] = t == "angle" ? U.Deg(v + hi) : U.Mm(v + hi);
                        prm["min"] = t == "angle" ? U.Deg(v + lo) : U.Mm(v + lo);
                    }
                    jm["params"] = prm;
                }
                var refs = new List<object>();
                for (int i = 0; i < mate.GetMateEntityCount(); i++)
                {
                    var me = (IMateEntity2)mate.MateEntity(i);
                    var comp = (IComponent2)me.ReferenceComponent;
                    int rt = me.ReferenceType2;   // swSelectType_e
                    var ent = me.Reference;
                    if (comp == null || comp.IsRoot() || string.IsNullOrEmpty(comp.Name2))
                    {
                        // the assembly's own plane or origin (layoutcad mates every component's planes to them)
                        var own = AssemblyDatumRef(asmDoc, rt, ent, (double[])me.EntityParams);
                        if (own == null)
                        {
                            status = "opaque"; reason = "mate on an assembly-level reference other than its origin planes or origin";
                            break;
                        }
                        refs.Add(new JObj { { "ref", own } });
                        continue;
                    }
                    // a nested component is named by its path from this assembly: "Sub-1/Pin-1"
                    var path = comp.Name2.Split('/');
                    string cid;
                    if (path.Length == 0 || !compIds.TryGetValue(path[0], out cid))
                    {
                        status = "opaque"; reason = "mate entity on an unknown component";
                        break;
                    }
                    var jr = new JObj { { "component", cid } };
                    if (path.Length > 1) jr["path"] = path.Skip(1).Select(n => (object)("c:" + n)).ToList();
                    JObj r = null;
                    if (ent is IFace2 || ent is IEdge || ent is IVertex) r = Host.ComponentLocalDescriptor(sw, comp, ent);
                    else if (rt == 4) r = ComponentPlaneRef(comp, (double[])me.EntityParams);   // swSelDATUMPLANES
                    else if (rt == 5 || rt == 6) r = ComponentDatumRef(comp, ent as IFeature);    // axes, points
                    else if (ent is ISketchSegment || ent is ISketchPoint) r = ComponentSketchRef(comp, ent);
                    if (r == null)
                    {
                        status = "opaque";
                        reason = rt == 4 ? "mate plane not found in its component" : "mate on reference type " + rt + " not mapped yet";
                        break;
                    }
                    jr["ref"] = r;
                    refs.Add(jr);
                }
                jm["refs"] = refs;
                if (status == "mapped" && (t == "gear" || t == "width" || t == "screw" || t == "hinge" || t == "symmetric" || t == "slot" ||
                                           t == "rackPinion" || t == "cam"))
                {
                    reason = MechanicalMate(asmDoc, f, t, jm, compIds);
                    if (reason != null && t == "gear" && (reason = GearFromDimensions(f, jm)) == null) status = "approximate";
                    if (reason != null && (t == "symmetric" || t == "slot"))
                    {
                        // SolidWorks 2018: the mate's own entities (read above) stand in for its data
                        foreach (JObj r in refs)
                            if (t == "symmetric") r["role"] = r.Obj("ref")?.Str("kind") == "originPlane" || IsPlaneFeatureRef(r) ? "plane" : "entity";
                        jm["refs"] = refs;
                        reason = null;
                        if (t == "symmetric") jm["params"] = new JObj { { "alignment", mate.Alignment } };
                        if (t == "slot")
                        {
                            jm["params"] = new JObj { { "constraint", "free" } };
                            status = "approximate";
                            jm["note"] = "slot mate constraint not readable in this SolidWorks version: replayed free";
                        }
                    }
                    if (reason != null && t == "rackPinion" && (reason = RackFromDimensions(f, jm)) == null) status = "approximate";
                    if (reason != null && t == "screw")
                    {
                        // SolidWorks 2018 has no screw mate data: the pitch cannot be read
                        jm["params"] = new JObj { { "pitch", 1 } };
                        status = "approximate";
                        jm["note"] = "screw pitch not readable in this SolidWorks version: the default 1 mm per turn is assumed";
                        reason = null;
                    }
                }
                if (reason != null) status = "opaque";
            }
            jm["status"] = status;
            if (reason != null) jm["reason"] = reason;
            Report.Add(asmId, f.Name, status, reason);
            return jm;
        }
    }
}
