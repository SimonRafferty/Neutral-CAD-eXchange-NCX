// Mates -> Fusion joints (FORMAT-SPEC 5, "Mates"; ADDING-A-CAD-SYSTEM 3, "Mates in a system that positions
// components with joints").
//
// Every component already sits where the file places it, so the joints are *as-built* joints: they keep the
// placement and only define what may still move. For each moving component, the mates it shares with one partner
// (another component, or the assembly itself) are read as the degrees of freedom they leave:
//   hinge, or concentric + a coincident plane across the axis  -> revolute about the axis
//   concentric alone                                            -> cylindrical
//   plane mates (coincident / distance) in 3 directions or lock -> rigid (to the assembly: grounded)
//   plane mates in 2 directions                                 -> slider along the line both planes keep
//   plane mates in 1 direction                                  -> planar
//   anything else                                               -> rigid at the file's placement (approximate)
// Gear, rack-and-pinion and screw mates become motion links between those joints. Mates with no counterpart
// (width, symmetric, slot, cam, ...) leave the components where the file puts them, reported.
#include "host.h"
#include "replay.h"

namespace ncxf {

namespace {

struct MateSide
{
    std::string unit;        // component id at this assembly level ("" = the assembly itself)
    Ptr<Occurrence> leaf;    // the occurrence holding the referenced entity (proxy path), null for the assembly
    std::string leafDoc;     // its document id
    json ref;
    std::string role;
};

V3 rotate(const Ptr<Matrix3D>& m, const V3& v)
{
    std::vector<double> a = m->asArray();
    return {a[0] * v[0] + a[1] * v[1] + a[2] * v[2], a[4] * v[0] + a[5] * v[1] + a[6] * v[2], a[8] * v[0] + a[9] * v[1] + a[10] * v[2]};
}

V3 originPlaneNormal(const std::string& p)
{
    if (p == "front") return {0, 0, 1};
    if (p == "top") return {0, 1, 0};
    return {1, 0, 0};
}

} // namespace

struct JointBuilder
{
    const json& model;
    const json& asmDoc;
    Ptr<Component> asmComp;
    std::map<std::string, Ptr<Occurrence>>& occOf;   // "<assembly id>|<component id>"
    Report& report;
    std::string asmId, asmName;
    std::map<std::string, std::string> docOfComp;     // "<assembly id>|<component id>" -> document id

    JointBuilder(const json& m, const json& d, const Ptr<Component>& c, std::map<std::string, Ptr<Occurrence>>& o, Report& r)
        : model(m), asmDoc(d), asmComp(c), occOf(o), report(r)
    {
        asmId = d.value("id", "");
        asmName = d.value("name", "");
        for (auto& doc : model["documents"])
            for (auto& comp : doc.value("components", json::array()))
                docOfComp[doc.value("id", "") + "|" + comp.value("id", "")] = comp.value("document", "");
    }

    MateSide side(const json& r)
    {
        MateSide s;
        s.ref = r["ref"];
        s.role = r.value("role", "");
        if (!r.contains("component")) return s;
        s.unit = r["component"].get<std::string>();
        std::string key = asmId + "|" + s.unit;
        auto it = occOf.find(key);
        if (it == occOf.end()) return s;
        Ptr<Occurrence> o = it->second;
        std::string doc = docOfComp[key];
        for (auto& p : r.value("path", json::array()))
        {
            std::string k2 = doc + "|" + p.get<std::string>();
            auto ci = occOf.find(k2);
            if (ci == occOf.end()) return s;
            o = ci->second->createForAssemblyContext(o);
            doc = docOfComp[k2];
        }
        s.leaf = o;
        s.leafDoc = doc;
        return s;
    }

