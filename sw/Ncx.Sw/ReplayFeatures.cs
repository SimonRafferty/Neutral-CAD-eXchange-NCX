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

        /// <summary>Select the profile: the whole sketch, or only its selected contours and regions (mark 4). A
        /// contour is found as the rebuilt contour bounded by the same sketch segments; a region as the rebuilt
        /// sketch's region that contains the recorded point and has the recorded area (FORMAT-SPEC 4, extrude).</summary>
        string SelectProfile(JObj def, int mark = 4)
        {
            var contours = def.Arr("contours");
            var regions = def.Arr("regions");
            if (contours.Count == 0 && regions.Count == 0) return SelectRef(def.Obj("profile"), false, 0);
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
                if (!hit.Select2(append, Mark(mark))) return "could not select a contour";
                append = true;
            }
            if (regions.Count > 0)
            {
                if (m.Source == null) return "profile sketch has no frame for its regions";
                var builtRegions = ((object[])((ISketch)skf.GetSpecificFeature2()).GetSketchRegions() ?? new object[0]).OfType<ISketchRegion>()
                    .Select(g => new { g, face = (g.GetFirstLoop() as ILoop2)?.GetFace() as IFace2 }).Where(x => x.face != null).ToList();
                foreach (JObj r in regions)
                {
                    var p = r.Vec("point");
                    double area = r.Num("area");
                    var pm = m.Source.ToModel(U.M(p[0]), U.M(p[1]));
                    // the region the point lies in (on its face), with the same area: the area tells two regions
                    // apart if the point were ever on a boundary between them
                    var hit = builtRegions.FirstOrDefault(x =>
                    {
                        var q = x.face.GetClosestPointOn(pm[0], pm[1], pm[2]);
                        return q != null && U.Mm(V.Dist(V.P3(q), pm)) < 1e-4 &&
                               Math.Abs(x.face.GetArea() * 1e6 - area) <= Math.Max(1e-3 * area, 1e-3);
                    });
                    if (hit == null)
                        return "selected region at (" + U.R(p[0]) + ", " + U.R(p[1]) + ") with area " + U.R(area) + " mm2 not found in the rebuilt sketch";
                    if (!hit.g.Select2(append, Mark(mark))) return "could not select a region";
                    append = true;
                }
            }
            return null;
        }

        /// <summary>An extrude or cut. A selected contour can run through segments the user turned into construction
        /// geometry after making the feature (SolidWorks keeps the selection, and the feature still builds), but a
        /// construction segment belongs to no contour of a rebuilt sketch: those segments are normal geometry while
        /// the feature is made, and construction again after, as in the source.</summary>
        string ReplayExtrude(JObj nf, JObj def, bool cut)
        {
            var skf = ProfileSketchFeature(def);
            var temp = skf == null ? new List<ISketchSegment>() : ConstructionInContours(def);
            if (temp.Count > 0) SetConstruction(skf, temp, false);
            try { return ReplayExtrudeCore(nf, def, cut); }
            finally { if (temp.Count > 0) SetConstruction(skf, temp, true); }
        }

        IFeature ProfileSketchFeature(JObj def)
        {
            IFeature skf;
            string skId = def.Obj("profile")?.Str("id");
            return skId != null && features.TryGetValue(skId, out skf) ? skf : null;
        }

        /// <summary>The construction segments of the profile sketch that a selected contour runs through.</summary>
        List<ISketchSegment> ConstructionInContours(JObj def)
        {
            var res = new List<ISketchSegment>();
            SketchMap m;
            string skId = def.Obj("profile")?.Str("id");
            if (skId == null || !sketchMaps.TryGetValue(skId, out m)) return res;
            foreach (List<object> contour in def.Arr("contours"))
                foreach (string id in contour.Cast<string>())
                {
                    ISketchSegment seg;
                    if (m.Segs.TryGetValue(id, out seg) && seg.ConstructionGeometry && !res.Contains(seg)) res.Add(seg);
                }
            return res;
        }

        void SetConstruction(IFeature skf, List<ISketchSegment> segs, bool construction)
        {
            doc.ClearSelection2(true);
            skf.Select2(false, 0);
            doc.EditSketch();
            foreach (var seg in segs) seg.ConstructionGeometry = construction;
            ((ISketchManager)doc.SketchManager).InsertSketch(true);
            doc.ClearSelection2(true);
        }

        string ReplayExtrudeCore(JObj nf, JObj def, bool cut)
        {
            doc.ClearSelection2(true);
            string why = SelectProfile(def);
            if (why != null) return "profile: " + why;
            Trace("profile selected");
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
            Trace("creating");
            int n0 = FeatureCount();
            string last0 = LastFeature()?.Name;
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
                                      r1, r2, false, false, def.Bool("normalCut"), false, true, false, true, false, t0, startOffset, flipStart, false);
            else
                feat = FM.FeatureExtrusion3(e2 == null, false, reverse, t1, t2, d1, d2, dr1, dr2, o1, o2, a1, a2,
                                            r1, r2, false, false, def.Bool("merge", true), false, true, t0, startOffset, flipStart);
            doc.ClearSelection2(true);
            // made: returned, or the tree grew, or it ends in a new feature (a sheet-metal cut can leave the count as it
            // was, SolidWorks 2026 on a through-all normal cut)
            bool made = feat != null || FeatureCount() != n0 || LastFeature()?.Name != last0;
            if (!made && cut && def.Bool("normalCut"))
            {
                // SolidWorks refuses a normal cut for some profiles it accepts straight: made straight, and the
                // geometry checks say whether that is the same cut
                var straight = (JObj)Json.Parse(Json.Write(def, false));
                straight.Remove("normalCut");
                string why2 = ReplayExtrudeCore(nf, straight, true);
                if (why2 == null) Approx(nf, "made as a straight cut (SolidWorks refused it normal to the sheet)");
                return why2;
            }
            if (!made) return (cut ? "cut" : "extrude") + " was not created";
            feat = feat ?? LastFeature();
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            if (def.Bool("linkToThickness"))
            {
                // the depth follows the sheet's thickness (a creation option the extrude and cut calls do not take)
                // SolidWorks applies it only with the feature's selections accessed (2026 ignores it silently
                // otherwise, 2018 faults); the depth is already the thickness, so a refusal costs only the link
                bool linked = false;
                try
                {
                    var data = (IExtrudeFeatureData2)feat.GetDefinition();
                    data.AccessSelections(doc, null);
                    data.LinkToThickness = true;
                    linked = feat.ModifyDefinition(data, doc, null) && ((IExtrudeFeatureData2)feat.GetDefinition()).LinkToThickness;
                    if (!linked) data.ReleaseSelectionAccess();
                }
                catch (System.Runtime.InteropServices.COMException) { }
                if (!linked) Approx(nf, "depth not linked to the sheet thickness (SolidWorks refused it): made at the thickness");
            }
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
                if (def.Has("transition")) data.TransitionType = Neutral.Code("filletTransition", def["transition"]);
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
