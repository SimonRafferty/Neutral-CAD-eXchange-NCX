#include "reference.h"

#include "host.h"

namespace ncxf {

namespace {

bool allPlanar(const Ptr<BRepBody>& b)
{
    Ptr<BRepFaces> fs = b->faces();
    for (size_t i = 0; i < fs->count(); i++)
        if (!Ptr<Plane>(fs->item(i)->geometry())) return false;
    return true;
}

std::vector<Ptr<BRepBody>> bodiesOf(const Ptr<Component>& c)
{
    std::vector<Ptr<BRepBody>> v;
    for (size_t i = 0; i < c->bRepBodies()->count(); i++) v.push_back(c->bRepBodies()->item(i));
    return v;
}

const json* payloadDoc(const json& model, const std::string& name)
{
    for (auto& d : model["documents"])
        if (d.value("name", "") == name) return &d;
    return nullptr;
}

json payloadBodyFps(const json* doc)
{
    json a = json::array();
    if (doc && doc->contains("bodies"))
        for (auto& b : (*doc)["bodies"])
            if (b.contains("fingerprint")) a.push_back(b["fingerprint"]);
    return a;
}

} // namespace

NoticeId NoticeId::fromHeader(const json& fields)
{
    NoticeId n;
    n.name = fields.value("notice", "");
    std::string s = fields.value("noticebox", "");
    size_t i = 0;
    while (!s.empty() && i <= s.size())
    {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        try
        {
            n.box.push_back(std::stod(s.substr(i, j - i)));
        }
        catch (...)
        {
            n.box.clear();
            break;
        }
        i = j + 1;
    }
    if (n.box.size() != 6) n.box.clear();
    return n;
}

int findNoticeBody(const std::vector<Ptr<BRepBody>>& bodies, const json& payloadBodies, const NoticeId& notice,
                   const Ptr<Matrix3D>& xf, std::string* how)
{
    std::vector<json> fps;
    if (!notice.box.empty())
    {
        // core 0.2.0: the notice's box in the file's frame (FORMAT-SPEC 2.6, within 0.01 mm)
        int hit = -1, n = 0;
        for (size_t i = 0; i < bodies.size(); i++)
        {
            if (!bodies[i]->isSolid()) continue;
            json fp = bodyFingerprint(bodies[i], xf);
            bool same = true;
            for (int k = 0; k < 6; k++)
                if (std::fabs(fp["bbox"][k].get<double>() - notice.box[k]) > 0.01) same = false;
            if (same)
            {
                hit = static_cast<int>(i);
                n++;
            }
        }
        if (n == 1)
        {
            if (how) *how = "body with the header's noticebox";
            return hit;
        }
        if (n == 0) return -1;   // the box says it is not among these bodies
    }
    if (bodies.size() < 2 && payloadBodies.size() >= bodies.size()) return -1;
    for (auto& b : bodies) fps.push_back(bodyFingerprint(b));
    std::vector<int> cands;
    for (size_t i = 0; i < bodies.size(); i++)
    {
        if (!bodies[i]->isSolid() || !allPlanar(bodies[i])) continue;
        double v = fps[i]["volume"].get<double>();
        bool matchesPayload = false;
        for (auto& p : payloadBodies)
        {
            double pv = p.value("volume", 0.0);
            if (std::fabs(pv - v) <= 1e-3 * std::max(pv, 1e-9)) matchesPayload = true;
        }
        if (matchesPayload) continue;
        double top = fps[i]["bbox"][4].get<double>();
        bool inFront = true;
        for (size_t j = 0; j < bodies.size(); j++)
            if (j != i && fps[j]["bbox"][1].get<double>() < top - 1e-6) inFront = false;
        if (inFront) cands.push_back(static_cast<int>(i));
    }
    if (cands.size() == 1 && how) *how = "body recognised by geometry (file without noticebox; Fusion's STEP reader drops solid names)";
    return cands.size() == 1 ? cands[0] : -1;
}

Reference loadReference(const fs::path& stepPath, const json& model, const NoticeId& noticeId, bool describeAll)
{
    Reference ref;
    Ptr<Application> app = Application::get();
    Ptr<ImportManager> im = app->importManager();
    Ptr<STEPImportOptions> opt = im->createSTEPImportOptions(stepPath.u8string());
    if (!opt)
    {
        ref.error = "STEP reader: " + lastError();
        return ref;
    }
    opt->isViewFit(false);
    Ptr<Document> doc = im->importToNewDocument(opt);
    trackDocument(doc);
    if (!doc)
    {
        ref.error = "STEP import failed: " + lastError();
        return ref;
    }
    Ptr<Design> des = doc->products()->itemByProductType("DesignProductType");
    if (!des)
    {
        doc->close(false);
        ref.error = "the STEP import made no design";
        return ref;
    }
    Ptr<Component> root = des->rootComponent();
    Ptr<OccurrenceList> occs = root->allOccurrences();
    ref.assembly = occs && occs->count() > 0;

    // the root's own bodies: a part file (the notice is one of them)
    std::vector<Ptr<BRepBody>> rootBodies = bodiesOf(root);
    std::string rootName;
    for (auto& d : model["documents"])
        if (d.value("id", "") == model.value("root", "")) rootName = d.value("name", "");
    if (!rootBodies.empty())
    {
        std::string how;
        int notice = findNoticeBody(rootBodies, payloadBodyFps(payloadDoc(model, rootName)), noticeId, nullptr, &how);
        RefComponent rc;
        rc.name = rootName;
        for (size_t i = 0; i < rootBodies.size(); i++)
        {
            // fingerprints cover solid bodies only (as the payload's do)
            if (static_cast<int>(i) == notice || !rootBodies[i]->isSolid()) continue;
            json fp = bodyFingerprint(rootBodies[i]);
            ref.fingerprint["bodies"].push_back(fp);
            rc.temp.push_back(TemporaryBRepManager::get()->copy(rootBodies[i]));
            json b{{"fingerprint", fp}};
            if (describeAll) b["faces"] = describeFaces(rootBodies[i]);
            rc.bodies.push_back(b);
        }
        rc.described = describeAll;
        if (notice >= 0) ref.noticeFound = how;
        ref.components[""] = rc;
    }
    // components: bodies in their own frame (described once), placed by every occurrence into the fingerprint
    for (size_t i = 0; occs && i < occs->count(); i++)
    {
        Ptr<Occurrence> o = occs->item(i);
        Ptr<Component> c = o->component();
        std::string name = c->name();
        if (isNoticeName(name, noticeId.name))
        {
            ref.noticeFound = "component '" + name + "'";
            continue;
        }
        std::vector<Ptr<BRepBody>> bs = bodiesOf(c);
        if (bs.empty()) continue;
        Ptr<Matrix3D> xf = o->transform2();
        for (auto& b : bs)
            if (b->isSolid()) ref.fingerprint["bodies"].push_back(bodyFingerprint(b, xf));
        if (!ref.components.count(name))
        {
            setMeasureContext(o);   // native faces of a component under an occurrence measure only through it
            RefComponent rc;
            rc.name = name;
            for (auto& b : bs)
            {
                if (!b->isSolid()) continue;
                rc.temp.push_back(TemporaryBRepManager::get()->copy(b));
                json bj{{"fingerprint", bodyFingerprint(b)}};
                if (describeAll) bj["faces"] = describeFaces(b);
                rc.bodies.push_back(bj);
            }
            rc.described = describeAll;
            ref.components[name] = rc;
        }
    }
    setMeasureContext(nullptr);
    doc->close(false);
    ref.ok = true;
    return ref;
}

std::string importPlain(const fs::path& stepPath, const Ptr<Component>& target, const json& model, const NoticeId& noticeId)
{
    Ptr<Application> app = Application::get();
    Ptr<ImportManager> im = app->importManager();
    Ptr<STEPImportOptions> opt = im->createSTEPImportOptions(stepPath.u8string());
    if (!opt) return "STEP reader: " + lastError();
    opt->isViewFit(false);
    Ptr<ObjectCollection> made = im->importToTarget2(opt, target);
    if (!made) return "STEP import failed: " + lastError();
    // delete the notice (a component in an assembly file, a body in a part file)
    std::string rootName;
    for (auto& d : model["documents"])
        if (d.value("id", "") == model.value("root", "")) rootName = d.value("name", "");
    Ptr<OccurrenceList> occs = target->allOccurrences();
    std::vector<Ptr<Occurrence>> drop;
    for (size_t i = 0; occs && i < occs->count(); i++)
        if (isNoticeName(occs->item(i)->component()->name(), noticeId.name)) drop.push_back(occs->item(i));
    for (auto& o : drop) o->deleteMe();
    for (size_t i = 0; occs && i < occs->count(); i++)
    {
        Ptr<Component> c = occs->item(i)->component();
        std::vector<Ptr<BRepBody>> bs = bodiesOf(c);
        if (bs.size() < 2) continue;
        const json* pd = payloadDoc(model, rootName);
        // the box is in the file's frame; the import may have placed the bodies under a moved occurrence
        int n = findNoticeBody(bs, payloadBodyFps(pd), noticeId, occs->item(i)->transform2());
        if (n >= 0) bs[n]->deleteMe();
    }
    return "";
}

} // namespace ncxf
