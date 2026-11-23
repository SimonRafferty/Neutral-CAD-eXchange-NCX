// Material definitions (FORMAT-SPEC 4.6, `definition`): a material's density, colour and physical properties in SI
// units, so a system without it can recreate it under its own name.
//
// Fusion keeps each material property as a FloatProperty whose value is in the property's own units() (a Fusion
// unit string), so every value goes through the units manager from or to the SI unit, never by a hand-written
// factor. Properties are found by id (Fusion's ids as seen in material property lists) or else by words of their
// name. A unit Fusion cannot convert leaves that property out, with a note; nothing here fails an import.
//
// NOT TESTED in Fusion (2026-11-23: the Fusion trial expired). Written against the SDK headers (it compiles), not
// run: the property ids, the unit strings Fusion accepts and addByCopy on the design's materials are the parts to
// check first when Fusion is available again.
#include "common.h"
#include "materialdef.h"

#include <algorithm>
#include <cctype>

namespace ncxf {

namespace {

struct PropertyMap
{
    const char* neutral;                 // FORMAT-SPEC name ("density" is the definition's top-level field)
    std::vector<std::string> ids;        // Fusion property ids
    std::vector<std::string> words;      // else: a property whose name holds this text (lower case)
    std::vector<std::string> si;         // the SI unit, as Fusion unit strings to try in turn; empty: unitless
};

const std::vector<PropertyMap>& Map()
{
    static const std::vector<PropertyMap> m = {
        {"density", {"structural_Density"}, {"density"}, {"kg / m^3", "kg/m^3"}},
        {"elasticModulus", {"structural_Young_modulus"}, {"young", "elastic modulus"}, {"Pa", "N / m^2", "kg / (m * s^2)"}},
        {"poissonRatio", {"structural_Poisson_ratio"}, {"poisson"}, {}},
        {"shearModulus", {"structural_Shear_modulus"}, {"shear modulus"}, {"Pa", "N / m^2", "kg / (m * s^2)"}},
        {"thermalExpansion", {"structural_Thermal_expansion_coefficient"}, {"thermal expansion"}, {"1 / K", "1/K", "1 / C"}},
        {"thermalConductivity", {"structural_Thermal_conductivity", "thermal_Thermal_conductivity"}, {"thermal conductivity"},
         {"W / (m * K)", "W / (m * C)"}},
        {"specificHeat", {"structural_Specific_heat", "thermal_Specific_heat"}, {"specific heat"}, {"J / (kg * K)", "J / (kg * C)"}},
        {"yieldStrength", {"structural_Minimum_yield_stress"}, {"yield"}, {"Pa", "N / m^2", "kg / (m * s^2)"}},
        {"tensileStrength", {"structural_Minimum_tensile_strength"}, {"tensile"}, {"Pa", "N / m^2", "kg / (m * s^2)"}},
        {"compressiveStrength", {}, {"compressive"}, {"Pa", "N / m^2", "kg / (m * s^2)"}},
    };
    return m;
}

std::string lowerText(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Ptr<FloatProperty> findProperty(const Ptr<Properties>& props, const PropertyMap& pm)
{
    if (!props) return nullptr;
    for (auto& id : pm.ids)
        if (Ptr<FloatProperty> p = props->itemById(id)) return p;
    for (size_t i = 0; i < props->count(); i++)
    {
        Ptr<Property> p = props->item(static_cast<int>(i));
        if (!p) continue;
        std::string n = lowerText(p->name());
        for (auto& w : pm.words)
            if (n.find(w) != std::string::npos)
                if (Ptr<FloatProperty> f = p) return f;
    }
    return nullptr;
}

// value in `from` units -> `to` units; false if Fusion cannot convert between them
bool convertUnits(const Ptr<UnitsManager>& um, double v, const std::string& from, const std::string& to, double& out)
{
    if (from == to) { out = v; return true; }
    if (!um) return false;
    double r = um->convert(v, from, to);
    if (r == -1 && !lastError().empty()) return false;
    out = r;
    return true;
}

} // namespace

json materialDefinition(const Ptr<Material>& m, const Ptr<UnitsManager>& um)
{
    json def = json::object();
    if (!m) return def;
    def["complete"] = true;   // Fusion holds the whole material
    Ptr<Properties> props = m->materialProperties();
    json phys = json::object();
    for (auto& pm : Map())
    {
        Ptr<FloatProperty> p = findProperty(props, pm);
        if (!p) continue;
        double v = p->value(), si = v;
        bool ok = pm.si.empty() || p->units().empty();
        for (size_t i = 0; !ok && i < pm.si.size(); i++) ok = convertUnits(um, v, p->units(), pm.si[i], si);
        if (!ok) continue;
        if (std::string(pm.neutral) == "density") def["density"] = si;
        else phys[pm.neutral] = si;
    }
    if (!phys.empty()) def["properties"] = phys;
    if (Ptr<Appearance> a = m->appearance())
        if (Ptr<Color> c = a->color())
            def["color"] = json::array({c->red() / 255.0, c->green() / 255.0, c->blue() / 255.0});
    std::string desc = m->description();
    if (!desc.empty()) def["description"] = desc;
    return def;
}

bool applyMaterialDefinition(const Ptr<Material>& m, const json& def, const Ptr<UnitsManager>& um, std::string& notes)
{
    if (!m || !def.is_object()) return false;
    Ptr<Properties> props = m->materialProperties();
    const json phys = def.value("properties", json::object());
    std::vector<std::string> missed;
    for (auto& pm : Map())
    {
        std::string key = pm.neutral;
        bool top = key == "density";
        if (top ? !def.contains("density") : !phys.contains(key)) continue;
        double si = top ? def["density"].get<double>() : phys[key].get<double>();
        Ptr<FloatProperty> p = findProperty(props, pm);
        if (!p || p->isReadOnly()) { missed.push_back(key); continue; }
        double v = si;
        bool ok = pm.si.empty() || p->units().empty();
        for (size_t i = 0; !ok && i < pm.si.size(); i++) ok = convertUnits(um, si, pm.si[i], p->units(), v);
        if (!ok || !p->value(v)) missed.push_back(key);
    }
    if (def.contains("description")) m->description(def["description"].get<std::string>());
    if (!missed.empty())
    {
        notes = "not set in Fusion: ";
        for (size_t i = 0; i < missed.size(); i++) notes += (i ? ", " : "") + missed[i];
    }
    return true;
}

} // namespace ncxf
