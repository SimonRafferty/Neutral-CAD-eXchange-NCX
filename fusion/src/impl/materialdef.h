// Material definitions (FORMAT-SPEC 4.6): Fusion material <-> neutral definition (SI units). See materialdef.cpp.
#pragma once

#include "common.h"

namespace ncxf {

Ptr<Material> anyMaterial();   // host.cpp: Fusion's steel (or any library material with a density)

// a Fusion material as a neutral definition: density, physical properties, colour, description (complete: true)
json materialDefinition(const Ptr<Material>& m, const Ptr<UnitsManager>& um);

// set a (copied, design) material's properties from a definition; `notes` lists what Fusion would not take
bool applyMaterialDefinition(const Ptr<Material>& m, const json& def, const Ptr<UnitsManager>& um, std::string& notes);

} // namespace ncxf
