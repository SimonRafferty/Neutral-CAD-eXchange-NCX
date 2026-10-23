// The file's own geometry (its STEP B-rep), imported with Fusion's STEP reader: the reference for the stale
// check, the face-by-face check and the plain-geometry fallback (ADDING-A-CAD-SYSTEM step 2.2).
#pragma once

#include "common.h"

namespace ncxf {

struct RefComponent
{
    std::string name;    // STEP product name = the payload document's name
    json bodies = json::array();   // [{fingerprint (component frame), faces: [descriptor...]}]
    bool described = false;
    std::vector<Ptr<BRepBody>> temp;   // temporary copies of its solid bodies (component frame): the fallback geometry
};

struct Reference
{
    bool ok = false;
    std::string error;
    json fingerprint = json{{"bodies", json::array()}};   // every model body in the file's frame (notice excluded)
    std::string noticeFound;                               // how the notice body was recognised ("" = none)
    bool assembly = false;
    std::map<std::string, RefComponent> components;        // by name; the root component under ""
    std::vector<std::string> warnings;
};

// How the notice body (FORMAT-SPEC 2.6) is recognised: its name in header form (assemblies: a component name),
// and, from core 0.2.0 files, its bounding box in the file's frame (header `noticebox`, mm).
struct NoticeId
{
    std::string name;
    std::vector<double> box;   // xmin,ymin,zmin,xmax,ymax,zmax, or empty
    static NoticeId fromHeader(const json& fields);
};

// Import `stepPath` into a temporary document, measure it, close the document. Face descriptors are
// computed for the components named in `describe` (all when empty and describeAll).
Reference loadReference(const fs::path& stepPath, const json& model, const NoticeId& notice, bool describeAll);

// Import a STEP file into `target` as plain geometry (the fallback), deleting the notice body / component.
// Returns "" or the reason it failed.
std::string importPlain(const fs::path& stepPath, const Ptr<Component>& target, const json& model, const NoticeId& notice);

// The notice body among bodies of one component (FORMAT-SPEC 2.6): Fusion's STEP reader drops solid names
// (every body arrives as "Body<n>"). With the header's `noticebox` it is the solid with that bounding box
// (0.01 mm, `xf` places the component's bodies in the file's frame); in older files, the one body that matches
// no payload body, lies wholly in front of (below the lowest y of) the others and has only planar faces.
int findNoticeBody(const std::vector<Ptr<BRepBody>>& bodies, const json& payloadBodies, const NoticeId& notice,
                   const Ptr<Matrix3D>& xf = nullptr, std::string* how = nullptr);

} // namespace ncxf
