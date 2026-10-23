// Shared declarations for the NCX Fusion implementation.
//
// Units: the payload is mm/deg; the Fusion API is cm/rad. Conversion happens only through the helpers here
// (mm2cm, cm2mm, deg2rad, rad2deg) at the boundary between payload values and API calls.
#pragma once

#include <Core/CoreAll.h>
#include <Fusion/FusionAll.h>

#include <windows.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ncx_core.h"

namespace ncxf {

using namespace adsk::core;
using namespace adsk::fusion;
using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kImplVersion = NCX_FUSION_VERSION;   // from CMakeLists.txt (project VERSION)
constexpr double kPi = 3.14159265358979323846;

inline double mm2cm(double v) { return v / 10.0; }
inline double cm2mm(double v) { return v * 10.0; }
inline double deg2rad(double v) { return v * kPi / 180.0; }
inline double rad2deg(double v) { return v * 180.0 / kPi; }

using V3 = std::array<double, 3>;
inline V3 v3(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }
inline V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline V3 sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline V3 mul(V3 a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
inline double dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline V3 cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
inline double norm(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 unit(V3 a)
{
    double n = norm(a);
    return n > 0 ? mul(a, 1.0 / n) : a;
}
inline double dist(V3 a, V3 b) { return norm(sub(a, b)); }

// Fusion point (cm) <-> payload point (mm)
inline Ptr<Point3D> ptCm(V3 mm) { return Point3D::create(mm2cm(mm[0]), mm2cm(mm[1]), mm2cm(mm[2])); }
inline V3 ptMm(const Ptr<Point3D>& p) { return {cm2mm(p->x()), cm2mm(p->y()), cm2mm(p->z())}; }
inline Ptr<Vector3D> vec(V3 v) { return Vector3D::create(v[0], v[1], v[2]); }
inline V3 vecOf(const Ptr<Vector3D>& v) { return {v->x(), v->y(), v->z()}; }

std::string lastError();   // Fusion's last API error message (empty if none)

// ---------------------------------------------------------------------------- core wrappers (ncx_core)
struct CoreResult
{
    int status = 0;
    std::string statusName;
    json report;
    std::string data;
};
CoreResult corePayloadRead(const std::string& stepText);
json coreCompare(const json& expected, const json& actual, const char* profile /* "native" | "exchange" */);
json coreMatch(const json& request);
json coreFaceCompare(const json& request);
std::string statusName(int s);

std::string readFile(const fs::path& p);
void writeFile(const fs::path& p, const std::string& s);
fs::path dataDir();   // %LOCALAPPDATA%\NcxFusion
fs::path tempDir();   // dataDir()/tmp, created on demand
void logLine(const std::string& msg);

// Documents this add-in created (scratch imports, rebuilt designs): the only ones its tooling may close.
void trackDocument(const Ptr<Document>& d);
std::vector<Ptr<Document>> trackedDocuments();

// Progress with a cancel that really stops (ADDING-A-CAD-SYSTEM step 5). progressBegin shows Fusion's progress
// dialog (only for an interactive import); progressStep updates it, pumps events and throws Cancelled when the
// user pressed Cancel. The importer catches it and closes everything it created.
struct Cancelled
{
    std::string where;
};
void progressBegin(const std::string& title, int steps, bool show);
void progressStep(const std::string& message);
void progressEnd();
// a request to cancel from a developer job (as if Cancel were pressed after `afterSteps` steps)
void progressCancelAfter(int afterSteps);

// The notice body (FORMAT-SPEC 2.6): header form of a name, and the matching rule.
std::string headerForm(const std::string& s);
bool isNoticeName(const std::string& bodyName, const std::string& noticeHeaderName);

} // namespace ncxf
