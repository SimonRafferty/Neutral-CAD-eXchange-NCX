// Exporter: a feature's resulting geometry (FORMAT-SPEC 4, "Feature results"). The bodies a feature changed, as they are just after
// it, go into the payload as a small STEP file, with the fingerprints of the bodies it replaced. An importer that
// cannot rebuild the feature (no counterpart in its CAD system, or the exporter could not map it) swaps those
// bodies in and carries on with the rest of the history: fixed geometry for that step, reported as approximate,
// instead of the whole part falling back to plain geometry.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        const int I_STEP_AP_PREF = 75;   // swUserPreferenceIntegerValue_e.swStepAP

        /// <summary>Which features carry their result: "opaque" (only those the exporter could not map),
        /// "extended" (also the feature types few CAD systems have; the default) or "all" (every geometric feature:
        /// for testing the importers' result path).</summary>
        public string Results = "extended";

        /// <summary>Native feature types exported as opaque whatever their mapping (testing the result path).</summary>
        public HashSet<string> ForceOpaque = new HashSet<string>();

        // SolidWorks feature types with no direct counterpart in most other CAD systems: an importer without them
        // still gets the right geometry from the result
        static readonly HashSet<string> ExtendedTypes = new HashSet<string>
        {
            "Hem", "Jog", "SMMiteredFlange", "CornerFeat", "BreakCorner", "SM3dBend", "LoftedBend", "EdgeFlange",
            "Gusset", "EndCap", "WeldCornerFeat", "Rib", "VarFillet", "Dome", "Emboss", "Deboss", "Scribe", "Wrap", "MirrorStock"
        };

        // feature types that never change a body
        static readonly HashSet<string> NoGeometry = new HashSet<string>
        {
            "ProfileFeature", "3DProfileFeature", "RefPlane", "RefAxis", "RefPoint", "CoordSys", "CosmeticThread",
            "WeldmentFeature", "FlatPattern", "SheetMetal", "Stock"
        };

        // set once a feature of the part is opaque: later features may depend on it, so they carry their results too
        bool opaqueSeen;

        /// <summary>Features whose export found something the API cannot rebuild (they carry their result).</summary>
        readonly HashSet<JObj> needsResult = new HashSet<JObj>();

        bool WantsResult(string type, JObj nf)
        {
            if (nf.Str("status") == "opaque") opaqueSeen = true;   // (a curve or sketch too: what is built on it may fail)
            if (NoGeometry.Contains(type)) return false;
            if (nf.Str("status") == "opaque" || opaqueSeen || needsResult.Contains(nf)) return true;
            // a full round cannot be made through SolidWorks' API (and few systems have one)
            if (nf.Obj("def")?.Str("filletType") == "fullRound") return true;
            return Results == "all" || (Results == "extended" && ExtendedTypes.Contains(type));
        }

        /// <summary>Attach the feature's result: the bodies just before and just after it are compared; the ones
        /// that differ are the ones it replaced (fingerprints) and the ones it made (STEP). Leaves the rollback bar
        /// just after the feature.</summary>
        void AttachResult(IFeature f, IFeatureManager fm, JObj nf)
        {
            fm.EditRollback(3, f.Name);   // swMoveRollbackBarToBeforeFeature
            var before = AllBodies().Select(Host.BodyFingerprint).ToList();
            fm.EditRollback(4, f.Name);   // swMoveRollbackBarToAfterFeature
            var after = AllBodies();
            var afterFp = after.Select(Host.BodyFingerprint).ToList();

            var keptBefore = new bool[before.Count];
            var made = new List<int>();
            for (int i = 0; i < after.Count; i++)
            {
                int same = -1;
                for (int j = 0; j < before.Count && same < 0; j++)
                    if (!keptBefore[j] && SameBody(before[j], afterFp[i])) same = j;
                if (same >= 0) keptBefore[same] = true;
                else made.Add(i);
            }
            var replaced = Enumerable.Range(0, before.Count).Where(j => !keptBefore[j]).Select(j => (object)before[j]).ToList();
            if (made.Count == 0 && replaced.Count == 0) return;   // no change to any body

            string step = BodiesAsStep(made.Select(i => after[i]).ToList());
            if (step == null)
            {
                Report.Add(docId, f.Name, "note", "its resulting bodies could not be written as STEP; no result carried");
                return;
            }
            nf["result"] = new JObj
            {
                { "format", "step" },
                { "replaces", replaced },
                { "bodies", made.Select(i => (object)new JObj { { "name", after[i].Name }, { "sheet", after[i].GetType() == 1 },
                                                                 { "fingerprint", afterFp[i] } }).ToList() },
                { "data", step }
            };
        }

        List<IBody2> AllBodies()
        {
            return ((object[])part.GetBodies2(-1, false) ?? new object[0]).Cast<IBody2>().ToList();   // solids and surfaces
        }

        /// <summary>The same body: equal volume, area and centroid (a body the feature did not touch).</summary>
        static bool SameBody(JObj a, JObj b)
        {
            Func<double, double, bool> eq = (x, y) => Math.Abs(x - y) <= 1e-9 * Math.Max(1, Math.Max(Math.Abs(x), Math.Abs(y)));
            var ca = a.Vec("centroid");
            var cb = b.Vec("centroid");
            return eq(a.Num("volume"), b.Num("volume")) && eq(a.Num("area"), b.Num("area")) &&
                   (int)a.Num("faces") == (int)b.Num("faces") && (int)a.Num("edges") == (int)b.Num("edges") &&
                   Enumerable.Range(0, 3).All(k => Math.Abs(ca[k] - cb[k]) <= 1e-6);
        }

        /// <summary>Copies of the bodies in a scratch part, saved as AP214 STEP; returns the file's text, or null.
        /// An empty list gives an empty string (the feature only removed bodies).</summary>
        string BodiesAsStep(List<IBody2> bodies)
        {
            if (bodies.Count == 0) return "";
            var scratch = (IModelDoc2)sw.NewDocument(sw.GetUserPreferenceStringValue(8), 0, 0, 0);   // swDefaultTemplatePart
            if (scratch == null) return null;
            string tmp = Path.Combine(Path.GetTempPath(), "ncx_result_" + Guid.NewGuid().ToString("N") + ".step");
            int savedAp = sw.GetUserPreferenceIntegerValue(I_STEP_AP_PREF);
            try
            {
                var sp = (IPartDoc)scratch;
                foreach (var b in bodies)
                {
                    if (Host.AddBody(scratch, b) == null) return null;
                }
                // the copies are named after the scratch feature: give them the source names
                var copies = ((object[])sp.GetBodies2(-1, false) ?? new object[0]).Cast<IBody2>().ToList();
                for (int i = 0; i < copies.Count && i < bodies.Count; i++)
                    try { copies[i].Name = bodies[i].Name; } catch (System.Runtime.InteropServices.COMException) { }
                sw.SetUserPreferenceIntegerValue(I_STEP_AP_PREF, 214);
                int err = 0, warn = 0;
                if (!scratch.Extension.SaveAs(tmp, 0, 1 | 2, null, ref err, ref warn) || !File.Exists(tmp)) return null;
                return File.ReadAllText(tmp);
            }
            finally
            {
                sw.SetUserPreferenceIntegerValue(I_STEP_AP_PREF, savedAp);
                sw.CloseDoc(scratch.GetTitle());
                if (File.Exists(tmp)) File.Delete(tmp);
            }
        }
    }
}
