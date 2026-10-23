// The neutral words for enumerated values in feature definitions (FORMAT-SPEC 4, "Enumerated values"), and the
// SolidWorks codes they stand for. The exporter writes the words; the replayer reads words, or the SolidWorks numbers
// that files from add-in 0.4.0 and earlier carry. One table per kind of value, so the spec's list and this file
// can be checked against each other.
using System;
using System.Collections.Generic;
using System.Linq;

namespace Ncx.Sw
{
    public static class Neutral
    {
        static readonly Dictionary<string, Dictionary<int, string>> Tables = new Dictionary<string, Dictionary<int, string>>
        {
            // swRefAxisType_e
            { "axisType", new Dictionary<int, string> { { 0, "line" }, { 1, "twoPlanes" }, { 2, "twoPoints" }, { 3, "cylindricalFace" }, { 4, "pointAndPlane" } } },
            // swRefPointType_e
            { "pointType", new Dictionary<int, string> { { 2, "alongCurve" }, { 3, "arcCenter" }, { 4, "faceCenter" }, { 5, "projection" }, { 6, "intersection" }, { 7, "sketchPoint" } } },
            // swEndConditions_e (as the hole and other features use it)
            { "endCondition", new Dictionary<int, string> { { 0, "blind" }, { 1, "throughAll" }, { 2, "throughNext" }, { 3, "upToVertex" }, { 4, "upToSurface" }, { 5, "offsetFromSurface" }, { 6, "midPlane" }, { 7, "upToBody" }, { 9, "throughAllBoth" }, { 10, "upToSelection" }, { 11, "upToNext" } } },
            // swCosmeticThreadType_e
            { "threadEnd", new Dictionary<int, string> { { 0, "blind" }, { 1, "upToNext" }, { 2, "throughFeature" } } },
            // swCosmeticThreadDiameterType_e
            { "threadDiameter", new Dictionary<int, string> { { 1, "conicalOffset" }, { 2, "major" }, { 3, "minor" } } },
            // swHemTypes_e
            { "hemType", new Dictionary<int, string> { { 0, "open" }, { 1, "closed" }, { 2, "teardrop" }, { 3, "rolled" }, { 4, "double" } } },
            // swHemPositionTypes_e
            { "hemPosition", new Dictionary<int, string> { { 0, "inside" }, { 1, "outside" } } },
            // swBreakCornerTypes_e
            { "breakType", new Dictionary<int, string> { { 0, "fillet" }, { 1, "chamfer" } } },
            // swClosedCornerTypes_e
            { "closedCorner", new Dictionary<int, string> { { 1, "butt" }, { 2, "overlap" }, { 3, "underlap" } } },
            // swSheetMetalReliefTypes_e
            { "relief", new Dictionary<int, string> { { 1, "rectangular" }, { 2, "tear" }, { 3, "obround" }, { 4, "none" }, { 5, "tearBend" } } },
            // swFlangePositionTypes_e (edge flange, miter flange, sketched bend)
            { "flangePosition", new Dictionary<int, string> { { 1, "materialInside" }, { 2, "materialOutside" }, { 3, "bendOutside" }, { 4, "bendCenterline" }, { 5, "bendSharp" }, { 6, "bendTangent" } } },
            // swFlangeOffsetTypes_e
            { "flangeOffset", new Dictionary<int, string> { { 1, "blind" }, { 2, "upToVertex" }, { 3, "upToSurface" }, { 4, "offsetFromSurface" }, { 5, "midPlane" }, { 6, "upToEdgeAndMerge" } } },
            // swJogPositionType_e
            { "jogPosition", new Dictionary<int, string> { { 1, "bendCenterline" }, { 2, "materialInside" }, { 3, "materialOutside" }, { 4, "bendOutside" } } },
            // swJogDimensionPositionType_e
            { "jogDimension", new Dictionary<int, string> { { 1, "insideOffset" }, { 2, "outsideOffset" }, { 3, "overall" } } },
            // swSolidworksWeldmentEndCondOptions_e (weldment trim / extend)
            { "trimCorner", new Dictionary<int, string> { { 0, "none" }, { 1, "miter" }, { 2, "butt1" }, { 3, "butt2" }, { 4, "endTrim" } } },
            // swGussetProfileType_e
            { "gussetProfile", new Dictionary<int, string> { { 0, "polygon" }, { 1, "triangle" } } },
            // swGussetThicknessType_e
            { "gussetThickness", new Dictionary<int, string> { { 0, "inner" }, { 1, "bothSides" }, { 2, "outer" } } },
            // swGussetProfileLocationType_e
            { "gussetLocation", new Dictionary<int, string> { { 0, "start" }, { 1, "center" }, { 2, "end" } } },
            // swRibExtrusionDirection_e
            { "ribExtrusion", new Dictionary<int, string> { { 0, "parallelToSketch" }, { 1, "normalToSketch" } } },
            // swBendAllowanceTypes_e
            { "bendAllowance", new Dictionary<int, string> { { 1, "bendTable" }, { 2, "kFactor" }, { 3, "bendAllowance" }, { 4, "bendDeduction" }, { 5, "bendCalculationTable" }, { 6, "gaugeTable" } } },
            // swRibType_e
            { "ribType", new Dictionary<int, string> { { 0, "linear" }, { 1, "natural" } } },
            // swRefPointAlongCurveType_e
            { "alongCurve", new Dictionary<int, string> { { 0, "distance" }, { 1, "percentage" }, { 2, "evenlyDistributed" } } },
            // swMateWidthOptions_e
            { "widthConstraint", new Dictionary<int, string> { { 0, "centered" }, { 1, "free" }, { 2, "distance" }, { 3, "percent" } } },
            // swSlotMateConstraintOptions_e
            { "slotConstraint", new Dictionary<int, string> { { 0, "free" }, { 1, "centered" }, { 2, "distance" }, { 3, "percent" } } },
            // swWzdGeneralHoleTypes_e
            { "holeKind", new Dictionary<int, string> { { 0, "counterbore" }, { 1, "countersink" }, { 2, "simple" }, { 3, "pipeTap" }, { 4, "tapped" }, { 5, "legacy" }, { 6, "counterboreSlot" }, { 7, "countersinkSlot" }, { 8, "slot" } } },
        };

