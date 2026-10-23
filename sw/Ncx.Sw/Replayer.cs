// Replayer: the neutral model -> SolidWorks documents, built feature by feature through the API.
//
// Every feature is either rebuilt or reported with the reason it could not be (FORMAT-SPEC section
// 5: only a "resolved" reference may build a feature). The result is verified against the payload's
// fingerprint.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        public const string Version = Exporter.Version;
        const int T_INPUT_DIM_ON_CREATE = 10;   // swInputDimValOnCreate: the Modify box would block the API

        readonly ISldWorks sw;
        public readonly Report Report = new Report();
        public readonly List<JObj> Verification = new List<JObj>();

        IModelDoc2 doc;
        IPartDoc part;
        string docId;
        readonly Dictionary<string, IFeature> features = new Dictionary<string, IFeature>();   // neutral id -> feature
        readonly List<IFeature> originPlanes = new List<IFeature>();
        readonly Dictionary<string, SketchMap> sketchMaps = new Dictionary<string, SketchMap>();   // sketch id -> entities
        // every rebuilt part's sketch maps (mates may reference a sketch entity in a component)
        readonly Dictionary<string, Dictionary<string, SketchMap>> sketchMapsByDoc = new Dictionary<string, Dictionary<string, SketchMap>>();
        readonly Dictionary<string, string> savedByDoc = new Dictionary<string, string>();       // doc id -> saved path
        readonly Dictionary<string, IFeature> transferredItems = new Dictionary<string, IFeature>(); // "doc|item" -> feature
        ISketchPoint originPoint;

        public Replayer(ISldWorks sw) { this.sw = sw; }

        /// <summary>Progress: called with a message before each step; return false to cancel.</summary>
        public Func<string, bool> Progress;
        public bool Cancelled { get; private set; }
        readonly List<IModelDoc2> created = new List<IModelDoc2>();

        void Step(string message)
        {
            if (Progress != null && !Progress(message))
            {
                Cancelled = true;
                throw new OperationCanceledException("cancelled by the user at: " + message);
            }
        }

        /// <summary>Replay every document of the model into outDir; returns the saved paths (root last).
        /// On cancel, every document this replay created is closed unsaved and nothing is left half-built.</summary>
        /// <summary>The model being replayed (cross-document references look names up in it).</summary>
        JObj Model;

        public List<string> ReplayModel(JObj model, string outDir)
        {
            Model = model;
            Directory.CreateDirectory(outDir);
            var saved = new List<string>();
            savedByDoc.Clear();
            created.Clear();
            bool inputDim = sw.GetUserPreferenceToggle(T_INPUT_DIM_ON_CREATE);
            sw.SetUserPreferenceToggle(T_INPUT_DIM_ON_CREATE, false);
            try
            {
                CloseEarlierOutputs(model, outDir);
                Step("checking the file's geometry against its payload");
                string stale = StaleCheck(model, outDir);
                if (stale != null)
                {
                    Report.Add(model.Str("root"), Path.GetFileName(ContainerPath ?? ""), "fallback",
                               stale + "; imported as plain geometry");
                    if (referenceRoot != null) saved.Add(PlainImport(model, outDir));
                    return saved;
                }
                var rebuilt = new Dictionary<string, IModelDoc2>();
                var fellBack = new HashSet<string>();
                fixedGeometry.Clear();
                // documents come in dependency order (a layout before the parts inserting it, parts before
                // assemblies); each is saved so later documents can reference it by path
                foreach (JObj d in model.Arr("documents"))
                {
                    Step("rebuilding " + d.Str("name"));
                    bool isRoot = d.Str("id") == model.Str("root");
                    IModelDoc2 rd;
                    string path;
                    if (d.Str("kind") == "assembly")
                    {
                        path = Path.Combine(outDir, d.Str("name") + ".SLDASM");
                        rd = ReplayAssembly(d, isRoot ? model.Obj("fingerprint") : null);
                    }
                    else
                    {
                        path = Path.Combine(outDir, d.Str("name") + ".SLDPRT");
                        rd = ReplayPart(d);
                        // the objective test: if the rebuilt geometry is not the source's, use the reference
                        // B-rep from the same file instead (partial rebuild kept for repair)
                        var ver = Verification.LastOrDefault(v => v.Str("document") == d.Str("id"));
                        if (ver != null && ver.Str("result") == "mismatch")
                        {
                            var diffs = ((List<object>)ver["differences"] ?? new List<object>()).Cast<JObj>()
                                .Where(x => x.Str("severity") == "mismatch").Select(x => x.Str("quantity")).Distinct();
                            rd = FallBack(d, rd, outDir, "rebuilt geometry differs in " + string.Join(", ", diffs),
                                          model.Arr("documents").Count == 1);
                            doc = rd;
                            part = rd as IPartDoc;
                            Verification.Remove(ver);
                            if (part != null) Verify(d);
                            fellBack.Add(d.Str("id"));
                        }
                        else if (part != null)
                            VerifyRebuilt(d);
                        if (resultsApplied && !fellBack.Contains(d.Str("id"))) fixedGeometry.Add(d.Str("id"));
                    }
                    Save(rd, path);
                    saved.Add(path);
                    savedByDoc[d.Str("id")] = path;
                    rebuilt[d.Str("id")] = rd;
                }
                VerifyFlex(model, rebuilt, fellBack);
            }
            catch (OperationCanceledException e)
            {
                // a defined state: nothing half-built stays open; documents already saved are complete
                CloseCreated(saved);
                Report.Add(model.Str("root"), "replay", "cancelled", e.Message);
            }
            catch
            {
                CloseCreated(saved);
                throw;
            }
            finally
            {
                sw.SetUserPreferenceToggle(T_INPUT_DIM_ON_CREATE, inputDim);
                CloseReference();
            }
            return saved;
        }

        void CloseCreated(List<string> saved)
        {
            foreach (var d in Enumerable.Reverse(created))
                try { if (!saved.Contains(d.GetPathName())) sw.CloseDoc(d.GetTitle()); } catch { }
        }

        /// <summary>A previous replay into the same folder may still have its documents open (SolidWorks then
        /// holds the files, and a new document cannot be saved over them): close them, assemblies first.
        /// Documents open from anywhere else are left alone.</summary>
        void CloseEarlierOutputs(JObj model, string outDir)
        {
            var paths = model.Arr("documents").Cast<JObj>()
                .Select(d => Path.Combine(outDir, d.Str("name") + (d.Str("kind") == "assembly" ? ".SLDASM" : ".SLDPRT")))
                .Reverse();
            foreach (var p in paths)
            {
                var open = sw.GetOpenDocumentByName(p) as IModelDoc2;
                if (open == null) continue;
                sw.CloseDoc(open.GetTitle());
                if (sw.GetOpenDocumentByName(p) != null)
                    throw new InvalidOperationException(p + " is open in SolidWorks (in use by another document); close it and retry");
            }
        }

        void Save(IModelDoc2 d, string path)
        {
            int err = 0, warn = 0;
            if (File.Exists(path)) File.Delete(path);
            if (!d.Extension.SaveAs(path, 0, 1, null, ref err, ref warn))   // current version, silent
                throw new InvalidOperationException("could not save " + path + " (error " + err + ")");
        }

        public IModelDoc2 ReplayPart(JObj d)
        {
            docId = d.Str("id");
            string template = sw.GetUserPreferenceStringValue(8);   // swDefaultTemplatePart
            // a mirrored part is made by mirroring its source, which creates the document
            var mirror = d.Arr("features").Cast<JObj>().FirstOrDefault(f => f.Str("type") == "mirrorPart" && f.Str("status") != "opaque");
            doc = mirror == null ? null : MirroredDocument(mirror.Obj("def"));
            if (doc == null) doc = (IModelDoc2)sw.NewDocument(template, 0, 0, 0);
            if (doc == null) throw new InvalidOperationException("cannot create a part from " + template);
            created.Add(doc);
            part = (IPartDoc)doc;
            features.Clear();
            resultsApplied = false;
            deferredSketches.Clear();
            pendingSheetMetal = null;
            originPlanes.Clear();
            sketchMaps.Clear();
            transferredItems.Clear();
            originPoint = null;
            equationTargets = new HashSet<string>(d.Arr("equations").Cast<JObj>().Select(e => e.Str("target")).Where(t => t != null),
                                                  StringComparer.OrdinalIgnoreCase);
            for (var f = (IFeature)doc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
            {
                string t = f.GetTypeName2();
                if (t == "RefPlane" && originPlanes.Count < 3) originPlanes.Add(f);
                if (t == "OriginProfileFeature" && originPoint == null)
                {
                    var osk = (ISketch)f.GetSpecificFeature2();
                    var pts = (object[])osk?.GetSketchPoints2();
                    if (pts != null && pts.Length > 0) originPoint = (ISketchPoint)pts[0];
                }
            }

            ReplayParameters(d);
            foreach (JObj nf in d.Arr("features"))
            {
                string name = nf.Str("name");
                Step(d.Str("name") + ": " + name);
                string failure;
                try
                {
                    if (nf.Str("status") == "opaque")
                    {
                        if (nf.Obj("result") == null) { Report.Add(docId, name, "fallback", nf.Str("reason")); continue; }
                        failure = nf.Str("reason");
                    }
                    else
                    {
                        failure = ReplayFeature(nf);
                        if (failure == null)
                        {
                            Report.Add(docId, name, nf.Str("status") == "approximate" ? "approximate" : "rebuilt", nf.Str("note"));
                            continue;
                        }
                    }
                }
                catch (Exception e)
                {
                    doc.ClearSelection2(true);
                    if (((ISketchManager)doc.SketchManager).ActiveSketch != null) ((ISketchManager)doc.SketchManager).InsertSketch(true);
                    // where it threw, so a report from the field can be traced
                    string at = (e.StackTrace ?? "").Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Contains("Ncx.Sw."));
                    failure = e.Message + (at == null ? "" : " (" + at + ")");
                }
                // not rebuilt: its carried result (FORMAT-SPEC 4, "Feature results") keeps the rest of the history rebuildable
                if (nf.Obj("result") == null) { Report.Add(docId, name, "failed", failure); continue; }
                string noResult;
                try { noResult = ApplyResult(nf); }
                catch (Exception e) { noResult = e.Message; }
                if (noResult == null)
                    Report.Add(docId, name, "approximate", "carried as fixed geometry (its resulting bodies), not rebuilt: " + failure);
                else
                    Report.Add(docId, name, "failed", failure + "; its carried result could not be used either: " + noResult);
            }
            ReplayEquations(d);
            doc.ForceRebuild3(false);
            ReplayConfigurations(d);
            doc.ForceRebuild3(false);
            sketchMapsByDoc[d.Str("id")] = sketchMaps.ToDictionary(k => k.Key, k => k.Value);
            ReplayPropertiesAndMaterials(doc, d);   // configurations exist now
            ReplayCutLists(doc, d);
            ReplayAppearances(doc, d);
            // a final naming pass: SolidWorks replaces some feature objects on a later rebuild or configuration
            // change (weldment structural members), and the new one has its default name; names carry intent
            // A replacement is found by position: the k-th payload feature of a type is the k-th feature of that
            // type in the tree (features of one type are created, and listed, in payload order).
            foreach (JObj nf in d.Arr("features"))
            {
                IFeature f;
                if (!features.TryGetValue(nf.Str("id"), out f)) continue;
                try { Name(f, nf.Str("name")); continue; }
                catch (System.Runtime.InteropServices.COMException) { }   // disconnected: find its replacement
                string type = nf.Obj("native")?.Str("type");
                int k = d.Arr("features").Cast<JObj>().Where(x => x.Obj("native")?.Str("type") == type && features.ContainsKey(x.Str("id")))
                         .Select(x => x.Str("id")).ToList().IndexOf(nf.Str("id"));
                var same = new List<IFeature>();
                for (var g = (IFeature)doc.FirstFeature(); g != null; g = (IFeature)g.GetNextFeature())
                    if (g.GetTypeName2() == type) same.Add(g);
                if (k < 0 || k >= same.Count) continue;
                Name(same[k], nf.Str("name"));
                features[nf.Str("id")] = same[k];
            }
            Verify(d);
            return doc;
        }

        /// <summary>Rebuild one feature; returns null on success or the reason it failed.</summary>
        string ReplayFeature(JObj nf)
        {
            var def = nf.Obj("def") ?? new JObj();
            switch (nf.Str("type"))
            {
                case "sketch": return ReplaySketch(nf, def);
                case "sketch3d": return ReplaySketch3D(nf, def);
                case "extrude": return ReplayExtrude(nf, def, false);
                case "cut": return ReplayExtrude(nf, def, true);
                case "fillet": return ReplayFillet(nf, def);
                case "chamfer": return ReplayChamfer(nf, def);
                case "revolve": return ReplayRevolve(nf, def, false);
                case "revolveCut": return ReplayRevolve(nf, def, true);
                case "plane": return ReplayPlane(nf, def);
                case "axis": return ReplayAxis(nf, def);
                case "point": return ReplayPoint(nf, def);
                case "coordinateSystem": return ReplayCoordinateSystem(nf, def);
                case "linearPattern": return ReplayLinearPattern(nf, def);
                case "circularPattern": return ReplayCircularPattern(nf, def);
                case "mirror": return ReplayMirror(nf, def);
                case "mirrorBody": return ReplayMirrorBody(nf, def);
                case "weldmentTrim": return ReplayWeldmentTrim(nf, def);
                case "endCap": return ReplayEndCap(nf, def);
                case "loftedBend": return ReplayLoftedBend(nf, def);
                case "extrudedSurface": return ReplayExtrudedSurface(nf, def);
                case "revolvedSurface": return ReplayRevolvedSurface(nf, def);
                case "rib": return ReplayRib(nf, def);
                case "cosmeticThread": return ReplayCosmeticThread(nf, def);
                case "gusset": return ReplayGusset(nf, def);
                case "hem": return ReplayHem(nf, def);
                case "breakCorner": return ReplayBreakCorner(nf, def);
                case "sketchedBend": return ReplaySketchedBend(nf, def);
                case "jog": return ReplayJog(nf, def);
                case "closedCorner": return ReplayClosedCorner(nf, def);
                case "miterFlange": return ReplayMiterFlange(nf, def);
                case "moveBody": return ReplayMoveBody(nf, def);
                case "deleteBody": return ReplayDeleteBody(nf, def);
                case "shell": return ReplayShell(nf, def);
                case "insertPart": return ReplayInsertPart(nf, def);
                case "mirrorPart": return ReplayMirrorPart(nf);
                case "draft": return ReplayDraft(nf, def);
                case "hole": return ReplayHole(nf, def);
                case "sheetMetal": return ReplaySheetMetalDefaults(nf, def);
                case "baseFlange": return ReplayBaseFlange(nf, def);
                case "edgeFlange": return ReplayEdgeFlange(nf, def);
                case "flatPattern": return ReplayFlatPattern(nf);
                case "sweep": return ReplaySweep(nf, def, false);
                case "sweepCut": return ReplaySweep(nf, def, true);
                case "loft": return ReplayLoft(nf, def, false);
                case "loftCut": return ReplayLoft(nf, def, true);
                case "combine": return ReplayCombine(nf, def);
                case "weldment": return ReplayWeldment(nf);
                case "structuralMember": return ReplayStructuralMember(nf, def);
                case "dome": return ReplayDome(nf, def);
                case "split": return ReplaySplit(nf, def);
                case "planarSurface": return ReplayPlanarSurface(nf, def);
                case "knit": return ReplayKnit(nf, def);
                case "wrap": return ReplayWrap(nf, def);
                case "unfold": return ReplayFolds(nf, def, false);
                case "fold": return ReplayFolds(nf, def, true);
                default: return "no replay for feature type " + nf.Str("type");
            }
        }

        // ------------------------------------------------------------------------- helpers
        IFeatureManager FM { get { return (IFeatureManager)doc.FeatureManager; } }

        /// <summary>True if the profile sketch was rebuilt with its normal reversed (directions must flip).</summary>
        bool ProfileMirrored(JObj profile)
        {
            SketchMap m;
            return profile != null && profile.Str("kind") == "feature" && sketchMaps.TryGetValue(profile.Str("id"), out m) && m.Mirrored;
        }

        int FeatureCount() { return FM.GetFeatureCount(false); }

        /// <summary>The newest feature: the flat pattern of a sheet-metal part stays last, so it is passed over.</summary>
        IFeature LastFeature()
        {
            for (int i = 0; ; i++)
            {
                var f = (IFeature)doc.FeatureByPositionReverse(i);
                if (f == null || f.GetTypeName2() != "FlatPattern") return f;
            }
        }

        void Name(IFeature f, string name)
        {
            if (f != null && !string.IsNullOrEmpty(name) && f.Name != name) f.Name = name;
        }

        SelectData Mark(int mark)
        {
            var sd = (SelectData)((ISelectionMgr)doc.SelectionManager).CreateSelectData();
            sd.Mark = mark;
            return sd;
        }

        /// <summary>Resolve a descriptor against the part's bodies. SolidWorks 2026 can still be finishing the previous
        /// feature's rebuild, which replaces the body objects mid-resolution (RPC_E_DISCONNECTED): rebuild and retry.</summary>
        JObj ResolveRef(JObj r, out List<object> ents)
        {
            try { return Host.Resolve(part, r, out ents); }
            catch (System.Runtime.InteropServices.COMException e) when ((uint)e.ErrorCode == 0x80010108)
            {
                doc.EditRebuild3();
                return Host.Resolve(part, r, out ents);
            }
        }

        /// <summary>Select a reference (plane, feature, or resolved face/edge). Returns null or why not.</summary>
        string SelectRef(JObj r, bool append, int mark)
        {
            switch (r.Str("kind"))
            {
                case "originPlane":
                {
                    int i = Array.IndexOf(new[] { "front", "top", "right" }, r.Str("plane"));
                    return originPlanes[i].Select2(append, mark) ? null : "could not select origin plane";
                }
                case "feature":
                {
                    IFeature f;
                    if (!features.TryGetValue(r.Str("id"), out f)) return "referenced feature " + r.Str("id") + " was not rebuilt";
                    return f.Select2(append, mark) ? null : "could not select feature " + f.Name;
                }
                case "transferred":
                {
                    IFeature tf;
                    string key = r.Str("document") + "|" + (r.Str("feature") ?? "plane:" + r.Str("plane"));
                    if (!transferredItems.TryGetValue(key, out tf)) tf = (IFeature)part.FeatureByName(r.Str("name"));
                    if (tf == null) return "transferred item " + r.Str("name") + " not found";
                    return tf.Select2(append, mark) ? null : "could not select " + tf.Name;
                }
                case "sketchEntity":
                {
                    SketchMap m;
                    if (!sketchMaps.TryGetValue(r.Str("feature"), out m)) return "sketch " + r.Str("feature") + " was not rebuilt";
                    var o = Resolve(m, r.Str("id"));
                    if (o == null) return "sketch entity " + r.Str("id") + " was not rebuilt";
                    bool ok = o is ISketchSegment ? ((ISketchSegment)o).Select4(append, Mark(mark)) : ((ISketchPoint)o).Select4(append, Mark(mark));
                    return ok ? null : "could not select sketch entity " + r.Str("id");
                }
                case "face":
                case "edge":
                case "vertex":   // resolved by point (FORMAT-SPEC 5)
                {
                    List<object> ents;
                    var res = ResolveRef(r, out ents);
                    if (res.Str("result") != "resolved")
                        return r.Str("kind") + " reference " + res.Str("result") + ": " + string.Join("; ", res.Arr("notes"));
                    foreach (var e in ents)
                    {
                        if (!((IEntity)e).Select4(append, Mark(mark))) return "could not select resolved " + r.Str("kind");
                        append = true;
                    }
                    return null;
                }
                default:
                    return "reference kind " + r.Str("kind") + " not supported";
            }
        }

        // -------------------------------------------------------------- parameters / equations
        void ReplayParameters(JObj d)
        {
            var em = (IEquationMgr)doc.GetEquationMgr();
            foreach (JObj p in d.Arr("parameters"))
            {
                string eq = "\"" + p.Str("name") + "\" = " + (p.Str("expression") ?? p.Num("value").ToString(System.Globalization.CultureInfo.InvariantCulture));
                if (em.Add2(-1, eq, true) < 0) Report.Add(docId, "parameter " + p.Str("name"), "failed", "could not add global variable");
            }
        }

        void ReplayEquations(JObj d)
        {
            var em = (IEquationMgr)doc.GetEquationMgr();
            foreach (JObj e in d.Arr("equations"))
            {
                string eq = e.Str("native") ?? ("\"" + e.Str("target") + "\" = " + e.Str("expression"));
                if (em.Add2(-1, eq, true) < 0) Report.Add(docId, "equation " + e.Str("target"), "failed", "could not add equation");
            }
        }

        // ------------------------------------------------------------------------ verification
        void Verify(JObj d)
        {
            var expected = new JObj { { "bodies", d.Arr("bodies").Select(b => ((JObj)b)["fingerprint"]).Where(x => x != null).ToList() } };
            var actual = Host.PartFingerprint(part);
            // another SolidWorks version fits a natural-end spline through the same points a little differently
            // (2018 vs 2026: ~1e-5..1e-4 in volume/area, a few hundredths of a mm along the curve): such a part is
            // compared at exchange tolerance and the difference reported, rather than replaced by plain geometry
            int srcApi = (int)(Model?.Obj("generator")?.Num("api") ?? 0);
            bool crossVersion = srcApi != 0 && srcApi != int.Parse(sw.RevisionNumber().Split('.')[0]);
            bool splines = d.Arr("features").Cast<JObj>().Any(nf => (nf.Obj("def")?.Arr("entities") ?? new List<object>())
                                                                    .Cast<JObj>().Any(e => e.Str("type") == "spline"));
            // a carried result came through STEP, which moves freeform faces slightly: exchange tolerance too
            string options = (crossVersion && splines) || resultsApplied ? "{\"profile\":\"exchange\"}" : null;
            var rep = (JObj)Json.Parse(NcxCore.FingerprintCompare(Json.Write(expected, true), Json.Write(actual, true), options).ReportJson);
            var entry = new JObj { { "document", docId }, { "result", rep.Str("result") }, { "differences", rep["differences"] },
                                   { "expected", expected }, { "actual", actual } };
            if (options != null)
            {
                entry["tolerance"] = "exchange";
                var native = (JObj)Json.Parse(NcxCore.FingerprintCompare(Json.Write(expected, true), Json.Write(actual, true)).ReportJson);
                if (native.Str("result") == "mismatch" && rep.Str("result") != "mismatch")
                    Report.Add(docId, "splines", "note", "SolidWorks " + sw.RevisionNumber().Split('.')[0] +
                               " fits natural-end splines slightly differently from the source's version; the part matches at exchange tolerance");
            }
            Verification.Add(entry);
        }
    }
}
