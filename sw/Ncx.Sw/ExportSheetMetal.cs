// Exporter: sheet metal. A base flange creates three tree items: the Sheet-Metal defaults feature (above it),
// the flange itself and the Flat-Pattern (kept last). The defaults and the flat pattern are carried as
// markers with their values; replay applies them to the items the base flange creates.
//
// An edge flange's profile sketch lies on a plane SolidWorks generates from the edge: the sketch is
// exported as usual (its frame carries the geometry) but marked "generated", and the edge flange replays
// it into the plane it generates in the target.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        // sketch name -> name of the feature that generates its plane (edge flanges)
        readonly Dictionary<string, string> generatedSketches = new Dictionary<string, string>();

        void CollectGeneratedSketches(List<IFeature> all)
        {
            generatedSketches.Clear();
            foreach (var f in all.Where(x => x.GetTypeName2() == "EdgeFlange"))
                for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                    if (sub.GetTypeName2() == "ProfileFeature") generatedSketches[sub.Name] = f.Name;
            // a Hole Wizard's position sketch: the hole makes it, then replays it in place
            foreach (var f in all.Where(x => x.GetTypeName2() == "HoleWzd"))
            {
                var position = HolePositionSketch(f);
                if (position != null) generatedSketches[position.Name] = f.Name;
            }
        }

        void ExportSheetMetalDefaults(IFeature f, JObj nf)
        {
            var d = (ISheetMetalFeatureData)f.GetDefinition();
            nf["type"] = "sheetMetal";
            nf["def"] = new JObj
            {
                { "thickness", U.Mm(d.Thickness) }, { "bendRadius", U.Mm(d.BendRadius) },
                { "bendAllowanceType", Neutral.Word("bendAllowance", d.BendAllowanceType) }, { "kFactor", d.KFactor },
                { "bendAllowance", U.Mm(d.BendAllowance) },
                { "autoRelief", d.UseAutoRelief }, { "reliefType", Neutral.Word("relief", d.AutoReliefType) }, { "reliefRatio", d.ReliefRatio }
            };
            if (d.BendAllowanceType == 1 && !string.IsNullOrEmpty(d.BendTableFile))   // swBendAllowanceBendTable
                Approx(nf, "bend table " + d.BendTableFile + " not carried (K-factor used)");
        }

        void ExportBaseFlange(IFeature f, JObj nf)
        {
            var d = (IBaseFlangeFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "baseFlange";
                IFeature sk = null;
                for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null && sk == null; sub = (IFeature)sub.GetNextSubFeature())
                    if (sub.GetTypeName2() == "ProfileFeature") sk = sub;
                var profile = sk == null ? null : FeatureRef(sk);
                if (profile == null) { Opaque(nf, "base flange profile not exported"); return; }
                var def = new JObj
                {
                    { "profile", profile }, { "thickness", U.Mm(d.Thickness) }, { "reverseThickness", d.ReverseThickness },
                    { "bendRadius", U.Mm(d.BendRadius) }, { "reverse", d.ReverseDirection }, { "kFactor", d.KFactor },
                    { "native", new JObj { { "offsetDirections", d.OffsetDirections }, { "d1OffsetType", d.D1OffsetType },
                                           { "d2OffsetType", d.D2OffsetType } } }
                };
                // an open profile is extruded: swFlangeOffsetTypes_e (1 blind, 5 mid-plane); a closed one has no depth.
                // An unused second direction reads back as blind with a ~0.0001 mm depth.
                double depth1 = U.Mm(d.D1OffsetDistance), depth2 = U.Mm(d.D2OffsetDistance);
                if (depth1 > 1e-3)
                {
                    string t1 = d.D1OffsetType == 5 ? "midPlane" : "blind";
                    if (d.D1OffsetType != 1 && d.D1OffsetType != 5) Approx(nf, "flange end condition " + d.D1OffsetType + " replayed blind");
                    def["end"] = new JObj { { "type", t1 }, { "depth", depth1 } };
                    if (t1 == "blind" && depth2 > 1e-3 && d.OffsetDirections == 2)   // extruded both ways
                        def["end2"] = new JObj { { "type", "blind" }, { "depth", depth2 } };
                    else if (t1 == "blind" && depth2 > 1e-3)
                        Approx(nf, "second direction (" + U.R(depth2) + " mm) not carried");
                }
                if (d.UseGaugeTable) Approx(nf, "gauge table " + d.GaugeTablePath + " not carried (its values are)");
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportHem(IFeature f, JObj nf)
        {
            var d = (IHemFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "hem";
                var edges = EntityRefs(d.Edges, nf, "hem edge");
                if (edges == null) return;
                nf["def"] = new JObj
                {
                    { "edges", edges }, { "hemType", Neutral.Word("hemType", d.Type) },
                    { "position", Neutral.Word("hemPosition", d.BendPosition) }, { "reverse", d.ReverseDirection },
                    { "length", U.Mm(d.Length) }, { "gap", U.Mm(d.GapDistance) }, { "angle", U.Deg(d.Angle) },
                    { "radius", U.Mm(d.Radius) }, { "miterGap", U.Mm(d.MiterGap) }
                };
                if (!d.UseDefaultBendAllowance) Approx(nf, "hem bend allowance not carried (sheet-metal default used)");
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportBreakCorner(IFeature f, JObj nf)
        {
            var d = (IBreakCornerFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "breakCorner";
                var ents = EntityRefs(d.Entities, nf, "corner");
                if (ents == null) return;
                nf["def"] = new JObj
                {
                    { "entities", ents }, { "breakType", Neutral.Word("breakType", d.BreakType) },
                    { "distance", U.Mm(d.Distance) }
                };
                if (d.CenteredOnBendLines || d.InternalCornersOnly) Approx(nf, "corner options (centred on bend lines, internal corners only) not carried");
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportSketchedBend(IFeature f, JObj nf)
        {
            var d = (ISketchedBendFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "sketchedBend";
                IFeature sk = null;
                for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null && sk == null; sub = (IFeature)sub.GetNextSubFeature())
                    if (sub.GetTypeName2() == "ProfileFeature") sk = sub;
                var line = sk == null ? null : FeatureRef(sk);
                if (line == null) { Opaque(nf, "bend line sketch not exported"); return; }
                // the face that stays put (SolidWorks also keeps the pick point, which is only where the face was
                // clicked - or the origin when selected through the API - so the face itself is carried)
                double x, y, z;
                var fixedFace = EntityRef(d.GetFixedFace(out x, out y, out z));
                if (fixedFace == null) { Opaque(nf, "fixed face not recognised"); return; }
                nf["def"] = new JObj
                {
                    { "line", line }, { "fixedFace", fixedFace },
                    { "angle", U.Deg(d.BendAngle) }, { "reverse", d.ReverseDirection }, { "position", Neutral.Word("flangePosition", d.PositionType) },
                    { "useDefaultRadius", d.UseDefaultBendRadius }, { "radius", U.Mm(d.BendRadius) }
                };
                if (!d.UseDefaultBendAllowance) Approx(nf, "bend allowance not carried (sheet-metal default used)");
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportJog(IFeature f, JObj nf)
        {
            var d = (IJogFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "jog";
                IFeature sk = null;
                for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null && sk == null; sub = (IFeature)sub.GetNextSubFeature())
                    if (sub.GetTypeName2() == "ProfileFeature") sk = sub;
                var line = sk == null ? null : FeatureRef(sk);
                if (line == null) { Opaque(nf, "jog line sketch not exported"); return; }
                var fixedFace = EntityRef(d.FixedFace);
                if (fixedFace == null) { Opaque(nf, "fixed face not recognised"); return; }
                nf["def"] = new JObj
                {
                    { "line", line }, { "fixedFace", fixedFace }, { "angle", U.Deg(d.JogAngle) },
                    { "offset", U.Mm(d.OffsetDistance) }, { "reverse", d.ReverseDirection },
                    { "dimensionPosition", Neutral.Word("jogDimension", d.DimensionPositionType) },
                    { "bendPosition", Neutral.Word("jogPosition", d.JogPositionType) }, { "fixProjectedLength", d.FixProjectedLength },
                    { "useDefaultRadius", d.UseDefaultBendRadius }, { "radius", U.Mm(d.BendRadius) }
                };
                if (d.ReverseOffset) Approx(nf, "reversed offset not carried");
                if (d.OffsetType != 1) Approx(nf, "jog offset up to a reference (type " + d.OffsetType + ") replayed as a distance");   // 1: blind
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportMiterFlange(IFeature f, JObj nf)
        {
            var d = (IMiterFlangeFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "miterFlange";
                IFeature sk = null;
                for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null && sk == null; sub = (IFeature)sub.GetNextSubFeature())
                    if (sub.GetTypeName2() == "ProfileFeature") sk = sub;
                var profile = sk == null ? null : FeatureRef(sk);
                if (profile == null) { Opaque(nf, "miter flange profile not exported"); return; }
                var edges = EntityRefs(d.Edges, nf, "miter flange edge");
                if (edges == null) return;
                nf["def"] = new JObj
                {
                    { "profile", profile }, { "edges", edges },
                    { "useDefaultRadius", d.UseDefaultBendRadius }, { "bendRadius", U.Mm(d.BendRadius) },
                    { "position", Neutral.Word("flangePosition", d.PositionType) }, { "trimSideBends", d.UsePositionTrimSideBends },
                    { "gap", U.Mm(d.GapDistance) }, { "startOffset", U.Mm(d.StartOffset) }, { "endOffset", U.Mm(d.EndOffset) },
                    { "useDefaultRelief", d.UseDefaultBendRelief }, { "reliefType", Neutral.Word("relief", d.ReliefType) },
                    { "useReliefRatio", d.UseReliefRatio }, { "reliefRatio", d.ReliefRatio },
                    { "reliefWidth", U.Mm(d.ReliefWidth) }, { "reliefDepth", U.Mm(d.ReliefDepth) }
                };
                if (!d.UseDefaultBendAllowance) Approx(nf, "miter flange bend allowance not carried (sheet-metal default used)");
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportClosedCorner(IFeature f, JObj nf)
        {
            var d = f.GetDefinition() as IClosedCornerFeatureData;
            if (d == null) { Opaque(nf, "corner feature kind not mapped yet"); return; }
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "closedCorner";
                var faces = EntityRefs(d.Faces, nf, "corner face to extend");
                if (faces == null) return;
                nf["def"] = new JObj
                {
                    { "faces", faces }, { "cornerType", Neutral.Word("closedCorner", d.CornerType) },
                    { "gap", U.Mm(d.GapDistance) }, { "overlapRatio", d.OverlapUnderlapRatio }, { "openBendRegion", d.OpenBendRegion }
                };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>Edges, faces or vertices as descriptors; null (feature made opaque) if one is not recognised.</summary>
        List<object> EntityRefs(object arr, JObj nf, string what)
        {
            var refs = new List<object>();
            foreach (var o in (object[])arr ?? new object[0])
            {
                var r = EntityRef(o);
                if (r == null) { Opaque(nf, what + " not recognised"); return null; }
                refs.Add(r);
            }
            if (refs.Count == 0) { Opaque(nf, "no " + what + " found"); return null; }
            return refs;
        }

        void ExportEdgeFlange(IFeature f, JObj nf)
        {
            var d = (IEdgeFlangeFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "edgeFlange";
                var edges = ((object[])d.Edges ?? new[] { d.Edge }).Where(e => e != null).ToList();
                // one profile sketch per edge, in edge order
                var profiles = new List<object>();
                for (var sub = (IFeature)f.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature())
                    if (sub.GetTypeName2() == "ProfileFeature") profiles.Add(FeatureRef(sub));
                if (edges.Count == 0 || profiles.Count != edges.Count || profiles.Any(x => x == null))
                {
                    Opaque(nf, edges.Count + " edges with " + profiles.Count + " profiles not mapped");
                    return;
                }
                var edgeRefs = edges.Select(EntityRef).ToList();
                if (edgeRefs.Any(x => x == null)) { Opaque(nf, "flange edge not recognised"); return; }
                var def = new JObj
                {
                    { "edge", edgeRefs[0] }, { "profile", profiles[0] }, { "angle", U.Deg(d.BendAngle) },
                    { "useDefaultRadius", d.UseDefaultBendRadius }, { "bendRadius", U.Mm(d.BendRadius) },
                    { "position", Neutral.Word("flangePosition", d.PositionType) },
                    { "useDefaultRelief", d.UseDefaultBendRelief }, { "reliefType", Neutral.Word("relief", d.AutoReliefType) },
                    { "useReliefRatio", d.UseReliefRatio }, { "reliefRatio", d.ReliefRatio },
                    { "reliefWidth", U.Mm(d.ReliefWidth) }, { "reliefDepth", U.Mm(d.ReliefDepth) }
                };
                if (edges.Count > 1) { def["edges"] = edgeRefs.Cast<object>().ToList(); def["profiles"] = profiles; }
                if (d.UsePositionOffset)   // swFlangeOffsetTypes_e; only a plain distance is carried
                {
                    def["positionOffset"] = new JObj { { "type", Neutral.Word("flangeOffset", d.PositionOffsetType) }, { "distance", U.Mm(d.PositionOffsetDistance) },
                                                       { "reverse", d.ReversePositionOffset } };
                    if (d.PositionOffsetType != 0 && d.PositionOffsetType != 1)
                        Approx(nf, "position offset up to a reference (type " + d.PositionOffsetType + ") carried as its distance");
                }
                if (!d.UseDefaultBendAllowance) Approx(nf, "flange bend allowance not carried (sheet-metal default used)");
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }
    }
}