    // the referenced entity, in the assembly's context (a proxy), or null
    Ptr<Base> entity(const MateSide& s, std::string& why)
    {
        std::string kind = s.ref.value("kind", "");
        Ptr<Component> c = s.leaf ? s.leaf->component() : asmComp;
        Ptr<Base> e;
        if (kind == "originPlane")
        {
            std::string p = s.ref.value("plane", "");
            Ptr<ConstructionPlane> pl = p == "front" ? c->xYConstructionPlane() : p == "top" ? c->xZConstructionPlane() : c->yZConstructionPlane();
            return s.leaf ? Ptr<Base>(pl->createForAssemblyContext(s.leaf)) : Ptr<Base>(pl);
        }
        if (kind == "origin")
        {
            Ptr<ConstructionPoint> o = c->originConstructionPoint();
            return s.leaf ? Ptr<Base>(o->createForAssemblyContext(s.leaf)) : Ptr<Base>(o);
        }
        if (kind == "face" || kind == "edge" || kind == "vertex")
        {
            if (!s.leaf)
            {
                why = "a B-rep reference on the assembly itself";
                return nullptr;
            }
            std::vector<Ptr<BRepBody>> bodies;
            for (size_t i = 0; i < c->bRepBodies()->count(); i++) bodies.push_back(c->bRepBodies()->item(i));
            setMeasureContext(s.leaf->nativeObject() ? s.leaf->nativeObject() : s.leaf);
            std::vector<Ptr<Base>> hit;
            json res = resolveRef(bodies, s.ref, hit);
            setMeasureContext(nullptr);
            if (res.value("result", "") != "resolved" || hit.size() != 1)
            {
                why = kind + " reference " + res.value("result", "?");
                return nullptr;
            }
            if (Ptr<BRepFace> f = hit[0]) return f->createForAssemblyContext(s.leaf);
            if (Ptr<BRepEdge> ed = hit[0]) return ed->createForAssemblyContext(s.leaf);
            if (Ptr<BRepVertex> v = hit[0]) return v->createForAssemblyContext(s.leaf);
        }
        why = "reference kind " + kind + " not used for joints";
        return e;
    }

    // a plane normal of a mate side in the assembly frame, if it is planar
    bool planeNormal(const MateSide& s, V3& n)
    {
        std::string kind = s.ref.value("kind", "");
        Ptr<Matrix3D> xf = s.leaf ? s.leaf->transform2() : Matrix3D::create();
        if (kind == "originPlane")
        {
            n = unit(rotate(xf, originPlaneNormal(s.ref.value("plane", ""))));
            return true;
        }
        if (kind == "face" && s.ref.contains("geom") && s.ref["geom"].value("type", "") == "plane")
        {
            n = unit(rotate(xf, v3(s.ref["geom"]["params"]["normal"])));
            return true;
        }
        if (kind == "feature")
        {
            // an assembly or component reference plane: its normal from the payload
            std::string doc = s.leaf ? s.leafDoc : asmId;
            std::string fid = s.ref.value("id", "");
            for (auto& d : model["documents"])
                if (d.value("id", "") == doc)
                    for (auto& f : d.value("features", json::array()))
                        if (f.value("id", "") == fid && f.contains("def") && f["def"].contains("normal"))
                        {
                            n = unit(rotate(xf, v3(f["def"]["normal"])));
                            return true;
                        }
        }
        return false;
    }

    // the axis a mate side gives: a cylindrical face, or a reference axis feature (its recorded line)
    bool cylinderAxis(const MateSide& s, V3& a)
    {
        Ptr<Matrix3D> xf = s.leaf ? s.leaf->transform2() : Matrix3D::create();
        if (s.ref.value("kind", "") == "face" && s.ref.contains("geom") && s.ref["geom"].value("type", "") == "cylinder")
        {
            a = unit(rotate(xf, v3(s.ref["geom"]["params"]["axis"])));
            return true;
        }
        if (s.ref.value("kind", "") == "feature")
        {
            std::string doc = s.leaf ? s.leafDoc : asmId;
            for (auto& d : model["documents"])
                if (d.value("id", "") == doc)
                    for (auto& f : d.value("features", json::array()))
                        if (f.value("id", "") == s.ref.value("id", "") && f.value("type", "") == "axis" && f.contains("def") &&
                            f["def"].contains("line"))
                        {
                            a = unit(rotate(xf, sub(v3(f["def"]["line"]["end"]), v3(f["def"]["line"]["start"]))));
                            return true;
                        }
        }
        return false;
    }

