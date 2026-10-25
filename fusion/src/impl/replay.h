// Replay: the neutral model -> a Fusion design, feature by feature.
//
// One NCX file becomes one Fusion design. A part payload is rebuilt in the design's root component. An
// assembly payload becomes components: every part document is built once, in its own component (with an
// identity occurrence while it is built, so component and model space coincide), then placed.
#pragma once

#include "common.h"
#include "reference.h"

namespace ncxf {

struct Report
{
    json entries = json::array();   // {document, item, result, reason}
    json pendingTrace;              // attached to the next entry (developer trace)
    void add(const std::string& doc, const std::string& item, const std::string& result, const std::string& reason = "")
    {
        entries.push_back(json{{"document", doc}, {"item", item}, {"result", result}, {"reason", reason}});
        if (pendingTrace.is_array()) entries.back()["bodies"] = pendingTrace;
        pendingTrace = nullptr;
        logLine(doc + " | " + item + " | " + result + (reason.empty() ? "" : " | " + reason));
    }
};

// A frame: origin (mm) and orthonormal axes, as a sketch's `frame` gives it.
struct Frame
{
    V3 o{0, 0, 0}, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    V3 toModel(double u, double v) const { return add(o, add(mul(x, u), mul(y, v))); }
    V3 dirToModel(double u, double v) const { return add(mul(x, u), mul(y, v)); }
};

struct SketchMap
{
    Ptr<Sketch> sketch;
    std::map<std::string, Ptr<SketchEntity>> ents;   // payload entity id -> curve or point
    std::map<std::string, V3> modelPos;              // payload point id -> model position (mm)
    Frame src;                                       // the payload's frame
    bool mirrored = false;                           // target normal opposite the source's
    Ptr<SketchPoint> origin;                         // the component origin in this sketch
    std::string space;                               // "3d" for 3D sketches
    std::string id;                                  // the sketch's payload feature id
    Ptr<Point3D> toSketch(const V3& modelMm) const;  // model mm -> sketch space (cm)
    V3 sketchToModel(const Ptr<Point3D>& p) const;   // sketch space (cm) -> model mm
};

class Replayer
{
public:
    Replayer(Report& report, const json& model, const fs::path& file);

    // Rebuild one part document into `comp` (empty). Returns "" or why the part could not be rebuilt at all.
    std::string replayPart(const json& doc, const Ptr<Component>& comp);

    // Bodies of the component being rebuilt (solid and surface).
    std::vector<Ptr<BRepBody>> bodies(bool solidOnly = false) const;

    bool resultsApplied = false;
    // why this rebuild cannot be expected to match the source to the native (same-kernel) tolerance although
    // nothing was carried: a section drawn from a size text instead of the source's library profile ("" = none)
    std::string looseReason;
    bool trace = false;            // developer: record the bodies after every feature in the report   // a carried result was used (verify at exchange tolerance)
    std::map<std::string, Ptr<Base>> features;   // payload feature id -> sketch / feature / construction geometry
    std::set<std::string> markers;                // feature ids reported but deliberately not built
    const json* refBodies = nullptr;              // the file's own B-rep of this part: [{fingerprint, faces}]
    Ptr<Occurrence> occurrence;                   // the occurrence the component is built under (null: root)
    // assembly-level features: the assembly's occurrences, by "<assembly id>|<component id>"
    std::map<std::string, Ptr<Occurrence>>* asmOccs = nullptr;

private:
    Report& report_;
    const json& model_;
    fs::path file_;
    json doc_;
    std::string docName_;
    Ptr<Component> comp_;
    Ptr<Design> design_;
    std::map<std::string, SketchMap> sketches_;
    const Frame* currentFrame_ = nullptr;   // the frame of the sketch being replayed (for frame-only planes)

    std::string replayFeature(json& nf);

    // params.cpp
    std::map<std::string, std::string> paramByNative_;   // native name (W, D1@SK_Base, D1@Boss-Base) -> Fusion parameter name
    std::string translateExpression(const std::string& e, std::string& why);
    void replayParameters(const json& doc);
    void replayEquations(const json& doc);
    void replayProperties(const json& doc);   // props.cpp

public:
    Ptr<Parameter> parameterFor(const std::string& native);
    std::map<std::string, Replayer*>* registry = nullptr;   // document id -> its replayer (insertPart sources)
    const json* sketchDef(const std::string& id) const
    {
        auto it = sketchDefs_.find(id);
        return it == sketchDefs_.end() ? nullptr : &it->second;
    }
    std::string replayInsertPart(json& nf, const json& def);
    void registerParameter(const std::string& native, const Ptr<Parameter>& p);
    std::string documentName() const { return docName_; }
    Ptr<Component> component() const { return comp_; }

private:
    std::string applyResult(const json& nf);
    // Does the part now hold what the feature's carried result says it should (the bodies it made, plus the
    // bodies from before it did not replace)? Native tolerance. False when the feature carries no result.
    // Used to pick the variant of a native rebuild whose orientation or datum the payload does not pin down.
    bool matchesCarriedResult(const json& nf, const std::vector<json>& before, std::string* detail = nullptr, const char* profile = "native");
    std::vector<json> solidFingerprints() const;

    // sketches (sketch.cpp)
    std::string replaySketch(json& nf, const json& def, const Ptr<Base>& planeOverride = nullptr);
    std::map<std::string, json> deferred_;
    std::map<std::string, json> sketchDefs_;   // sketch id -> its payload definition   // sketches on planes a feature generates: feature name -> sketch feature
    Ptr<Base> planeFor(const json& planeRef, std::string& why);
    Ptr<Base> sketchEntityFor(SketchMap& m, const json& e, std::string& why);
    std::string addConstraint(SketchMap& m, const json& c);
    std::string addDimension(SketchMap& m, const json& d);

