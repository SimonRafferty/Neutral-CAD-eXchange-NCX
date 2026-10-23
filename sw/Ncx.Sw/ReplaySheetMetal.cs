// Replayer: sheet metal. The base flange creates the Sheet-Metal defaults feature and the Flat-Pattern
// itself; the exported markers for those are matched to what it created (renamed, defaults applied).
// An edge flange generates its profile plane from the edge, so it replays its (deferred) profile sketch
// into that plane before creating the flange.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        JObj pendingSheetMetal;   // the defaults marker, applied once the base flange has created the feature

        IFeature FindFeature(string typeName, Func<IFeature, bool> accept = null)
        {
            for (var f = (IFeature)doc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                if (f.GetTypeName2() == typeName && (accept == null || accept(f))) return f;
            return null;
        }

        bool Registered(IFeature f)
        {
            string n = f.Name;
            return features.Values.Any(x =>
            {
                try { return x.Name == n; }
                catch (System.Runtime.InteropServices.COMException) { return false; }   // replaced by SolidWorks
            });
        }

        string ReplaySheetMetalDefaults(JObj nf, JObj def)
        {
            var existing = FindFeature("SheetMetal", f => !Registered(f));
            if (existing == null) { pendingSheetMetal = nf; return null; }   // normal case: made by the base flange
            return ApplySheetMetalDefaults(existing, nf);
        }

        string ApplySheetMetalDefaults(IFeature f, JObj nf)
        {
            Name(f, nf.Str("name"));
            features[nf.Str("id")] = f;
            var want = nf.Obj("def");
            var d = (ISheetMetalFeatureData)f.GetDefinition();
            bool change = false;
            if (d.BendAllowanceType != Neutral.Code("bendAllowance", want["bendAllowanceType"])) { d.BendAllowanceType = Neutral.Code("bendAllowance", want["bendAllowanceType"]); change = true; }
            if (Math.Abs(d.KFactor - want.Num("kFactor")) > 1e-9) { d.KFactor = want.Num("kFactor"); change = true; }
            if (Math.Abs(U.Mm(d.BendAllowance) - want.Num("bendAllowance")) > 1e-6) { d.BendAllowance = U.M(want.Num("bendAllowance")); change = true; }
            if (d.UseAutoRelief != want.Bool("autoRelief")) { d.UseAutoRelief = want.Bool("autoRelief"); change = true; }
            if (d.AutoReliefType != Neutral.Code("relief", want["reliefType"])) { d.AutoReliefType = Neutral.Code("relief", want["reliefType"]); change = true; }
            if (Math.Abs(d.ReliefRatio - want.Num("reliefRatio")) > 1e-9) { d.ReliefRatio = want.Num("reliefRatio"); change = true; }
            if (change && !f.ModifyDefinition(d, doc, null)) return "sheet-metal defaults could not be applied";
            return null;
        }

        string ReplayBaseFlange(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("profile"), false, 0);
            if (why != null) return "profile: " + why;
            // a sketch rebuilt with its normal reversed swaps the sides of an open profile and the extrusion
            bool mirrored = ProfileMirrored(def.Obj("profile"));
            var e1 = def.Obj("end") ?? new JObj();   // absent for a closed profile
            int t1 = e1.Str("type") == "midPlane" ? 6 : 0;   // swEndConditions_e
            // merge fails the feature when there is nothing to merge with (the usual first-feature case); the
            // 2018 data does not say whether a flange merged, so it merges whenever a body exists
            bool merge = ((object[])part.GetBodies2(0, true) ?? new object[0]).Length > 0;
            int n0 = FeatureCount();
            var e2 = def.Obj("end2");   // extruded both ways (direction-to-use 0 takes both distances)
            var feat = FM.InsertSheetMetalBaseFlange2(U.M(def.Num("thickness")), def.Bool("reverseThickness") != mirrored,
                                                      U.M(def.Num("bendRadius")), U.M(e1.Num("depth")), e2 == null ? 0 : U.M(e2.Num("depth")),
                                                      def.Bool("reverse") != mirrored, t1, 0, 0,
                                                      null, true, 0, 0, 0, 0.5, true, merge, false, false);
            string err = Created(n0, feat, nf, "base flange");
            if (err != null) return err;
            if (pendingSheetMetal != null)
            {
                var sm = FindFeature("SheetMetal", f => !Registered(f));
                if (sm == null) return "the base flange made no sheet-metal feature";
                var p = pendingSheetMetal;
                pendingSheetMetal = null;
                return ApplySheetMetalDefaults(sm, p);
            }
            return null;
        }

        string ReplayFlatPattern(JObj nf)
        {
            var fp = FindFeature("FlatPattern", f => !Registered(f));
            if (fp == null) return "no flat pattern was created";
            Name(fp, nf.Str("name"));
            features[nf.Str("id")] = fp;
            return null;
        }

        string ReplayEdgeFlange(JObj nf, JObj def)
        {
            // a multi-edge flange lists its edges and one profile per edge; a single one just `edge` and `profile`
            var edgeRefs = def.Has("edges") ? def.Arr("edges").Cast<JObj>().ToList() : new List<JObj> { def.Obj("edge") };
            var profRefs = def.Has("profiles") ? def.Arr("profiles").Cast<JObj>().ToList() : new List<JObj> { def.Obj("profile") };
            if (edgeRefs.Count != profRefs.Count) return "flange edges and profiles do not pair up";
            var edges = new List<IEdge>();
            foreach (var er in edgeRefs)
            {
                List<object> ents;
                var res = ResolveRef(er, out ents);
                if (res.Str("result") != "resolved" || ents.Count != 1) return "flange edge " + res.Str("result");
                edges.Add((IEdge)ents[0]);
            }
            var skNfs = new List<JObj>();
            for (int i = 0; i < edges.Count; i++)
            {
                JObj skNf;
                if (profRefs[i] == null || !deferredSketches.TryGetValue(profRefs[i].Str("id"), out skNf)) return "flange profile sketch not found";
                // the profile: SolidWorks generates the plane from the edge and angle; the sketch is then edited
                var edge = edges[i];
                IFeature generated = null;
                string why = ReplaySketch(skNf, skNf.Obj("def"), () =>
                {
                    generated = (IFeature)doc.InsertSketchForEdgeFlange(edge, U.Rad(def.Num("angle")), false);
                    if (generated == null) return "could not generate the flange profile plane";
                    doc.ClearSelection2(true);
                    generated.Select2(false, 0);
                    doc.EditSketch();
                    return ((ISketchManager)doc.SketchManager).ActiveSketch == null ? "could not edit the flange profile" : null;
                });
                if (why != null) return "flange profile: " + why;
                if (skNf.Str("status") == "approximate") Approx(nf, "profile: " + skNf.Str("note"));
                skNfs.Add(skNf);
            }

            int opts = (def.Bool("useDefaultRadius") ? 1 : 0) | (def.Bool("useReliefRatio") ? 64 : 0) | (def.Bool("useDefaultRelief") ? 128 : 0);
            doc.ClearSelection2(true);
            int n0 = FeatureCount();
            // arrays of SolidWorks objects must reach it as VT_DISPATCH (out of process too)
            var feat = FM.InsertSheetMetalEdgeFlange2(edges.Select(x => new DispatchWrapper(x)).ToArray(),
                                                      skNfs.Select(x => new DispatchWrapper(features[x.Str("id")])).ToArray(), opts,
                                                      U.Rad(def.Num("angle")), U.M(def.Num("bendRadius")), Neutral.Code("flangePosition", def["position"]), 0,
                                                      Neutral.Code("relief", def["reliefType"]), def.Num("reliefRatio"),
                                                      U.M(def.Num("reliefWidth")), U.M(def.Num("reliefDepth")), 1, null);
            string err = Created(n0, feat, nf, "edge flange");
            if (err != null) return err;
            var po = def.Obj("positionOffset");
            if (po != null)
            {
                // the creation call has no position offset: set it through the data
                var ef = features[nf.Str("id")];
                var d = (IEdgeFlangeFeatureData)ef.GetDefinition();
                d.AccessSelections(doc, null);
                d.UsePositionOffset = true;
                d.PositionOffsetType = Neutral.Code("flangeOffset", po["type"], 1);
                d.PositionOffsetDistance = U.M(po.Num("distance"));
                d.ReversePositionOffset = po.Bool("reverse");
                if (!ef.ModifyDefinition(d, doc, null)) return "edge flange position offset not applied";
            }
            // the flange owns its profiles now (SolidWorks may have replaced the sketches): register those, in order
            int k = 0;
            for (var sub = (IFeature)features[nf.Str("id")].GetFirstSubFeature(); sub != null && k < skNfs.Count; sub = (IFeature)sub.GetNextSubFeature())
                if (sub.GetTypeName2() == "ProfileFeature")
                {
                    Name(sub, skNfs[k].Str("name"));
                    features[skNfs[k].Str("id")] = sub;
                    k++;
                }
            return null;
        }

        string ReplayHem(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("edges"), false, 0);
            if (why != null) return "hem edge: " + why;
            int n0 = FeatureCount();
            FM.InsertSheetMetalHem2(Neutral.Code("hemType", def["hemType"]), Neutral.Code("hemPosition", def["position"]), def.Bool("reverse"), U.M(def.Num("length")),
                                    U.M(def.Num("gap")), U.Rad(def.Num("angle")), U.M(def.Num("radius")), U.M(def.Num("miterGap")),
                                    null, true, 0, 0, false, 0.5, 0, 0);
            return Created(n0, null, nf, "hem");
        }

        string ReplayBreakCorner(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("entities"), false, 0);
            if (why != null) return "corner: " + why;
            int n0 = FeatureCount();
            doc.InsertSheetMetalBreakCorner(Neutral.Code("breakType", def["breakType"]), U.M(def.Num("distance")));
            return Created(n0, null, nf, "break corner");
        }

        string ReplaySketchedBend(JObj nf, JObj def)
        {
            IFeature line;
            if (!features.TryGetValue(def.Obj("line")?.Str("id") ?? "", out line)) return "bend line sketch not rebuilt";
            doc.ClearSelection2(true);
            line.Select2(false, 0);
            string why = SelectAll(new object[] { def.Obj("fixedFace") }, true, 0);
            if (why != null) return "fixed face: " + why;
            int n0 = FeatureCount();
            FM.InsertSheetMetal3dBend(U.Rad(def.Num("angle")), def.Bool("useDefaultRadius"), U.M(def.Num("radius")),
                                      def.Bool("reverse"), (short)Neutral.Code("flangePosition", def["position"]), null);
            return Created(n0, null, nf, "sketched bend");
        }

        /// <summary>A miter flange: its profile sketch and edges (mark 0). It goes before the flat pattern, not last.</summary>
        string ReplayMiterFlange(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("profile"), false, 0);
            if (why != null) return "miter profile: " + why;
            why = SelectAll(def.Arr("edges"), true, 0);
            if (why != null) return "miter edge: " + why;
            FM.InsertSheetMetalMiterFlange(def.Bool("useDefaultRadius"), U.M(def.Num("bendRadius")), U.M(def.Num("gap")),
                                           def.Bool("useDefaultRelief"), def.Bool("useReliefRatio"), def.Num("reliefRatio"),
                                           U.M(def.Num("reliefWidth")), U.M(def.Num("reliefDepth")), Neutral.Code("relief", def["reliefType"]),
                                           def.Bool("trimSideBends"), Neutral.Code("flangePosition", def["position"]), U.M(def.Num("startOffset")),
                                           U.M(def.Num("endOffset")), null);
            doc.ClearSelection2(true);
            var feat = FindFeature("SMMiteredFlange", x => !Registered(x));
            if (feat == null) return "miter flange was not created";
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            return null;
        }

        /// <summary>A closed corner: made from the faces to extend, then its type, gap and ratio set through its data. It
        /// goes before the flat pattern, not last.</summary>
        string ReplayClosedCorner(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("faces"), false, 0);
            if (why != null) return "corner face: " + why;
            doc.InsertSheetMetalClosedCorner();
            doc.ClearSelection2(true);
            var feat = FindFeature("CornerFeat", x => !Registered(x));
            if (feat == null) return "closed corner was not created";
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            var d = (IClosedCornerFeatureData)feat.GetDefinition();
            d.AccessSelections(doc, null);
            d.CornerType = Neutral.Code("closedCorner", def["cornerType"]);
            d.GapDistance = U.M(def.Num("gap"));
            d.OverlapUnderlapRatio = def.Num("overlapRatio");
            d.OpenBendRegion = def.Bool("openBendRegion");
            return feat.ModifyDefinition(d, doc, null) ? null : "corner values not applied";
        }

        string ReplayJog(JObj nf, JObj def)
        {
            IFeature line;
            if (!features.TryGetValue(def.Obj("line")?.Str("id") ?? "", out line)) return "jog line sketch not rebuilt";
            doc.ClearSelection2(true);
            line.Select2(false, 0);
            string why = SelectAll(new object[] { def.Obj("fixedFace") }, true, 0);
            if (why != null) return "fixed face: " + why;
            int n0 = FeatureCount();
            // this call takes the angle in DEGREES (unlike the rest of the API), and numbers the dimension and bend
            // positions one lower than the feature data reports them. The angle is rounded: a jog is discontinuous at
            // 90 degrees (90.0000000000002, the radian round trip of 90, folds it differently)
            doc.InsertSheetMetalJog(Math.Round(def.Num("angle"), 9), U.M(def.Num("radius")), U.M(def.Num("offset")), def.Bool("reverse"),
                                    def.Bool("fixProjectedLength"), (short)(Neutral.Code("jogDimension", def["dimensionPosition"]) - 1), (short)(Neutral.Code("jogPosition", def["bendPosition"]) - 1));
            return Created(n0, null, nf, "jog");
        }

        static void Approx(JObj nf, string note)
        {
            nf["status"] = "approximate";
            nf["note"] = nf.Has("note") ? nf.Str("note") + "; " + note : note;
        }
    }
}
