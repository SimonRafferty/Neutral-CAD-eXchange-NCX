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
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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

// --------------------------------------------------------------------------------------- UI commands
class UiEventHandler : public CustomEventHandler
{
public:
    void notify(const Ptr<CustomEventArgs>& args) override
    {
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
    return true;
}

extern "C" XI_EXPORT bool stop(const char* /*context*/)
{
    stopping = true;
    if (watcher.joinable()) watcher.join();
    unloadImpl();
    if (ui) removeCommands();
    app->unregisterCustomEvent(kJobEvent);
    app->unregisterCustomEvent(kUiEvent);
    log("NCX add-in stopped");
    ui = nullptr;
    app = nullptr;
    return true;
}
