// Replayer: revolve, reference planes, patterns, mirror, shell.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        /// <summary>Select every reference in `refs` with one mark; null or the first failure.</summary>
        string SelectAll(IEnumerable<object> refs, bool append, int mark)
        {
            foreach (JObj r in refs)
            {
                string why = SelectRef(r, append, mark);
                if (why != null) return why;
                append = true;
            }
            return null;
        }

        string Created(int n0, IFeature feat, JObj nf, string what)
        {
            doc.ClearSelection2(true);
            if (FeatureCount() == n0) return what + " was not created";
            feat = feat ?? LastFeature();
            Name(feat, nf.Str("name"));
            features[nf.Str("id")] = feat;
            return null;
        }

        /// <summary>Does a rebuilt reference axis point the other way from the source's? An axis made from two planes
        /// (or two points) has no inherent direction, and a revolve's direction is taken about it.</summary>
        bool AxisFlipped(JObj axisRef)
        {
            if (axisRef?.Str("kind") != "feature") return false;
            IFeature f;
            var src = Model?.Arr("documents").Cast<JObj>().SelectMany(d => d.Arr("features").Cast<JObj>())
                .FirstOrDefault(x => x.Str("id") == axisRef.Str("id") && x.Str("type") == "axis");
            var line = src?.Obj("def")?.Obj("line");
            if (line == null || !features.TryGetValue(axisRef.Str("id"), out f)) return false;
            var p = (double[])((IRefAxis)f.GetSpecificFeature2()).GetRefAxisParams();
            var now = new[] { p[3] - p[0], p[4] - p[1], p[5] - p[2] };
            return V.Dot(now, V.Sub(line.Vec("end"), line.Vec("start"))) < 0;
        }

        string ReplayRevolve(JObj nf, JObj def, bool cut)
        {
            doc.ClearSelection2(true);
            string why = SelectProfile(def);
            if (why != null) return "profile: " + why;
            why = SelectRef(def.Obj("axis"), true, 16);   // explicit axis: mark 16
            if (why != null) return "axis: " + why;
            string mode = def.Str("mode", "oneDirection");
            bool single = mode != "twoDirection";
            int t1 = mode == "midPlane" ? 6 : 0;   // swEndCondMidPlane / blind
            int n0 = FeatureCount();
            bool reverse = def.Bool("reverse") != ProfileMirrored(def.Obj("profile")) != AxisFlipped(def.Obj("axis"));
            var thin = def.Obj("thin");
            int tt;
            if (thin == null || !ThinTypes.TryGetValue(thin.Str("type") ?? "", out tt)) tt = 0;
            var feat = FM.FeatureRevolve2(single, true, thin != null, cut, reverse, false, t1, 0,   // a thin feature is still a solid
                                          U.Rad(def.Num("angle", 360)), U.Rad(def.Num("angle2")), false, false, 0, 0,
                                          tt, U.M(thin?.Num("thickness") ?? 0), U.M(thin?.Num("thickness2") ?? 0),
                                          def.Bool("merge", true), false, true);
            return Created(n0, feat, nf, cut ? "revolve cut" : "revolve");
        }

        string ReplayPlane(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            var refs = def.Arr("references").Cast<JObj>().ToList();
            if (refs.Count == 0 || refs.Count > 3) return "plane needs 1-3 references";
            var c = new int[3];
            var v = new double[3];
            for (int i = 0; i < refs.Count; i++)
            {
                string why = SelectRef(refs[i].Obj("ref"), i > 0, i);   // first reference mark 0, then 1, 2
                if (why != null) return "plane reference " + i + ": " + why;
                c[i] = (int)refs[i].Num("constraint");
                v[i] = (c[i] & 16) != 0 ? U.Rad(refs[i].Num("value")) : U.M(refs[i].Num("value"));
            }
            int n0 = FeatureCount();
            var feat = (IFeature)FM.InsertRefPlane(c[0], v[0], c[1], v[1], c[2], v[2]);
            string err = Created(n0, feat, nf, "plane");
            if (err != null) return err;
            // check the result against the recorded plane
            var rp = (IRefPlane)features[nf.Str("id")].GetSpecificFeature2();
            var a = (double[])((IMathTransform)rp.Transform).ArrayData;
            var origin = U.Mm(new[] { a[9], a[10], a[11] });
            var n = new[] { a[6], a[7], a[8] };
            var want = def.Vec("origin");
            var wantN = def.Vec("normal");
            if (want != null && wantN != null)
            {
                double off = Math.Abs(V.Dot(V.Sub(origin, want), V.Unit(wantN)));
                double ang = 1 - Math.Abs(V.Dot(V.Unit(n), V.Unit(wantN)));
                if (off > 1e-3 || ang > 1e-9)
                    return "plane differs from the source (offset " + off.ToString("G4") + " mm)";
                // a reversed normal is the same plane; sketches on it detect it and flip directions
            }
            return null;
        }

        string ReplayLinearPattern(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            var d1 = def.Obj("dir1");
            var d2 = def.Obj("dir2");
            string why = SelectRef(d1.Obj("ref"), false, 1);
            if (why != null) return "direction 1: " + why;
            if (d2 != null && (why = SelectRef(d2.Obj("ref"), true, 2)) != null) return "direction 2: " + why;
            if ((why = SelectAll(def.Arr("seeds"), true, 4)) != null) return "seed: " + why;
            if ((why = SelectPatternExtras(def)) != null) return why;
            int n0 = FeatureCount();
            var feat = FM.FeatureLinearPattern5((int)d1.Num("count"), U.M(d1.Num("spacing")),
                                                d2 == null ? 1 : (int)d2.Num("count"), d2 == null ? 0 : U.M(d2.Num("spacing")),
                                                d1.Bool("reverse"), d2 != null && d2.Bool("reverse"), "NULL", "NULL",
                                                def.Bool("geometryPattern"), false, false, false, true, true, false, false,
                                                false, false, 0, 0, def.Bool("seedOnly"), false);
            return Created(n0, feat, nf, "linear pattern") ?? SkipInstances(nf, def);
        }

        string ReplayCircularPattern(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("axis"), false, 1);
            if (why != null) return "axis: " + why;
            if ((why = SelectAll(def.Arr("seeds"), true, 4)) != null) return "seed: " + why;
            if ((why = SelectPatternExtras(def)) != null) return why;
            int n0 = FeatureCount();
            var feat = FM.FeatureCircularPattern5((int)def.Num("count"), U.Rad(def.Num("angle")), def.Bool("reverse"), "NULL",
                                                  def.Bool("geometryPattern"), def.Bool("equalSpacing"), false, false, false,
                                                  false, 1, 0, "NULL", false);
            return Created(n0, feat, nf, "circular pattern") ?? SkipInstances(nf, def);
        }

        /// <summary>Bodies (mark 256) and faces (mark 128) a pattern copies besides features.</summary>
        string SelectPatternExtras(JObj def)
        {
            foreach (JObj r in def.Arr("bodies"))
            {
                var body = FindBody(r);
                if (body == null) return "patterned body not found";
                if (!body.Select2(true, Mark(256))) return "could not select a patterned body";
            }
            string why = SelectAll(def.Arr("faces"), true, 128);
            return why == null ? null : "patterned face: " + why;
        }

        /// <summary>Instances the source skipped, set on the new pattern.</summary>
        string SkipInstances(JObj nf, JObj def)
        {
            var skipped = def.Arr("skipped");
            if (skipped.Count == 0) return null;
            var f = features[nf.Str("id")];
            var items = skipped.Select(x => Convert.ToInt32(x)).ToArray();
            object data = f.GetDefinition();
            if (data is ILinearPatternFeatureData) ((ILinearPatternFeatureData)data).SkippedItemArray = items;
            else if (data is ICircularPatternFeatureData) ((ICircularPatternFeatureData)data).SkippedItemArray = items;
            else return "skipped instances: not a linear or circular pattern";
            return f.ModifyDefinition(data, doc, null) ? null : "skipped instances could not be applied";
        }

        string ReplayMirrorBody(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("plane"), false, 2);
            if (why != null) return "mirror plane: " + why;
            foreach (JObj r in def.Arr("bodies"))
            {
                var body = FindBody(r);
                if (body == null) return "mirrored body not found";
                if (!body.Select2(true, Mark(256))) return "could not select a mirrored body";
            }
            int n0 = FeatureCount();
            var feat = FM.InsertMirrorFeature2(true, false, def.Bool("merge"), def.Bool("knit"), 0);
            return Created(n0, feat, nf, "mirror body");
        }

        string ReplayDeleteBody(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            bool append = false;
            foreach (JObj r in def.Arr("bodies"))
            {
                var body = FindBody(r);
                if (body == null) return "body to " + (def.Bool("keep") ? "keep" : "delete") + " not found";
                if (!body.Select2(append, Mark(0))) return "could not select a body";
                append = true;
            }
            int n0 = FeatureCount();
            var feat = FM.InsertDeleteBody2(def.Bool("keep"));
            return Created(n0, feat, nf, def.Bool("keep") ? "keep body" : "delete body");
        }

        string ReplayMirror(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("plane"), false, 2);
            if (why != null) return "mirror plane: " + why;
            if ((why = SelectAll(def.Arr("seeds"), true, 1)) != null) return "mirrored feature: " + why;
            int n0 = FeatureCount();
            var feat = FM.InsertMirrorFeature2(false, def.Bool("geometryPattern"), true, false, 0);
            return Created(n0, feat, nf, "mirror");
        }

        /// <summary>The wizard data reports a detailed hole type (swWzdHoleTypes_e, e.g. 14 = counterbore
        /// through); HoleWizard5 wants its family (swWzdGeneralHoleTypes_e).</summary>
        static int GeneralHoleType(int t)
        {
            if (t == 2 || t == 7 || (t >= 10 && t <= 21)) return 0;                                   // counterbore
            if (t == 3 || t == 8 || t == 24 || t == 29 || t == 30 || (t >= 43 && t <= 45)) return 1;  // countersink
            if ((t >= 37 && t <= 42)) return 3;                                                         // pipe tap
            if ((t >= 31 && t <= 36) || (t >= 46 && t <= 56)) return 4;                                // tap
            if (t >= 57 && t <= 68) return 6;                                                           // counterbore slot
            if (t == 71 || (t >= 76 && t <= 79) || t == 90) return 7;                                  // countersink slot
            if (t == 69 || t == 70 || (t >= 72 && t <= 75)) return 8;                                 // slot
            return 2;                                                                                   // hole
        }

        string ReplayHole(JObj nf, JObj def)
        {
            var w = def.Obj("wizard");
            if (w == null || w.Str("app") != "SOLIDWORKS") return "hole wizard data from another system not mapped yet";
            var pts = def.Arr("points").Select(p => ((List<object>)p).Select(Convert.ToDouble).Select(U.M).ToArray()).ToList();
            // the face is found by its descriptor; the hole is placed by picking that face at the first centre
            List<object> faces;
            var res = ResolveRef(def.Obj("face"), out faces);
            if (res.Str("result") != "resolved") return "hole face " + res.Str("result");
            doc.ClearSelection2(true);
            var p0 = pts[0];
            if (!doc.Extension.SelectByID2("", "FACE", p0[0], p0[1], p0[2], false, 0, null, 0)) return "could not pick the hole face";
            int n0 = FeatureCount();
            var feat = FM.HoleWizard5(GeneralHoleType((int)w.Num("holeType")), (int)w.Num("standard"), (int)w.Num("fastenerType"), w.Str("size"),
                                      (short)w.Num("endCondition"), 0, U.M(w.Num("depth")), 0,
                                      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, "", w.Bool("reverse"), true, true, false, false, false);
            string err = Created(n0, feat, nf, "hole");
            if (err != null) return err;
            // further holes are points in the hole's position sketch. That sketch is under-constrained, so
            // the picked point snaps and drags as others are added (2026 moved it 0.13 mm): finally every
            // point is set to its recorded centre.
            var hf = features[nf.Str("id")];
            IFeature sk = (IFeature)hf.GetFirstSubFeature();
            while (sk != null && sk.GetTypeName2() != "ProfileFeature") sk = (IFeature)sk.GetNextSubFeature();
            if (sk == null) return "hole position sketch not found";
            doc.ClearSelection2(true);
            sk.Select2(false, 0);
            doc.EditSketch();
            var smgr = (ISketchManager)doc.SketchManager;
            var active = (ISketch)smgr.ActiveSketch;
            var fr = new SketchFrame(sw, active);
            var want = pts.Select(fr.ToSketch).ToList();
            foreach (var s in want.Skip(1)) smgr.CreatePoint(s[0], s[1], 0);
            var have = ((object[])active.GetSketchPoints2() ?? new object[0]).Cast<ISketchPoint>().ToList();
            var free = new List<double[]>(want);
            foreach (var sp in have)
            {
                // nearest unclaimed centre (the projected origin and other construction points stay put)
                var best = free.OrderBy(s => Math.Pow(s[0] - sp.X, 2) + Math.Pow(s[1] - sp.Y, 2)).FirstOrDefault();
                if (best == null || Math.Abs(best[0] - sp.X) + Math.Abs(best[1] - sp.Y) > 0.005) continue;
                sp.SetCoords(best[0], best[1], 0);
                free.Remove(best);
            }
            smgr.InsertSketch(true);
            doc.EditRebuild3();
            // the position sketch with its relations and dimensions: its points are replaced by the payload's
            // sketch (the holes follow the points of the sketch when it closes)
            var posRef = def.Obj("positionSketch");
            JObj skNf;
            if (posRef == null || !deferredSketches.TryGetValue(posRef.Str("id"), out skNf)) return null;
            Func<string> open = () =>
            {
                doc.ClearSelection2(true);
                if (!sk.Select2(false, 0)) return "could not select the hole's position sketch";
                doc.EditSketch();
                var act = (ISketch)smgr.ActiveSketch;
                if (act == null) return "could not edit the hole's position sketch";
                doc.ClearSelection2(true);
                bool any = false;
                foreach (ISketchPoint q in (object[])act.GetSketchPoints2() ?? new object[0]) any |= q.Select4(any, null);
                if (any) doc.Extension.DeleteSelection2(0);
                return null;
            };
            string why = ReplaySketch(skNf, skNf.Obj("def"), open);
            doc.EditRebuild3();
            return why == null ? null : "position sketch: " + why;
        }

        string ReplayDraft(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectRef(def.Obj("neutral"), false, 1);
            if (why != null) return "neutral plane: " + why;
            if ((why = SelectAll(def.Arr("faces"), true, 2)) != null) return "drafted face: " + why;
            int n0 = FeatureCount();
            var feat = FM.InsertMultiFaceDraft(U.Rad(def.Num("angle")), def.Bool("reverse"), false,
                                               (int)def.Num("propagation"), false, false);
            return Created(n0, feat, nf, "draft");
        }

        string ReplayShell(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def.Arr("faces"), false, 0);
            if (why != null) return "removed face: " + why;
            int n0 = FeatureCount();
            doc.InsertFeatureShell(U.M(def.Num("thickness")), def.Bool("outward"));
            return Created(n0, null, nf, "shell");
        }
    }
}
