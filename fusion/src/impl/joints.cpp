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
        // keyed by the build key of the assembly (a flexible build is "<id>#flex") and of the component's document
        for (auto& doc : model["documents"])
            for (auto& comp : doc.value("components", json::array()))
            {
                docOfComp[doc.value("id", "") + "|" + comp.value("id", "")] = buildKeyOf(model, comp);
                docOfComp[doc.value("id", "") + "#flex|" + comp.value("id", "")] = buildKeyOf(model, comp);
            }
        for (auto& c : asmDoc.value("components", json::array()))
            if (placesFlexible(model, c)) flexibleUnits.insert(c.value("id", ""));
    }

    // Mates that reach into a flexible sub-assembly join the part they name, not the sub-assembly as a whole
    // (FORMAT-SPEC 5: a flexible sub-assembly's parts move with this assembly's motion): such a side's unit is
    // "<component>/<path...>", its occurrence the part's (a proxy through the sub-assembly)
    std::set<std::string> flexibleUnits;
    std::map<std::string, Ptr<Occurrence>> leafOcc;
    std::map<std::string, std::string> leafDocOf;
    Ptr<Occurrence> occFor(const std::string& unit)
    {
        auto it = leafOcc.find(unit);
        if (it != leafOcc.end()) return it->second;
        auto jt = occOf.find(asmId + "|" + unit);
        return jt == occOf.end() ? nullptr : jt->second;
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
        s.leafDoc = baseDocId(doc);
        if (flexibleUnits.count(s.unit) && !r.value("path", json::array()).empty())
        {
            for (auto& p : r["path"]) s.unit += "/" + p.get<std::string>();
            leafOcc[s.unit] = o;
            leafDocOf[s.unit] = s.leafDoc;
        }
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
            // a mate's descriptor is in the referenced part's own frame (FORMAT-SPEC 5; checked against the mates'
            // frames), and Fusion measures a component's faces where its occurrence puts them: the faces are
            // measured through the leaf's full path (the proxy), whose transform is the part's place in the root.
            // (Through the leaf's native occurrence, its place inside its sub-assembly was applied: nested references
            // did not resolve, and their joints fell back to the component origin, a metre from the pins.)
            // inside a sub-assembly the leaf is an occurrence of the sub-assembly's component, not a root path:
            // measured through the sub-assembly's occurrence in the root
            Ptr<Occurrence> ctx = s.leaf;
            Ptr<Component> rootComp = asmComp->parentDesign()->rootComponent();
            // (also a leaf reached through a path, and every leaf of a model in the upright wrapper, common.h)
            if (asmComp != rootComp)
            {
                Ptr<OccurrenceList> subs = rootComp->allOccurrencesByComponent(asmComp);
                if (subs && subs->count() > 0)
                    if (Ptr<Occurrence> p = s.leaf->createForAssemblyContext(subs->item(0))) ctx = p;
            }
            setMeasureContext(ctx);
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
            std::string doc = s.leaf ? s.leafDoc : baseDocId(asmId);
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
            std::string doc = s.leaf ? s.leafDoc : baseDocId(asmId);
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

    // The volume of a component of this assembly (mm^3): its document's bodies, or a sub-assembly's components'.
    double docVolume(const std::string& docId, int depth = 0) const
    {
        if (depth > 20) return 0;
        for (auto& d : model["documents"])
        {
            if (d.value("id", "") != baseDocId(docId)) continue;
            double v = 0;
            for (auto& b : d.value("bodies", json::array()))
                if (b.contains("fingerprint")) v += b["fingerprint"].value("volume", 0.0);
            for (auto& c : d.value("components", json::array())) v += docVolume(c.value("document", ""), depth + 1);
            return v;
        }
        return 0;
    }
    double unitVolume(const std::string& unit) const
    {
        auto lt = leafDocOf.find(unit);
        if (lt != leafDocOf.end()) return docVolume(lt->second);
        auto it = docOfComp.find(asmId + "|" + unit);
        return it == docOfComp.end() ? 0 : docVolume(it->second);
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
        // Every pair of units that share mates gets its own joint (pairs with the assembly or a fixed component
        // first). One joint per moving unit, to the partner it shared most mates with, left closed chains open: in
        // a four-bar linkage the last link and the pins through two parts were joined to one side only, so they came
        // apart when dragged (measured on a real folding step). Fusion solves the closed loops the joints make; all
        // are as-built, at the file's placement, so they agree with each other.
        //
        // A connector (a bolt, nut, washer or pin: a part much smaller than every part it is mated to) adds only
        // redundant loops, and Fusion's solver locks on them (measured on the folding step: with every fastener
        // joined to both parts it passes through, and to the fasteners it touches, no joint could be driven). A
        // connector gets one joint, to the partner it shares most mates with (the assembly or a fixed component
        // first), and moves with that part.
        std::map<std::string, std::set<std::string>> partnersOf;
        for (auto& kv : byPair)
        {
            partnersOf[kv.first.first].insert(kv.first.second);
            partnersOf[kv.first.second].insert(kv.first.first);
        }
        std::set<std::string> connectors;
        for (auto& kv : partnersOf)
        {
            if (kv.first.empty() || kv.second.size() < 2) continue;
            double v = unitVolume(kv.first), biggest = 0;
            for (auto& o : kv.second)
                if (!o.empty()) biggest = std::max(biggest, unitVolume(o));
                else   // the assembly and its fixed components (one partner here): the largest fixed one
                    for (auto& f : fixed) biggest = std::max(biggest, unitVolume(f));
            if (v > 0 && biggest > 0 && v < 0.1 * biggest) connectors.insert(kv.first);
        }
        // A connector's partner: the assembly or a fixed component first, then a part that is not a connector
        // itself, then the most mates. A pair is kept when it is the chosen pair of one of its connectors, or joins
        // no connector at all. (Skipping every other pair of each connector from both ends dropped a nut whose
        // chosen partner was another connector, a nut on a washer: both skipped the pair, and the nut was left with
        // no joint at all; measured on the folding step.) Each connector adds one joint, so no loop is made.
        std::map<std::string, std::pair<std::string, std::string>> bestOf;   // connector -> its chosen pair
        for (auto& x : connectors)
        {
            std::pair<std::string, std::string> best;
            size_t bestN = 0;
            int bestPrio = -1;
            for (auto& kv : byPair)
            {
                if (kv.first.first != x && kv.first.second != x) continue;
                std::string other = kv.first.first == x ? kv.first.second : kv.first.first;
                int prio = other.empty() ? 2 : connectors.count(other) ? 0 : 1;
                if (prio > bestPrio || (prio == bestPrio && kv.second.size() > bestN))
                {
                    best = kv.first;
                    bestN = kv.second.size();
                    bestPrio = prio;
                }
            }
            if (bestPrio >= 0) bestOf[x] = best;
        }
        // A connector concentric with its chosen partner and, on the same axis, with another part that is not a
        // connector is the pin that part pivots on: the part's concentric mate with the pin also joins it to the
        // partner (the right-hand folding step: the short link is mated only to its pivot bolt, the bolt to the
        // hanger; with the bolt's other pairs skipped, the short link kept only a planar joint and came adrift).
        auto concentricLine = [&](size_t i, std::pair<V3, V3>& line) {
            std::string t = mates[i].value("type", "");
            if ((t != "concentric" && t != "hinge") || !mates[i].contains("frame")) return false;
            line = {v3(mates[i]["frame"]["origin"]), unit(v3(mates[i]["frame"]["z"]))};
            return true;
        };
        for (auto& bo : bestOf)
        {
            const std::string& c = bo.first;
            const std::string partner = bo.second.first == c ? bo.second.second : bo.second.first;
            std::vector<std::pair<V3, V3>> pinLines;   // the connector's axes shared with its partner
            for (size_t i : byPair[bo.second])
            {
                std::pair<V3, V3> l;
                if (concentricLine(i, l)) pinLines.push_back(l);
            }
            if (pinLines.empty()) continue;
            for (auto& kv : byPair)
            {
                if (kv.first == bo.second || (kv.first.first != c && kv.first.second != c)) continue;
                std::string y = kv.first.first == c ? kv.first.second : kv.first.first;
                if (y.empty() || y == partner || connectors.count(y)) continue;
                for (size_t i : kv.second)
                {
                    std::pair<V3, V3> l;
                    if (!concentricLine(i, l)) continue;
                    bool same = false;
                    for (auto& p : pinLines)
                    {
                        V3 d = sub(l.first, p.first);
                        V3 perp = sub(d, mul(p.second, dot(d, p.second)));
                        if (std::fabs(std::fabs(dot(l.second, p.second)) - 1) < 1e-6 && norm(perp) < 1e-3) same = true;
                    }
                    if (!same) continue;
                    auto key = std::minmax(y, partner);
                    auto& into = byPair[{key.first, key.second}];
                    if (std::find(into.begin(), into.end(), i) == into.end()) into.push_back(i);
                }
            }
        }
        std::set<std::pair<std::string, std::string>> skip;
        std::map<std::pair<std::string, std::string>, std::string> moverOf;   // a connector's pair: the connector moves
        for (auto& kv : byPair)
        {
            bool any = false, chosen = false;
            for (const std::string& u : {kv.first.first, kv.first.second})
                if (connectors.count(u))
                {
                    any = true;
                    if (bestOf[u] == kv.first)
                    {
                        chosen = true;
                        if (!moverOf.count(kv.first)) moverOf[kv.first] = u;
                    }
                }
            if (any && !chosen) skip.insert(kv.first);
        }
        // A part mated only to connectors that each chose another partner (a nut on a bolt and on another nut,
        // both joined to the hanger) would be left with no joint (measured on the folding step: a free nut). It is
        // joined like a connector: one joint, to the partner it shares most mates with, moving with it.
        for (auto& kv : partnersOf)
        {
            const std::string& u = kv.first;
            if (u.empty() || connectors.count(u)) continue;
            std::pair<std::string, std::string> best;
            size_t bestN = 0;
            int bestPrio = -1;
            bool kept = false;
            for (auto& bp : byPair)
            {
                if (bp.first.first != u && bp.first.second != u) continue;
                if (!skip.count(bp.first)) kept = true;
                std::string other = bp.first.first == u ? bp.first.second : bp.first.first;
                int prio = other.empty() ? 2 : connectors.count(other) ? 0 : 1;
                if (prio > bestPrio || (prio == bestPrio && bp.second.size() > bestN))
                {
                    best = bp.first;
                    bestN = bp.second.size();
                    bestPrio = prio;
                }
            }
            if (kept || bestPrio < 0) continue;
            skip.erase(best);
            moverOf[best] = u;
        }
        // every mate of each unit (any partner): a connector's joint looks at all of them (jointFor)
        std::map<std::string, std::vector<size_t>> matesOfUnit;
        for (auto& kv : byPair)
            for (size_t i : kv.second)
            {
                if (!kv.first.first.empty()) matesOfUnit[kv.first.first].push_back(i);
                if (!kv.first.second.empty()) matesOfUnit[kv.first.second].push_back(i);
            }
        std::vector<std::pair<std::string, std::string>> pairs;
        for (auto& kv : byPair)
            if ((kv.first.first.empty() || kv.first.second.empty()) && !skip.count(kv.first)) pairs.push_back(kv.first);
        for (auto& kv : byPair)
            if (!kv.first.first.empty() && !kv.first.second.empty() && !skip.count(kv.first)) pairs.push_back(kv.first);
        for (auto& pr : pairs)
        {
            // the moving side: the unit that is not the assembly; a connector's chosen pair, the connector; between
            // two other moving units, the first in the file
            std::string x = pr.first.empty() ? pr.second : pr.first, partner = pr.first.empty() ? std::string() : pr.second;
            if (moverOf.count(pr) && moverOf[pr] != x) std::swap(x, partner);
            if (x.empty()) continue;
            progressTick(asmName + ": joint for " + x);
            std::string jname;
            std::map<std::string, Ptr<Base>> made;
            bool asConnector = moverOf.count(pr) > 0;
            std::string kind = jointFor(x, partner, byPair[pr], mates, sides, jname, made, asConnector ? &matesOfUnit[x] : nullptr);
            for (auto& kv : made)
                if (!jointOf.count(kv.first)) jointOf[kv.first] = kv.second;   // the first joint of a unit drives its links
            for (size_t i : byPair[pr]) covered[i] = kind;
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
                         std::vector<std::vector<MateSide>>& sides, std::string& jname, std::map<std::string, Ptr<Base>>& jointOf,
                         const std::vector<size_t>* connectorMates = nullptr)
    {
        Ptr<Occurrence> ox = occFor(x);
        Ptr<Occurrence> op = partner.empty() ? nullptr : occFor(partner);
        if (!ox) return "~the component " + x + " was not placed: no joint";
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
        // two concentric mates on parallel axes apart (a part held on two pins) leave no rotation: rigid
        // (measured on the folding step: such pairs had become revolute joints about one of the pins)
        bool twoPins = false;
        {
            std::vector<std::pair<V3, V3>> lines;
            for (size_t i : idx)
                if ((mates[i].value("type", "") == "concentric" || mates[i].value("type", "") == "hinge") && mates[i].contains("frame"))
                    lines.push_back({v3(mates[i]["frame"]["origin"]), unit(v3(mates[i]["frame"]["z"]))});
            for (size_t a = 0; a < lines.size(); a++)
                for (size_t b = a + 1; b < lines.size(); b++)
                {
                    V3 d = sub(lines[b].first, lines[a].first);
                    V3 perp = sub(d, mul(lines[a].second, dot(d, lines[a].second)));
                    if (std::fabs(std::fabs(dot(lines[a].second, lines[b].second)) - 1) < 1e-6 && norm(perp) > 1e-3) twoPins = true;
                }
        }
        if (types.count("lock") || (dn.size() >= 3 && !cyl) || twoPins) motion = "rigid";
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
        // A connector has one joint (run), but in the source it is held by its mates with several parts (a bolt
        // concentric with a plate and seated on another face; a nut on the bolt and on a washer). A joint that
        // left it sliding or spinning off its partner came apart when dragged, in Fusion and after the export
        // (measured on the folding step). It keeps its place on its partner: revolute about its own axis when any
        // of its mates seats it across that axis, else rigid; approximate, with the reason.
        bool fastener = false, seatedOnPartner = false;   // recorded on the joint for the export
        if (connectorMates && (motion == "cylindrical" || motion == "planar" || motion == "slider"))
        {
            // the axis must be one the partner shares (a concentric mate with it): a nut seated on another nut was
            // joined about its own bolt's axis to a nut on another bolt, which no mate between the two can say
            std::set<std::string> fixedHere;
            for (auto& c : asmDoc.value("components", json::array()))
                if (c.value("fixed", false)) fixedHere.insert(c.value("id", ""));
            auto unitOf = [&](const std::string& u) { return fixedHere.count(u) ? std::string() : u; };
            auto withPartner = [&](size_t i) {
                for (auto& s : sides[i])
                    if (s.unit != x && unitOf(s.unit) == partner) return true;
                return false;
            };
            const MateSide* c2 = cyl;
            for (size_t i : *connectorMates)
            {
                if (c2) break;
                std::string t = mates[i].value("type", "");
                if ((t != "concentric" && t != "hinge" && t != "coincident") || !withPartner(i)) continue;
                for (auto& s : sides[i])
                {
                    V3 a;
                    if (!c2 && s.unit == x && (t != "hinge" || s.role == "concentric") && cylinderAxis(s, a)) c2 = &s;
                }
            }
            bool seated = false;
            V3 ax{0, 0, 0};
            fastener = true;
            if (c2 && cylinderAxis(*c2, ax))
                for (size_t i : *connectorMates)
                {
                    std::string t = mates[i].value("type", "");
                    if (t != "coincident" && t != "distance" && t != "hinge") continue;
                    for (auto& s : sides[i])
                    {
                        V3 n;
                        if (s.unit == x && (t != "hinge" || s.role == "coincident") && planeNormal(s, n) && std::fabs(std::fabs(dot(n, ax)) - 1) < 1e-6)
                        {
                            seated = true;
                            if (withPartner(i)) seatedOnPartner = true;
                        }
                    }
                }
            std::string was = motion;
            if (c2 && seated)
            {
                cyl = c2;
                axis = ax;
                motion = "revolute";
            }
            else
                motion = "rigid";
            other = true;
            approxWhy += " (a fastener: its mates with " + (partner.empty() ? std::string("the assembly") : partner) + " alone leave it " +
                         (was == "cylindrical" ? "sliding along its axis" : "sliding off") + "; its other mates hold it in the source, so it is joined " +
                         motion + " to this partner and moves with it)";
        }
        // the assembly itself as the partner: a component the mates fix is grounded; one that may move needs a
        // grounded occurrence to joint to
        if (partner.empty() && motion == "rigid")
        {
            holdOccurrence(ox, true, asmComp == asmComp->parentDesign()->rootComponent());
            jointOf[x] = ox;
            return (other ? "~" : "") + std::string("fixed to the assembly: the component is grounded") + approxWhy;
        }
        if (!op)
        {
            // the moving part's partner is the assembly: joint it to the assembly's grounded component, if any
            // the fixed component the mates name (a re-imported Fusion export has several fixed components: the
            // last one in the file, a fixed nut, became every such joint's partner), else the largest fixed one
            std::set<std::string> fixedIds;
            for (auto& c : asmDoc.value("components", json::array()))
                if (c.value("fixed", false)) fixedIds.insert(c.value("id", ""));
            for (size_t i : idx)
                for (auto& s : sides[i])
                    if (!op && s.unit != x && fixedIds.count(s.unit)) op = occOf[asmId + "|" + s.unit];
            double big = -1;
            if (!op)
                for (auto& f : fixedIds)
                    if (unitVolume(f) > big)
                    {
                        big = unitVolume(f);
                        op = occOf[asmId + "|" + f];
                    }
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
        std::string geomNote;
        if (!g)
        {
            Ptr<ConstructionPoint> o = ox->component()->originConstructionPoint()->createForAssemblyContext(ox);
            g = JointGeometry::createByPoint(o);
            if (motion == "revolute" || motion == "cylindrical")   // a slider or planar joint's origin does not matter
            {
                geomNote = " (at the component's origin: " + (cyl ? "its pin face was not found, " + why : std::string("no face for its axis")) + ")";
                other = true;
            }
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
        // Fusion reads a joint made in a sub-assembly back without its occurrences or geometry (also through its
        // proxy; measured on L12's arm), so the export could not write it: the joint keeps its occurrences' names,
        // its origin (a point on its axis) and its axis or plane normal, in this assembly's frame
        {
            V3 org{0, 0, 0};
            if (cyl && cyl->leaf && cyl->ref.contains("geom") && cyl->ref["geom"].contains("params") && cyl->ref["geom"]["params"].contains("origin"))
                org = applyMm(cyl->leaf->transform2(), v3(cyl->ref["geom"]["params"]["origin"]));
            json rec{{"one", ox->name()}, {"two", op ? op->name() : std::string()}, {"motion", motion},
                     {"origin", json::array({org[0], org[1], org[2]})}, {"axis", json::array({axis[0], axis[1], axis[2]})}};
            if (fastener) rec["fastener"] = true, rec["seatedOnPartner"] = seatedOnPartner;
            if (Ptr<Attributes> ats = j->attributes()) ats->add("NCX", "joint", rec.dump());
        }
        // the joint's frame as made, in this assembly's frame (mm), for the import's reply (developer check)
        if (Ptr<JointGeometry> jg = j->geometry())
        {
            Ptr<Point3D> o = jg->origin();
            // read back, the axis of a joint made on a face of a placed part is in that part's own frame (measured:
            // a bolt turned 90 deg read its own -Y for the assembly's X); the origin is in the assembly's: the axis is
            // turned by the part's placement for the check
            Ptr<Vector3D> z = jg->primaryAxisVector()->copy();
            Ptr<Base> e1 = jg->entityOne();
            Ptr<Occurrence> ctx;
            if (Ptr<BRepFace> f = e1) ctx = f->assemblyContext();
            if (ctx) z->transformBy(ctx->transform2());
            // a model in the upright wrapper (common.h): the frame is recorded in the file's frame, as the mates' are
            Ptr<Point3D> oo = o->copy();
            if (Ptr<Occurrence> w = modelWrapper())
            {
                Ptr<Matrix3D> inv = w->transform2()->copy();
                inv->invert();
                z->transformBy(inv);
                oo->transformBy(inv);   // sub-assemblies are built inside the wrapper too
            }
            o = oo;
            jointFrames().push_back(json{{"assembly", asmName}, {"buildKey", asmId}, {"joint", jname}, {"motion", motion},
                                         {"one", ox->fullPathName()}, {"two", op ? op->fullPathName() : std::string()},
                                         {"origin", json::array({cm2mm(o->x()), cm2mm(o->y()), cm2mm(o->z())})},
                                         {"axis", json::array({z->x(), z->y(), z->z()})}, {"byFace", (bool)cyl || planarByFace},
                                         {"second", json::array({jg->secondaryAxisVector()->x(), jg->secondaryAxisVector()->y(), jg->secondaryAxisVector()->z()})},
                                         {"third", json::array({jg->thirdAxisVector()->x(), jg->thirdAxisVector()->y(), jg->thirdAxisVector()->z()})}});
        }
        if (!j->occurrenceOne() || !j->geometry())
            geomNote += " (Fusion reports the joint without its occurrences or geometry when read back)";
        return (other ? "~" : "") + motion + " joint " + jname + approxWhy + geomNote;
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

json& jointFrames()
{
    static json frames = json::array();
    return frames;
}

void buildJoints(const json& model, const json& asmDoc, const Ptr<Component>& asmComp, std::map<std::string, Ptr<Occurrence>>& occOf,
                 Report& report)
{
    JointBuilder jb(model, asmDoc, asmComp, occOf, report);
    std::map<std::string, Ptr<Base>> jointOf;
    jb.run(jointOf);
}

} // namespace ncxf
