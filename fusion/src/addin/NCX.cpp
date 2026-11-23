// NCX add-in for Autodesk Fusion: the part Fusion loads.
//
// It does three things and nothing else:
//  1. adds the two commands, "Import NCX..." and "Export NCX...", to Fusion's File menu (and to the
//     Design workspace's Insert panel), and runs them through the implementation DLL;
//  2. loads that implementation (NcxFusionImpl.dll) from a private copy, so a rebuilt implementation can be
//     reloaded without restarting Fusion (developer convenience; users never notice);
//  3. developer job watcher: when the folder %LOCALAPPDATA%\NcxFusion\jobs exists, every "<name>.job.json"
//     dropped there is run on Fusion's main thread and answered with "<name>.result.json". Fusion has no
//     out-of-process API; this is how the add-in is tested without anyone clicking (see FINDINGS.md).
//
// Long work never runs inside a Fusion command event: ImportManager.importToTarget refuses to run there.
// Commands and jobs fire a custom event, and the work runs in its handler.
#include <Core/CoreAll.h>
#include <Fusion/FusionAll.h>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "jobapi.h"

using namespace adsk::core;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const char* kJobEvent = "NCX.job";
const char* kUiEvent = "NCX.ui";
const char* kVersion = NCX_FUSION_VERSION;   // from CMakeLists.txt (project VERSION)

Ptr<Application> app;
Ptr<UserInterface> ui;

HMODULE implModule = nullptr;
ncxf_job_fn implJob = nullptr;
ncxf_free_fn implFree = nullptr;
ncxf_unload_fn implUnload = nullptr;
int implGeneration = 0;
std::string implError;

std::atomic<bool> stopping{false};
std::atomic<bool> jobBusy{false};
std::thread watcher;

fs::path addinDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&addinDir), &self);
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(buf).parent_path();
}

fs::path dataDir()
{
    PWSTR p = nullptr;
    fs::path res;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) res = fs::path(p) / L"NcxFusion";
    CoTaskMemFree(p);
    return res;
}

void log(const std::string& msg)
{
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    std::error_code ec;
    fs::create_directories(dataDir(), ec);
    std::ofstream f(dataDir() / L"ncx.log", std::ios::app);
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    f << ts << "  " << msg << "\n";
}

// ----------------------------------------------------------------------------------- implementation
void unloadImpl()
{
    if (!implModule) return;
    if (implUnload) implUnload();
    FreeLibrary(implModule);
    implModule = nullptr;
    implJob = nullptr;
    implFree = nullptr;
    implUnload = nullptr;
}

