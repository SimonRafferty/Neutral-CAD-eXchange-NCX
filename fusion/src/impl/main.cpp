// NcxFusionImpl.dll entry points: one JSON request in, one JSON reply out (see jobapi.h).
#include "common.h"

#include <cstring>

using namespace ncxf;

namespace ncxf {
json probe(const json& req);   // probe.cpp: developer experiments
json importJob(const json& req);   // importer.cpp
json uiImport(const json& req);
json uiExport(const json& req);
json screenshot(const json& req);   // probe.cpp
json closeDocs(const json& req);    // probe.cpp
json exportJob(const json& req);    // exporter.cpp
}

namespace {

json dispatch(const json& req)
{
    std::string cmd = req.value("cmd", "");
    if (cmd == "probe") return probe(req);
    if (cmd == "import") return importJob(req);
    if (cmd == "export") return exportJob(req);
    if (cmd == "uiImport") return uiImport(req);
    if (cmd == "uiExport") return uiExport(req);
    if (cmd == "screenshot") return screenshot(req);
    if (cmd == "closeDocs") return closeDocs(req);
    if (cmd == "version") return json{{"ok", true}, {"impl", kImplVersion}, {"core", ncx_version()}};
    return json{{"ok", false}, {"error", "unknown cmd " + cmd}};
}

char* dup(const std::string& s)
{
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

} // namespace

extern "C" __declspec(dllexport) char* ncxf_job(const char* request)
{
    json reply;
    try
    {
        reply = dispatch(json::parse(request ? request : "{}"));
    }
    catch (const std::exception& e)
    {
        reply = json{{"ok", false}, {"error", std::string("exception: ") + e.what()}};
    }
    catch (...)
    {
        // /EHa: structured exceptions (access violations) land here instead of killing Fusion
        reply = json{{"ok", false}, {"error", "unexpected structured exception in the implementation"}};
    }
    return dup(reply.dump());
}

extern "C" __declspec(dllexport) void ncxf_free(char* p) { std::free(p); }

extern "C" __declspec(dllexport) void ncxf_unload() {}
