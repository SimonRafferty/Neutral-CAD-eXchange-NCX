// Replayer: dome, split body, planar surface, knit, wrap, and sheet-metal unfold / fold (FORMAT-SPEC 4,
// "Feature definitions").
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
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("tools"), false, 0);
            if (why != null) return "split tool: " + why;
            var bodies = (object[])FM.PreSplitBody();
            if (bodies == null || bodies.Length == 0) { doc.ClearSelection2(true); return "the split tools cut no body"; }
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
        string ReplayFolds(JObj nf, JObj def, bool fold)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("fixedFace"), false, 1);
            if (why != null) return "fixed face: " + why;
            foreach (JObj b in def.Arr("bends"))
            {
                var bend = Bend(b);
                if (bend == null) return "bend " + b.Str("name") + " was not rebuilt";
                if (!bend.Select2(true, 4)) return "could not select bend " + bend.Name;
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
            if (!features.TryGetValue(b.Str("feature"), out owner)) return null;
            int i = 0, want = (int)b.Num("sub");
            for (var sub = (IFeature)owner.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature(), i++)
                if (i == want) return sub;
            return null;
        }
    }
}
