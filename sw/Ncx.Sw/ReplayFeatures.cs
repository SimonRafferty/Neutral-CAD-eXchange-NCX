// Replayer: solid features (extrude/cut, fillet, chamfer for now).
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        static readonly Dictionary<string, int> EndTypes = new Dictionary<string, int>
        {
            { "blind", 0 }, { "throughAll", 1 }, { "upToVertex", 3 }, { "upToSurface", 4 }, { "offsetFromSurface", 5 },
            { "midPlane", 6 }, { "upToBody", 7 }, { "throughAllBoth", 9 }, { "upToNext", 11 }
        };

        static readonly Dictionary<string, int> ThinTypes = new Dictionary<string, int>
        {
            { "oneDirection", 0 }, { "oppositeDirection", 1 }, { "midPlane", 2 }, { "twoDirection", 3 }
        };

        /// <summary>Select the profile: the whole sketch, or only its selected contours (mark 4), each found as
        /// the rebuilt contour bounded by the same sketch segments.</summary>
        string SelectProfile(JObj def)
        {
            var contours = def.Arr("contours");
            if (contours.Count == 0) return SelectRef(def.Obj("profile"), false, 0);
            string skId = def.Obj("profile")?.Str("id");
            SketchMap m;
            IFeature skf;
            if (skId == null || !sketchMaps.TryGetValue(skId, out m) || !features.TryGetValue(skId, out skf))
                return "profile sketch " + skId + " was not rebuilt";
            // rebuilt segment -> neutral id (segment ids are unique per segment type)
            var idOf = new Dictionary<string, string>();
            foreach (var kv in m.Segs)
            {
                var a = (int[])kv.Value.GetID();
                idOf["s" + kv.Value.GetType() + "_" + a[0] + "_" + a[1]] = kv.Key;
            }
            var built = (object[])((ISketch)skf.GetSpecificFeature2()).GetSketchContours() ?? new object[0];
            bool append = false;
            foreach (List<object> want in contours)
            {
                var wanted = new HashSet<string>(want.Cast<string>());
                ISketchContour hit = null;
                foreach (ISketchContour c in built)
                {
                    var have = new HashSet<string>(((object[])c.GetSketchSegments() ?? new object[0]).Select(s =>
                    {
                        var a = (int[])((ISketchSegment)s).GetID();
                        string id;
                        return idOf.TryGetValue("s" + ((ISketchSegment)s).GetType() + "_" + a[0] + "_" + a[1], out id) ? id : "?";
                    }));
                    if (have.SetEquals(wanted)) { hit = c; break; }
                }
                if (hit == null) return "selected contour (" + string.Join(",", wanted) + ") not found in the rebuilt sketch";
                if (!hit.Select2(append, Mark(4))) return "could not select a contour";
                append = true;
            }
            return null;
        }

        string ReplayExtrude(JObj nf, JObj def, bool cut)
        {
            doc.ClearSelection2(true);
            string why = SelectProfile(def);
            if (why != null) return "profile: " + why;
            var start = def.Obj("start");
            int t0 = 0;
            double startOffset = 0;
            bool flipStart = false;
            if (start != null)
            {
                t0 = start.Str("type") == "offset" ? 3 : start.Str("type") == "surface" ? 1 : start.Str("type") == "vertex" ? 2 : 0;
                startOffset = U.M(start.Num("distance"));
                flipStart = start.Bool("reverse");
                if (start.Obj("target") != null)
                {
                    why = SelectRef(start.Obj("target"), true, 32);   // the start entity
                    if (why != null) return "start " + start.Str("type") + ": " + why;
                }
            }
            var e1 = def.Obj("end");
            var e2 = def.Obj("end2");
            int t1, t2 = 0;
            if (!EndTypes.TryGetValue(e1.Str("type"), out t1)) return "end condition " + e1.Str("type") + " not supported";
            if (e2 != null && !EndTypes.TryGetValue(e2.Str("type"), out t2)) return "end condition " + e2.Str("type") + " not supported";
            foreach (var e in new[] { e1, e2 })
            {
                if (e?.Obj("target") == null) continue;
                why = SelectRef(e.Obj("target"), true, 1);
                if (why != null) return "up-to target: " + why;
            }
            double d1 = U.M(e1.Num("depth")), d2 = e2 == null ? 0 : U.M(e2.Num("depth"));
            bool dr1 = e1.Has("draft"), dr2 = e2 != null && e2.Has("draft");
            double a1 = U.Rad(e1.Num("draft")), a2 = e2 == null ? 0 : U.Rad(e2.Num("draft"));
            bool o1 = e1.Bool("draftOutward"), o2 = e2 != null && e2.Bool("draftOutward");
            bool r1 = e1.Bool("offsetReverse"), r2 = e2 != null && e2.Bool("offsetReverse");
            int n0 = FeatureCount();
            IFeature feat;
            bool reverse = def.Bool("reverse") != ProfileMirrored(def.Obj("profile"));
            var thin = def.Obj("thin");
            if (thin != null)
            {
                int tt;
                if (!ThinTypes.TryGetValue(thin.Str("type") ?? "", out tt)) tt = 0;
                double k1 = U.M(thin.Num("thickness")), k2 = U.M(thin.Num("thickness2")), cap = U.M(thin.Num("capThickness"));
                int caps = thin.Bool("capEnds") ? 1 : 0;
                if (cut)
                    feat = FM.FeatureCutThin2(e2 == null, def.Bool("flipSide"), reverse, t1, t2, d1, d2, dr1, dr2, o1, o2, a1, a2,
                                              r1, r2, false, false, k1, k2, cap, tt, caps, false, 0, false, true, t0, startOffset, flipStart);
                else
                    feat = FM.FeatureExtrusionThin2(e2 == null, false, reverse, t1, t2, d1, d2, dr1, dr2, o1, o2, a1, a2,
                                                    r1, r2, false, false, def.Bool("merge", true), k1, k2, cap, tt, caps, false, 0,
                                                    false, true, t0, startOffset, flipStart);
            }
            else if (cut)
                feat = FM.FeatureCut4(e2 == null, def.Bool("flipSide"), reverse, t1, t2, d1, d2, dr1, dr2, o1, o2, a1, a2,
                                      r1, r2, false, false, false, false, true, false, true, false, t0, startOffset, flipStart, false);
            else
                feat = FM.FeatureExtrusion3(e2 == null, false, reverse, t1, t2, d1, d2, dr1, dr2, o1, o2, a1, a2,
                                            r1, r2, false, false, def.Bool("merge", true), false, true, t0, startOffset, flipStart);
            doc.ClearSelection2(true);
            if (FeatureCount() == n0) return (cut ? "cut" : "extrude") + " was not created";
            feat = feat ?? LastFeature();
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            return null;
        }

        string ReplayFillet(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string kind = def.Str("filletType") ?? "constant";
            if (kind == "variable") return ReplayVariableFillet(nf, def);
            // FeatureFillet3 type 3 makes a face fillet, whatever is selected: the carried result is used instead
            if (kind == "fullRound") return "a full-round fillet cannot be made through SolidWorks' API";
            int ftyp = kind == "face" ? 2 : kind == "fullRound" ? 3 : 0;   // swFeatureFilletType_e
            bool append = false;
            if (ftyp == 0)
                foreach (JObj r in def.Arr("edges"))
                {
                    string why = SelectRef(r, append, 1);
                    if (why != null) { doc.ClearSelection2(true); return why; }
                    append = true;
                }
            else
            {
                // face fillet: sets 1, 2 (marks 2, 4); full round: side 1, centre, side 2 (marks 2, 4, 8)
                int[] marks = { 2, 4, 8 };
                for (int i = 0; i < (ftyp == 2 ? 2 : 3); i++)
                    foreach (JObj r in def.Arr("faces" + (i + 1)))
                    {
                        string why = SelectRef(r, append, marks[i]);
                        if (why != null) { doc.ClearSelection2(true); return "face set " + (i + 1) + ": " + why; }
                        append = true;
                    }
            }
            int n0 = FeatureCount();
            // the exported edges are the user's picks; propagation is reapplied when the source used it
            var radii = def.Arr("radii");
            int options = (radii.Count > 0 ? 0 : 2) | (def.Bool("propagate") ? 1 : 0);   // uniform radius | propagate
            object radiiArr = radii.Count > 0 ? radii.Select(x => U.M(Convert.ToDouble(x))).ToArray() : null;
            var feat = FM.FeatureFillet3(options, U.M(def.Num("radius")), 0, 0, ftyp, 0, 0, radiiArr, null, null, null, null, null, null);
            doc.ClearSelection2(true);
            if (FeatureCount() == n0) return "fillet was not created";
            feat = feat ?? LastFeature();
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            return null;
        }

        /// <summary>A variable-radius fillet: made at the default radius, then each vertex radius set through its data
        /// (which vertex the creation call's radius list applies to is not documented).</summary>
        string ReplayVariableFillet(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("edges"), false, 1);
            if (why != null) { doc.ClearSelection2(true); return why; }
            // the default radius may be 0 (every vertex set): start from the vertex radii, set exactly below
            var vr0 = def.Arr("vertexRadii").Cast<JObj>().Select(x => U.M(x.Num("radius"))).ToList();
            double r0 = def.Num("radius") > 0 ? U.M(def.Num("radius")) : vr0.DefaultIfEmpty(0.001).First();
            var radii = vr0.Count >= 2 ? vr0.ToArray() : Enumerable.Repeat(r0, 2).ToArray();
            int n0 = FeatureCount();
            object made = FM.FeatureFillet3(def.Bool("propagate") ? 1 : 0, r0, 0, 0, 1, 0, 0, radii, null, null, null, null, null, null);
            doc.ClearSelection2(true);
            if (FeatureCount() == n0) return "variable fillet was not created";
            var feat = (IFeature)(made ?? LastFeature());
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            var data = (IVariableFilletFeatureData2)feat.GetDefinition();
            data.AccessSelections(doc, null);
            try
            {
                foreach (JObj vr in def.Arr("vertexRadii"))
                {
                    List<object> ents;
                    var res = ResolveRef(vr.Obj("vertex"), out ents);
                    if (res.Str("result") != "resolved") { data.ReleaseSelectionAccess(); return "fillet vertex " + res.Str("result"); }
                    data.SetRadius(ents[0], U.M(vr.Num("radius")));
                }
            }
            catch (System.Runtime.InteropServices.COMException e)
            {
                data.ReleaseSelectionAccess();
                return "vertex radius not set (" + e.Message + ")";
            }
            if (!feat.ModifyDefinition(data, doc, null)) return "vertex radii not applied";
            return null;
        }

        string ReplayChamfer(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            if (def.Str("chamferType") == "vertex")
            {
                string why = SelectRef(def.Obj("vertex"), false, 0);   // a vertex chamfer takes mark 0
                if (why != null) return "chamfer vertex: " + why;
                var ds = def.Vec("distances") ?? new double[3];
                int c0 = FeatureCount();
                IFeature vf = FM.InsertFeatureChamfer(0, 3, 0, 0, 0, U.M(ds[0]), U.M(ds[1]), U.M(ds[2]));   // swChamferVertex
                return Created(c0, vf, nf, "vertex chamfer");
            }
            bool append = false;
            foreach (JObj r in def.Arr("edges"))
            {
                string why = SelectRef(r, append, 1);
                if (why != null) { doc.ClearSelection2(true); return why; }
                append = true;
            }
            int type = def.Str("chamferType") == "angleDistance" ? 1 : def.Str("chamferType") == "distanceDistance" ? 2 : 16;
            int n0 = FeatureCount();
            int options = def.Bool("propagate") ? 4 : 0;   // swFeatureChamferTangentPropagation
            IFeature feat = FM.InsertFeatureChamfer(options, type, U.M(def.Num("distance")), U.Rad(def.Num("angle")),
                                                    U.M(def.Num("distance2")), 0, 0, 0);
            doc.ClearSelection2(true);
            if (FeatureCount() == n0) return "chamfer was not created";
            feat = feat ?? LastFeature();
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            var flips = def.Arr("flipped");
            if (flips.Any(x => x is bool && (bool)x)) return FlipChamfer(feat, def);
            return null;
        }

        /// <summary>Apply per-edge flips recorded by the exporter (distance/angle side).</summary>
        string FlipChamfer(IFeature feat, JObj def)
        {
            var data = (IChamferFeatureData2)feat.GetDefinition();
            data.AccessSelections(doc, null);
            var edges = (object[])data.Edges ?? new object[0];
            var flips = def.Arr("flipped");
            // the feature's edges come back in selection order, which is the exported order
            for (int i = 0; i < edges.Length && i < flips.Count; i++)
                if (flips[i] is bool && (bool)flips[i]) data.SetIsFlipped(edges[i], true);
            bool ok = feat.ModifyDefinition(data, doc, null);
            return ok ? null : "chamfer flips could not be applied";
        }
    }
}
