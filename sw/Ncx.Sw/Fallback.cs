// Replayer: fall back to the reference B-rep carried in the same file (DESIGN section 6). When a part
// cannot be rebuilt faithfully, the user still gets exactly the right geometry - imported from the
// container's own STEP - under the same name, so derived parts and assemblies keep working, and the
// partial parametric rebuild is kept beside it for repair.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        const int T_INTERCONNECT = 691;   // swMultiCAD_Enable3DInterconnect
        public string ContainerPath;       // the NCX file being replayed (its B-rep is the fallback)
        string refDir;
        readonly Dictionary<string, string> refParts = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        // the imported reference parts themselves (open while the replay runs), for face-by-face verification
        string rootRefPath;   // the saved reference when the file's root is a part
        // each saved reference part's fingerprint (path -> fingerprint): a writer may name its STEP products other
        // than its documents (Fusion writes time stamps), so a part is also found by its geometry
        readonly Dictionary<string, JObj> refFingerprints = new Dictionary<string, JObj>(StringComparer.OrdinalIgnoreCase);
        JObj referenceFingerprint;   // measured on the B-rep actually in the file (the stale check)
        IModelDoc2 referenceRoot;     // the imported reference, kept open for a whole-file fallback

        /// <summary>Import the container's B-rep once and save each product's geometry as a part:
        /// name -> path of "&lt;name&gt;__ref.SLDPRT".</summary>
        void ImportReference(string outDir)
        {
            if (refDir != null || ContainerPath == null) return;
            refDir = Path.Combine(outDir, "_reference");   // helper parts, kept out of the user's results
            Directory.CreateDirectory(refDir);
            bool ic = sw.GetUserPreferenceToggle(T_INTERCONNECT);
            sw.SetUserPreferenceToggle(T_INTERCONNECT, false);   // real bodies, not a linked import
            IModelDoc2 imp = null;
            var opened = new List<IModelDoc2>();
            try
            {
                int err = 0;
                object data = sw.GetImportFileData(ContainerPath);   // typed, or LoadFile4 binds late and fails
                imp = (IModelDoc2)sw.LoadFile4(ContainerPath, "r", data, ref err);
                if (imp == null) return;
                // the notice body (FORMAT-SPEC 2.6) is for readers without NCX: never part of the reference
                RemoveNotice(imp, NoticeName(ContainerPath));
                imp.ForceRebuild3(false);
                referenceFingerprint = Host.DocFingerprint(imp);
                if (imp.GetType() == 2)
                {
                    // names of products really in the file (a "<name>_1" product is also filed under "<name>", below;
                    // that alias must not hide a real product of that name: Fusion's time-stamp names collide that way)
                    var saved = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    foreach (IComponent2 c in ((object[])((IAssemblyDoc)imp).GetComponents(false) ?? new object[0]))
                    {
                        var md = (IModelDoc2)c.GetModelDoc2();
                        if (md == null || md.GetType() != 1) continue;
                        string name = System.Text.RegularExpressions.Regex.Replace(c.Name2.Split('/').Last(), @"-\d+$", "");
                        if (!saved.Add(name)) continue;
                        refParts[name] = SaveRef(md, name);
                        if (refParts[name] != null)
                            try { refFingerprints[refParts[name]] = Host.PartFingerprint((IPartDoc)md); } catch (Exception) { }
                        // a part already open under its own name (the rebuilt one) is imported as "<name>_1": file it
                        // under the payload's name too, or the fallback would not find it
                        string bare = System.Text.RegularExpressions.Regex.Replace(name, @"_\d+$", "");
                        if (bare != name && !refParts.ContainsKey(bare)) refParts[bare] = refParts[name];
                    }
                }
                else
                {
                    rootRefPath = SaveRef(imp, "__root");   // the file's B-rep is the root part's
                    refParts[Path.GetFileNameWithoutExtension(ContainerPath)] = rootRefPath;
                }
            }
            finally
            {
                sw.SetUserPreferenceToggle(T_INTERCONNECT, ic);
                if (imp != null) referenceRoot = imp;
            }
        }

        /// <summary>The notice body's name from the file's header (notice=...), or null if it has none.</summary>
        static string NoticeName(string containerPath)
        {
            var rep = (JObj)Json.Parse(NcxCore.PayloadRead(File.ReadAllBytes(containerPath)).ReportJson);
            return rep.Obj("fields")?.Str("notice");
        }

        /// <summary>A name in the form the header carries it (FORMAT-SPEC 2.2, 2.6): ; ' \ and non-printing characters
        /// as _, and path characters as _ (as an assembly's notice component has them). Spaces are dropped:
        /// SolidWorks 2018 loses the space after a quoted ' in a STEP name.</summary>
        static string NoticeForm(string s)
        {
            return string.Concat((s ?? "").Where(c => c != ' ')
                                          .Select(c => c < 0x20 || c > 0x7E || ";'\\/:*?\"<>|".IndexOf(c) >= 0 ? '_' : c));
        }

        /// <summary>Is this body or component name the notice? Compared in header form; an importer may shorten a
        /// long name, so a name that is the start of it (7 characters or more) counts too.</summary>
        static bool IsNotice(string name, string notice)
        {
            string n = NoticeForm(name), h = NoticeForm(notice);
            return n == h || (n.Length >= 7 && h.StartsWith(n)) || n.StartsWith(h);
        }

        /// <summary>Delete the notice from an imported part (a body) or assembly (a top-level component).</summary>
        static void RemoveNotice(IModelDoc2 imported, string notice)
        {
            if (string.IsNullOrEmpty(notice)) return;
            if (imported.GetType() == 2)
            {
                imported.ClearSelection2(true);
                bool sel = false;
                foreach (IComponent2 c in (object[])((IAssemblyDoc)imported).GetComponents(true) ?? new object[0])
                {
                    string n = System.Text.RegularExpressions.Regex.Replace(c.Name2 ?? "", @"-\d+$", "");
                    if (IsNotice(n, notice)) sel |= c.Select4(sel, null, false);
                }
                if (sel) imported.Extension.DeleteSelection2(0);
                imported.ClearSelection2(true);
                return;
            }
            var part = imported as IPartDoc;
            if (part == null) return;
            imported.ClearSelection2(true);
            bool any = false;
            foreach (IBody2 b in (object[])part.GetBodies2(0, false) ?? new object[0])
            {
                if (IsNotice(b.Name, notice)) any |= b.Select2(any, null);
            }
            if (any) ((IFeatureManager)imported.FeatureManager).InsertDeleteBody2(false);   // delete the selected bodies
            imported.ClearSelection2(true);
        }

        /// <summary>The saved reference part whose geometry is this document's (its payload fingerprint, at the
        /// exchange tolerances the stale check uses), or null.</summary>
        string ReferenceByGeometry(JObj d)
        {
            // the document's bodies' fingerprints, as Verify compares them
            var fps = d.Arr("bodies").Select(x => ((JObj)x)["fingerprint"]).Where(x => x != null).ToList();
            if (fps.Count == 0) return null;
            var fp = new JObj { { "bodies", fps } };
            foreach (var kv in refFingerprints)
            {
                try
                {
                    var rep = (JObj)Json.Parse(NcxCore.FingerprintCompare(Json.Write(fp, true), Json.Write(kv.Value, true),
                                                                          "{\"profile\":\"exchange\"}").ReportJson);
                    if (rep.Str("result") != "mismatch") return kv.Key;
                }
                catch (Exception) { }
            }
            return null;
        }

        void CloseReference()
        {
            if (referenceRoot != null) sw.CloseDoc(referenceRoot.GetTitle());
            referenceRoot = null;
        }

        /// <summary>The stale-payload check (FORMAT-SPEC 6): does the payload describe the B-rep in the file?
        /// Returns null if it does, else the reason it does not.</summary>
        string StaleCheck(JObj model, string outDir)
        {
            ImportReference(outDir);
            if (referenceFingerprint == null || model.Obj("fingerprint") == null) return null;   // nothing to compare
            // the file's B-rep went through STEP: freeform faces drift slightly, hence the exchange tolerances
            var rep = (JObj)Json.Parse(NcxCore.FingerprintCompare(Json.Write(model.Obj("fingerprint"), true),
                                                                   Json.Write(referenceFingerprint, true),
                                                                   "{\"profile\":\"exchange\"}").ReportJson);
            Verification.Add(new JObj { { "document", "(file B-rep vs payload)" }, { "result", rep.Str("result") },
                                        { "differences", rep["differences"] } });
            if (rep.Str("result") != "mismatch") return null;
            var what = ((List<object>)rep["differences"] ?? new List<object>()).Cast<JObj>()
                .Where(x => x.Str("severity") == "mismatch").Select(x => x.Str("quantity")).Distinct();
            return "the geometry in the file no longer matches the payload (" + string.Join(", ", what) + ")";
        }

        /// <summary>Whole-file fallback for a stale payload: the plain imported geometry, saved under the root's name.</summary>
        string PlainImport(JObj model, string outDir)
        {
            var root = model.Arr("documents").Cast<JObj>().FirstOrDefault(d => d.Str("id") == model.Str("root"));
            string ext = referenceRoot.GetType() == 2 ? ".SLDASM" : ".SLDPRT";
            string path = Path.Combine(outDir, (root?.Str("name") ?? Path.GetFileNameWithoutExtension(ContainerPath)) + ext);
            int err = 0, warn = 0;
            if (File.Exists(path)) File.Delete(path);
            referenceRoot.Extension.SaveAs(path, 0, 1, null, ref err, ref warn);
            referenceRoot = null;   // now the user's result; stays open
            return path;
        }

        string SaveRef(IModelDoc2 md, string name)
        {
            string path = Path.Combine(refDir, name + "__ref.SLDPRT");
            int err = 0, warn = 0;
            // SolidWorks keeps a file it opened read-only locked after closing it (until it exits): a reference copy
            // from an earlier import of the same file in this session may not be replaceable; then a fresh name
            for (int n = 2; File.Exists(path); n++)
            {
                try { File.Delete(path); }
                catch (IOException) { path = Path.Combine(refDir, name + "__ref_" + n + ".SLDPRT"); }
                catch (UnauthorizedAccessException) { path = Path.Combine(refDir, name + "__ref_" + n + ".SLDPRT"); }
            }
            return md.Extension.SaveAs(path, 0, 1 | 2, null, ref err, ref warn) ? path : null;   // silent copy
        }

        /// <summary>The reference part file (the file's own B-rep) of a part document, or null.</summary>
        string ReferenceFor(JObj d, string outDir, bool singlePartFile)
        {
            ImportReference(outDir);
            string refPath;
            if (!refParts.TryGetValue(d.Str("name"), out refPath) && !(singlePartFile && refParts.TryGetValue("__root_any", out refPath)))
                refPath = singlePartFile ? refParts.Values.FirstOrDefault() : null;
            // a part file that also carries other documents (the layout it is derived from): its B-rep is the root's
            if (refPath == null && rootRefPath != null && d.Str("id") == Model?.Str("root")) refPath = rootRefPath;
            if (refPath == null) refPath = ReferenceByGeometry(d);
            return refPath;
        }

        /// <summary>A new part holding the reference geometry (solid bodies, link broken); inserted says whether the
        /// geometry went in (the document is returned either way).</summary>
        IModelDoc2 FromReference(string refPath, out bool inserted)
        {
            var nd = (IModelDoc2)sw.NewDocument(sw.GetUserPreferenceStringValue(8), 0, 0, 0);
            created.Add(nd);
            quiet?.Add(nd);
            int n0 = ((IFeatureManager)nd.FeatureManager).GetFeatureCount(false);
            ((IPartDoc)nd).InsertPart2(refPath, 1 | 512);
            inserted = ((IFeatureManager)nd.FeatureManager).GetFeatureCount(false) != n0;
            return nd;
        }

        /// <summary>A plain part (FORMAT-SPEC 4: no history in the source, imported geometry only): the file's own
        /// bodies as they are, with its properties; nothing rebuilt or compared, a note rather than a fallback.</summary>
        IModelDoc2 PlainPart(JObj d, string outDir, bool singlePartFile)
        {
            string refPath = ReferenceFor(d, outDir, singlePartFile);
            bool inserted = false;
            var nd = refPath == null ? null : FromReference(refPath, out inserted);
            if (!inserted)
            {
                Report.Add(d.Str("id"), d.Str("name"), "failed", "a plain part (no history in the source), but the file's geometry for it could not be used");
                return nd ?? (IModelDoc2)sw.NewDocument(sw.GetUserPreferenceStringValue(8), 0, 0, 0);
            }
            doc = nd;   // the part the property and appearance replays work on
            part = nd as IPartDoc;
            ReplayPropertiesAndMaterials(nd, d);
            ReplayAppearances(nd, d);
            Report.Add(d.Str("id"), d.Str("name"), "note", "no history in the source (imported geometry): its bodies as the file has them");
            Verification.Add(new JObj { { "document", d.Str("id") }, { "result", "plain" } });
            return nd;
        }

        /// <summary>Replace a part that could not be rebuilt faithfully by its reference geometry.
        /// Returns the new document, or the original one if no reference geometry is available.</summary>
        IModelDoc2 FallBack(JObj d, IModelDoc2 partial, string outDir, string reason, bool singlePartFile)
        {
            string refPath = ReferenceFor(d, outDir, singlePartFile);
            if (refPath == null)
            {
                Report.Add(d.Str("id"), d.Str("name"), "failed", reason + "; no reference geometry for a fallback");
                return partial;
            }
            // keep the partial parametric rebuild beside the result
            string keep = Path.Combine(outDir, d.Str("name") + "__partial.SLDPRT");
            int err = 0, warn = 0;
            if (File.Exists(keep)) File.Delete(keep);
            partial.Extension.SaveAs(keep, 0, 1 | 2, null, ref err, ref warn);
            sw.CloseDoc(partial.GetTitle());

            bool inserted;
            var nd = FromReference(refPath, out inserted);   // solid bodies, link broken: plain imported geometry
            if (!inserted)
            {
                Report.Add(d.Str("id"), d.Str("name"), "failed", reason + "; reference geometry could not be inserted");
                return nd;
            }
            Report.Add(d.Str("id"), d.Str("name"), "fallback",
                       "imported reference geometry used (" + reason + "); partial parametric rebuild kept as " + Path.GetFileName(keep));
            return nd;
        }
    }
}