        /// <summary>The wizard data reports a detailed hole type (swWzdHoleTypes_e, e.g. 14 = counterbore
        /// through); HoleWizard5 wants its family (swWzdGeneralHoleTypes_e).</summary>
        public static int GeneralHoleType(int t)
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

        // swRefPlaneReferenceConstraints_e: one primary constraint plus option bits (flip is carried as its own flag)
        static readonly Dictionary<int, string> PlanePrimary = new Dictionary<int, string>
        {
            { 1, "parallel" }, { 2, "perpendicular" }, { 4, "coincident" }, { 8, "distance" }, { 16, "angle" }, { 32, "tangent" },
            { 64, "project" }, { 128, "midPlane" }, { 4096, "parallelToScreen" }
        };
        static readonly Dictionary<int, string> PlaneOptions = new Dictionary<int, string>
        {
            { 512, "originOnCurve" }, { 1024, "projectToNearest" }, { 2048, "projectAlongSketchNormal" }, { 8192, "referenceFlip" }
        };
        const int PlaneFlip = 256;

        /// <summary>The neutral word for a SolidWorks code, or the code itself (a number) when the table has no word.</summary>
        public static object Word(string table, int code)
        {
            string w;
            return Tables[table].TryGetValue(code, out w) ? (object)w : code;
        }

        /// <summary>The SolidWorks code for a word, or a number written by an older exporter; dflt when absent.</summary>
        public static int Code(string table, object value, int dflt = 0)
        {
            if (value == null) return dflt;
            var s = value as string;
            if (s == null) return Convert.ToInt32(value);
            foreach (var kv in Tables[table])
                if (kv.Value == s) return kv.Key;
            throw new ArgumentException("unknown " + table + " '" + s + "'");
        }

        /// <summary>A reference-plane constraint as {constraint: word, options: [words]} (flip is separate).</summary>
        public static void PlaneConstraint(JObj target, int code)
        {
            var primary = PlanePrimary.Where(kv => (code & kv.Key) != 0).Select(kv => kv.Value).ToList();
            target["constraint"] = primary.Count == 1 ? (object)primary[0] : code;   // several primaries: kept as the code
            var options = PlaneOptions.Where(kv => (code & kv.Key) != 0).Select(kv => (object)kv.Value).ToList();
            if (options.Count > 0) target["options"] = options;
        }

        /// <summary>The SolidWorks code of a plane reference: its words (with flip), or the number older files carry.</summary>
        public static int PlaneConstraintCode(JObj r)
        {
            object c = r["constraint"];
            var s = c as string;
            if (s == null) return c == null ? 0 : Convert.ToInt32(c);
            int code = PlanePrimary.First(kv => kv.Value == s).Key;
            foreach (var o in r.Arr("options"))
                code |= PlaneOptions.First(kv => kv.Value == (string)o).Key;
            if (r.Bool("flip")) code |= PlaneFlip;
            return code;
        }
    }
}
