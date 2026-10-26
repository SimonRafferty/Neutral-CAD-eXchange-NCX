// Replayer: dome, split body, planar surface, knit, wrap, and sheet-metal unfold / fold (FORMAT-SPEC 4,
// "Feature definitions").
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        string ReplayDome(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("faces"), false, 1);   // the dome face takes mark 1 (mark 0 makes nothing)
            if (why != null) return "dome face: " + why;
            int n0 = FeatureCount();
            doc.InsertDome(U.M(def.Num("height")), def.Bool("reverse"), def.Bool("elliptical"));
            return Created(n0, null, nf, "dome");
        }

        string ReplaySplit(JObj nf, JObj def)
        {
            // the bodies the split is limited to (FORMAT-SPEC 4 split `bodies`; absent: every body the tools cut), by
            // their boxes before the split
            var targetBoxes = new List<double[]>();
            foreach (JObj r in def.Arr("bodies"))
            {
                var tb = FindBody(r);
                if (tb == null) return "body to split not found";
                targetBoxes.Add((double[])tb.GetBodyBox());
            }
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("tools"), false, 0);
            if (why != null) return "split tool: " + why;
            var bodies = (object[])FM.PreSplitBody();
            if (bodies == null || bodies.Length == 0) { doc.ClearSelection2(true); return "the split tools cut no body"; }
            if (targetBoxes.Count > 0)
            {
                // only the pieces of the listed bodies are marked: the others stay whole
                Func<double[], double[], bool> within = (a, b) => a[0] >= b[0] - 1e-6 && a[1] >= b[1] - 1e-6 && a[2] >= b[2] - 1e-6 &&
                                                                  a[3] <= b[3] + 1e-6 && a[4] <= b[4] + 1e-6 && a[5] <= b[5] + 1e-6;
                bodies = bodies.Where(b => targetBoxes.Any(tb => within((double[])((IBody2)b).GetBodyBox(), tb))).ToArray();
                if (bodies.Length == 0) { doc.ClearSelection2(true); return "the split tools cut none of its bodies"; }
            }
            int n0 = FeatureCount();
            // every resulting body kept; origins as null dispatches and save paths as empty strings (anything else
            // makes nothing)
            FM.PostSplitBody(bodies.Select(b => new DispatchWrapper(b)).ToArray(), def.Bool("consume"),
                             bodies.Select(b => new DispatchWrapper(null)).ToArray(), bodies.Select(b => "").ToArray());
            return Created(n0, null, nf, "split");
        }

        string ReplayPlanarSurface(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectProfile(def);
            if (why != null) return "profile: " + why;
            int n0 = FeatureCount();
            doc.InsertPlanarRefSurface();
            return Created(n0, null, nf, "planar surface");
        }

        string ReplayKnit(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            bool append = false;
            foreach (JObj r in def.Arr("surfaces"))   // surfaces take mark 1 (mark 0 makes nothing)
            {
                if (r.Str("kind") == "body")
                {
                    var b = FindBody(r);
                    if (b == null) return "surface to knit not found";
                    if (!b.Select2(append, Mark(1))) return "could not select a surface to knit";
                }
                else
                {
                    string why = SelectRef(r, append, 1);
                    if (why != null) return "surface to knit: " + why;
                }
                append = true;
            }
            int n0 = FeatureCount();
            FM.InsertSewRefSurface(def.Bool("gapControl"), def.Bool("formSolid"), def.Bool("mergeEntities"),
                                   U.M(def.Num("tolerance", 0.1)), U.M(def.Num("maxGap", 0.1)));
            return Created(n0, null, nf, "knit");
        }

        string ReplayWrap(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("sketch"), false, 4);   // the sketch mark 4, the face mark 1
            if (why == null) why = SelectRef(def.Obj("face"), true, 1);
            if (why != null) return "wrap: " + why;
            int n0 = FeatureCount();
            FM.InsertWrapFeature2((int)def.Num("nativeType", 1), U.M(def.Num("thickness")), def.Bool("reverse"), 0, 0);
            return Created(n0, null, nf, "wrap");
        }

        /// <summary>Sheet-metal unfold or fold: the fixed face (mark 1) and the bends (mark 4).</summary>
        /// <summary>The bends each unfold took (by its feature id): a fold that reverses it takes them again.</summary>
        readonly Dictionary<string, List<IFeature>> unfoldBends = new Dictionary<string, List<IFeature>>();

        string ReplayFolds(JObj nf, JObj def, bool fold)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("fixedFace"), false, 1);
            if (why != null) return "fixed face: " + why;
            List<IFeature> again;
            string uid = def.Obj("unfold")?.Str("id");
            if (fold && uid != null && unfoldBends.TryGetValue(uid, out again) && again.Count > 0)
            {
                // the unfold it reverses: its bends (another system's flat strips may not be faces of their own)
                foreach (var bend in again)
                    if (!bend.Select2(true, 4)) return "could not select bend " + bend.Name + " of the unfold";
                int m0 = FeatureCount();
                doc.InsertSheetMetalFold();
                return Created(m0, null, nf, "fold");
            }
            foreach (JObj b in def.Arr("bends"))
            {
                var bend = Bend(b);
                if (bend == null && b.Obj("face") != null)
                {
                    // a system without bend sub-features (FORMAT-SPEC 4: found by its face): the bend's face
                    // selects the bend, as a click on it does
                    string w = SelectRef(b.Obj("face"), true, 4);
                    if (w != null) return "bend " + (b.Str("name") ?? "") + ": " + w;
                    continue;
                }
                if (bend == null) return "bend " + b.Str("name") + " was not rebuilt";
                if (!bend.Select2(true, 4)) return "could not select bend " + bend.Name;
            }
            if (!fold)
            {
                // the bends taken, as features (a selected bend face stands for the bend it belongs to)
                var sm = (ISelectionMgr)doc.SelectionManager;
                var taken = new List<IFeature>();
                for (int i = 1; i <= sm.GetSelectedObjectCount2(-1); i++)
                {
                    if (sm.GetSelectedObjectMark(i) != 4) continue;
                    object o = sm.GetSelectedObject6(i, -1);
                    var bf = o as IFeature ?? (o as IFace2)?.GetFeature() as IFeature;
                    if (bf != null) taken.Add(bf);
                }
                unfoldBends[nf.Str("id")] = taken;
            }
            int n0 = FeatureCount();
            if (fold) doc.InsertSheetMetalFold();
            else doc.InsertSheetMetalUnfold();
            return Created(n0, null, nf, fold ? "fold" : "unfold");
        }

        /// <summary>A bend: the sub-feature at its index under the rebuilt feature it belongs to.</summary>
        IFeature Bend(JObj b)
        {
            IFeature owner;
            if (b.Str("feature") == null || !features.TryGetValue(b.Str("feature"), out owner)) return null;   // none: found by its face
            int i = 0, want = (int)b.Num("sub");
            for (var sub = (IFeature)owner.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature(), i++)
                if (i == want) return sub;
            return null;
        }
    }
}