    // Suppressed in the assembly's active configuration. Assemblies list no configurations in schema 0.1, and the
    // SolidWorks exporter keys an assembly mate's state "cfg:active" (features use the configuration's id): the
    // active configuration's id, "cfg:active", or the only key there is, decide.
    bool mateSuppressed(const json& m) const
    {
        if (!m.contains("suppressed") || !m["suppressed"].is_object() || m["suppressed"].empty()) return false;
        const json& s = m["suppressed"];
        std::string active = asmDoc.value("activeConfiguration", "");
        if (!active.empty() && s.contains(active)) return s[active].get<bool>();
        if (s.contains("cfg:active")) return s["cfg:active"].get<bool>();
        if (s.size() == 1) return s.begin()->get<bool>();
        return false;
    }

    void run(std::map<std::string, Ptr<Base>>& jointOf)
    {
        const json mates = asmDoc.value("mates", json::array());
        std::set<std::string> fixed;
        for (auto& c : asmDoc.value("components", json::array()))
            if (c.value("fixed", false)) fixed.insert(c.value("id", ""));
        // mates by the pair of units they join
        std::map<std::pair<std::string, std::string>, std::vector<size_t>> byPair;
        std::vector<std::vector<MateSide>> sides(mates.size());
        static const std::set<std::string> mechanical{"gear", "screw", "rackPinion", "cam"};
        // suppressed mates (FORMAT-SPEC 5, Mates: `suppressed` {config: bool}) hold nothing in the source and may not
        // hold in the placement (L11's Distance-Lift): left out of the joints, and said so
        std::set<size_t> suppressed;
        for (size_t i = 0; i < mates.size(); i++)
            if (mateSuppressed(mates[i])) suppressed.insert(i);
        for (size_t i = 0; i < mates.size(); i++)
        {
            for (auto& r : mates[i].value("refs", json::array())) sides[i].push_back(side(r));
            if (suppressed.count(i) || mechanical.count(mates[i].value("type", ""))) continue;
            // the assembly and its fixed components do not move: to a moving component they are one partner
            std::set<std::string> units;
            for (auto& s : sides[i]) units.insert(fixed.count(s.unit) ? std::string() : s.unit);
            if (units.size() != 2) continue;
            auto a = *units.begin(), b = *units.rbegin();
            byPair[{a, b}].push_back(i);
        }
        std::map<size_t, std::string> covered;   // mate index -> how it was carried
        std::set<std::string> done;
        // every moving unit: one joint, to the partner it shares most mates with (the assembly or a fixed one first)
        for (auto& c : asmDoc.value("components", json::array()))
        {
            std::string x = c.value("id", "");
            if (fixed.count(x)) continue;
            std::pair<std::string, std::string> best;
            size_t bestN = 0;
            int bestPrio = -1;
            for (auto& kv : byPair)
            {
                if (kv.first.first != x && kv.first.second != x) continue;
                std::string other = kv.first.first == x ? kv.first.second : kv.first.first;
                int prio = other.empty() ? 2 : 0;
                if (prio > bestPrio || (prio == bestPrio && kv.second.size() > bestN))
                {
                    best = kv.first;
                    bestN = kv.second.size();
                    bestPrio = prio;
                }
            }
            if (bestN == 0) continue;
            std::string partner = best.first == x ? best.second : best.first;
            std::string jname;
            std::string kind = jointFor(x, partner, byPair[best], mates, sides, jname, jointOf);
            for (size_t i : byPair[best]) covered[i] = kind;
            done.insert(x);
        }
        // mechanical mates: motion links between the joints made above
        for (size_t i = 0; i < mates.size(); i++)
        {
            std::string t = mates[i].value("type", "");
            if (!mechanical.count(t) || suppressed.count(i)) continue;
            covered[i] = motionLink(mates[i], sides[i], jointOf);
        }
        for (size_t i = 0; i < mates.size(); i++)
        {
            std::string name = mates[i].value("name", mates[i].value("id", ""));
            auto it = covered.find(i);
            if (suppressed.count(i))
                report.add(asmName, name, "note", "the " + mates[i].value("type", std::string("")) +
                                                      " mate is suppressed in the source (it holds nothing there): not made as a joint (Fusion joins a component "
                                                      "pair with one joint, which the active mates define); the components stay where the file places them");
            else if (it == covered.end())
                report.add(asmName, name, "approximate", "components placed where the file puts them; the " + mates[i].value("type", "") +
                                                             " mate is covered by the joints of the components it joins");
            else if (it->second.rfind("~", 0) == 0)
                report.add(asmName, name, "approximate", it->second.substr(1));
            else
                report.add(asmName, name, "rebuilt", it->second);
        }
    }

