// Exporter: a SolidWorks part -> the neutral model (FORMAT-SPEC section 4).
//
// The feature tree is walked in order. Every feature becomes a neutral feature that is mapped,
// approximate (with a note) or opaque (with a reason): nothing is dropped silently. Geometric
// references are described in the state the feature saw - the rollback bar is moved to just before
// the feature - and the bar is put back at the end. The user's document is never saved.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        public const string Version = "0.5.3";
        readonly ISldWorks sw;
        public readonly Report Report = new Report();

        // per document
        IModelDoc2 doc;
        IPartDoc part;
        string docId;
        readonly Dictionary<string, string> featureIds = new Dictionary<string, string>();   // native name -> id
        readonly Dictionary<string, string> originPlanes = new Dictionary<string, string>(); // native name -> front/top/right
        readonly Dictionary<string, SketchIds> sketchMaps = new Dictionary<string, SketchIds>(); // sketch name -> entity ids
        int nextFeature;
        HashSet<string> equationTargets = new HashSet<string>();   // "D1@Sketch1" of every equation in the part

        // tree items that are not modelling features
        static readonly HashSet<string> Skip = new HashSet<string>
        {
            "DetailCabinet", "InkMarkupFolder", "SurfaceBodyFolder", "SolidBodyFolder", "EnvFolder", "EqnFolder",
            "MaterialFolder", "CommentsFolder", "FavoriteFolder", "HistoryFolder", "SelectionSetFolder", "SensorFolder",
            "DocsFolder", "OriginProfileFeature", "CutListFolder", "BlockFolder", "LiveSectionFolder", "MateGroup"
        };

        // across documents: the payload carries every document reachable from the root (FORMAT-SPEC 4)
        readonly List<object> documents = new List<object>();
        readonly Dictionary<string, string> docIdByPath = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, Dictionary<string, string>> featureIdsByDoc = new Dictionary<string, Dictionary<string, string>>();
        readonly Dictionary<string, Dictionary<string, string>> originPlanesByDoc = new Dictionary<string, Dictionary<string, string>>();
        readonly Dictionary<string, Dictionary<string, SketchIds>> sketchIdsByDoc = new Dictionary<string, Dictionary<string, SketchIds>>();
        readonly List<IModelDoc2> openedHere = new List<IModelDoc2>();
        // per part: items an Insert Part brought in (derived feature name -> reference into the source document)
        readonly Dictionary<string, JObj> transferred = new Dictionary<string, JObj>();
        readonly List<KeyValuePair<IFeature, JObj>> exportedFeatures = new List<KeyValuePair<IFeature, JObj>>();

        public Exporter(ISldWorks sw) { this.sw = sw; }

        /// <summary>The whole payload model for a part or assembly and every document it depends on.</summary>
        public JObj BuildModel(IModelDoc2 root)
        {
            documents.Clear();
            docIdByPath.Clear();
            docsById.Clear();
            var openBefore = new HashSet<string>(VisibleDocuments().Select(x => x.GetTitle()));
            quiet = new QuietUpdates(sw);
            try
            {
                string rootId = ExportDoc(root);
                ExportFlex();   // needs every document: a probe is measured on all the parts it moves
                var model = new JObj
                {
                    { "format", "ncx" }, { "schema", NcxCore.SchemaVersion },
                    { "generator", new JObj { { "app", "SOLIDWORKS" }, { "version", sw.RevisionNumber() },
                                              { "api", int.Parse(sw.RevisionNumber().Split('.')[0]) },
                                              { "exporter", "ncx-sw " + Version } } },
                    { "units", new JObj { { "length", "mm" }, { "angle", "deg" }, { "mass", "kg" } } },
                    { "root", rootId },
                    { "documents", documents },
                    { "fingerprint", Host.DocFingerprint(root) }
                };
                return model;
            }
            finally
            {
                foreach (var d in openedHere) sw.CloseDoc(d.GetTitle());
                openedHere.Clear();
                quiet.Dispose();
                quiet = null;
                // SolidWorks as it was: the exported model and whatever the user had open stay, anything else the
                // export opened (a layout, a scratch part) is closed
                foreach (var d in VisibleDocuments())
                    if (!openBefore.Contains(d.GetTitle()) && d.GetTitle() != root.GetTitle())
                        try { sw.CloseDoc(d.GetTitle()); } catch (System.Runtime.InteropServices.COMException) { }
            }
        }

        QuietUpdates quiet;   // screen updates off during an export (every rollback otherwise redraws)

        /// <summary>The documents open in their own window (not components loaded by an assembly).</summary>
        List<IModelDoc2> VisibleDocuments()
        {
            var res = new List<IModelDoc2>();
            foreach (var o in (object[])sw.GetDocuments() ?? new object[0])
            {
                var d = o as IModelDoc2;
                try { if (d != null && d.Visible) res.Add(d); } catch (System.Runtime.InteropServices.COMException) { }
            }
            return res;
        }

        /// <summary>Export a document after the documents it depends on; returns its id.</summary>
        string ExportDoc(IModelDoc2 d)
        {
            string path = d.GetPathName();
            string id;
            if (docIdByPath.TryGetValue(path, out id)) return id;
            string name = System.IO.Path.GetFileNameWithoutExtension(path);
            id = "doc:" + name;
            for (int k = 2; docIdByPath.ContainsValue(id); k++) id = "doc:" + name + "-" + k;
            docIdByPath[path] = id;
            docsById[id] = d;
            quiet?.Add(d);
            JObj json;
            if (d.GetType() == 2)   // swDocASSEMBLY
                json = ExportAssembly(d, id);
            else
            {
                foreach (var src in InsertedSources(d)) ExportDoc(src);
                json = ExportPart(d, id);
            }
            documents.Add(json);
            return id;
        }

        /// <summary>Source documents of the Insert Part (Stock) and Mirror Part (MirrorStock) features of a part.</summary>
        IEnumerable<IModelDoc2> InsertedSources(IModelDoc2 d)
        {
            var res = new List<IModelDoc2>();
            for (var f = (IFeature)d.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
            {
                string type = f.GetTypeName2();
                IModelDoc2 src = null;
                if (type == "Stock")
                {
                    var data = f.GetDefinition() as IDerivedPartFeatureData;
                    if (data != null) src = data.GetModelDoc() as IModelDoc2 ?? OpenSilently(data.PathName);
                }
                else if (type == "MirrorStock")   // a mirrored part (opposite-hand mirror components, Mirror Part)
                {
                    var data = f.GetDefinition() as IMirrorPartFeatureData;
                    if (data != null)
                    {
                        data.AccessSelections((ModelDoc2)d, null);   // its source is only reported inside an access (coclass wanted)
                        try { src = data.GetModelDoc() as IModelDoc2 ?? OpenSilently(data.PathName); }
                        finally { data.ReleaseSelectionAccess(); }
                    }
                    if (src == null) src = OpenSilently(ExternalPart(f));
                    if (src == null) mirrorSources.TryGetValue(d.GetPathName(), out src);   // from its assembly's mirror
                }
                if (src != null) res.Add(src);
            }
            return res;
        }

        /// <summary>The part a feature refers to, from its external references (a mirrored part's data reports no
        /// path).</summary>
        static string ExternalPart(IFeature f)
        {
            object models, comps, feats, types, status, ents, featComps;
            f.ListExternalFileReferences(out models, out comps, out feats, out types, out status, out ents, out featComps);
            return ((object[])models ?? new object[0]).Cast<string>()
                .FirstOrDefault(m => m != null && m.EndsWith(".SLDPRT", StringComparison.OrdinalIgnoreCase));
        }

        IModelDoc2 OpenSilently(string path)
        {
            if (string.IsNullOrEmpty(path)) return null;
            var d = (IModelDoc2)sw.GetOpenDocumentByName(path);
            if (d != null) return d;
            int err = 0, warn = 0;
            d = (IModelDoc2)sw.OpenDoc6(path, path.EndsWith(".SLDASM", StringComparison.OrdinalIgnoreCase) ? 2 : 1, 1 | 2, "", ref err, ref warn);   // silent, read-only
            if (d != null) openedHere.Add(d);
            return d;
        }

        public JObj ExportPart(IModelDoc2 d, string id)
        {
            doc = d;
            part = (IPartDoc)d;
            docId = id;
            featureIds.Clear();
            originPlanes.Clear();
            sketchMaps.Clear();
            modelPoints.Clear();
            modelNormals.Clear();
            transferred.Clear();
            exportedFeatures.Clear();
            opaqueSeen = false;
            featureIdsByDoc[id] = featureIds.ToDictionary(k => k.Key, k => k.Value);
            nextFeature = 1;
            ExportParameters(out var eqs);   // the dimensions equations drive are driving, not reference dimensions
            equationTargets = new HashSet<string>(eqs.Cast<JObj>().Select(e => e.Str("target")), StringComparer.OrdinalIgnoreCase);
            var features = new List<object>();
            var fm = (IFeatureManager)d.FeatureManager;
            try
            {
                var all = new List<IFeature>();
                for (var f = (IFeature)d.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                {
                    // a Hole Wizard's position sketch is its sub-feature: carried as a sketch just before the hole
                    var position = f.GetTypeName2() == "HoleWzd" ? HolePositionSketch(f) : null;
                    if (position != null) all.Add(position);
                    // an edge flange's profile sketch is often only its sub-feature (edited from the flange's
                    // PropertyManager): carried as a sketch just before the flange, like a hole's position sketch
                    if (f.GetTypeName2() == "EdgeFlange")
                        for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                            if (sub.GetTypeName2() == "ProfileFeature" && all.All(x => x.Name != sub.Name)) all.Add(sub);
                    all.Add(f);
                    // cosmetic threads live only under the feature whose edge they are on
                    for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                        if (sub.GetTypeName2() == "CosmeticThread") all.Add(sub);
                }
                CollectTransferred(all);
                CollectGeneratedSketches(all);
                foreach (var f in all)
                {
                    string t = f.GetTypeName2();
                    if (Skip.Contains(t) || t.EndsWith("Folder")) continue;
                    if (transferred.ContainsKey(f.Name)) continue;   // brought in by an Insert Part
                    if (t == "RefPlane" && originPlanes.Count < 3)
                    {
                        originPlanes[f.Name] = new[] { "front", "top", "right" }[originPlanes.Count];
                        continue;
                    }
                    var nf = ExportFeature(f, t, fm);
                    if (nf != null) features.Add(nf);
                }
            }
            finally
            {
                fm.EditRollback(1, "");   // swMoveRollbackBarToEnd: leave the model as it was
            }
            foreach (var measure in afterExport) measure();   // what can only be measured in the finished model
            afterExport.Clear();
            // not a fault: the shape arrives exactly (seeded coordinates); only later edits may differ
            foreach (JObj nf in features)
                if (nf.Str("type") == "sketch" && nf.Obj("def")?.Str("constrained") == "under")
                    Report.Add(id, nf.Str("name"), "note", "under-defined sketch: it arrives with exactly this shape, but when edited it may move differently in another CAD system");
            featureIdsByDoc[id] = featureIds.ToDictionary(k => k.Key, k => k.Value);
            originPlanesByDoc[id] = originPlanes.ToDictionary(k => k.Key, k => k.Value);
            sketchIdsByDoc[id] = sketchMaps.ToDictionary(k => k.Key, k => k.Value);
            string activeCfg;
            var configurations = ExportConfigurations(d, out activeCfg);
            var parameters = ExportParameters(out var equations);
            AddEquationTargets(equations, features);
            var bodies = ((object[])part.GetBodies2(0, false) ?? new object[0]).Cast<IBody2>().ToList();
            var json = new JObj
            {
                { "id", id }, { "kind", "part" }, { "name", System.IO.Path.GetFileNameWithoutExtension(d.GetPathName()) },
                { "sourceFile", System.IO.Path.GetFileName(d.GetPathName()) },
                { "configurations", configurations },
                { "activeConfiguration", activeCfg },
                { "parameters", parameters },
                { "equations", equations },
                { "features", features },
                { "bodies", bodies.Select((b, i) => (object)new JObj { { "id", "body:" + (i + 1) }, { "name", b.Name },
                                                                        { "fingerprint", Host.BodyFingerprint(b) } }).ToList() }
            };
            AddPropertiesAndMaterials(d, id, json);
            AddCutLists(d, id, json);
            AddAppearances(d, id, json);
            return json;
        }

        /// <summary>Progress: called with a message before each feature; return false to cancel (no file is
        /// written and the document's rollback bar is restored).</summary>
        public Func<string, bool> Progress;

        JObj ExportFeature(IFeature f, string type, IFeatureManager fm)
        {
            if (Progress != null && !Progress(System.IO.Path.GetFileName(doc.GetPathName()) + ": " + f.Name))
                throw new OperationCanceledException("export cancelled by the user at " + f.Name);
            string id = "f:" + (nextFeature++);
            featureIds[f.Name] = id;
            var nf = new JObj { { "id", id }, { "type", "opaque" }, { "name", f.Name },
                                { "native", new JObj { { "app", "SOLIDWORKS" }, { "type", type } } } };
            exportedFeatures.Add(new KeyValuePair<IFeature, JObj>(f, nf));   // per-configuration data added later
            try
            {
                switch (type)
                {
                    case "ProfileFeature":
                        Rollback(fm, f);
                        nf["type"] = "sketch";
                        nf["def"] = ExportSketch(f, nf);
                        break;
                    case "3DProfileFeature":   // a 3D sketch: model coordinates, no plane
                        Rollback(fm, f);
                        nf["type"] = "sketch3d";
                        nf["def"] = ExportSketch(f, nf);
                        break;
                    case "Extrusion":
                    case "ICE":
                    case "Cut":
                    case "BossThin":   // thin extrudes have their own type names
                    case "CutThin":
                        Rollback(fm, f);
                        ExportExtrude(f, nf);
                        break;
                    case "Fillet":
                        ExportFillet(f, nf);
                        break;
                    case "VarFillet":
                        ExportVariableFillet(f, nf);
                        break;
                    case "Chamfer":
                        ExportChamfer(f, nf);
                        break;
                    case "Revolution":
                    case "RevCut":
                    case "RevolutionThin":
                    case "RevCutThin":
                        ExportRevolve(f, nf, type.StartsWith("RevCut"));
                        break;
                    case "RefPlane":
                        ExportRefPlane(f, nf);
                        break;
                    case "RefAxis":
                        ExportRefAxis(f, nf);
                        break;
                    case "MirrorSolid":
                        ExportMirrorBody(f, nf);
                        break;
                    case "WeldCornerFeat":   // weldment trim/extend
                        ExportWeldmentTrim(f, nf);
                        break;
                    case "EndCap":
                        ExportEndCap(f, nf);
                        break;
                    case "LoftedBend":
                        ExportLoftedBend(f, nf);
                        break;
                    case "ExtruRefSurface":
                        ExportExtrudedSurface(f, nf);
                        break;
                    case "RevolvRefSurf":
                        ExportRevolvedSurface(f, nf);
                        break;
                    case "MoveCopyBody":
                        ExportMoveBody(f, nf);
                        break;
                    case "Gusset":
                        ExportGusset(f, nf);
                        break;
                    case "CosmeticThread":
                        // its edge exists once its parent feature is built, and the bar may still be before it: read at
                        // the end of the tree (EditRollback mode 2 does not leave the edge readable)
                        fm.EditRollback(1, "");   // swMoveRollbackBarToEnd
                        ExportCosmeticThread(f, nf);
                        break;
                    case "Rib":
                        ExportRib(f, nf);
                        break;
                    case "DeleteBody":
                        ExportDeleteBody(f, nf);
                        break;
                    case "RefPoint":
                        ExportRefPoint(f, nf);
                        break;
                    case "CoordSys":
                        ExportCoordinateSystem(f, nf);
                        break;
                    case "LPattern":
                        ExportLinearPattern(f, nf);
                        break;
                    case "CirPattern":
                        ExportCircularPattern(f, nf);
                        break;
                    case "MirrorPattern":
                        ExportMirror(f, nf);
                        break;
                    case "Shell":
                        ExportShell(f, nf);
                        break;
                    case "Stock":
                        ExportInsertPart(f, nf);
                        break;
                    case "MirrorStock":
                        ExportMirrorPart(f, nf);
                        break;
                    case "Draft":
                        ExportDraft(f, nf);
                        break;
                    case "HoleWzd":
                        ExportHoleWizard(f, nf);
                        break;
                    case "SheetMetal":
                        ExportSheetMetalDefaults(f, nf);
                        break;
                    case "SMBaseFlange":
                        ExportBaseFlange(f, nf);
                        break;
                    case "SolidToSheetMetal":
                        ExportConvertToSheetMetal(f, nf);
                        break;
                    case "EdgeFlange":
                        ExportEdgeFlange(f, nf);
                        DeferBendGeometry(f, nf);
                        break;
                    case "Hem":
                        ExportHem(f, nf);
                        DeferBendGeometry(f, nf);
                        break;
                    case "BreakCorner":
                        ExportBreakCorner(f, nf);
                        break;
                    case "SMMiteredFlange":
                        ExportMiterFlange(f, nf);
                        DeferBendGeometry(f, nf);
                        break;
                    case "CornerFeat":
                        ExportClosedCorner(f, nf);
                        break;
                    case "Jog":
                        ExportJog(f, nf);
                        DeferBendGeometry(f, nf);
                        break;
                    case "SM3dBend":   // sketched bend
                        ExportSketchedBend(f, nf);
                        DeferBendGeometry(f, nf);
                        break;
                    case "Sweep":
                    case "SweepCut":
                        Rollback(fm, f);
                        ExportSweep(f, nf, type == "SweepCut");
                        break;
                    case "Blend":
                    case "BlendCut":
                        Rollback(fm, f);
                        ExportLoft(f, nf, type == "BlendCut");
                        break;
                    case "CombineBodies":
                        Rollback(fm, f);   // the bodies as they were before the combine
                        ExportCombine(f, nf);
                        break;
                    case "WeldmentFeature":
                        nf["type"] = "weldment";   // marker: the part is a weldment
                        break;
                    case "WeldMemberFeat":
                        ExportStructuralMember(f, nf);
                        break;
                    case "Dome":
                        ExportDome(f, nf);
                        break;
                    case "Split":
                        ExportSplit(f, nf);
                        break;
                    case "PlanarSurface":
                        ExportPlanarSurface(f, nf);
                        break;
                    case "SewRefSurface":   // knit
                        ExportKnit(f, nf);
                        break;
                    case "Emboss":   // wrap (emboss, deboss, scribe)
                    case "Deboss":
                    case "Scribe":
                    case "Wrap":
                        ExportWrap(f, nf);
                        break;
                    case "UnFold":
                        ExportFolds(f, nf, false);
                        break;
                    case "Fold":
                        ExportFolds(f, nf, true);
                        break;
                    case "FlatPattern":
                        nf["type"] = "flatPattern";   // made by the base flange; replay renames it
                        break;
                    case "MacroFeature" when f.GetFaceCount() == 0:
                        // another add-in's feature that adds no faces to the model: reference data such as a 3D scan
                        // (x3DA) that the real geometry is modelled over. Nothing to rebuild; what follows does not
                        // depend on it geometrically, so it must not make every later feature carry its result
                        Opaque(nf, "reference data from another add-in (" + MacroProgId(f) + "): no geometry, not carried");
                        nf["noGeometry"] = true;
                        break;
                    default:
                        Opaque(nf, "feature type " + type + " is not mapped yet");
                        break;
                }
            }
            catch (Exception e)
            {
                // where it threw, so a report from the field can be traced (as the replayer does)
                string at = (e.StackTrace ?? "").Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Contains("Ncx.Sw."));
                Opaque(nf, "export failed: " + e.Message + (at == null ? "" : " (" + at + ")"));
            }
            MeasureAfterFeature(f, fm, nf);
            if (ForceOpaque.Contains(type)) Opaque(nf, "exported as opaque on request (testing)");
            if (!nf.Has("status")) nf["status"] = "mapped";
            if (WantsResult(type, nf))
            {
                try { AttachResult(f, fm, nf); }
                catch (Exception e)
                {
                    string at = (e.StackTrace ?? "").Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Contains("Ncx.Sw."));
                    Report.Add(docId, f.Name, "note", "its result could not be captured: " + e.Message + (at == null ? "" : " (" + at + ")"));
                }
            }
            Report.Add(docId, f.Name, nf.Str("status"), nf.Str("reason") ?? nf.Str("note"));
            return nf;
        }

        static string MacroProgId(IFeature f)
        {
            try { return ((IMacroFeatureData)f.GetDefinition())?.GetProgId() ?? "macro feature"; }
            catch (Exception e) when (e is System.Runtime.InteropServices.COMException || e is InvalidCastException) { return "macro feature"; }
        }

        void Rollback(IFeatureManager fm, IFeature f)
        {
            // references are described in the state the feature saw
            fm.EditRollback(3, f.Name);   // swMoveRollbackBarToBeforeFeature
        }

        static void Opaque(JObj nf, string reason)
        {
            nf["type"] = "opaque";
            nf["status"] = "opaque";
            nf["reason"] = reason;
            nf.Remove("def");
        }

        /// <summary>Measurements a feature's export defers until the model is rolled forward to its end.</summary>
        readonly List<Action> afterExport = new List<Action>();

        static void Approx(JObj nf, string note)
        {
            nf["status"] = "approximate";
            nf["note"] = nf.Has("note") ? nf.Str("note") + "; " + note : note;
        }

        // ----------------------------------------------------------------- parameters
        List<object> ExportParameters(out List<object> equations)
        {
            var pars = new List<object>();
            equations = new List<object>();
            var em = (IEquationMgr)doc.GetEquationMgr();
            int n = em.GetCount();
            for (int i = 0; i < n; i++)
            {
                string eq = em.get_Equation(i);
                int eqSign = eq.IndexOf('=');
                if (eqSign < 0) continue;
                string lhs = eq.Substring(0, eqSign).Trim().Trim('"');
                string rhs = eq.Substring(eqSign + 1).Trim();
                if (em.get_GlobalVariable(i))
                    pars.Add(new JObj { { "name", lhs }, { "value", em.get_Value(i) }, { "unit", "none" }, { "expression", rhs } });
                else
                    equations.Add(new JObj { { "target", lhs }, { "expression", rhs }, { "native", eq }, { "status", "mapped" } });
            }
            return pars;
        }

        /// <summary>Each equation's target in neutral form (FORMAT-SPEC 4, "Equations"): a sketch dimension by its sketch
        /// and dimension ids, or a feature's own dimension (D1@Boss-Base) as the one field of the feature's definition
        /// that holds its value (end.depth). Left out when that field is not unique.</summary>
        void AddEquationTargets(List<object> equations, List<object> features)
        {
            var byName = new Dictionary<string, JObj>(StringComparer.OrdinalIgnoreCase);
            foreach (JObj nf in features)
                if (nf.Str("name") != null && !byName.ContainsKey(nf.Str("name"))) byName[nf.Str("name")] = nf;
            foreach (JObj eq in equations)
            {
                string t = eq.Str("target");
                int at = t.IndexOf('@');
                JObj nf;
                if (at < 0 || !byName.TryGetValue(t.Substring(at + 1), out nf) || nf.Obj("def") == null) continue;
                string dimName = t.Substring(0, at);
                var def = nf.Obj("def");
                if (nf.Str("type") == "sketch" || nf.Str("type") == "sketch3d")
                {
                    var dm = def.Arr("dimensions").Cast<JObj>().FirstOrDefault(x => string.Equals(x.Str("name"), dimName, StringComparison.OrdinalIgnoreCase));
                    if (dm != null) eq["targetRef"] = new JObj { { "kind", "dimension" }, { "feature", nf.Str("id") }, { "id", dm.Str("id") } };
                    continue;
                }
                var dim = doc.Parameter(t) as IDimension;
                if (dim == null) continue;
                double sys = dim.SystemValue;   // m or rad
                var want = new[] { U.Mm(sys), U.Deg(sys) };
                var hits = new List<string>();
                NumericFields(def, "", (path, v) =>
                {
                    if (want.Any(w => Math.Abs(w - v) <= 1e-6 * Math.Max(1, Math.Abs(v)))) hits.Add(path);
                });
                if (hits.Count == 1)
                    eq["targetRef"] = new JObj { { "kind", "featureField" }, { "feature", nf.Str("id") }, { "field", hits[0] } };
            }
        }

        /// <summary>Every number in a definition with its dotted path, not descending into references or lists.</summary>
        static void NumericFields(JObj o, string prefix, Action<string, double> visit)
        {
            if (o.Has("kind") && o.Has("by")) return;   // a reference
            foreach (var kv in o)
            {
                string path = prefix.Length == 0 ? kv.Key : prefix + "." + kv.Key;
                if (kv.Value is double) visit(path, (double)kv.Value);
                else if (kv.Value is int) visit(path, (int)kv.Value);
                else if (kv.Value is JObj) NumericFields((JObj)kv.Value, path, visit);
            }
        }

        // ------------------------------------------------------------------- references
        JObj FeatureRef(IFeature f)
        {
            string id;
            JObj tr;
            // a sketch used by several features (a transferred sketch, or one sketch whose regions several features
            // use) is listed under each of them as "<name><2>", "<name><3>"...
            string bare = System.Text.RegularExpressions.Regex.Replace(f.Name, @"<\d+>$", "");
            if (transferred.TryGetValue(f.Name, out tr) || transferred.TryGetValue(bare, out tr)) return tr;
            if (originPlanes.TryGetValue(f.Name, out var plane))
                return new JObj { { "kind", "originPlane" }, { "by", "id" }, { "plane", plane } };
            if (featureIds.TryGetValue(f.Name, out id) || featureIds.TryGetValue(bare, out id))
                return new JObj { { "kind", "feature" }, { "by", "id" }, { "id", id } };
            return null;
        }

        /// <summary>A reference to whatever SolidWorks selected: a plane/axis/feature or sketch entity by id,
        /// a face/edge by geometry.</summary>
        JObj EntityRef(object ent)
        {
            if (ent == null) return null;
            if (ent is IFace2 || ent is IEdge) return Host.Descriptor(ent);
            if (ent is IVertex) return Host.VertexDescriptor((IVertex)ent);
            if (ent is ISketchSegment || ent is ISketchPoint)
            {
                var owner = (IFeature)(ent is ISketchSegment ? ((ISketchSegment)ent).GetSketch() : ((ISketchPoint)ent).GetSketch());
                SketchIds ids;
                string fid;
                if (owner == null || !sketchMaps.TryGetValue(owner.Name, out ids) || !featureIds.TryGetValue(owner.Name, out fid)) return null;
                string local = ids.Of(ent);
                return local == null ? null : new JObj { { "kind", "sketchEntity" }, { "by", "id" }, { "feature", fid }, { "id", local } };
            }
            var f = ent as IFeature ?? FeatureOf(ent);
            return f == null ? null : FeatureRef(f);
        }

        /// <summary>The feature whose specific object (RefPlane, RefAxis, ...) is `obj`.</summary>
        IFeature FeatureOf(object obj)
        {
            for (var f = (IFeature)doc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
            {
                string t = f.GetTypeName2();
                if (t != "RefPlane" && t != "RefAxis" && t != "RefPoint" && t != "CoordSys") continue;
                if (ReferenceEquals(f.GetSpecificFeature2(), obj)) return f;
            }
            return null;
        }
    }
}
