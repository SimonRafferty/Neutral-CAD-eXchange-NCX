// The Fusion "host" of the NCX reference protocol (FORMAT-SPEC 5) and fingerprint (6): what
// python/ncx/occhost.py does for OpenCASCADE and sw/Ncx.Sw/Host.cs for SolidWorks, done through the Fusion API.
// Everything leaving this module is in mm (radians for a cone's semiAngle, as the core expects).
#pragma once

#include "common.h"

namespace ncxf {

// Per-body fingerprint {volume, area, centroid, moments, bbox, faces, edges} (mm). `xf` (optional) places the
// body: a component body measured in its own frame, moved to the frame the fingerprint is wanted in.
json bodyFingerprint(const Ptr<BRepBody>& b, const Ptr<Matrix3D>& xf = nullptr);
json fingerprintOf(const std::vector<Ptr<BRepBody>>& bodies);
// A body's fingerprint for a feature result: as bodyFingerprint for a solid; a surface body gets volume 0, the
// area-weighted centre of its faces and zero moments (Fusion gives a surface no mass properties).
json resultFingerprint(const Ptr<BRepBody>& b);
// a face is not a plane, cylinder, cone, sphere or torus (FORMAT-SPEC 6, `freeform`)
bool isFreeform(const Ptr<BRepBody>& b);

// Analytic description of a face's surface / an edge's curve: returns the type, fills params (mm).
std::string surfaceInfo(const Ptr<BRepFace>& f, json& params);
std::string curveInfo(const Ptr<BRepEdge>& e, json& params);

double distanceMm(const Ptr<Base>& entity, const V3& pMm);

// Fusion's MeasureManager only measures entities in the root's context: a native face or edge of a component
// under an occurrence measures as nothing. Measurements of such entities go through a proxy in this occurrence,
// and points are moved between the component's frame and the root's by its transform.
void setMeasureContext(const Ptr<Occurrence>& occ);
std::vector<V3> edgePoints(const Ptr<BRepEdge>& e, const std::vector<double>& fractions);
double edgeLengthMm(const Ptr<BRepEdge>& e);
std::array<double, 6> boxMm(const Ptr<Base>& entity);

// A reference descriptor of a face or edge (FORMAT-SPEC 5), as an exporter writes it.
json descriptor(const Ptr<Base>& entity);

// The faces / edges of some bodies, with ids "face:<body>:<i>" for one resolution.
struct Target
{
    std::vector<Ptr<Base>> ents;
    std::vector<std::string> ids;
    std::vector<Ptr<BRepBody>> bodies;
    std::map<std::string, size_t> index;
    Target(const std::vector<Ptr<BRepBody>>& bodies, const std::string& kind);
    std::vector<std::string> neighbours(size_t i) const;
};

// Resolve one reference (face, edge, vertex, body) against bodies. Returns the core's result (or the host's,
// for vertices and bodies); `matched` gets the entities only when the result is "resolved".
json resolveRef(const std::vector<Ptr<BRepBody>>& bodies, const json& ref, std::vector<Ptr<Base>>& matched,
                double tolerance = 0.001);

// Reference faces of a body, described once (used for the face-by-face check after a rebuild).
json describeFaces(const Ptr<BRepBody>& b, const std::string& progressMessage = "");
// Face-by-face check (FORMAT-SPEC 6.3): referenceBodies = [{fingerprint, faces: [descriptor...]}] (from
// describeFaces), against the rebuilt bodies, which must be in the same frame.
json faceCompare(const json& referenceBodies, const std::vector<Ptr<BRepBody>>& rebuilt, double tolerance = 0.001,
                 const std::string& progressMessage = "");

// Row-major 4x4 (mm) <-> Matrix3D (cm)
Ptr<Matrix3D> matrixFromRowMajorMm(const json& m);
json rowMajorMm(const Ptr<Matrix3D>& m);
V3 applyMm(const Ptr<Matrix3D>& m, const V3& pMm);

} // namespace ncxf
