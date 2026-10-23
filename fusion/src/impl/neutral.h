// Enumerated values in feature definitions (FORMAT-SPEC 4, "Enumerated values").
//
// Files from core 0.2.0 on carry neutral words (`"distance"`, `"counterbore"`, `"throughAll"`); files written by
// the SolidWorks add-in 0.4.0 and earlier carry SolidWorks enumeration numbers. The importer works in words and
// turns a number into its word here, with the same tables as sw/Ncx.Sw/Neutral.cs (the only place the numbers
// are written down). A value that is neither a known word nor a known number comes back as "" so the caller
// reports the feature instead of guessing.
#pragma once

#include "common.h"

namespace ncxf {
namespace neutral {

// table: axisType, pointType, endCondition, threadEnd, threadDiameter, hemType, hemPosition, breakType,
// closedCorner, relief, flangePosition, flangeOffset, jogPosition, jogDimension, trimCorner, gussetProfile,
// gussetThickness, gussetLocation, ribExtrusion, bendAllowance, ribType, alongCurve, widthConstraint,
// slotConstraint, holeKind, cornerTreatment, connectedSegments
inline std::string word(const char* table, const json& v)
{
    static const std::map<std::string, std::map<int, std::string>> tables = {
        {"axisType", {{0, "line"}, {1, "twoPlanes"}, {2, "twoPoints"}, {3, "cylindricalFace"}, {4, "pointAndPlane"}}},
        {"pointType", {{2, "alongCurve"}, {3, "arcCenter"}, {4, "faceCenter"}, {5, "projection"}, {6, "intersection"}, {7, "sketchPoint"}}},
        {"endCondition", {{0, "blind"}, {1, "throughAll"}, {2, "throughNext"}, {3, "upToVertex"}, {4, "upToSurface"}, {5, "offsetFromSurface"},
                          {6, "midPlane"}, {7, "upToBody"}, {9, "throughAllBoth"}, {10, "upToSelection"}, {11, "upToNext"}}},
        {"threadEnd", {{0, "blind"}, {1, "upToNext"}, {2, "throughFeature"}}},
        {"threadDiameter", {{1, "conicalOffset"}, {2, "major"}, {3, "minor"}}},
        {"hemType", {{0, "open"}, {1, "closed"}, {2, "teardrop"}, {3, "rolled"}, {4, "double"}}},
        {"hemPosition", {{0, "inside"}, {1, "outside"}}},
        {"breakType", {{0, "fillet"}, {1, "chamfer"}}},
        {"closedCorner", {{1, "butt"}, {2, "overlap"}, {3, "underlap"}}},
        {"relief", {{1, "rectangular"}, {2, "tear"}, {3, "obround"}, {4, "none"}, {5, "tearBend"}}},
        {"flangePosition", {{1, "materialInside"}, {2, "materialOutside"}, {3, "bendOutside"}, {4, "bendCenterline"}, {5, "bendSharp"}, {6, "bendTangent"}}},
        {"flangeOffset", {{1, "blind"}, {2, "upToVertex"}, {3, "upToSurface"}, {4, "offsetFromSurface"}, {5, "midPlane"}, {6, "upToEdgeAndMerge"}}},
        {"jogPosition", {{1, "bendCenterline"}, {2, "materialInside"}, {3, "materialOutside"}, {4, "bendOutside"}}},
        {"jogDimension", {{1, "insideOffset"}, {2, "outsideOffset"}, {3, "overall"}}},
        {"trimCorner", {{0, "none"}, {1, "miter"}, {2, "butt1"}, {3, "butt2"}, {4, "endTrim"}}},
        {"gussetProfile", {{0, "polygon"}, {1, "triangle"}}},
        {"gussetThickness", {{0, "inner"}, {1, "bothSides"}, {2, "outer"}}},
        {"gussetLocation", {{0, "start"}, {1, "center"}, {2, "end"}}},
        {"ribExtrusion", {{0, "parallelToSketch"}, {1, "normalToSketch"}}},
        {"bendAllowance", {{1, "bendTable"}, {2, "kFactor"}, {3, "bendAllowance"}, {4, "bendDeduction"}, {5, "bendCalculationTable"}, {6, "gaugeTable"}}},
        {"ribType", {{0, "linear"}, {1, "natural"}}},
        {"alongCurve", {{0, "distance"}, {1, "percentage"}, {2, "evenlyDistributed"}}},
        {"widthConstraint", {{0, "centered"}, {1, "free"}, {2, "distance"}, {3, "percent"}}},
        {"slotConstraint", {{0, "free"}, {1, "centered"}, {2, "distance"}, {3, "percent"}}},
        {"holeKind", {{0, "counterbore"}, {1, "countersink"}, {2, "simple"}, {3, "pipeTap"}, {4, "tapped"}, {5, "legacy"}, {6, "counterboreSlot"},
                      {7, "countersinkSlot"}, {8, "slot"}}},
        // structuralMember (FORMAT-SPEC 4: older files carry 1 miter, 2 butt1, 3 butt2; 1 simpleCut, 2 copedCut)
        {"cornerTreatment", {{0, "none"}, {1, "miter"}, {2, "butt1"}, {3, "butt2"}}},
        {"connectedSegments", {{1, "simpleCut"}, {2, "copedCut"}}},
    };
    if (v.is_string()) return v.get<std::string>();
    if (!v.is_number()) return "";
    auto t = tables.find(table);
    if (t == tables.end()) return "";
    auto it = t->second.find(v.get<int>());
    return it == t->second.end() ? "" : it->second;
}

inline std::string field(const json& obj, const char* key, const char* table, const std::string& dflt = "")
{
    if (!obj.is_object() || !obj.contains(key)) return dflt;
    return word(table, obj[key]);
}

// A reference plane's constraint: the primary word ("distance"), from {constraint: word, options, flip} or from
// the SolidWorks bit set older files carry (8 = distance, 256 = flip, option bits ignored).
inline std::string planeConstraint(const json& r)
{
    if (!r.contains("constraint")) return "";
    const json& c = r["constraint"];
    if (c.is_string()) return c.get<std::string>();
    if (!c.is_number()) return "";
    static const std::vector<std::pair<int, const char*>> primary = {
        {1, "parallel"}, {2, "perpendicular"}, {4, "coincident"}, {8, "distance"}, {16, "angle"},
        {32, "tangent"}, {64, "project"}, {128, "midPlane"}, {4096, "parallelToScreen"}};
    int code = c.get<int>();
    std::string w;
    for (auto& p : primary)
        if (code & p.first)
        {
            if (!w.empty()) return "";   // several primaries: not one word
            w = p.second;
        }
    return w;
}

// Its flip flag: `flip` in new files, bit 256 of the number in older ones.
inline bool planeFlip(const json& r)
{
    if (r.contains("flip") && r["flip"].is_boolean()) return r["flip"].get<bool>();
    if (r.contains("constraint") && r["constraint"].is_number()) return (r["constraint"].get<int>() & 256) != 0;
    return false;
}

} // namespace neutral
} // namespace ncxf