// Load NcxFusionImpl.dll (and ncx_core.dll beside it) from a fresh private copy, so the files in the add-in
// folder stay free to be replaced by a rebuild.
bool loadImpl()
{
    unloadImpl();
    implError.clear();
    std::error_code ec;
    fs::path src = addinDir();
    fs::path dir = dataDir() / L"impl" / (std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++implGeneration));
    fs::create_directories(dir, ec);
    for (const wchar_t* name : {L"NcxFusionImpl.dll", L"ncx_core.dll"})
    {
        fs::copy_file(src / name, dir / name, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            implError = "cannot copy " + (src / name).string() + ": " + ec.message();
            return false;
        }
    }
    implModule = LoadLibraryExW((dir / L"NcxFusionImpl.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!implModule)
    {
        implError = "LoadLibrary failed, error " + std::to_string(GetLastError());
        return false;
    }
    implJob = reinterpret_cast<ncxf_job_fn>(GetProcAddress(implModule, "ncxf_job"));
    implFree = reinterpret_cast<ncxf_free_fn>(GetProcAddress(implModule, "ncxf_free"));
    implUnload = reinterpret_cast<ncxf_unload_fn>(GetProcAddress(implModule, "ncxf_unload"));
    if (!implJob || !implFree)
    {
        implError = "NcxFusionImpl.dll lacks ncxf_job/ncxf_free";
        unloadImpl();
        return false;
    }
    // remove copies left by earlier sessions (the current one is in use)
    for (auto& e : fs::directory_iterator(dataDir() / L"impl", ec))
        if (e.path() != dir) fs::remove_all(e.path(), ec);
    return true;
}

json callImpl(const json& request)
{
    if (!implModule && !loadImpl()) return json{{"ok", false}, {"error", "implementation not loaded: " + implError}};
    char* reply = implJob(request.dump().c_str());
    json res;
    try
    {
        res = json::parse(reply ? reply : "{}");
    }
    catch (const std::exception& e)
    {
        res = json{{"ok", false}, {"error", std::string("bad reply: ") + e.what()}};
    }
    if (reply) implFree(reply);
    return res;
}

// --------------------------------------------------------------------------------------- the stub's own jobs
json listDocuments()
{
    json docs = json::array();
    Ptr<Documents> all = app->documents();
    for (size_t i = 0; all && i < all->count(); i++)
    {
        Ptr<Document> d = all->item(i);
        if (!d) continue;
        json j{{"name", d->name()}, {"isModified", d->isModified()}, {"isSaved", d->isSaved()},
               {"isActive", app->activeDocument() && app->activeDocument()->name() == d->name()}};
        if (Ptr<DataFile> df = d->dataFile())
        {
            j["dataFile"] = df->name();
            if (Ptr<DataProject> p = df->parentProject()) j["project"] = p->name();
        }
        docs.push_back(j);
    }
    return docs;
}

json runJob(const json& job)
{
    std::string cmd = job.value("cmd", "");
    if (cmd == "ping")
        return json{{"ok", true}, {"addin", kVersion}, {"implLoaded", implModule != nullptr}, {"fusion", app->version()}};
    if (cmd == "docs") return json{{"ok", true}, {"documents", listDocuments()}};
    if (cmd == "reload")
    {
        bool ok = loadImpl();
        return json{{"ok", ok}, {"error", implError}, {"generation", implGeneration}};
    }
    return callImpl(job);
}

void handleJobFile(const fs::path& running)
{
    json result;
    auto t0 = std::chrono::steady_clock::now();
    try
    {
        std::ifstream in(running);
        json job = json::parse(in);
        result = runJob(job);
    }
    catch (const std::exception& e)
    {
        result = json{{"ok", false}, {"error", std::string("job failed: ") + e.what()}};
    }
    result["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fs::path out = running;
    out.replace_extension(L"");                     // x.job.json.running -> x.job.json
    out = out.parent_path() / (out.stem().stem().wstring() + L".result.json");
    {
        std::ofstream o(out.wstring() + L".tmp");
        o << result.dump(1);
    }
    std::error_code ec;
    fs::rename(out.wstring() + L".tmp", out, ec);
    fs::remove(running, ec);
}

class JobEventHandler : public CustomEventHandler
{
public:
    void notify(const Ptr<CustomEventArgs>& args) override
    {
        handleJobFile(fs::u8path(args->additionalInfo()));
        jobBusy = false;
    }
} jobEventHandler;

void watch()
{
    fs::path dir = dataDir() / L"jobs";
    while (!stopping)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::error_code ec;
        if (jobBusy || !fs::is_directory(dir, ec)) continue;
        fs::path next;
        for (auto& e : fs::directory_iterator(dir, ec))
        {
            std::wstring n = e.path().filename().wstring();
            if (n.size() > 9 && n.substr(n.size() - 9) == L".job.json" && (next.empty() || e.path() < next)) next = e.path();
        }
        if (next.empty()) continue;
        fs::path running = next.wstring() + L".running";
        fs::rename(next, running, ec);
        if (ec) continue;
        jobBusy = true;
        if (!app->fireCustomEvent(kJobEvent, running.u8string())) jobBusy = false;
    }
}

// --------------------------------------------------------------------------------------- update notice
// (user 2026-11-23: "a warning to the user with a click here to update link", never mandatory; same behaviour as
// the SolidWorks add-in's UpdateCheck.cs.) When Fusion loads the add-in, a background thread asks GitHub for the
// releases (at most once a day; the answer is kept in %LOCALAPPDATA%\NcxFusion\update.json), so no command waits for
// the network. At the next Import or Export, if a newer Fusion release (tag "fusion-v<version>") exists, one box
// offers its page: Yes opens it in the browser, No carries on. Once per Fusion session. Offline or any error: nothing.
// No personal details are sent. Off switch: UpdateCheck = 0 (DWORD) under HKCU\Software\NCX Translator.
// NOT TESTED in Fusion (2026-11-23, no Fusion licence here): compiled against the SDK only.
std::mutex updateMutex;
std::string updateVersion, updateUrl;   // the newest Fusion release found
bool updateOffered = false;
std::thread updateThread;

std::vector<int> versionParts(const std::string& v)
{
    std::vector<int> out;
    std::stringstream ss(v);
    std::string part;
    while (std::getline(ss, part, '.'))
    {
        try { out.push_back(std::stoi(part)); }
        catch (...) { return {}; }
    }
    return out;
}

bool newer(const std::string& a, const std::string& b)   // a > b
{
    std::vector<int> x = versionParts(a), y = versionParts(b);
    if (x.empty() || y.empty()) return false;
    size_t n = std::max(x.size(), y.size());
    x.resize(n);
    y.resize(n);
    return x > y;
}

bool updateCheckDisabled()
{
    DWORD v = 1, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\NCX Translator", L"UpdateCheck", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v == 0;
}

std::string httpsGet(const wchar_t* host, const wchar_t* path)
{
    std::string body;
    HINTERNET s = WinHttpOpen(L"NCX-Fusion-update-check", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return body;
    WinHttpSetTimeouts(s, 8000, 8000, 8000, 8000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    DWORD status = 0, len = sizeof(status);
    if (r && WinHttpAddRequestHeaders(r, L"Accept: application/vnd.github+json", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD) &&
        WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX) &&
        status == 200)
    {
        DWORD avail = 0;
        while (!stopping && WinHttpQueryDataAvailable(r, &avail) && avail > 0)
        {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, &chunk[0], avail, &got) || got == 0) break;
            body.append(chunk.data(), got);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return status == 200 ? body : std::string();
}

void checkForUpdate()
{
    try
    {
        if (updateCheckDisabled()) return;
        fs::path cache = dataDir() / L"update.json";
        std::error_code ec;
        json cached;
        if (fs::exists(cache, ec))
        {
            std::ifstream in(cache);
            cached = json::parse(in, nullptr, false);
        }
        long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (cached.is_object() && now - cached.value("checked", 0LL) < 24 * 3600)
        {
            std::lock_guard<std::mutex> lock(updateMutex);
            updateVersion = cached.value("version", "");
            updateUrl = cached.value("url", "");
            return;
        }
        json releases = json::parse(httpsGet(L"api.github.com", L"/repos/SimonRafferty/Neutral-CAD-eXchange-NCX/releases?per_page=50"), nullptr, false);
        if (!releases.is_array()) return;
        std::string best, bestUrl;
        const std::string prefix = "fusion-v";   // the Fusion add-in's releases; the SolidWorks ones are "v..."
        for (auto& r : releases)
        {
            if (!r.is_object() || r.value("draft", false) || r.value("prerelease", false)) continue;
            std::string tag = r.value("tag_name", "");
            if (tag.compare(0, prefix.size(), prefix) != 0) continue;
            std::string v = tag.substr(prefix.size());
            if (best.empty() || newer(v, best))
            {
                best = v;
                bestUrl = r.value("html_url", "");
            }
        }
        if (best.empty()) return;
        {
            std::lock_guard<std::mutex> lock(updateMutex);
            updateVersion = best;
            updateUrl = bestUrl;
        }
        fs::create_directories(dataDir(), ec);
        std::ofstream out(cache);
        out << json{{"checked", now}, {"version", best}, {"url", bestUrl}}.dump();
    }
    catch (...) {}   // never disturb Fusion
}

// Before Import or Export: offer the download page if a newer release exists (once per session).
void offerUpdate()
{
    std::string v, url;
    {
        std::lock_guard<std::mutex> lock(updateMutex);
        if (updateOffered || updateVersion.empty() || updateUrl.empty() || !newer(updateVersion, kVersion)) return;
        updateOffered = true;
        v = updateVersion;
        url = updateUrl;
    }
    if (!ui) return;
    DialogResults r = ui->messageBox("A newer NCX add-in for Fusion is available: " + v + " (this computer has " + kVersion + ").\n\n"
                                     "Open the download page now? (Close Fusion before you run the new installer.)\n\n"
                                     "Choose No to carry on with this version.",
                                     "NCX", YesNoButtonType, InformationIconType);
    if (r == DialogYes)
    {
        std::wstring w(url.begin(), url.end());   // a GitHub URL: ASCII
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// --------------------------------------------------------------------------------------- UI commands
class UiEventHandler : public CustomEventHandler
{
public:
    void notify(const Ptr<CustomEventArgs>& args) override
    {
        offerUpdate();   // a newer release? one Yes/No box, once per session
        json reply = callImpl(json{{"cmd", args->additionalInfo()}, {"ui", true}});
        if (!reply.value("ok", false) && reply.contains("error") && ui)
            ui->messageBox("NCX: " + reply["error"].get<std::string>());
    }
} uiEventHandler;

class ExecuteHandler : public CommandEventHandler
{
public:
    explicit ExecuteHandler(std::string what) : what_(std::move(what)) {}
    void notify(const Ptr<CommandEventArgs>&) override { app->fireCustomEvent(kUiEvent, what_); }

private:
    std::string what_;
};

class CreatedHandler : public CommandCreatedEventHandler
{
public:
    explicit CreatedHandler(std::string what) : exec_(what) {}
    void notify(const Ptr<CommandCreatedEventArgs>& args) override
    {
        Ptr<Command> cmd = args->command();
        if (cmd) cmd->execute()->add(&exec_);
    }

private:
    ExecuteHandler exec_;
};

CreatedHandler importCreated("uiImport");
CreatedHandler exportCreated("uiExport");

const char* kImportId = "NCX_Import";
const char* kExportId = "NCX_Export";

void addCommands()
{
    Ptr<CommandDefinitions> defs = ui->commandDefinitions();
    std::error_code ec;
    std::string res = fs::is_directory(addinDir() / L"resources", ec) ? (addinDir() / L"resources").u8string() : std::string();
    Ptr<CommandDefinition> imp = defs->itemById(kImportId);
    if (!imp) imp = defs->addButtonDefinition(kImportId, "Import NCX...", "Import a neutral CAD (NCX) file and rebuild its parametric history.", res);
    Ptr<CommandDefinition> exp = defs->itemById(kExportId);
    if (!exp) exp = defs->addButtonDefinition(kExportId, "Export NCX...", "Export this design as a neutral CAD (NCX) file.", res);
    if (imp) imp->commandCreated()->add(&importCreated);
    if (exp) exp->commandCreated()->add(&exportCreated);
    // File menu (quick access toolbar)
    if (Ptr<Toolbar> qat = ui->toolbars()->itemById("QAT"))
        if (Ptr<DropDownControl> file = qat->controls()->itemById("FileSubMenuCommand"))
        {
            Ptr<ToolbarControls> c = file->controls();
            if (!c->itemById(kImportId)) c->addCommand(imp, "", false);
            if (!c->itemById(kExportId)) c->addCommand(exp, "", false);
        }
    // Design workspace, Solid tab, Insert panel
    if (Ptr<ToolbarPanel> panel = ui->allToolbarPanels()->itemById("InsertPanel"))
    {
        Ptr<ToolbarControls> c = panel->controls();
        if (!c->itemById(kImportId)) c->addCommand(imp, "", false);
        if (!c->itemById(kExportId)) c->addCommand(exp, "", false);
    }
}

void removeCommands()
{
    auto drop = [](Ptr<ToolbarControls> c) {
        if (!c) return;
        for (const char* id : {kImportId, kExportId})
            if (Ptr<ToolbarControl> x = c->itemById(id)) x->deleteMe();
    };
    if (Ptr<Toolbar> qat = ui->toolbars()->itemById("QAT"))
        if (Ptr<DropDownControl> file = qat->controls()->itemById("FileSubMenuCommand")) drop(file->controls());
    if (Ptr<ToolbarPanel> panel = ui->allToolbarPanels()->itemById("InsertPanel")) drop(panel->controls());
    for (const char* id : {kImportId, kExportId})
        if (Ptr<CommandDefinition> d = ui->commandDefinitions()->itemById(id)) d->deleteMe();
}

} // namespace

extern "C" XI_EXPORT bool run(const char* /*context*/)
{
    app = Application::get();
    if (!app) return false;
    ui = app->userInterface();
    log(std::string("NCX add-in ") + kVersion + " starting in Fusion " + app->version());
    if (Ptr<CustomEvent> e = app->registerCustomEvent(kJobEvent)) e->add(&jobEventHandler);
    if (Ptr<CustomEvent> e = app->registerCustomEvent(kUiEvent)) e->add(&uiEventHandler);
    if (ui) addCommands();
    if (!loadImpl()) log("implementation not loaded: " + implError);
    stopping = false;
    watcher = std::thread(watch);
    updateThread = std::thread(checkForUpdate);   // in the background: is a newer release out?
    return true;
}

extern "C" XI_EXPORT bool stop(const char* /*context*/)
{
    stopping = true;
    if (watcher.joinable()) watcher.join();
    if (updateThread.joinable()) updateThread.join();   // at most the 8 s network timeout
    unloadImpl();
    if (ui) removeCommands();
    app->unregisterCustomEvent(kJobEvent);
    app->unregisterCustomEvent(kUiEvent);
    log("NCX add-in stopped");
    ui = nullptr;
    app = nullptr;
    return true;
}
