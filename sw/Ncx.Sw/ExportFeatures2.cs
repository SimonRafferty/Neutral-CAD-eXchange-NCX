// Exporter: revolve, reference planes, patterns, mirror, shell.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        List<object> FeatureRefs(object arr, JObj nf, string what)
        {
            var refs = new List<object>();
            foreach (var o in (object[])arr ?? new object[0])
            {
                var r = EntityRef(o);
                if (r == null) { Opaque(nf, what + " not recognised"); return null; }
                refs.Add(r);
            }
            return refs;
        }

        List<object> BodyRefs(object arr, JObj nf, string what)
        {
            var refs = new List<object>();
            foreach (var o in (object[])arr ?? new object[0])
            {
                var r = BodyRef(o);
                if (r == null) { Opaque(nf, what + " not recognised"); return null; }
                refs.Add(r);
            }
            return refs;
        }

        /// <summary>What a pattern copies besides features (bodies, faces), and the instances it skips.</summary>
        bool PatternExtras(JObj def, object bodies, int bodyCount, object faces, int faceCount, int skipCount, object skipped, JObj nf)
        {
            if (bodyCount > 0)
            {
                var b = BodyRefs(bodies, nf, "patterned body");
                if (b == null) return false;
                def["bodies"] = b;
            }
            if (faceCount > 0)
            {
                var f = FeatureRefs(faces, nf, "patterned face");
                if (f == null) return false;
                def["faces"] = f;
            }
            if (skipCount > 0) def["skipped"] = ((int[])skipped ?? new int[0]).Cast<object>().ToList();   // instance indices
            return true;
        }

        void ExportRevolve(IFeature f, JObj nf, bool cut)
        {
            var d = (IRevolveFeatureData2)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = cut ? "revolveCut" : "revolve";
                var def = new JObj { { "profile", ProfileRef(f, nf) } };
                if (nf.Str("status") == "opaque") return;
                var axis = EntityRef(d.Axis);
                if (axis == null) { Opaque(nf, "revolve axis not recognised"); return; }
                def["axis"] = axis;
                int t = d.Type;   // swRevolveType_e
                def["mode"] = t % 3 == 0 ? "oneDirection" : t % 3 == 1 ? "midPlane" : "twoDirection";
                def["angle"] = U.Deg(d.GetRevolutionAngle(true));
                if (t % 3 == 2) def["angle2"] = U.Deg(d.GetRevolutionAngle(false));
                def["reverse"] = d.ReverseDirection;
                if (!cut) def["merge"] = d.Merge;
                if (d.GetContoursCount() > 0)
                {
                    string cwhy;
                    var contours = Contours((object[])d.Contours, ((IFeature)f.GetFirstSubFeature()).Name, out cwhy);
                    if (contours == null) Approx(nf, "selected contours not carried (" + cwhy + "); whole sketch used");
                    else def["contours"] = contours;
                }
                if (d.IsThinFeature())
                    def["thin"] = new JObj
                    {
                        { "type", ThinTypes[Math.Max(0, Math.Min(3, d.ThinWallType))] },
                        { "thickness", U.Mm(d.GetWallThickness(true)) }, { "thickness2", U.Mm(d.GetWallThickness(false)) }
                    };
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportRefPlane(IFeature f, JObj nf)
        {
            var d = (IRefPlaneFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "plane";
                var refs = new List<object>();
                int n = d.GetSelectionsCount();
                for (int i = 0; i < n; i++)
                {
                    var r = EntityRef(d.get_Reference(i));
                    if (r == null) { Opaque(nf, "plane reference " + i + " not recognised"); return; }
                    int c = d.get_Constraint(i);   // swRefPlaneReferenceConstraints_e flags
                    double v = d.get_AngleOrDistance(i);
                    var pr = new JObj { { "ref", r } };
                    Neutral.PlaneConstraint(pr, c);
                    pr["value"] = (c & 16) != 0 ? U.Deg(v) : U.Mm(v);   // angle in degrees, else mm
                    pr["flip"] = (c & 256) != 0;
                    refs.Add(pr);
                }
                // the resulting plane, so replay can check it and other systems can rebuild it directly
                var rp = (IRefPlane)f.GetSpecificFeature2();
                var mt = (IMathTransform)rp.Transform;
                var a = (double[])mt.ArrayData;
                nf["def"] = new JObj
                {
                    { "references", refs },
                    { "origin", U.Mm(new[] { a[9], a[10], a[11] }) }, { "normal", new[] { a[6], a[7], a[8] } }
                };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        JObj PatternDirection(object axis, bool reverse, double spacing, int count, JObj nf, string which)
        {
            var r = EntityRef(axis);
            if (r == null) { Opaque(nf, which + " direction not recognised"); return null; }
            return new JObj { { "ref", r }, { "reverse", reverse }, { "spacing", U.Mm(spacing) }, { "count", count } };
        }

        void ExportLinearPattern(IFeature f, JObj nf)
        {
            var d = (ILinearPatternFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "linearPattern";
                var seeds = FeatureRefs(d.PatternFeatureArray, nf, "pattern seed");
                if (seeds == null) return;
                var def = new JObj { { "seeds", seeds } };
                if (!PatternExtras(def, d.PatternBodyArray, d.GetPatternBodyCount(), d.PatternFaceArray, d.GetPatternFaceCount(),
                                   d.GetSkippedItemCount(), d.SkippedItemArray, nf)) return;
                def["dir1"] = PatternDirection(d.D1Axis, d.D1ReverseDirection, d.D1Spacing, d.D1TotalInstances, nf, "first");
                if (d.D2TotalInstances > 1)
                    def["dir2"] = PatternDirection(d.D2Axis, d.D2ReverseDirection, d.D2Spacing, d.D2TotalInstances, nf, "second");
                if (nf.Str("status") == "opaque") return;
                def["geometryPattern"] = d.GeometryPattern;
                def["seedOnly"] = d.D2PatternSeedOnly;
                if (d.VarySketch) Approx(nf, "vary sketch not carried");
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportCircularPattern(IFeature f, JObj nf)
        {
            var d = (ICircularPatternFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "circularPattern";
                var seeds = FeatureRefs(d.PatternFeatureArray, nf, "pattern seed");
                if (seeds == null) return;
                var axis = EntityRef(d.Axis);
                if (axis == null) { Opaque(nf, "pattern axis not recognised"); return; }
                var def = new JObj
                {
                    { "seeds", seeds }, { "axis", axis }, { "count", d.TotalInstances }, { "angle", U.Deg(d.Spacing) },
                    { "equalSpacing", d.EqualSpacing }, { "reverse", d.ReverseDirection }, { "geometryPattern", d.GeometryPattern }
                };
                if (!PatternExtras(def, d.PatternBodyArray, d.GetPatternBodyCount(), d.PatternFaceArray, d.GetPatternFaceCount(),
                                   d.GetSkippedItemCount(), d.SkippedItemArray, nf)) return;
                if (d.Direction2) Approx(nf, "second direction not carried");
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportMirror(IFeature f, JObj nf)
        {
            var d = (IMirrorPatternFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "mirror";
                var seeds = FeatureRefs(d.PatternFeatureArray, nf, "mirrored feature");
                if (seeds == null) return;
                if (d.GetMirrorFaceCount() > 0) { Opaque(nf, "face mirrors not mapped yet"); return; }
                var plane = EntityRef(d.Plane);
                if (plane == null) { Opaque(nf, "mirror plane not recognised"); return; }
                nf["def"] = new JObj { { "seeds", seeds }, { "plane", plane }, { "geometryPattern", d.GeometryPattern } };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportMirrorBody(IFeature f, JObj nf)
        {
            var d = (IMirrorSolidFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "mirrorBody";
                var bodies = BodyRefs(d.PatternBodyArray, nf, "mirrored body");
                if (bodies == null) return;
                var plane = EntityRef(d.Face);
                if (plane == null) { Opaque(nf, "mirror plane not recognised"); return; }
                nf["def"] = new JObj { { "bodies", bodies }, { "plane", plane }, { "merge", d.Merge }, { "knit", d.KnitSurface } };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportDeleteBody(IFeature f, JObj nf)
        {
            var d = (IDeleteBodyFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            try
            {
                nf["type"] = "deleteBody";
                var bodies = BodyRefs(d.Bodies, nf, "deleted body");
                if (bodies == null) return;
                nf["def"] = new JObj { { "bodies", bodies }, { "keep", d.KeepBodies } };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>A Hole Wizard's position sketch: its first sketch sub-feature (the one whose points place the holes).</summary>
        static IFeature HolePositionSketch(IFeature hole)
        {
            for (var sub = (IFeature)hole.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                if (sub.GetTypeName2() == "ProfileFeature") return sub;
            return null;
        }

        /// <summary>The distinct diameters (mm, ascending) of a hole feature's cylindrical faces.</summary>
        static List<double> HoleDiameters(IFeature f)
        {
            var out_ = new List<double>();
            foreach (IFace2 face in (object[])f.GetFaces() ?? new object[0])
            {
                var s = (ISurface)face.GetSurface();
                if (!s.IsCylinder()) continue;
                double dia = Math.Round(U.Mm(((double[])s.CylinderParams)[6]) * 2, 6);
                if (!out_.Any(x => Math.Abs(x - dia) < 1e-4)) out_.Add(dia);
            }
            out_.Sort();
            return out_;
        }

        void ExportHoleWizard(IFeature f, JObj nf)
        {
            var d = (IWizardHoleFeatureData2)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "hole";
                // the data's Face is not a usable entity; the position sketch sits on the hole face
                JObj face = EntityRef(d.Face);
                if (face == null)
                {
                    for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null && face == null; sub = (IFeature)sub.GetNextSubFeature())
                        if (sub.GetTypeName2() == "ProfileFeature")
                        {
                            int t = 0;
                            face = EntityRef(((ISketch)sub.GetSpecificFeature2()).GetReferenceEntity(ref t));
                        }
                }
                if (face == null) { Opaque(nf, "hole face not recognised"); return; }
                var pts = ((object[])d.GetSketchPoints() ?? new object[0]).Cast<ISketchPoint>()
                    .Select(p => (object)U.Mm(ModelPoint(p))).ToList();
                if (pts.Count == 0) { Opaque(nf, "hole positions not found"); return; }
                // every size the wizard reports, so another system (or a different standards table) can be checked
                var sizes = new JObj
                {
                    { "diameter", U.Mm(d.Diameter) }, { "holeDiameter", U.Mm(d.HoleDiameter) }, { "holeDepth", U.Mm(d.HoleDepth) },
                    { "counterBoreDiameter", U.Mm(d.CounterBoreDiameter) }, { "counterBoreDepth", U.Mm(d.CounterBoreDepth) },
                    { "counterSinkDiameter", U.Mm(d.CounterSinkDiameter) }, { "counterSinkAngle", U.Deg(d.CounterSinkAngle) },
                    { "tapDrillDiameter", U.Mm(d.TapDrillDiameter) }, { "threadDepth", U.Mm(d.ThreadDepth) },
                    { "drillAngle", U.Deg(d.DrillAngle) }
                };
                // SolidWorks leaves sizes its table supplies at 0 (an M6 counterbore's drilled diameter): the hole's own
                // cylindrical faces give them, smallest = the drilled hole, largest = a counterbore. Measured once the
                // model is rolled forward: the feature has no faces in the rolled-back state it is exported in.
                int kind = Neutral.GeneralHoleType(d.Type);
                afterExport.Add(() =>
                {
                    var dias = HoleDiameters(f);
                    if (dias.Count == 0) return;
                    sizes["measuredDiameters"] = dias.Cast<object>().ToList();
                    if (sizes.Num("holeDiameter") <= 0) sizes["holeDiameter"] = dias[0];
                    if ((kind == 0 || kind == 6) && sizes.Num("counterBoreDiameter") <= 0 && dias.Count > 1)
                        sizes["counterBoreDiameter"] = dias[dias.Count - 1];
                });
                nf["def"] = new JObj
                {
                    { "face", face }, { "points", pts },
                    { "holeType", Neutral.Word("holeKind", kind) },
                    { "end", new JObj { { "type", Neutral.Word("endCondition", d.EndCondition) }, { "depth", U.Mm(d.Depth) } } },
                    { "wizard", new JObj { { "app", "SOLIDWORKS" }, { "holeType", d.Type }, { "standard", d.Standard2 },
                                           { "fastenerType", d.FastenerType2 }, { "size", d.FastenerSize },
                                           { "endCondition", d.EndCondition }, { "depth", U.Mm(d.Depth) },
                                           { "reverse", d.ReverseDirection } } },
                    { "sizes", sizes }
                };
                // the position sketch (its relations and dimensions); the centres alone if it was not carried
                var position = HolePositionSketch(f);
                var posRef = position == null ? null : FeatureRef(position);
                if (posRef != null) ((JObj)nf["def"])["positionSketch"] = posRef;
                else Approx(nf, "position-sketch dimensions not carried (hole centres are)");
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportDraft(IFeature f, JObj nf)
        {
            var d = (IDraftFeatureData2)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "draft";
                if (d.Type != 0) { Opaque(nf, "draft type " + d.Type + " (parting line/step) not mapped yet"); return; }   // neutral plane only
                var neutral = EntityRef(d.NeutralPlane);
                if (neutral == null) { Opaque(nf, "neutral plane not recognised"); return; }
                var faces = FeatureRefs(d.FacesToDraft, nf, "drafted face");
                if (faces == null) return;
                nf["def"] = new JObj
                {
                    { "neutral", neutral }, { "faces", faces }, { "angle", U.Deg(d.Angle) },
                    { "reverse", d.ReverseDirection }, { "propagation", d.FacePropagation }
                };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportShell(IFeature f, JObj nf)
        {
            var d = (IShellFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "shell";
                var faces = FeatureRefs(d.FacesRemoved, nf, "removed face");
                if (faces == null) return;
                if (d.GetMultipleThicknessFacesCount() > 0) Approx(nf, "multi-thickness faces replayed at the default thickness");
                nf["def"] = new JObj { { "thickness", U.Mm(d.Thickness) }, { "outward", d.Direction }, { "faces", faces } };
            }
            finally { d.ReleaseSelectionAccess(); }
        }
    }
}
