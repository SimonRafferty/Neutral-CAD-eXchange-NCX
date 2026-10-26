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
        int variantUndo = 1;   // the undo steps one variant's `make` took (TryVariants)
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
            Name(f, nf.Str("name"));   // refused when the name is SolidWorks' hidden default one ("Sheet-Metal")
            features[nf.Str("id")] = f;
            var want = nf.Obj("def");
            var d = (ISheetMetalFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            bool change = false;
            // the bend allowance through its CustomBendAllowance object (setting KFactor / BendAllowanceType on the
            // feature data directly is accepted and silently dropped by ModifyDefinition)
            var ba = (CustomBendAllowance)d.GetCustomBendAllowance();
            int baType = Neutral.Code("bendAllowance", want["bendAllowanceType"]);
            if (ba.Type != baType) { ba.Type = baType; change = true; }
            if (Math.Abs(ba.KFactor - want.Num("kFactor")) > 1e-9) { ba.KFactor = want.Num("kFactor"); change = true; }
            if (Math.Abs(U.Mm(ba.BendAllowance) - want.Num("bendAllowance")) > 1e-6) { ba.BendAllowance = U.M(want.Num("bendAllowance")); change = true; }
            // the feature follows the part's hidden defaults until told to override them (otherwise ModifyDefinition
            // keeps the defaults and clears the relief ratio)
            if (change)
            {
                d.SetOverrideDefaultParameter2(1, true);   // swSheetMetalOverrideDefaultParameters_BendAllowance
                d.SetCustomBendAllowance(ba);
            }
            bool relief = false;
            if (d.UseAutoRelief != want.Bool("autoRelief")) relief = true;
            if (d.AutoReliefType != Neutral.Code("relief", want["reliefType"])) relief = true;
            if (Math.Abs(d.ReliefRatio - want.Num("reliefRatio")) > 1e-9) relief = true;
            if (relief)
            {
                d.SetOverrideDefaultParameter2(2, true);   // swSheetMetalOverrideDefaultParameters_AutoRelief
                d.UseAutoRelief = want.Bool("autoRelief");
                d.AutoReliefType = Neutral.Code("relief", want["reliefType"]);
                d.ReliefRatio = want.Num("reliefRatio");
                change = true;
            }
            if (!change) d.ReleaseSelectionAccess();
            if (change && !f.ModifyDefinition(d, doc, null)) return "sheet-metal defaults could not be applied";
            if (change) doc.EditRebuild3();   // the next sheet-metal feature fails on a part waiting for this rebuild
            return null;
        }

        /// <summary>Convert to Sheet Metal (FORMAT-SPEC 4): the fixed face and the bend edges selected, then
        /// InsertConvertToSheetMetal2. The result the feature carries decides the thickness sense: if the bodies do
        /// not match it, the other sense is tried. The Sheet-Metal feature it makes takes the recorded defaults.</summary>
        string ReplayConvertToSheetMetal(JObj nf, JObj def)
        {
            var res = nf.Obj("result");
            string last = null;
            foreach (bool reverse in new[] { def.Bool("reverse"), !def.Bool("reverse") })
            {
                doc.ClearSelection2(true);
                string why = SelectRef(def.Obj("fixedFace"), false, 0);
                if (why != null) return "fixed face: " + why;
                foreach (JObj e in def.Arr("bendEdges"))
                    if ((why = SelectRef(e, true, 0)) != null) return "bend edge: " + why;
                int n0 = FeatureCount();
                FM.InsertConvertToSheetMetal2(U.M(def.Num("thickness")), reverse, false, U.M(def.Num("bendRadius")),
                                              U.M(def.Num("gap", 0.1)), 2, def.Num("reliefRatio", 0.5), 0, 0.5, def.Bool("keepBody"));
                doc.ClearSelection2(true);
                if (FeatureCount() == n0) { last = "convert to sheet metal was not created"; continue; }
                if (res == null || MatchesResult(res)) { last = null; break; }
                last = "rebuilt bodies differ from its result" + (reverse == def.Bool("reverse") ? "" : " in both thickness senses");
                doc.EditUndo2(1);
            }
            if (last != null) return last;
            IFeature made = null;
            for (var f = (IFeature)doc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                if (f.GetTypeName2() == "SolidToSheetMetal") made = f;
            if (made == null) return "convert to sheet metal was not created";
            Name(made, nf.Str("name"));
            features[nf.Str("id")] = made;
            // the Sheet-Metal feature it made takes the recorded defaults (K-factor, relief)
            if (pendingSheetMetal != null)
            {
                var sm = FindFeature("SheetMetal", f => !Registered(f));
                var p = pendingSheetMetal;
                pendingSheetMetal = null;
                if (sm != null) return ApplySheetMetalDefaults(sm, p);
            }
            return null;
        }

        /// <summary>Developer trace: each carried body's nearest rebuilt body (distance, volumes, centroids).</summary>
        string ResultGap(JObj res)
        {
            var bodies = ((object[])part.GetBodies2(-1, false) ?? new object[0]).Cast<IBody2>().Select(Host.BodyFingerprint).ToList();
            Func<double[], string> v = a => a == null ? "-" : string.Join(",", a.Select(x => x.ToString("0.###")));
            return string.Join("; ", res.Arr("bodies").Cast<JObj>().Select(rb =>
            {
                var fp = rb.Obj("fingerprint");
                var best = bodies.OrderBy(b => BodyDistance(fp, b)).FirstOrDefault();
                return best == null ? "no bodies" : "d=" + BodyDistance(fp, best).ToString("0.####") + " vol " + fp.Num("volume").ToString("0.#") + "/" +
                       best.Num("volume").ToString("0.#") + " c " + v(fp.Vec("centroid")) + "/" + v(best.Vec("centroid"));
            }));
        }

        /// <summary>Every body of the feature's carried result has a body of the rebuilt part with the same
        /// fingerprint (exchange tolerance: the result went through STEP).</summary>
        bool MatchesResult(JObj res)
        {
            var bodies = ((object[])part.GetBodies2(-1, false) ?? new object[0]).Cast<IBody2>().Select(Host.BodyFingerprint).ToList();
            return res.Arr("bodies").Cast<JObj>().All(rb => bodies.Any(b => BodyDistance(rb.Obj("fingerprint"), b) < 1e-3));
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
            var e2 = def.Obj("end2");   // extruded both ways (direction-to-use 0 takes both distances)
            // the part's sheet-metal defaults go in with the base flange that makes its Sheet-Metal feature (changing
            // them afterwards through ModifyDefinition is accepted and has no effect)
            var smDef = pendingSheetMetal?.Obj("def");
            CustomBendAllowance allowance = null;
            if (smDef != null)
            {
                var ba = (CustomBendAllowance)FM.CreateCustomBendAllowance();
                ba.Type = Neutral.Code("bendAllowance", smDef["bendAllowanceType"]);
                ba.KFactor = smDef.Num("kFactor", 0.5);
                if (ba.Type == 3) ba.BendAllowance = U.M(smDef.Num("bendAllowance"));   // only a direct allowance takes one (0 spoils the others)
                allowance = ba;
            }
            double reliefRatio = smDef?.Num("reliefRatio", 0.5) ?? 0.5;
            int reliefType = smDef == null ? 0 : Math.Max(0, Neutral.Code("relief", smDef["reliefType"]));
            Func<Tuple<bool, bool>, string> make = v =>
            {
                doc.ClearSelection2(true);
                string w = SelectRef(def.Obj("profile"), false, 0);
                if (w != null) return "profile: " + w;
                int n0 = FeatureCount();
                var feat = FM.InsertSheetMetalBaseFlange2(U.M(def.Num("thickness")), v.Item1 != mirrored,
                                                          U.M(def.Num("bendRadius")), U.M(e1.Num("depth")), e2 == null ? 0 : U.M(e2.Num("depth")),
                                                          v.Item2 != mirrored, t1, 0, 0,
                                                          allowance, true, reliefType, 0, 0, reliefRatio, true, merge, false, false);
                return Created(n0, feat, nf, "base flange");
            };
            bool r0 = def.Bool("reverseThickness"), d0 = def.Bool("reverse");
            // another system's flange: which side its thickness lies on (SolidWorks takes either flag for that on a
            // closed profile), by the carried result
            var vs = new List<Tuple<bool, bool>> { Tuple.Create(r0, d0) };
            // FORMAT-SPEC 4: a closed profile's thickness lies against frame.z, `reverseThickness` along it; SolidWorks
            // ignores its own reverse-thickness flag there and takes the side from its direction flag. Its own files
            // keep both flags as SolidWorks set them; another system's are read by the spec first.
            if (Foreign && def.Obj("end") == null && r0) vs[0] = Tuple.Create(r0, !d0);
            if (Foreign)
                foreach (var v in new[] { Tuple.Create(r0, d0), Tuple.Create(!r0, d0), Tuple.Create(r0, !d0), Tuple.Create(!r0, !d0) })
                    if (!vs.Contains(v)) vs.Add(v);
            string err = TryVariants(nf, vs, make, v => "thickness " + (v.Item1 ? "reversed" : "not reversed") + (v.Item2 ? ", direction reversed" : ""), "base flange");
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
            // the profile: SolidWorks generates the plane from the edge and angle; the sketch is then edited
            Func<JObj, IEdge, JObj, string> profile = (skNf, edge, skDef) =>
            {
                IFeature generated = null;
                string why = ReplaySketch(skNf, skDef, () =>
                {
                    generated = (IFeature)doc.InsertSketchForEdgeFlange(edge, U.Rad(def.Num("angle")), false);
                    if (generated == null) return "could not generate the flange profile plane";
                    doc.ClearSelection2(true);
                    generated.Select2(false, 0);
                    doc.EditSketch();
                    return ((ISketchManager)doc.SketchManager).ActiveSketch == null ? "could not edit the flange profile" : null;
                });
                return why == null ? null : "flange profile: " + why;
            };
            for (int i = 0; i < edges.Count; i++)
            {
                JObj skNf;
                if (profRefs[i] == null || !deferredSketches.TryGetValue(profRefs[i].Str("id"), out skNf)) return "flange profile sketch not found";
                string why = profile(skNf, edges[i], skNf.Obj("def"));
                if (why != null) return why;
                if (skNf.Str("status") == "approximate") Approx(nf, "profile: " + skNf.Str("note"));
                skNfs.Add(skNf);
            }

            int opts = (def.Bool("useDefaultRadius") ? 1 : 0) | (def.Bool("useReliefRatio") ? 64 : 0) | (def.Bool("useDefaultRelief") ? 128 : 0);
            Func<Tuple<string, string>, string> make = v =>
            {
                string position = v.Item1;
                variantUndo = 1;
                doc.ClearSelection2(true);
                int n0 = FeatureCount();
                // arrays of SolidWorks objects must reach it as VT_DISPATCH (out of process too)
                var feat = FM.InsertSheetMetalEdgeFlange2(edges.Select(x => new DispatchWrapper(x)).ToArray(),
                                                          skNfs.Select(x => new DispatchWrapper(features[x.Str("id")])).ToArray(), opts,
                                                          U.Rad(def.Num("angle")), U.M(def.Num("bendRadius")), Neutral.Code("flangePosition", position), 0,
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
                    if (!ef.ModifyDefinition(d, doc, null))
                    {
                        doc.EditUndo2(variantUndo);   // the flange made without it
                        return "edge flange position offset not applied";
                    }
                    variantUndo++;
                }
                var len = def.Obj("length");
                if (len != null && len.Has("datum") && !Foreign)
                {
                    // the line the flange length (and so the profile) is measured from; set only where it differs
                    var ef = features[nf.Str("id")];
                    var d = (IEdgeFlangeFeatureData)ef.GetDefinition();
                    int datum = Neutral.Code("flangeDatum", v.Item2 ?? len["datum"], 1);
                    if (d.OffsetDimType != datum)
                    {
                        d.AccessSelections(doc, null);
                        d.OffsetDimType = datum;
                        if (!ef.ModifyDefinition(d, doc, null))
                        {
                            doc.EditUndo2(variantUndo);   // the flange made without it
                            return "edge flange length datum not applied";
                        }
                        variantUndo++;
                    }
                }
                return null;
            };
            string p0 = def.Str("position") ?? "materialInside", l0 = def.Obj("length")?.Str("datum");
            var len0 = def.Obj("length");
            bool made = false;
            if (Foreign && edges.Count == 1 && len0 != null && len0.Str("type") == "blind" && len0.Num("distance") > 0)
            {
                // another system's profile is its writer's drawing of the flange, and what a profile's height means
                // (and the line its length is measured from) differs between systems: a probe flange is made from it,
                // and the profile is redrawn taller by what the probe's volume lacks against the carried result's,
                // over the flange's width and the sheet's thickness (its length runs along the profile plane, one for
                // one with the profile's height). Without a result, SolidWorks' reading of the length (from its
                // default datum) against the carried length.
                if (make(Tuple.Create(p0, l0)) == null)
                {
                    var res = nf.Obj("result");
                    double delta;
                    var xs = skNfs[0].Obj("def").Arr("entities").Cast<JObj>().Where(en => en.Has("p"))
                                     .Select(en => Convert.ToDouble(((System.Collections.IList)en["p"])[0])).ToList();
                    double width = xs.Count > 1 ? xs.Max() - xs.Min() : 0, t = SheetThickness();
                    if (res != null && width > 0 && t > 0)
                    {
                        double want = res.Arr("bodies").Cast<JObj>().Sum(b => b.Obj("fingerprint")?.Num("volume") ?? 0);
                        double have = ((object[])part.GetBodies2(0, false) ?? new object[0]).Cast<IBody2>().Sum(b => Host.BodyFingerprint(b).Num("volume"));
                        delta = (want - have) / (width * t);
                    }
                    else
                    {
                        var pd = (IEdgeFlangeFeatureData)features[nf.Str("id")].GetDefinition();
                        delta = len0.Num("distance") - U.Mm(pd.OffsetDistance);
                    }
                    if (Math.Abs(delta) <= 1e-6 && (res == null || MatchesResult(res))) made = true;   // the probe is the flange
                    else
                    {
                        doc.EditUndo2(variantUndo);
                        // the undo gives the profile back as a new sketch object: the last sketch (before the flat pattern)
                        IFeature back = null;
                        for (int q = 0; q < 3 && back == null; q++)
                        {
                            var g = (IFeature)doc.FeatureByPositionReverse(q);
                            if (g != null && g.GetTypeName2() == "ProfileFeature") back = g;
                        }
                        if (back == null || back.GetTypeName2() != "ProfileFeature") return "the flange profile was not given back after a trial";
                        features[skNfs[0].Str("id")] = back;
                        if (Math.Abs(delta) > 1e-6)
                        {
                            var skNf = skNfs[0];
                            doc.ClearSelection2(true);
                            if (!back.Select2(false, 0) || !doc.Extension.DeleteSelection2(0)) return "flange profile could not be redrawn";
                            // the undo gives the edge back as a new object too
                            List<object> again;
                            var rr = ResolveRef(edgeRefs[0], out again);
                            if (rr.Str("result") != "resolved" || again.Count != 1) return "flange edge " + rr.Str("result") + " after a trial";
                            edges[0] = (IEdge)again[0];
                            var taller = (JObj)Json.Parse(Json.Write(skNf.Obj("def"), false));
                            Func<object, object> up = o =>
                            {
                                var q = ((System.Collections.IList)o).Cast<object>().Select(Convert.ToDouble).ToList();
                                return new List<object> { q[0], q[1] > 1e-9 ? q[1] + delta : q[1] };
                            };
                            foreach (JObj en in taller.Arr("entities"))
                                foreach (var key in new[] { "p", "start", "end" })
                                    if (en.Has(key)) en[key] = up(en[key]);
                            foreach (JObj dm in taller.Arr("dimensions"))
                                if (dm.Has("value")) dm["value"] = dm.Num("value") + delta;
                            string why = profile(skNf, edges[0], taller);
                            if (why != null) return why;
                            Approx(nf, "profile redrawn " + delta.ToString("0.###") + " mm taller: its height means something else in SolidWorks (" +
                                       len0.Num("distance").ToString("0.###") + " mm from the " + l0 + ")");
                        }
                    }
                }
            }
            var vs = new List<Tuple<string, string>> { Tuple.Create(p0, l0) };
            // another system's flange: where its bend sits against the edge and the line its length is measured from
            // (which moves the profile), by the carried result
            if (Foreign)
                foreach (var pos in new[] { "materialInside", "bendSharp", "materialOutside", "bendOutside", "bendTangent", "bendCenterline" })
                    if (pos != p0) vs.Add(Tuple.Create(pos, l0));
            string verr = made ? null : TryVariants(nf, vs, make, v => "position " + v.Item1, "edge flange");
            if (verr != null) return verr;
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

        /// <summary>Is the file from another CAD system? (its settings may map to SolidWorks' differently)</summary>
        bool Foreign => !string.Equals(Model?.Obj("generator")?.Str("app"), "SOLIDWORKS", StringComparison.OrdinalIgnoreCase);

        /// <summary>The geometry decides (FORMAT-SPEC 4): a feature from another system's file whose settings its writer
        /// could not map one to one (a hem's or bend's position and sense) is made with each variant in turn until its
        /// bodies are the feature's carried result; a variant that differs is undone. `make` returns null when the
        /// feature was made. Without a result, only the first variant.</summary>
        string TryVariants<T>(JObj nf, IList<T> variants, Func<T, string> make, Func<T, string> describe, string what)
        {
            var res = nf.Obj("result");
            string first = null;
            for (int i = 0; i < variants.Count; i++)
            {
                variantUndo = 1;
                string w = make(variants[i]);
                if (w != null) { first = first ?? w; continue; }
                if (System.Environment.GetEnvironmentVariable("NCX_TRACE_VARIANTS") != null && res != null)
                    Console.Error.WriteLine("variant " + describe(variants[i]) + ": " + ResultGap(res));   // developer
                if (res == null || MatchesResult(res))
                {
                    if (i > 0) Approx(nf, what + " made as " + describe(variants[i]) + " (the writer's setting gave other geometry; chosen by the carried result)");
                    return null;
                }
                first = first ?? "its geometry differs from its carried result";
                doc.EditUndo2(variantUndo);
                if (res == null) break;
            }
            return what + ": no setting gives the carried result (" + (first ?? "none tried") + ")";
        }

        /// <summary>The part's sheet thickness (mm), from its sheetMetal feature in the payload, or 0.</summary>
        double SheetThickness()
        {
            var d = Model?.Arr("documents").Cast<JObj>().FirstOrDefault(x => x.Str("id") == docId);
            var sm = d?.Arr("features").Cast<JObj>().FirstOrDefault(f => f.Str("type") == "sheetMetal");
            return sm?.Obj("def")?.Num("thickness") ?? 0;
        }

        string ReplayHem(JObj nf, JObj def)
        {
            Func<string, bool, double, string> make = (pos, rev, len) =>
            {
                doc.ClearSelection2(true);
                string why = SelectAll(def.Arr("edges"), false, 0);
                if (why != null) return "hem edge: " + why;
                int n0 = FeatureCount();
                FM.InsertSheetMetalHem2(Neutral.Code("hemType", def["hemType"]), Neutral.Code("hemPosition", pos), rev, U.M(len),
                                        U.M(def.Num("gap")), U.Rad(def.Num("angle")), U.M(def.Num("radius")), U.M(def.Num("miterGap")),
                                        null, true, 0, 0, false, 0.5, 0, 0);
                return Created(n0, null, nf, "hem");
            };
            string p0 = def.Str("position") ?? "inside";
            bool r0 = def.Bool("reverse");
            double l0 = def.Num("length");
            if (!Foreign) return make(p0, r0, l0);
            // another system's hem: its position, sense and how it measures the length may be SolidWorks' other ones
            double t = SheetThickness(), g = def.Num("gap");
            var vs = new List<Tuple<string, bool, double>>();
            foreach (var len in new[] { l0, l0 - g - t, l0 + g + t })
                foreach (var pos in new[] { p0, p0 == "inside" ? "outside" : "inside" })
                    foreach (var rev in new[] { r0, !r0 })
                        if (len > 0) vs.Add(Tuple.Create(pos, rev, len));
            return TryVariants(nf, vs, v => make(v.Item1, v.Item2, v.Item3),
                               v => "position " + v.Item1 + (v.Item2 ? ", reversed" : "") + ", length " + v.Item3.ToString("0.###") + " mm", "hem");
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
            Func<string, bool, string> make = (pos, rev) =>
            {
                doc.ClearSelection2(true);
                line.Select2(false, 0);
                string why = SelectAll(new object[] { def.Obj("fixedFace") }, true, 0);
                if (why != null) return "fixed face: " + why;
                int n0 = FeatureCount();
                FM.InsertSheetMetal3dBend(U.Rad(def.Num("angle")), def.Bool("useDefaultRadius"), U.M(def.Num("radius")),
                                          rev, (short)Neutral.Code("flangePosition", pos), null);
                return Created(n0, null, nf, "sketched bend");
            };
            string p0 = def.Str("position") ?? "materialInside";
            bool r0 = def.Bool("reverse");
            if (!Foreign) return make(p0, r0);
            // another system's fold: where its bend sits on the line and which way it turns, by the carried result
            var vs = new List<Tuple<string, bool>>();
            foreach (var pos in new[] { p0, "materialInside", "materialOutside", "bendCenterline", "bendOutside" }.Distinct())
                foreach (var rev in new[] { r0, !r0 })
                    vs.Add(Tuple.Create(pos, rev));
            return TryVariants(nf, vs, v => make(v.Item1, v.Item2), v => "position " + v.Item1 + (v.Item2 ? ", reversed" : ""), "sketched bend");
        }

        /// <summary>Faces moved along their normals (FORMAT-SPEC 4 offsetFaces): Move Face, offset type; the sense is
        /// checked against the carried result.</summary>
        string ReplayOffsetFaces(JObj nf, JObj def)
        {
            double dist = def.Num("distance");
            Func<bool, string> make = rev =>
            {
                doc.ClearSelection2(true);
                string why = SelectAll(def.Arr("faces"), false, 1);   // the faces take mark 1 (mark 0 makes nothing)
                if (why != null) return "face: " + why;
                int n0 = FeatureCount();
                FM.InsertMoveFace3(0, rev, 0, U.M(Math.Abs(dist)), null, null, 0, 0);   // swMoveFaceTypeOffset, blind
                return Created(n0, null, nf, "offset faces");
            };
            return TryVariants(nf, new[] { dist < 0, dist >= 0 }, make, rev => rev ? "reversed" : "not reversed", "offset faces");
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
