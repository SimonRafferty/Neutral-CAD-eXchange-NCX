#include "common.h"

#include <shlobj.h>

#include <chrono>
#include <fstream>
#include <sstream>

namespace ncxf {

std::string lastError()
{
    std::string msg;
    Ptr<Application> app = Application::get();
    if (app) app->getLastError(&msg);
    return msg;
}

std::string statusName(int s)
{
    switch (s)
    {
    case NCX_OK: return "OK";
    case NCX_NO_PAYLOAD: return "NO_PAYLOAD";
    case NCX_HEADER_UNREADABLE: return "HEADER_UNREADABLE";
    case NCX_CHUNK_MISSING: return "CHUNK_MISSING";
    case NCX_DIGEST_MISMATCH: return "DIGEST_MISMATCH";
    case NCX_SCHEMA_TOO_NEW: return "SCHEMA_TOO_NEW";
    case NCX_INVALID_JSON: return "INVALID_JSON";
    case NCX_INVALID_MODEL: return "INVALID_MODEL";
    case NCX_STEP_UNPARSEABLE: return "STEP_UNPARSEABLE";
    case NCX_BAD_ARGUMENT: return "BAD_ARGUMENT";
    default: return "INTERNAL";
    }
}

namespace {
std::string take(char* p, size_t n = std::string::npos)
{
    if (!p) return {};
    std::string s = n == std::string::npos ? std::string(p) : std::string(p, n);
    ncx_free(p);
    return s;
}
json parseOr(const std::string& s)
{
    if (s.empty()) return json::object();
    try
    {
        return json::parse(s);
    }
    catch (...)
    {
        return json{{"raw", s}};
    }
}
} // namespace

CoreResult corePayloadRead(const std::string& stepText)
{
    char* out = nullptr;
    size_t outLen = 0;
    char* rep = nullptr;
    CoreResult r;
    r.status = ncx_payload_read(stepText.data(), stepText.size(), &out, &outLen, &rep);
    r.statusName = statusName(r.status);
    r.data = take(out, outLen);
    r.report = parseOr(take(rep));
    return r;
}

json coreCompare(const json& expected, const json& actual, const char* profile)
{
    char* rep = nullptr;
    std::string opt = std::string("{\"profile\":\"") + profile + "\"}";
    ncx_fingerprint_compare_ex(expected.dump().c_str(), actual.dump().c_str(), opt.c_str(), &rep);
    return parseOr(take(rep));
}

json coreMatch(const json& request)
{
    char* rep = nullptr;
    ncx_reference_match(request.dump().c_str(), &rep);
    return parseOr(take(rep));
}

json coreFaceCompare(const json& request)
{
    char* rep = nullptr;
    ncx_face_compare(request.dump().c_str(), &rep);
    return parseOr(take(rep));
}

std::string readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

void writeFile(const fs::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::binary);
    f << s;
}

fs::path dataDir()
{
    PWSTR p = nullptr;
    fs::path res;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) res = fs::path(p) / L"NcxFusion";
    CoTaskMemFree(p);
    return res;
}

fs::path tempDir()
{
    fs::path d = dataDir() / L"tmp";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

void logLine(const std::string& msg)
{
    std::error_code ec;
    fs::create_directories(dataDir(), ec);
    std::ofstream f(dataDir() / L"ncx.log", std::ios::app);
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    f << ts << "  impl: " << msg << "\n";
}

namespace {
std::vector<Ptr<Document>>& registry()
{
    static std::vector<Ptr<Document>> docs;
    return docs;
}
} // namespace

void trackDocument(const Ptr<Document>& d)
{
    if (d) registry().push_back(d);
}

std::vector<Ptr<Document>> trackedDocuments() { return registry(); }

namespace {
struct ProgressState
{
    Ptr<ProgressDialog> dlg;
    int value = 0;
    int cancelAfter = -1;
} progress;
} // namespace

void progressBegin(const std::string& title, int steps, bool show)
{
    progress.value = 0;
    progress.dlg = nullptr;
    if (!show) return;
    Ptr<UserInterface> ui = Application::get()->userInterface();
    if (!ui) return;
    progress.dlg = ui->createProgressDialog();
    progress.dlg->isCancelButtonShown(true);
    progress.dlg->show(title, "%p%", 0, std::max(1, steps), 0);
}

void progressStep(const std::string& message)
{
    progress.value++;
    if (progress.cancelAfter >= 0 && progress.value > progress.cancelAfter)
    {
        progress.cancelAfter = -1;
        throw Cancelled{message};
    }
    if (!progress.dlg) return;
    progress.dlg->message(message);
    progress.dlg->progressValue(progress.value);
    adsk::doEvents();
    if (progress.dlg->wasCancelled()) throw Cancelled{message};
}

void progressEnd()
{
    if (progress.dlg) progress.dlg->hide();
    progress.dlg = nullptr;
    progress.cancelAfter = -1;
}

void progressCancelAfter(int afterSteps) { progress.cancelAfter = afterSteps; }

std::string headerForm(const std::string& s)
{
    std::string r;
    for (unsigned char c : s)
    {
        if (c == ' ') continue;   // spaces dropped (FORMAT-SPEC 2.6, "Matching names")
        if (c == ';' || c == '\'' || c == '\\' || c < 0x20 || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            r += '_';
        else
            r += static_cast<char>(c);
    }
    return r;
}

bool isNoticeName(const std::string& bodyName, const std::string& noticeHeaderName)
{
    if (bodyName.empty()) return false;
    std::string a = headerForm(bodyName), b = headerForm(noticeHeaderName);
    if (!b.empty())
    {
        if (a == b) return true;
        if (a.size() >= 7 && b.compare(0, a.size(), a) == 0) return true;   // shortened by the importer
    }
    // files written before the header's notice key: a name starting with READ ME
    return headerForm(bodyName).rfind("README", 0) == 0;
}

} // namespace ncxf