    std::string jointFor(const std::string& x, const std::string& partner, const std::vector<size_t>& idx, const json& mates,
                         std::vector<std::vector<MateSide>>& sides, std::string& jname, std::map<std::string, Ptr<Base>>& jointOf)
    {
        Ptr<Occurrence> ox = occOf[asmId + "|" + x];
        Ptr<Occurrence> op = partner.empty() ? nullptr : occOf[asmId + "|" + partner];
        std::set<std::string> types;
        for (size_t i : idx) types.insert(mates[i].value("type", ""));
        jname = mates[idx[0]].value("name", "");
        // the cylinder (axis) and plane normals the mates use, on x's side
        const MateSide* cyl = nullptr;
        std::vector<V3> normals;
        bool other = false;
        for (size_t i : idx)
        {
            std::string t = mates[i].value("type", "");
            // a coincident mate between two axes is a concentric one
            bool axes = t == "coincident" && sides[i].size() == 2;
            for (auto& s : sides[i])
            {
                V3 a;
                if (!cylinderAxis(s, a)) axes = false;
            }
            for (auto& s : sides[i])
            {
                V3 a;
                if ((t == "concentric" || axes || (t == "hinge" && s.role == "concentric")) && cylinderAxis(s, a) && s.unit == x) cyl = &s;
            }
            if (axes) continue;
            if (mates[i].contains("params") && (mates[i]["params"].contains("min") || mates[i]["params"].contains("max")))
                other = true;   // a limit mate: Fusion joint limits are not set
            if (t == "coincident" || t == "distance")
            {
                V3 n;
                bool got = false;
                for (auto& s : sides[i])
                    if (s.unit == x && planeNormal(s, n)) got = true;
                if (!got)
                    for (auto& s : sides[i])
                        if (planeNormal(s, n)) got = true;
                if (got) normals.push_back(n);
                else other = true;
            }
            else if (t != "concentric" && t != "hinge" && t != "parallel" && t != "lock")
                other = true;
        }
        if (types.count("hinge"))
            for (size_t i : idx)
                for (auto& s : sides[i])
                    if (s.role == "coincident" && s.unit == x)
                    {
                        V3 n;
                        if (planeNormal(s, n)) normals.push_back(n);
                    }
        auto distinct = [](const std::vector<V3>& ns) {
            std::vector<V3> d;
            for (auto& n : ns)
            {
                bool same = false;
                for (auto& m : d)
                    if (std::fabs(std::fabs(dot(n, m)) - 1) < 1e-6) same = true;
                if (!same) d.push_back(n);
            }
            return d;
        };
        std::vector<V3> dn = distinct(normals);
        std::string motion;
        V3 axis{0, 0, 0};
        if (types.count("lock") || (dn.size() >= 3 && !cyl)) motion = "rigid";
        else if (cyl)
        {
            cylinderAxis(*cyl, axis);
            bool across = false;
            for (auto& n : dn)
                if (std::fabs(std::fabs(dot(n, axis)) - 1) < 1e-6) across = true;
            motion = (types.count("hinge") || across) ? "revolute" : "cylindrical";
        }
        else if (dn.size() == 2)
        {
            motion = "slider";
            axis = unit(cross(dn[0], dn[1]));
        }
        else if (dn.size() == 1)
        {
            motion = "planar";
            axis = dn[0];
        }
        else
            motion = "rigid";
        std::string approxWhy = other ? " (point, limit and other mates are kept as this placement)" : "";
        // the assembly itself as the partner: a component the mates fix is grounded; one that may move needs a
        // grounded occurrence to joint to
        if (partner.empty() && motion == "rigid")
        {
            ox->isGrounded(true);
            jointOf[x] = ox;
            return (other ? "~" : "") + std::string("fixed to the assembly: the component is grounded") + approxWhy;
        }
        if (!op)
        {
            // the moving part's partner is the assembly: joint it to the assembly's grounded component, if any
            for (auto& c : asmDoc.value("components", json::array()))
                if (c.value("fixed", false)) op = occOf[asmId + "|" + c.value("id", "")];
            if (!op) return "~no grounded component to make a " + motion + " joint to: the component stays where the file places it";
        }
        // joint geometry: the cylinder for an axis motion, a mated plane face for a planar one, else the
        // component's origin
        Ptr<JointGeometry> g;
        std::string why;
        if (cyl)
        {
            Ptr<Base> f = entity(*cyl, why);
            if (Ptr<BRepFace> ff = f) g = JointGeometry::createByNonPlanarFace(ff, MiddleKeyPoint);
        }
        bool planarByFace = false;
        if (!g && motion == "planar")
            for (size_t i : idx)
                for (auto& s : sides[i])
                    if (!g && s.unit == x && s.ref.value("kind", "") == "face")
                    {
                        Ptr<Base> f = entity(s, why);
                        if (Ptr<BRepFace> ff = f)
                            if (Ptr<Plane>(ff->geometry()))
                            {
                                g = JointGeometry::createByPlanarFace(ff, nullptr, CenterKeyPoint);
                                planarByFace = (bool)g;
                            }
                    }
        if (!g)
        {
            Ptr<ConstructionPoint> o = ox->component()->originConstructionPoint()->createForAssemblyContext(ox);
            g = JointGeometry::createByPoint(o);
        }
        if (!g) return "~joint geometry not available (" + why + "): the component stays where the file places it";
        Ptr<AsBuiltJoints> abj = asmComp->asBuiltJoints();
        Ptr<AsBuiltJointInput> in = abj->createInput(ox, op, g);
        if (!in) return "~as-built joint input refused (" + lastError() + "): the component stays where the file places it";
        bool ok;
        Ptr<Base> dirEntity;
        if (motion == "revolute") ok = in->setAsRevoluteJointMotion(ZAxisJointDirection);
        else if (motion == "cylindrical") ok = in->setAsCylindricalJointMotion(ZAxisJointDirection);
        else if (motion == "planar" && planarByFace)
            ok = in->setAsPlanarJointMotion(ZAxisJointDirection);
        else if (motion == "slider" || motion == "planar")
        {
            // a straight edge of the component along the direction
            Ptr<Component> c = ox->component();
            for (size_t b = 0; !dirEntity && b < c->bRepBodies()->count(); b++)
            {
                Ptr<BRepEdges> es = c->bRepBodies()->item(b)->edges();
                for (size_t i = 0; i < es->count() && !dirEntity; i++)
                {
                    Ptr<Line3D> l = es->item(i)->geometry();
                    if (!l) continue;
                    V3 d = unit(rotate(ox->transform2(), unit(sub(ptMm(l->endPoint()), ptMm(l->startPoint())))));
                    if (std::fabs(std::fabs(dot(d, axis)) - 1) < 1e-6) dirEntity = es->item(i)->createForAssemblyContext(ox);
                }
            }
            if (dirEntity)
                ok = motion == "slider" ? in->setAsSliderJointMotion(CustomJointDirection, dirEntity) : in->setAsPlanarJointMotion(CustomJointDirection, dirEntity);
            else
            {
                ok = in->setAsRigidJointMotion();
                approxWhy += " (no straight edge along its " + motion + " direction: made rigid)";
                other = true;
                motion = "rigid";
            }
        }
        else
            ok = in->setAsRigidJointMotion();
        if (!ok && motion != "rigid")
        {
            approxWhy += " (" + motion + " motion refused: " + lastError() + "; made rigid)";
            other = true;
            motion = "rigid";
            ok = in->setAsRigidJointMotion();
        }
        if (!ok) return "~" + motion + " motion refused (" + lastError() + "): the component stays where the file places it";
        Ptr<AsBuiltJoint> j = abj->add(in);
        if (!j) return "~" + motion + " joint refused (" + lastError() + "): the component stays where the file places it";
        j->name(jname);
        jointOf[x] = j;
        return (other ? "~" : "") + motion + " joint " + jname + approxWhy;
    }

