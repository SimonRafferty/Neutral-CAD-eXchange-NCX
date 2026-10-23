// Replayer: weldment trim/extend and end caps, sheet-metal lofted bends, extruded and revolved surfaces
// (FORMAT-SPEC 4.9).
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        string ReplayWeldmentTrim(JObj nf, JObj def)
        {
            var trimmed = new List<DispatchWrapper>();
            foreach (JObj r in def.Arr("trimmed"))
            {
                var b = FindBody(r);
                if (b == null) return "body to trim not found";
                trimmed.Add(new DispatchWrapper(b));
            }
            var boundary = new List<DispatchWrapper>();
            foreach (JObj r in def.Arr("boundary"))
            {
                if (r.Str("kind") == "body")
                {
                    var b = FindBody(r);
                    if (b == null) return "trim boundary body not found";
                    boundary.Add(new DispatchWrapper(b));
                    continue;
                }
                List<object> ents;
                var res = ResolveRef(r, out ents);
                if (res.Str("result") != "resolved") return "trim boundary face " + res.Str("result");
                boundary.AddRange(ents.Select(e => new DispatchWrapper(e)));
            }
            doc.ClearSelection2(true);
            int n0 = FeatureCount();
            // options 2: trim where the bodies pass the boundary (the extension and gap options are not carried)
            var feat = FM.InsertWeldmentTrimFeature2(Neutral.Code("trimCorner", def["cornerType"], 4), 2, 0, trimmed.ToArray(), boundary.ToArray());
            return Created(n0, feat, nf, "weldment trim");
        }

        string ReplayEndCap(JObj nf, JObj def)
        {
            List<object> ents;
            var res = ResolveRef(def.Obj("face"), out ents);
            if (res.Str("result") != "resolved") return "end cap face " + res.Str("result");
            doc.ClearSelection2(true);
            int n0 = FeatureCount();
            var feat = FM.InsertEndCapFeature2(U.M(def.Num("depth")), !def.Bool("offsetByRatio"), def.Bool("chamfer"),
                                               U.M(def.Num("offset")), def.Num("ratio"), U.M(def.Num("chamferDistance")),
                                               ents.Select(e => new DispatchWrapper(e)).ToArray());
            return Created(n0, feat, nf, "end cap");
        }

        string ReplayLoftedBend(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            bool append = false;
            foreach (JObj r in def.Arr("profiles"))
            {
                string why = SelectRef(r, append, 1);
                if (why != null) return "lofted bend profile: " + why;
                append = true;
            }
            int n0 = FeatureCount();
            double t = U.M(def.Num("thickness"));
            // the bend radius is the sheet-metal default (applied from the defaults marker); the thickness as a start
            var feat = FM.InsertSheetMetalLoftedBend2(0, t, def.Bool("formed"), t, def.Bool("referToEndPoint"),
                                                      (int)def.Num("faceting"), U.M(def.Num("maxDeviation")),
                                                      (int)def.Num("bendLines", 12), def.Num("facetValue"), 0.2);
            string err = Created(n0, feat, nf, "lofted bend");
            if (err != null) return err;
            if (pendingSheetMetal != null)   // the defaults marker, as after a base flange
            {
                var sm = FindFeature("SheetMetal", x => !Registered(x));
                if (sm == null) return "the lofted bend made no sheet-metal feature";
                var p = pendingSheetMetal;
                pendingSheetMetal = null;
                return ApplySheetMetalDefaults(sm, p);
            }
            return null;
        }

        string ReplayExtrudedSurface(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectProfile(def);
            if (why != null) return "profile: " + why;
            var e1 = def.Obj("end");
            var e2 = def.Obj("end2");
            int t1, t2 = 0;
            if (!EndTypes.TryGetValue(e1.Str("type"), out t1)) return "end condition " + e1.Str("type") + " not supported";
            if (e2 != null && !EndTypes.TryGetValue(e2.Str("type"), out t2)) return "end condition " + e2.Str("type") + " not supported";
            bool reverse = def.Bool("reverse") != ProfileMirrored(def.Obj("profile"));
            int n0 = FeatureCount();
            FM.FeatureExtruRefSurface2(e2 == null, false, reverse, t1, t2, U.M(e1.Num("depth")), e2 == null ? 0 : U.M(e2.Num("depth")),
                                       false, false, false, false, 0, 0, false, false, false, false, false, false, false, false);
            return Created(n0, null, nf, "extruded surface");
        }

        string ReplayRib(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectProfile(def);
            if (why != null) return "profile: " + why;
            // a mirrored sketch frame swaps the sides: the fill direction and a one-sided rib's side follow it
            bool mirrored = ProfileMirrored(def.Obj("profile"));
            var draft = def.Obj("draft");
            int n0 = FeatureCount();
            FM.InsertRib(def.Bool("twoSided"), def.Bool("reverseThickness") != mirrored, U.M(def.Num("thickness")), 0,
                         def.Bool("flipMaterial") != mirrored, draft != null, draft?.Bool("outward") ?? false,
                         U.Rad(draft?.Num("angle") ?? 0), Neutral.Code("ribExtrusion", def["extrusion"]) == 1, draft?.Bool("fromWall") ?? false);
            return Created(n0, null, nf, "rib");
        }

        string ReplayMoveBody(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            foreach (JObj r in def.Arr("bodies"))
            {
                var b = FindBody(r);
                if (b == null) return "body to move not found";
                if (!b.Select2(true, Mark(1))) return "could not select the body to move";
            }
            var t = def.Vec("translate") ?? new double[3];
            var rot = def.Obj("rotate");
            var o = rot?.Vec("origin") ?? new double[3];
            var a = rot?.Vec("angles") ?? new double[3];
            int n0 = FeatureCount();
            // InsertMoveCopyBody2 takes its angles in z, y, x order (its "RotAngleX" turns about z), whereas the feature
            // data - and the payload - give them about x, y, z
            FM.InsertMoveCopyBody2(U.M(t[0]), U.M(t[1]), U.M(t[2]), 0, U.M(o[0]), U.M(o[1]), U.M(o[2]),
                                   U.Rad(a[2]), U.Rad(a[1]), U.Rad(a[0]), def.Bool("copy"), (int)def.Num("copies", 1));
            return Created(n0, null, nf, "move/copy body");
        }

        /// <summary>A weldment gusset: made on its two faces, then every value set through its data (how the creation
        /// call's arguments map to the profile is not documented).</summary>
        string ReplayGusset(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("faces"), false, 0);
            if (why != null) return "gusset face: " + why;
            int n0 = FeatureCount();
            FM.InsertGussetFeature(U.M(def.Num("thickness")), (short)Neutral.Code("gussetThickness", def["thicknessType"]), (short)Neutral.Code("gussetLocation", def["location"]), true,
                                   U.M(def.Num("d1")), U.M(def.Num("d2")), U.M(def.Num("d3")), U.Rad(def.Num("angle")), U.M(def.Num("d4")),
                                   def.Bool("offset"), U.M(def.Num("offsetDistance")), 0, false, false, false);
            string err = Created(n0, null, nf, "gusset");
            if (err != null) return err;
            var feat = features[nf.Str("id")];
            var d = (IGussetFeatureData)feat.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            d.ProfileType = Neutral.Code("gussetProfile", def["profileType"]);
            d.ProfileDistance1 = U.M(def.Num("d1"));
            d.ProfileDistance2 = U.M(def.Num("d2"));
            d.ProfileDistance3 = U.M(def.Num("d3"));
            d.ProfileDistance4 = U.M(def.Num("d4"));
            d.ProfileAngle = U.Rad(def.Num("angle"));
            d.UseAngle = def.Bool("useAngle");
            d.ThicknessType = Neutral.Code("gussetThickness", def["thicknessType"]);
            d.Thickness = U.M(def.Num("thickness"));
            d.ProfileLocation = Neutral.Code("gussetLocation", def["location"]);
            d.OffsetUsed = def.Bool("offset");
            d.ProfileOffsetDistance = U.M(def.Num("offsetDistance"));
            d.FlipOffsetDirection = def.Bool("flipOffset");
            d.FlipProfileDistanceParameters = def.Bool("flipDistances");
            return feat.ModifyDefinition(d, doc, null) ? null : "gusset values not applied";
        }

        /// <summary>A cosmetic thread: made on its edge, then every value set through its data (how the creation call's
        /// arguments map is not documented).</summary>
        string ReplayCosmeticThread(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("edge"), false, 0);
            if (why != null) return "thread edge: " + why;
            var before = new HashSet<string>(AllThreads().Select(f => f.Name));
            FM.InsertCosmeticThread2((short)def.Num("applyThread", 1), U.M(def.Num("diameter")), U.M(def.Num("depth")), def.Str("callout") ?? "");
            doc.ClearSelection2(true);
            var feat = AllThreads().FirstOrDefault(f => !before.Contains(f.Name));
            if (feat == null) return "cosmetic thread was not created";
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            var d = (ICosmeticThreadFeatureData)feat.GetDefinition();
            d.AccessSelections(doc, null);
            d.EndCondition = Neutral.Code("threadEnd", def["endCondition"]);
            d.BlindDepth = U.M(def.Num("depth"));
            d.Diameter = U.M(def.Num("diameter"));
            d.ApplyThread = (int)def.Num("applyThread");
            if (def.Str("standardType") != null) d.StandardType = def.Str("standardType");
            if (def.Str("size") != null) d.Size = def.Str("size");
            d.ThreadCallout = def.Str("callout") ?? "";
            if (!feat.ModifyDefinition(d, doc, null)) return "thread values not applied";
            return null;
        }

        /// <summary>The cosmetic threads of the part (sub-features of the features whose edges they are on).</summary>
        IEnumerable<IFeature> AllThreads()
        {
            for (var f = (IFeature)doc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                    if (sub.GetTypeName2() == "CosmeticThread") yield return sub;
        }

        string ReplayRevolvedSurface(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectProfile(def);   // the profile sketch carries its centreline: the axis
            if (why != null) return "profile: " + why;
            bool reverse = def.Bool("reverse") != ProfileMirrored(def.Obj("profile"));
            int n0 = FeatureCount();
            doc.InsertRevolvedRefSurface(U.Rad(def.Num("angle", 360)), reverse, U.Rad(def.Num("angle2")), (int)def.Num("revolveType"));
            return Created(n0, null, nf, "revolved surface");
        }
    }
}
