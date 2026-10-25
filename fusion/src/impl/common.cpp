#include "common.h"

#include <shlobj.h>

#include <algorithm>
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
using Clock = std::chrono::steady_clock;
// The dialog is updated and Fusion's events pumped at most this often (each update repaints the dialog and runs
// Fusion's event loop; updating on every step made imports visibly slower), and at least this often in long phases.
constexpr double kPumpSeconds = 0.25;
struct ProgressState
{
    Ptr<ProgressDialog> dlg;
    int value = 0, max = 1;
    int cancelAfter = -1;
    int noCancel = 0;              // ProgressNoCancel depth
    int pumps = 0;                 // adsk::doEvents calls (measurement)
    bool cancelPending = false;    // Cancel pressed inside a ProgressNoCancel
    // measurement
    Clock::time_point t0, last, phaseStart, lastPump;
    std::string lastMessage, phase;
    std::vector<json> gaps;                 // every gap between two steps: {seconds, before, after}
    std::map<std::string, double> phases;   // seconds per phase
} progress;

double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

void recordStep(const std::string& message)
{
    double g = since(progress.last);
    progress.gaps.push_back(json{{"seconds", g}, {"before", progress.lastMessage}, {"after", message}, {"phase", progress.phase}});
    progress.last = Clock::now();
    progress.lastMessage = message;
}
} // namespace

void progressPhase(const std::string& phase)
{
    if (!progress.phase.empty()) progress.phases[progress.phase] += since(progress.phaseStart);
    progress.phase = phase;
    progress.phaseStart = Clock::now();
}

json progressTimeline()
{
    std::vector<json> g = progress.gaps;
    // the time from the last step to now counts as a gap too (the end of the import)
    g.push_back(json{{"seconds", since(progress.last)}, {"before", progress.lastMessage}, {"after", "(end)"}, {"phase", progress.phase}});
    std::sort(g.begin(), g.end(), [](const json& a, const json& b) { return a["seconds"].get<double>() > b["seconds"].get<double>(); });
    if (g.size() > 15) g.resize(15);
    std::map<std::string, double> ph = progress.phases;
    if (!progress.phase.empty()) ph[progress.phase] += since(progress.phaseStart);
    return json{{"steps", progress.value}, {"updates", progress.gaps.size()}, {"doEvents", progress.pumps}, {"seconds", since(progress.t0)}, {"longestGaps", g}, {"phases", ph}};
}

void progressBegin(const std::string& title, int steps, bool show)
{
    progress.value = 0;
    progress.max = std::max(1, steps);
    progress.cancelPending = false;
    progress.pumps = 0;
    progress.dlg = nullptr;
    progress.t0 = progress.last = progress.phaseStart = progress.lastPump = Clock::now();
    progress.lastMessage = "(start)";
    progress.phase.clear();
    progress.gaps.clear();
    progress.phases.clear();
    if (!show) return;
    Ptr<UserInterface> ui = Application::get()->userInterface();
    if (!ui) return;
    progress.dlg = ui->createProgressDialog();
    if (!progress.dlg) return;
    progress.dlg->isCancelButtonShown(true);
    progress.dlg->cancelButtonText("Cancel");
    progress.dlg->show(title, "Starting...", 0, progress.max, 0);
    adsk::doEvents();   // paint the dialog before the first long call
}

void progressSetTotal(int steps)
{
    progress.max = std::max(progress.value + 1, steps);
    if (progress.dlg) progress.dlg->maximumValue(progress.max);
}

namespace {
// The dialog's message is a format string (%p, %v, %m): a '%' in a document or feature name is shown as U+FF05.
std::string dialogText(const std::string& m)
{
    std::string r;
    for (char c : m)
        if (c == '%') r += "\xEF\xBC\x85";
        else r += c;
    return r + "\n%p%";
}

// Fusion's events: the dialog repaints, Cancel is seen, Fusion's window stays responsive. Called on every step
// and on ticks; throws Cancelled when the user pressed Cancel.
void pump(const std::string& message)
{
    recordStep(message);   // the gaps measured are between two updates the user sees
    progress.lastPump = Clock::now();
    progress.pumps++;
    adsk::doEvents();
    if (progress.dlg && progress.dlg->wasCancelled()) progress.cancelPending = true;
    if (progress.cancelPending && progress.noCancel == 0)
    {
        progress.cancelPending = false;
        throw Cancelled{message};
    }
}
} // namespace

void progressStep(const std::string& message)
{
    progress.value++;
    progress.lastMessage = message;
    if (progress.cancelAfter >= 0 && progress.value > progress.cancelAfter)
    {
        progress.cancelAfter = -1;
        progress.cancelPending = true;   // as if Cancel were pressed now
    }
    // a step within kPumpSeconds of the last update only counts; a pending cancel is honoured at once
    if (since(progress.lastPump) < kPumpSeconds && !(progress.cancelPending && progress.noCancel == 0)) return;
    if (progress.dlg)
    {
        if (progress.value >= progress.max)
        {
            progress.max = progress.value + std::max(10, progress.max / 10);   // an estimate that ran short
            progress.dlg->maximumValue(progress.max);
        }
        progress.dlg->message(dialogText(message));
        progress.dlg->progressValue(progress.value);
    }
    pump(message);
}

void progressTick(const std::string& message)
{
    if (since(progress.lastPump) < kPumpSeconds) return;
    std::string m = message.empty() ? progress.lastMessage : message;
    if (progress.dlg && !message.empty()) progress.dlg->message(dialogText(m));
    pump(m);
}

ProgressNoCancel::ProgressNoCancel() { progress.noCancel++; }
ProgressNoCancel::~ProgressNoCancel() { progress.noCancel--; }

void progressEnd()
{
    if (progress.dlg) progress.dlg->hide();
    progress.dlg = nullptr;
    progress.cancelAfter = -1;
    progress.cancelPending = false;
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
