// Replayer: sweeps and lofts. Selection marks: sweep profile 1, guide curves 2, path 4; loft sections 1, in order,
// guide curves 2, centreline 4.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        string ReplaySweep(JObj nf, JObj def, bool cut)
        {
            doc.ClearSelection2(true);
            var circ = def.Obj("circularProfile");
            string why;
            if (circ == null && (why = SelectRef(def.Obj("profile"), false, 1)) != null) return "profile: " + why;
            if ((why = SelectRef(def.Obj("path"), circ == null, 4)) != null) return "path: " + why;
            if ((why = SelectAll(def.Arr("guides"), true, 2)) != null) return "guide curve: " + why;
            int n0 = FeatureCount();
            bool propagate = def.Bool("tangentPropagation"), align = def.Bool("alignWithEndFaces"), keep = def.Bool("maintainTangency");
            bool smooth = def.Bool("advancedSmoothing"), mergeSmooth = def.Bool("mergeSmoothFaces", true);
            int twist = (int)def.Num("twistControl"), pathAlign = (int)def.Num("pathAlignment");
            int start = (int)def.Num("startTangency"), end = (int)def.Num("endTangency"), dir = (int)def.Num("direction");
            double twistAngle = U.Rad(def.Num("twistAngle"));
            double dia = circ == null ? 0 : U.M(circ.Num("diameter"));
            IFeature feat;
            if (cut)
                feat = FM.InsertCutSwept5(propagate, align, (short)twist, keep, smooth, (short)start, (short)end, false, 0, 0, 0,
                                          (short)pathAlign, true, true, twistAngle, mergeSmooth, false, false, false,
                                          circ != null, dia, dir);
            else
                feat = FM.InsertProtrusionSwept4(propagate, align, (short)twist, keep, smooth, (short)start, (short)end, false, 0, 0, 0,
                                                 (short)pathAlign, def.Bool("merge", true), true, true, twistAngle, mergeSmooth,
                                                 circ != null, dia, dir);
            return Created(n0, feat, nf, cut ? "cut sweep" : "sweep");
        }

        string ReplayLoft(JObj nf, JObj def, bool cut)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("sections"), false, 1);
            if (why != null) return "section: " + why;
            if ((why = SelectAll(def.Arr("guides"), true, 2)) != null) return "guide curve: " + why;
            if (def.Has("centerline") && (why = SelectRef(def.Obj("centerline"), true, 4)) != null) return "centreline: " + why;
            int n0 = FeatureCount();
            bool closed = def.Bool("closed"), keep = def.Bool("maintainTangency");
            short start = (short)def.Num("startTangency"), end = (short)def.Num("endTangency");
            IFeature feat;
            if (cut)
                feat = FM.InsertCutBlend(closed, keep, false, 1.0, start, end, false, 0, 0, 0, true, true);
            else
                feat = FM.InsertProtrusionBlend2(closed, keep, false, 1.0, start, end, def.Num("startTangentLength", 1),
                                                 def.Num("endTangentLength", 1), def.Bool("reverseStartTangent"),
                                                 def.Bool("reverseEndTangent"), false, 0, 0, 0, def.Bool("merge", true), true, true, 0);
            return Created(n0, feat, nf, cut ? "cut loft" : "loft");
        }
    }
}