    std::string motionLink(const json& mate, const std::vector<MateSide>& ss, std::map<std::string, Ptr<Base>>& jointOf)
    {
        std::string t = mate.value("type", "");
        const json p = mate.value("params", json::object());
        std::vector<std::string> units;
        for (auto& s : ss)
            if (!s.unit.empty() && std::find(units.begin(), units.end(), s.unit) == units.end()) units.push_back(s.unit);
        auto jointOfUnit = [&](const std::string& u) -> Ptr<AsBuiltJoint> {
            auto it = jointOf.find(u);
            return it == jointOf.end() ? nullptr : Ptr<AsBuiltJoint>(it->second);
        };
        Ptr<MotionLinks> links = asmComp->motionLinks();
        Ptr<MotionLinkInput> in;
        if (t == "gear" && units.size() == 2)
        {
            Ptr<AsBuiltJoint> j1 = jointOfUnit(units[0]), j2 = jointOfUnit(units[1]);
            if (!j1 || !j2) return "~placed, not coupled: the gears have no revolute joints to link";
            double a = p.contains("ratio") ? p["ratio"][0].get<double>() : 1, b = p.contains("ratio") ? p["ratio"][1].get<double>() : 1;
            in = links->createInput(j1, j2);
            if (!in) return "~placed, not coupled: " + lastError();
            in->valueOne(ValueInput::createByString("360 deg"));
            in->valueTwo(ValueInput::createByString(std::to_string(360.0 * a / b) + " deg"));
            in->isReversed(p.value("reverse", false));
        }
        else if (t == "rackPinion")
        {
            std::string rack, pinion;
            for (auto& s : ss)
            {
                if (s.role == "rack") rack = s.unit;
                if (s.role == "pinion") pinion = s.unit;
            }
            Ptr<AsBuiltJoint> jp = jointOfUnit(pinion), jr = jointOfUnit(rack);
            if (!jp || !jr) return "~placed, not coupled: the rack or the pinion has no joint to link";
            in = links->createInput(jp, jr);
            if (!in) return "~placed, not coupled: " + lastError();
            in->valueOne(ValueInput::createByString("360 deg"));
            if (!in->valueTwo(ValueInput::createByString(std::to_string(p.value("travelPerTurn", 0.0)) + " mm")))
                return "~placed, not coupled: the rack travel was refused (" + lastError() + ")";
            in->isReversed(p.value("reverse", false));
        }
        else if (t == "screw")
        {
            std::string mover;
            for (auto& u : units)
                if (jointOfUnit(u)) mover = u;
            Ptr<AsBuiltJoint> j = jointOfUnit(mover);
            if (!j) return "~placed, not coupled: no cylindrical joint to turn into a screw";
            in = links->createInput(j);
            if (!in) return "~placed, not coupled: " + lastError();
            in->motionOne(CylindricalJointRotateMotionType);
            in->motionTwo(CylindricalJointSlideMotionType);
            in->valueOne(ValueInput::createByString("360 deg"));
            in->valueTwo(ValueInput::createByString(std::to_string(p.value("pitch", 1.0)) + " mm"));
            in->isReversed(p.value("reverse", false));
        }
        else
            return "~placed, not coupled: Fusion has no " + t + " coupling";
        Ptr<MotionLink> ml = links->add(in);
        if (!ml) return "~placed, not coupled: the motion link was refused (" + lastError() + ")";
        ml->name(mate.value("name", ""));
        return t + " coupling as motion link " + mate.value("name", "");
    }
};

void buildJoints(const json& model, const json& asmDoc, const Ptr<Component>& asmComp, std::map<std::string, Ptr<Occurrence>>& occOf,
                 Report& report)
{
    JointBuilder jb(model, asmDoc, asmComp, occOf, report);
    std::map<std::string, Ptr<Base>> jointOf;
    jb.run(jointOf);
}

} // namespace ncxf