    // features (features.cpp)
    std::string replayExtrude(json& nf, const json& def, bool cut);
    Ptr<BRepFace> sheetOppositeFace(const Ptr<ObjectCollection>& prof, double depthMm);
    void sheetCutOptions(json& nf, const json& def, const Ptr<ExtrudeFeature>& f, const Ptr<ObjectCollection>& prof, bool cut);
    std::string replayRevolve(json& nf, const json& def, bool cut);
    std::string replayFillet(json& nf, const json& def);
    std::string replayChamfer(json& nf, const json& def);
    std::string replayPlane(json& nf, const json& def);
    std::string replayAxis(json& nf, const json& def);
    std::string replayPoint(json& nf, const json& def);
    Ptr<ObjectCollection> profilesFor(const json& def, std::string& why);
    int regionRetry_ = 0;   // profilesFor's retries: 1 projected curves made construction, 2 model edges in the plane projected

    // features2.cpp
    std::string seedsFor(const json& def, Ptr<ObjectCollection>& out, bool& bodiesMode);
    std::string replayLinearPattern(json& nf, const json& def);
    std::string replayCircularPattern(json& nf, const json& def);
    std::string replayMirror(json& nf, const json& def, bool bodies);
    std::string replayDeleteBody(json& nf, const json& def);
    std::string replayCombine(json& nf, const json& def);
    std::string replayMoveBody(json& nf, const json& def);
    std::string replayShell(json& nf, const json& def);
    std::string replayDraft(json& nf, const json& def);
    Ptr<Path> pathFor(const json& ref, std::string& why);
    std::string replaySweep(json& nf, const json& def, bool cut);
    std::string replayLoft(json& nf, const json& def, bool cut);
    std::string replaySurfaceExtrude(json& nf, const json& def);
    std::string replaySurfaceRevolve(json& nf, const json& def);
    std::string replayPlanarSurface(json& nf, const json& def);
    std::string replayKnit(json& nf, const json& def);
    std::string replaySplit(json& nf, const json& def);
    std::string replayMarker(json& nf, const std::string& why);
    std::string replayFaceFillet(json& nf, const json& def);
    std::string replayVertexChamfer(json& nf, const json& def);
    std::string replayVariableFillet(json& nf, const json& def);
    std::string replayRib(json& nf, const json& def);
    std::string replayHole(json& nf, const json& def);
    std::string replayCoordinateSystem(json& nf, const json& def);
    std::string replayFacePoint(json& nf, const json& def, const Ptr<BRepFace>& face);
    std::string replayThinRevolve(json& nf, const json& def);
    std::string replaySheetMetal(json& nf, const json& def);   // sheetmetal.cpp
    std::string replayConvertToSheetMetal(json& nf, const json& def);
    std::string replayBaseFlange(json& nf, const json& def);
    std::string replayEdgeFlange(json& nf, const json& def);
    std::string replayHem(json& nf, const json& def);
    std::string replaySketchedBend(json& nf, const json& def);
    std::string replayBreakCorner(json& nf, const json& def);
    std::string replayLoftedBend(json& nf, const json& def);
    std::string replayClosedCorner(json& nf, const json& def);
    std::string replayMiterFlange(json& nf, const json& def);
    std::string replayJog(json& nf, const json& def);
    std::string miterSquareEnds(json& nf, const json& def, double h, double t);
    std::string replayUnfold(json& nf, const json& def, bool fold);
    std::map<std::string, Ptr<UnfoldFeature>> unfolds_;   // unfold feature by the bends' key, for the fold
    std::string replayDeferredSketchByFrame(const std::string& byFeature);
    void linkFlangeHeight(const Ptr<FlangeFeature>& f, const std::vector<std::string>& profileSketchIds, double h, double built);
    std::string replayStructuralMember(json& nf, const json& def);   // weldment.cpp
    std::string replaySketch3D(json& nf, const json& def);           // sketch.cpp
    std::string resolveEntities(const json& refs, std::vector<Ptr<Base>>& out);
    Ptr<Base> refObject(const json& ref, std::string& why);   // plane/axis/point/face/edge/vertex/feature
    void name(const Ptr<Base>& obj, const std::string& n);
    // +1 when Fusion's axis runs the way the payload's axis reference does, -1 when opposite, 0 unknown
    int axisSense(const json& axisRef, const Ptr<Base>& fusionAxis);

    friend struct ReplayAccess;
};

std::string featureName(const Ptr<Base>& obj);

// importer.cpp helpers shared with the assembly import
const json* findDoc(const json& model, const std::string& id);
json& jointFrames();   // joints.cpp: the frames of the joints made by this import (developer check)
void holdOccurrence(const Ptr<Occurrence>& o, bool fixed, bool topLevel);   // assembly.cpp
json expectedFingerprint(const json& doc);
Ptr<Design> newDesign(Ptr<Document>& doc);
std::vector<Ptr<BRepBody>> componentBodies(const Ptr<Component>& c, bool solidOnly);
void nameBodies(const Ptr<Component>& c, const json& doc);
std::string differencesText(const json& cmp);
json flexCheck(const json& model, std::map<std::string, Replayer*>& rebuilt, const std::set<std::string>& skipDocs);

// joints.cpp
void buildJoints(const json& model, const json& asmDoc, const Ptr<Component>& asmComp, std::map<std::string, Ptr<Occurrence>>& occOf,
                 Report& report);

// assembly.cpp
json importAssembly(const json& model, Reference& ref, const Ptr<Design>& des, Report& report, const json& opts,
                    const fs::path& file, json& verification, std::string& status);

} // namespace ncxf
