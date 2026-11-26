#include "app/controller.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <set>
#include <thread>

#include "app/runtime.h"
#include "core/app_control.h"
#include "core/download_log.h"
#include "core/first_run.h"
#include "core/game.h"
#include "core/game_dlls.h"
#include "core/installation.h"
#include "core/instance_folders.h"
#include "core/log_vault.h"
#include "core/patch_files.h"
#include "core/retention.h"
#include "core/save_recovery.h"
#include "core/process.h"
#include "core/settings.h"
#include "core/updater.h"

using Microsoft::WRL::ComPtr;

namespace rwnd::app {

constexpr const char* LegalVersion = "2026-11-23";

namespace {

std::optional<std::string> Str(const Json& p, const char* name) {
    return GetString(p, name);
}
bool Bool(const Json& p, const char* name) {
    return GetBool(p, name).value_or(false);
}
fs::path CrashQueueDirectory() {
    return crash_reports::QueueDirectory();
}

Json Release(const ReleaseInfo& r) {
    return {
        {"version", r.version}, {"url", r.url}, {"size", r.size}, {"notes", r.notes ? Json(*r.notes) : Json(nullptr)}};
}

std::wstring BrowseFolder(HWND owner, const std::wstring& title, const std::wstring& initial) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return L"";
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title.c_str());
    if (!initial.empty() && DirExists(initial)) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            dialog->SetFolder(folder.Get());
    }
    if (dialog->Show(owner) != S_OK) return L"";
    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&result)) || FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) return L"";
    std::wstring out = path;
    CoTaskMemFree(path);
    return out;
}

fs::path ResolveDownloadTarget(const std::wstring& chosen) {
    std::wstring trimmed = TrimW(chosen);
    if (trimmed.empty()) Throw(ErrorKind::Argument, "The path is empty.");
    fs::path full = FullPath(trimmed);
    if (FileExists(full)) Throw(ErrorKind::IO, "Choose a folder, not a file.");
    bool usable = !DirExists(full) || IsDirectoryEmpty(full) || DirExists(full / L".DepotDownloader") ||
                  FileExists(full / L"RainbowSix.exe");
    return usable ? full : full / L"ParaBellum";
}

uint64_t FolderBytes(const fs::path& folder) {
    if (!DirExists(folder)) return 0;
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return ec ? 0 : total;
}

std::string Gigabytes(int64_t bytes) {
    char buffer[32];
    double gb = static_cast<double>(std::max<int64_t>(0, bytes)) / (1024.0 * 1024 * 1024);
    snprintf(buffer, sizeof buffer, "%.1f", gb);
    std::string s = buffer;
    if (EndsWith(s, ".0")) s.resize(s.size() - 2);
    return s + " GB";
}

fs::path DefaultDownloadFolder() {
    return PathRoot(InstanceFolders::DataDirectory()) / L"Games" / L"RWND" / L"ParaBellum";
}

std::string DriveText(const fs::path& root) {
    std::string d = Narrow(root.wstring());
    while (!d.empty() && d.back() == '\\') d.pop_back();
    return d;
}

}

Controller::Controller(StartMode mode, bool startInTray, std::wstring previewDirectory)
    : mode_(mode), startInTray_(startInTray), previewDirectory_(std::move(previewDirectory)) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (mode_ == StartMode::Preview) {
        settings::SetEndpoint(L"R6REWIND_API_URL", "http://127.0.0.1:9/api");
        settings::SetEndpoint(L"RWND_UPDATE_URL", "http://127.0.0.1:9/latest.json");
        previewVault_ = fs::path(TempDirectory()) / (L"rwnd-preview-" + Widen(Guid::New().N()));
    }
    if (mode_ != StartMode::Setup) NewSession(mode_ == StartMode::Preview ? 0 : -1);
}

Controller::~Controller() {
    closing_.Cancel();
    StopChat();
    if (!previewVault_.empty()) try {
            DeleteTree(previewVault_);
        } catch (...) {
        }
}

void Controller::NewSession(int previewAccounts, const std::string& previewGamePath) {
    std::lock_guard lock(sessionMutex_);
    if (accounts_) {
        retired_.push_back(std::move(accounts_));
        retiredStores_.push_back(std::move(store_));
        retiredApis_.push_back(std::move(api_));
    }
    api_ = std::make_unique<AccountClient>();
    fs::path vault =
        previewAccounts >= 0 ? previewVault_ / Widen(Guid::New().N()) : fs::path(InstanceFolders::DataDirectory());
    store_ = std::make_unique<CredentialStore>(vault, api_->ApiBaseUrl());
    accounts_ = std::make_unique<AccountManager>(*api_, *store_, nullptr, AccountBook::Limit);
    int limit = AccountBook::Limit;
    if (Installation::IsReleaseBuild()) {
        limit = 1;
    }
    accounts_->SetLimit(limit);
    Instances() = fs::path(InstanceFolders::DataDirectory()) / L"Instances";
    if (previewAccounts >= 0) {
        static const char* ids[] = {"10000000-0000-4000-8000-000000000001", "20000000-0000-4000-8000-000000000002",
                                    "30000000-0000-4000-8000-000000000003"};
        static const char* names[] = {"Player1", "Player2", "Player3"};
        for (int i = 0; i < previewAccounts && i < limit; i++) {
            RememberedAccount a{*Guid::Parse(ids[i]), names[i], std::nullopt, std::nullopt,
                                UtcNowTicks() + 30LL * 86400 * TicksPerSecond};
            if (i != 1) a.refreshToken = "preview";
            accounts_->Upsert(a);
        }
        if (previewAccounts > 0) accounts_->Upsert(*accounts_->Find(*Guid::Parse(ids[0])));
        prefs_ = Preferences();
        prefs_.gamePath = previewGamePath.empty()  ? "C:\\Games\\OldR6\\ParaBellum"
                          : previewGamePath == "-" ? ""
                                                   : previewGamePath;
        InstanceFolders::SetGameFolder(Widen(prefs_.gamePath));
    } else {
        prefs_ = Preferences::Load();
        InstanceFolders::SetGameFolder(Widen(prefs_.gamePath));
        try {
            InstanceFolders::MoveStoreOutOfGame(InstanceFolders::DataDirectory(), Widen(prefs_.gamePath));
        } catch (...) {
        }
        std::set<Guid> running;
        for (auto& [id, process] : GameInstanceGuard::Running(Instances())) running.insert(id);
        std::thread([roots = std::vector<fs::path>{Instances(),
                                                   fs::path(InstanceFolders::DataDirectory()) / L"Instances"}] {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            retention::AtStart(InstanceFolders::DataDirectory(), roots);
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        }).detach();
        auto tokens = accounts_->DropExtras(running);
        if (!tokens.empty()) {
            AccountManager* manager = accounts_.get();
            std::thread([manager, tokens, cancel = closing_.Token()] {
                for (auto& t : tokens) try {
                        manager->Revoke(t, cancel);
                    } catch (...) {
                    }
            }).detach();
        }
    }
}

Preferences Controller::Prefs() {
    std::lock_guard lock(sessionMutex_);
    return prefs_;
}

ChatClient& Controller::Chat(const Guid& id) {
    std::unique_ptr<ChatClient> old;
    std::lock_guard lock(chatMutex_);
    if (chat_ && chatAccount_ == id) return *chat_;
    old = std::move(chat_);
    chatAccount_ = id;
    chat_ = std::make_unique<ChatClient>(
        WinHttp(),
        [this, id](const CancelToken& cancel) { return api_->ChatTicket(accounts_->Session(id, cancel), cancel); },
        [this, id](const std::string& event, const Json& data) {
            Json out = data;
            out[event == "chat.message" ? "account" : "id"] = id.D();
            shell_->Emit(event, out);
        });
    chat_->Start();
    if (old) std::thread([client = std::move(old)]() mutable { client.reset(); }).detach();
    return *chat_;
}

void Controller::StopChat() {
    std::unique_ptr<ChatClient> chat;
    {
        std::lock_guard lock(chatMutex_);
        chat = std::move(chat_);
    }
    chat.reset();
}

fs::path Controller::Instances() const {
    return fs::path(InstanceFolders::StorageDirectory()) / L"Instances";
}

void Controller::SavePrefs(const std::function<void(Preferences&)>& change) {
    std::lock_guard lock(sessionMutex_);
    change(prefs_);
    InstanceFolders::SetGameFolder(Widen(prefs_.gamePath));
    if (mode_ != StartMode::Preview) prefs_.Save();
}

std::map<Guid, InstanceProcess> Controller::Running() {
    if (mode_ == StartMode::Preview) return {};
    std::map<Guid, InstanceProcess> all;
    try {
        all = GameInstanceGuard::Running(Instances());
    } catch (const Error&) {
    }
    fs::path home = fs::path(InstanceFolders::DataDirectory()) / L"Instances";
    if (!IEquals(home.wstring(), Instances().wstring())) try {
            all.merge(GameInstanceGuard::Running(home));
        } catch (const Error&) {
        }
    return all;
}

void Controller::RefreshLogKey(const fs::path& root, const LoginSession& login, const CancelToken& cancel) {
    try {
        HttpResponse response = api_->SignedGet("launcher/log-key", login, cancel, 10000);
        if (!response.IsSuccess()) return;
        auto key = log_vault::ProfileKey::FromJson(ParseJson(response.body));
        if (key && key->playerId == login.playerId) log_vault::SaveKey(root, *key);
    } catch (const Error& e) {
        if (e.kind == ErrorKind::Canceled) throw;
    }
}

namespace {
fs::path OutbreakAnswerFile() {
    return fs::path(InstanceFolders::DataDirectory()) / L"outbreak-live.json";
}
}

bool Controller::OutbreakLive(const LoginSession& login, const CancelToken& cancel) {
    bool answered = false;
    try {
        HttpResponse response = api_->SignedGet("launcher/outbreak", login, cancel, 10000);
        if (response.IsSuccess())
            outbreakLive_ = GetBool(ParseJson(response.body), "live").value_or(false) ? 1 : 0;
        else if (response.status == 404)
            outbreakLive_ = 0;
        if (response.IsSuccess() || response.status == 404) {
            outbreakChecked_ = UtcNowTicks();
            answered = true;
        }
    } catch (const Error& e) {
        if (e.kind == ErrorKind::Canceled) throw;
    }
    if (answered)
        play_content::StoreLive(OutbreakAnswerFile(), login.playerId.N(), outbreakLive_.load() == 1);
    else if (outbreakLive_.load() == -1)
        if (auto stored = play_content::StoredLive(OutbreakAnswerFile(), login.playerId.N()))
            outbreakLive_ = *stored ? 1 : 0;
    return outbreakLive_.load() == 1;
}

play_content::LiveAnswer Controller::OutbreakAnswer(const LoginSession& login, const CancelToken& cancel) {
    auto ask = [&]() -> play_content::LiveReply {
        try {
            HttpResponse response = api_->SignedGet("launcher/outbreak", login, cancel, 10000);
            if (response.IsSuccess()) return {GetBool(ParseJson(response.body), "live").value_or(false), "200", false};
            if (response.status == 404) return {false, "404", false};
            bool retry = response.status >= 500 || response.status == 408 || response.status == 429;
            return {std::nullopt, std::to_string(response.status), retry};
        } catch (const Error& e) {
            if (e.kind == ErrorKind::Canceled || e.kind == ErrorKind::LoginRequired) throw;
            return {std::nullopt, e.kind == ErrorKind::Timeout ? "timeout" : "error", true};
        }
    };
    auto answer = play_content::ResolveLive(ask, OutbreakAnswerFile(), login.playerId.N(), outbreak::Ready(),
                                            [&cancel](int ms) { return cancel.Sleep(ms); });
    cancel.ThrowIfCanceled();
    outbreakLive_ = answer.on ? 1 : 0;
    if (answer.source == play_content::Source::Live) outbreakChecked_ = UtcNowTicks();
    return answer;
}

bool Controller::OutbreakLiveSignedOut(const CancelToken& cancel) {
    try {
        HttpRequest request;
        request.url = ResolveUrl(api_->ApiBaseUrl(), "launcher/outbreak");
        request.timeoutMs = 10000;
        HttpResponse response = WinHttp()->Send(request, cancel);
        return response.IsSuccess() && GetBool(ParseJson(response.body), "live").value_or(false);
    } catch (const Error& e) {
        if (e.kind == ErrorKind::Canceled) throw;
        return false;
    }
}

void Controller::FirstRunOutbreak(SteamDownload& game, const std::optional<std::string>& user,
                                  const std::optional<std::string>& password, bool remember, const fs::path& target,
                                  int64_t gameStillNeeded, const CancelToken& cancel) {
    InstanceFolders::SetGameFolder(target.wstring());
    struct Restore {
        Controller* c;
        ~Restore() { InstanceFolders::SetGameFolder(Widen(c->Prefs().gamePath)); }
    } restore{this};
    try {
        if (outbreak::Ready()) return;
        std::optional<LoginSession> login;
        if (auto id = accounts_->Selected(); id && accounts_->CanSignInSilently(*id)) try {
                login = accounts_->Session(*id, cancel);
            } catch (const Error& e) {
                if (e.kind == ErrorKind::Canceled) throw;
            }
        bool live = login ? OutbreakLive(*login, cancel) : OutbreakLiveSignedOut(cancel);
        ULARGE_INTEGER spare{};
        GetDiskFreeSpaceExW(PathRoot(target).c_str(), &spare, nullptr, nullptr);
        if (!first_run::FetchOutbreak(live, false, static_cast<int64_t>(spare.QuadPart), gameStillNeeded,
                                      outbreak::InstallBytes - outbreak::StoreBytes()))
            return;
        if (outbreakInstalling_.exchange(true)) return;
        struct Done {
            Controller* c;
            ~Done() { c->outbreakInstalling_ = false; }
        } done{this};
        auto forward = [this](const std::string& stage, std::optional<double> fraction, const std::string& detail) {
            shell_->Emit("steam.progress", {{"stage", stage},
                                            {"part", 0},
                                            {"parts", 0},
                                            {"job", "outbreak"},
                                            {"fraction", fraction ? Json(*fraction) : Json(nullptr)},
                                            {"detail", detail.empty() ? Json(nullptr) : Json(detail)}});
        };
        SteamDownload more;
        more.onPrompt = game.onPrompt;
        more.onQrCode = game.onQrCode;
        more.onProgress = [forward](const SteamProgress& s) {
            forward(s.stage == "Downloading" ? "Downloading Outbreak" : s.stage, s.fraction, s.detail.value_or(""));
        };
        {
            std::lock_guard lock(steamMutex_);
            download_ = &more;
        }
        struct Back {
            Controller* c;
            SteamDownload* d;
            ~Back() {
                std::lock_guard lock(c->steamMutex_);
                c->download_ = d;
            }
        } back{this, &game};
        bool invited = false;
        if (login) try {
                invited = !api_->NeedsInvite(*login, cancel);
            } catch (const Error& e) {
                if (e.kind == ErrorKind::Canceled) throw;
            }
        if (invited) {
            AccountManager* manager = accounts_.get();
            Guid id = login->playerId;
            outbreak::Remote remote{api_->ApiBaseUrl(),
                                    [manager, id](const CancelToken& c) { return manager->Session(id, c).token; }};
            outbreak::Install(&more, user, password, remember, remote, forward, cancel);
        } else {
            outbreak::DownloadSteamFiles(more, user, password, remember, forward, cancel);
        }
    } catch (const Error& e) {
        if (e.kind == ErrorKind::Canceled) throw;
    }
}

void Controller::WireSteam(SteamDownload& download) {
    download.onPrompt = [this](const SteamPrompt& prompt) {
        static const char* kinds[] = {"password", "appCode", "emailCode", "mobileApproval"};
        shell_->Emit("steam.prompt", {{"kind", kinds[static_cast<int>(prompt.kind)]},
                                      {"email", prompt.email ? Json(*prompt.email) : Json(nullptr)},
                                      {"retry", prompt.retry}});
    };
    download.onProgress = [this](const SteamProgress& progress) {
        shell_->Emit("steam.progress", {{"stage", progress.stage},
                                        {"part", progress.part},
                                        {"parts", progress.parts},
                                        {"fraction", progress.fraction ? Json(*progress.fraction) : Json(nullptr)},
                                        {"detail", progress.detail ? Json(*progress.detail) : Json(nullptr)}});
    };
    download.onQrCode = [this](const std::vector<std::vector<bool>>& modules) {
        Json rows = Json::array();
        for (auto& row : modules) {
            std::string line;
            for (bool dark : row) line += dark ? '1' : '0';
            rows.push_back(line);
        }
        shell_->Emit("steam.qr", {{"rows", rows}});
    };
}

SteamJob Controller::GameJob(first_run::Size size) {
    SteamJob job = SteamJob::ParaBellum();
    if (size != first_run::Size::Full) {
        fs::path list = fs::path(InstanceFolders::DataDirectory()) /
                        (L"steam-" + Widen(first_run::SizeName(size)) + L"-filelist.txt");
        CreateDirectories(list.parent_path());
        WriteAllText(list, first_run::FileList(size));
        job.fileList = list;
    }
    return job;
}

void Controller::RememberDownload(const fs::path& folder, const std::string& kind, const std::string& text,
                                  int status) {
    int64_t now = UtcNowTicks();
    try {
        SavePrefs([&](Preferences& prefs) {
            prefs.downloadStop = DownloadStop{Narrow(folder.wstring()), kind, text, download_log::UtcText(now)};
        });
    } catch (const Error&) {
    }
    const char* what = kind == "running"   ? "start"
                       : kind == "stopped" ? "stopped by you"
                       : kind == "done"    ? "done"
                                           : "error";
    std::string logged = status ? text + " (code " + std::to_string(status) + ")" : text;
    if (mode_ != StartMode::Preview)
        download_log::Append(download_log::File(), download_log::Line(now, what, folder, logged));
}

bool Controller::SteamDownloadRunning() {
    std::lock_guard lock(steamMutex_);
    return download_ != nullptr;
}

void Controller::RefreshOutbreakLater() {
    if (mode_ != StartMode::Launcher || UtcNowTicks() - outbreakChecked_.load() < 300 * TicksPerSecond) return;
    auto id = accounts_->Selected();
    if (!id || !accounts_->CanSignInSilently(*id) || outbreakRefreshing_.exchange(true)) return;
    std::thread([this, id = *id] {
        try {
            OutbreakLive(accounts_->Session(id, closing_.Token()), closing_.Token());
        } catch (...) {
        }
        outbreakChecked_ = std::max(outbreakChecked_.load(), UtcNowTicks() - 240 * TicksPerSecond);
        outbreakRefreshing_ = false;
    }).detach();
}

Json Controller::OutbreakJson() {
    if (mode_ == StartMode::Preview)
        return previewOutbreak_.is_object()
                   ? previewOutbreak_
                   : Json{
                         {"live", false}, {"ready", false}, {"update", false}, {"steam", false}, {"installing", false}};
    RefreshOutbreakLater();
    Json status = outbreak::Status();
    status["live"] = outbreakLive_.load() == 1;
    status["installing"] = outbreakInstalling_.load();
    return status;
}

Json Controller::AccountJson(const RememberedAccount& a, const std::map<Guid, InstanceProcess>& running) {
    auto game = running.find(a.playerId);
    return {{"playerId", a.playerId.D()},
            {"displayName", a.displayName},
            {"avatarUrl", a.avatarUrl ? Json(*a.avatarUrl) : Json(nullptr)},
            {"hasRefresh", a.refreshToken.has_value()},
            {"canSignInSilently", accounts_->CanSignInSilently(a.playerId)},
            {"inGame", game != running.end()},
            {"gameStartTicks", game != running.end() ? Json(game->second.startUtcTicks) : Json(nullptr)}};
}

Json Controller::Snapshot() {
    std::lock_guard lock(sessionMutex_);
    auto running = Running();
    Json accounts = Json::array();
    for (auto& a : accounts_->Entries()) accounts.push_back(AccountJson(a, running));
    auto selected = accounts_->Selected();
    return {{"accounts", accounts},
            {"selected", selected ? Json(selected->D()) : Json(nullptr)},
            {"runningCount", running.size()},
            {"busy", busy_.load()},
            {"wasCorrupt", accounts_->WasCorrupt()},
            {"needsGameSetup", prefs_.NeedsGameSetup()},
            {"accountLimit", accounts_->Limit()},
            {"prefs",
             {{"gamePath", prefs_.gamePath},
              {"friendNotifications", prefs_.friendNotifications},
              {"messageNotifications", prefs_.messageNotifications},
              {"keepInTray", prefs_.keepInTray},
              {"updateNotifications", prefs_.updateNotifications},
              {"steamDownloadFolder", prefs_.steamDownloadFolder ? Json(*prefs_.steamDownloadFolder) : Json(nullptr)},
              {"downloadSize", prefs_.downloadSize},
              {"mouseMultiplier", prefs_.mouseMultiplier ? Json(*prefs_.mouseMultiplier) : Json(nullptr)},
              {"fpsLimit", prefs_.fpsLimit ? Json(*prefs_.fpsLimit) : Json(nullptr)}}},
            {"instances", Narrow(Instances().wstring())},
            {"outbreak", OutbreakJson()},
            {"nowTicks", UtcNowTicks()}};
}

Guid Controller::RequireId(const Json& params) {
    auto id = GetGuid(params, "id");
    if (!id) Throw(ErrorKind::Argument, "No account was given.");
    return *id;
}

void Controller::Toast(const std::string& text) {
    shell_->Emit("toast", {{"text", text}});
}

void Controller::Notify(const std::string& title, const std::string& text) {
    PostToUi([this, title, text] {
        if (shell_->IsVisible() && shell_->IsActive())
            Toast(text);
        else if (tray_)
            tray_->Notify(Widen(title), Widen(text));
    });
}

bool Controller::CloseRequested() {
    if (quitting_ || mode_ != StartMode::Launcher || !tray_) return true;
    Preferences p = Prefs();
    if (!p.keepInTray) {
        if (busy_) {
            Toast("Wait for the current sign-in or launch to finish before closing.");
            return false;
        }
        quitting_ = true;
        return true;
    }
    shell_->HideToTray();
    if (!p.trayHintShown) {
        tray_->Notify(L"RWND Parabellum is still running",
                      L"It stays in the tray so friend requests reach you. Right-click the icon to quit.");
        SavePrefs([](Preferences& prefs) { prefs.trayHintShown = true; });
    }
    return false;
}

void Controller::Quit() {
    quitting_ = true;
    closing_.Cancel();
    shell_->Destroy();
}

int Controller::Run() {
    shell_ = std::make_unique<Shell>(mode_, startInTray_);
    if (mode_ == StartMode::Launcher)
        shell_->MayCall = [this](const std::string& method) {
            static const std::set<std::string> beforeTerms{
                "app.info",        "legal.state",           "legal.accept", "shell.openWebsite",
                "window.minimize", "window.toggleMaximize", "window.close", "window.startResize",
                "shell.exit",      "shell.showFromTray"};
            return beforeTerms.contains(method) || Prefs().legalAccepted == LegalVersion;
        };
    shell_->closing = closing_.Token();
    shell_->OnCloseRequested = [this] { return CloseRequested(); };
    shell_->OnDestroyed = [this] {
        closing_.Cancel();
        tray_.reset();
    };
    if (mode_ == StartMode::Launcher) {
        tray_ = std::make_unique<Tray>();
        tray_->onOpen = [this] { shell_->ShowFromTray(); };
        tray_->onPlay = [this] {
            shell_->ShowFromTray();
            shell_->Emit("tray.play");
        };
        tray_->onSignOut = [this] {
            shell_->ShowFromTray();
            shell_->Emit("tray.signOut");
        };
        tray_->onNotificationClicked = [this] {
            shell_->ShowFromTray();
            shell_->Emit("tray.friends");
        };
        tray_->onQuit = [this] {
            if (busy_) {
                shell_->ShowFromTray();
                Toast("Wait for the current sign-in or launch to finish before quitting.");
                return;
            }
            Quit();
        };
    }
    Register();
    shell_->Create();
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    closing_.Cancel();
    return 0;
}

void Controller::Register() {
    Shell& s = *shell_;

    s.On("app.info", [this](const Json&, const CancelToken&) -> Json {
        auto registered = Installation::RegisteredDirectory();
        Json media = Json::array();
        static const std::vector<std::wstring> extensions = {L".mp4", L".m4v", L".wmv", L".mov"};
        auto addFolder = [&](const fs::path& folder, const std::string& host) {
            if (!DirExists(folder)) return;
            for (auto& f : ListFiles(folder))
                if (std::find(extensions.begin(), extensions.end(), ToLowerW(f.extension().wstring())) !=
                    extensions.end())
                    media.push_back("https://" + host + "/" + EscapeDataString(Narrow(f.filename().wstring())));
        };
        for (auto& name : payload::Names("Media")) {
            std::wstring file = Widen(name.substr(6));
            if (std::find(extensions.begin(), extensions.end(), ToLowerW(fs::path(file).extension().wstring())) !=
                extensions.end())
                media.push_back("https://" + Narrow(MediaHost) + "/" + EscapeDataString(name.substr(6)));
        }
        addFolder(fs::path(InstanceFolders::DataDirectory()) / L"Media", Narrow(UserMediaHost));
        for (auto& url : settings::Strings("loginVideos"))
            if (auto u = Url::Parse(url); u && u->IsHttps()) media.push_back(url);
        auto site = settings::String("websiteUrl");
        bool installed = Installation::IsReleaseBuild() && Installation::IsInstalledHere();
        return {{"mode", mode_ == StartMode::Setup     ? "setup"
                         : mode_ == StartMode::Preview ? "preview"
                                                       : "launcher"},
                {"version", DisplayVersion()},
                {"releaseBuild", Installation::IsReleaseBuild()},
                {"installed", installed},
                {"installDirectory", Narrow(ExeDirectory().wstring())},
                {"defaultInstallDirectory", Narrow(Installation::DefaultDirectory().wstring())},
                {"registeredDirectory", registered ? Json(Narrow(registered->wstring())) : Json(nullptr)},
                {"defaultDownloadFolder", Narrow(DefaultDownloadFolder().wstring())},
                {"media", media},
                {"maximized", IsZoomed(shell_->Hwnd()) != FALSE},
                {"websiteUrl", site ? Json(*site) : Json(nullptr)}};
    });
    s.On(
        "window.minimize",
        [this](const Json&, const CancelToken&) -> Json {
            ShowWindow(shell_->Hwnd(), SW_MINIMIZE);
            return nullptr;
        },
        true);
    s.On(
        "window.toggleMaximize",
        [this](const Json&, const CancelToken&) -> Json {
            ShowWindow(shell_->Hwnd(), IsZoomed(shell_->Hwnd()) ? SW_RESTORE : SW_MAXIMIZE);
            return nullptr;
        },
        true);
    s.On(
        "window.close",
        [this](const Json&, const CancelToken&) -> Json {
            shell_->Close();
            return nullptr;
        },
        true);
    s.On(
        "window.startResize",
        [this](const Json& p, const CancelToken&) -> Json {
            static const std::map<std::string, WPARAM> edges = {
                {"l", HTLEFT},     {"r", HTRIGHT},     {"t", HTTOP},         {"b", HTBOTTOM},
                {"tl", HTTOPLEFT}, {"tr", HTTOPRIGHT}, {"bl", HTBOTTOMLEFT}, {"br", HTBOTTOMRIGHT}};
            auto edge = edges.find(Str(p, "edge").value_or(""));
            if (edge != edges.end() && !IsZoomed(shell_->Hwnd())) {
                ReleaseCapture();
                SendMessageW(shell_->Hwnd(), WM_NCLBUTTONDOWN, edge->second, 0);
            }
            return nullptr;
        },
        true);

    s.On("crash.pending", [this](const Json&, const CancelToken&) -> Json {
        Json reports = Json::array();
        if (mode_ != StartMode::Launcher) return reports;
        std::lock_guard lock(crashMutex_);
        for (const auto& report : crash_reports::PendingReports(CrashQueueDirectory()))
            if (!resolvingCrashReports_.contains(report.id)) reports.push_back(report.ForUi());
        for (const auto& [id, report] : unsavedCrashReports_)
            if (!resolvingCrashReports_.contains(id)) reports.push_back(report.ForUi());
        return reports;
    });
    s.On("crash.resolve", [this](const Json& params, const CancelToken& cancel) -> Json {
        Require(mode_ == StartMode::Launcher, "The crash reporter is unavailable here.");
        return ResolveCrash(params, cancel);
    });
    s.On("shell.setBusy", [this](const Json& p, const CancelToken&) -> Json {
        busy_ = Bool(p, "busy");
        return nullptr;
    });
    s.On("shell.notify", [this](const Json& p, const CancelToken&) -> Json {
        Notify(Str(p, "title").value_or(""), Str(p, "text").value_or(""));
        return nullptr;
    });
    s.On(
        "shell.trayNotify",
        [this](const Json& p, const CancelToken&) -> Json {
            if (tray_) tray_->Notify(Widen(Str(p, "title").value_or("")), Widen(Str(p, "text").value_or("")));
            return nullptr;
        },
        true);
    s.On(
        "shell.state",
        [this](const Json&, const CancelToken&) -> Json {
            return {{"visible", shell_->IsVisible()}, {"active", shell_->IsActive()}};
        },
        true);
    s.On(
        "shell.updateTray",
        [this](const Json& p, const CancelToken&) -> Json {
            if (!tray_) return nullptr;
            auto account = Str(p, "account");
            std::wstring name = account ? Widen(*account) : L"";
            tray_->Update(account ? &name : nullptr, Widen(Str(p, "playLabel").value_or("")), Bool(p, "canPlay"));
            return nullptr;
        },
        true);
    s.On("shell.openWebsite", [](const Json& p, const CancelToken&) -> Json {
        auto site = Url::Parse(settings::String("websiteUrl").value_or(""));
        if (site && site->IsHttps())
            process::Open(Widen(ResolveUrl(site->Origin() + site->pathAndQuery, Str(p, "path").value_or(""))));
        return nullptr;
    });
    s.On("legal.state",
         [this](const Json&, const CancelToken&) -> Json { return Json{{"accepted", Prefs().legalAccepted}}; });
    s.On("legal.accept", [this](const Json& p, const CancelToken&) -> Json {
        const std::string version = Str(p, "version").value_or("");
        if (version != LegalVersion) Throw(ErrorKind::InvalidData, "Invalid version");
        SavePrefs([&](Preferences& prefs) { prefs.legalAccepted = version; });
        return nullptr;
    });
    s.On("shell.openFolder", [this](const Json& p, const CancelToken&) -> Json {
        std::string which = Str(p, "which").value_or("");
        if (which == "game") {
            fs::path game = FullPath(TrimW(Widen(Prefs().gamePath)));
            if (Prefs().gamePath.empty() || !DirExists(game))
                Throw(ErrorKind::InvalidOperation, "The game folder isn't there. Choose it again.");
            process::Start(process::SystemTool(L"..\\explorer.exe"), {game.wstring()}, {});
            return nullptr;
        }
        fs::path folder = which == "instances" ? Instances() : ExeDirectory();
        CreateDirectories(folder);
        process::Start(process::SystemTool(L"..\\explorer.exe"), {folder.wstring()}, {});
        return nullptr;
    });
    s.On("shell.openAppControl", [](const Json&, const CancelToken&) -> Json {
        process::Open(app_control::SettingsPage);
        return nullptr;
    });
    s.On("shell.openNotice", [](const Json&, const CancelToken&) -> Json {
        fs::path notice = ExeDirectory() / L"DepotDownloader-NOTICE.txt";
        process::Open(FileExists(notice) ? notice.wstring() : Widen(SteamDownload::SourceUrl));
        return nullptr;
    });
    s.On(
        "shell.browseFolder",
        [this](const Json& p, const CancelToken&) -> Json {
            std::wstring initial = TrimW(Widen(Str(p, "initial").value_or("")));
            if (!DirExists(initial) && Bool(p, "parentFallback")) {
                fs::path parent = fs::path(initial).parent_path();
                if (!parent.empty() && DirExists(parent)) initial = parent.wstring();
            }
            std::wstring chosen = BrowseFolder(shell_->Hwnd(), Widen(Str(p, "title").value_or("")), initial);
            return chosen.empty() ? Json(nullptr) : Json(Narrow(chosen));
        },
        true);
    s.On(
        "shell.exit",
        [this](const Json&, const CancelToken&) -> Json {
            Quit();
            return nullptr;
        },
        true);
    s.On(
        "shell.showFromTray",
        [this](const Json&, const CancelToken&) -> Json {
            shell_->ShowFromTray();
            return nullptr;
        },
        true);

    s.On("setup.resolveTarget", [](const Json& p, const CancelToken&) -> Json {
        try {
            return {{"target", Narrow(Installation::ResolveTarget(Widen(Str(p, "folder").value_or(""))).wstring())}};
        } catch (const Error&) {
            return {{"target", nullptr}};
        }
    });
    s.On("setup.install", [this](const Json& p, const CancelToken&) -> Json {
        fs::path exe = Installation::Install(
            Widen(Str(p, "folder").value_or("")), Bool(p, "desktop"), Bool(p, "startMenu"),
            [this](const std::string& stage) { shell_->Emit("setup.progress", {{"text", stage + "..."}}); });
        shell_->Emit("setup.progress", {{"text", "Done. Starting RWND Parabellum..."}});
        process::Start(exe, {L"--after", std::to_wstring(GetCurrentProcessId())}, exe.parent_path());
        PostToUi([this] { Quit(); });
        return nullptr;
    });
    s.On("setup.openInstalled", [this](const Json&, const CancelToken&) -> Json {
        if (auto existing = Installation::RegisteredDirectory())
            process::Start(Installation::ExeIn(*existing), {}, *existing);
        PostToUi([this] { Quit(); });
        return nullptr;
    });

    s.On("session.snapshot", [this](const Json&, const CancelToken&) -> Json { return Snapshot(); });
    s.On("accounts.select", [this](const Json& p, const CancelToken&) -> Json {
        std::lock_guard lock(sessionMutex_);
        accounts_->Select(RequireId(p));
        return Snapshot();
    });
    s.On("accounts.signIn", [this](const Json& p, const CancelToken& cancel) -> Json {
        auto expected = GetGuid(p, "expected");
        SignInProvider provider = SignInProvider::Discord;
        if (const Json* name = Field(p, "provider"); name && !name->is_null()) {
            if (*name == "steam")
                provider = SignInProvider::Steam;
            else if (*name != "discord")
                Throw(ErrorKind::Argument, "Unknown sign-in provider.");
        }
        bool remembered = accounts_->Add(expected, cancel, provider);
        return {{"remembered", remembered}};
    });
    s.On("accounts.providers", [this](const Json&, const CancelToken& cancel) -> Json {
        if (mode_ == StartMode::Preview) return {{"steam", previewSteam_.load()}};
        return {{"steam", api_->Providers(cancel).steam}};
    });
    s.On("accounts.signOut", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        if (GameInstanceGuard::Running(Instances()).contains(id))
            Throw(ErrorKind::InvalidOperation, "Close this account's game before signing out.");
        return {{"revoked", accounts_->Remove(id, cancel)}};
    });
    s.On("accounts.profile", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        LoginSession login = accounts_->Session(id, cancel);
        PlayerProfile profile = api_->Me(login, cancel);
        if (profile.id == id && !busy_) accounts_->UpdateIdentity(profile);
        if (profile.id == id && accounts_->Selected() == id) OutbreakLive(login, cancel);
        return profile.ToJson();
    });
    s.On("news.announcements",
         [this](const Json&, const CancelToken& cancel) -> Json { return api_->Announcements(cancel); });
    s.On("polls.pending", [this](const Json& p, const CancelToken& cancel) -> Json {
        auto id = GetGuid(p, "id");
        if (!id || !accounts_->CanSignInSilently(*id)) return nullptr;
        try {
            return api_->PendingPoll(accounts_->Session(*id, cancel), cancel);
        } catch (const Error&) {
            return nullptr;
        }
    });
    s.On("social.load", [this](const Json& p, const CancelToken& cancel) -> Json {
        Json presence = nullptr;
        std::thread presenceThread([&] {
            try {
                presence = api_->Presence(cancel);
            } catch (const Error&) {
            }
        });
        Json friends = nullptr, note = nullptr;
        bool canceled = false;
        auto id = GetGuid(p, "id");
        if (id && accounts_->CanSignInSilently(*id)) {
            try {
                friends = api_->Friends(accounts_->Session(*id, cancel), cancel);
            } catch (const Error& e) {
                if (e.kind == ErrorKind::LoginRequired)
                    note = "Sign in to see your friends.";
                else if (e.kind == ErrorKind::Canceled)
                    canceled = true;
                else
                    note = "Friends are unavailable right now.";
            }
        } else
            note = "Sign in to see your friends.";
        presenceThread.join();
        return {{"friends", friends}, {"note", note}, {"presence", presence}, {"canceled", canceled}};
    });
    s.On("social.search", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        if (!accounts_->CanSignInSilently(id)) ThrowLoginRequired();
        return api_->SearchPlayers(accounts_->Session(id, cancel), Str(p, "query").value_or(""), cancel);
    });
    s.On("social.action", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        api_->FriendAction(accounts_->Session(id, cancel), Str(p, "action").value_or(""),
                           Str(p, "username").value_or(""), cancel);
        if (mode_ != StartMode::Preview) Chat(id).RefreshSoon();
        return nullptr;
    });

    s.On("servers.get", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        if (mode_ == StartMode::Preview) {
            std::lock_guard lock(sessionMutex_);
            if (!previewServers_.is_object()) return {{"available", false}};
            Json out = previewServers_;
            out["available"] = true;
            return out;
        }
        if (!accounts_->CanSignInSilently(id)) return {{"available", false}};
        try {
            auto choice = api_->Servers(accounts_->Session(id, cancel), cancel);
            if (!choice) return {{"available", false}};
            (*choice)["available"] = true;
            return *choice;
        } catch (const Error& e) {
            if (e.kind == ErrorKind::Canceled) throw;
            return {{"available", false}};
        }
    });
    s.On("servers.set", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        std::vector<std::string> allowed;
        if (const Json* list = Field(p, "allowed"); list && list->is_array())
            for (const auto& a : *list)
                if (a.is_string() && allowed.size() < 8) allowed.push_back(a.get<std::string>());
        if (allowed.empty()) Throw(ErrorKind::Argument, "Pick at least one server.");
        if (mode_ == StartMode::Preview) {
            std::lock_guard lock(sessionMutex_);
            Json sample = previewServers_.is_object() ? previewServers_ : Json::object();
            Json list = Json::array();
            for (const auto& a : allowed) list.push_back(a);
            sample["allowed"] = list;
            previewServers_ = AccountClient::ServerChoice(sample);
            Json out = previewServers_;
            out["available"] = true;
            return out;
        }
        Json out = api_->SetServers(accounts_->Session(id, cancel), allowed, cancel);
        out["available"] = true;
        return out;
    });

    s.On("chat.state", [this](const Json& p, const CancelToken&) -> Json {
        Guid id = RequireId(p);
        if (mode_ == StartMode::Preview)
            return {{"id", id.D()}, {"available", false}, {"connected", false}, {"friends", Json::array()}};
        Json state = Chat(id).State();
        state["id"] = id.D();
        return state;
    });
    s.On("chat.history", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        auto before = GetInt(p, "before");
        return Chat(id).History(Str(p, "with").value_or(""), before && *before > 0 ? before : std::nullopt, cancel);
    });
    s.On("chat.send", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        std::string text = Str(p, "text").value_or("");
        if (text.size() > ChatClient::MaxText * 4) Throw(ErrorKind::Argument, "That message is too long.");
        return Chat(id).Send(Str(p, "to").value_or(""), text, cancel);
    });
    s.On("chat.read", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        Chat(id).Read(Str(p, "with").value_or(""), GetInt(p, "upTo").value_or(0), cancel);
        return nullptr;
    });
    s.On("chat.notify", [this](const Json& p, const CancelToken&) -> Json {
        if (Prefs().messageNotifications) Notify(Str(p, "title").value_or(""), Str(p, "text").value_or(""));
        return nullptr;
    });

    s.On("game.play", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        auto step = [this](const std::string& title, const std::string& detail) {
            shell_->Emit("play.step", {{"title", title}, {"detail", detail}});
        };
        auto account = accounts_->Find(id);
        if (!account) Throw(ErrorKind::InvalidOperation, "Unknown account.");
        try {
            step("Checking your account", "Confirming " + account->displayName + " with RWND...");
            fs::path source = FullPath(Trim(Prefs().gamePath));
            int port = GameInstanceGuard::CheckAndChoosePort(Instances(), id, source);
            LoginSession login = accounts_->Session(id, cancel);
            PlayerProfile profile = api_->Me(login, cancel);
            if (profile.id != id)
                Throw(ErrorKind::InvalidData, "The account profile does not match the selected account.");
            shell_->Emit("play.profile", profile.ToJson());
            play_content::LiveAnswer outbreakAnswer = OutbreakAnswer(login, cancel);
            bool outbreakOn = outbreakAnswer.on;
            bool outbreakPending = false;
            if (outbreakOn && (outbreakInstalling_ || !outbreak::Ready())) {
                if (api_->NeedsInvite(login, cancel))
                    Throw(ErrorKind::InvalidOperation, AccountClient::InviteOnlyMessage);
                outbreakOn = false;
                outbreakPending = true;
            }
            AccountManager* manager = accounts_.get();
            PatchFiles::Configure(api_->ApiBaseUrl(),
                                  [manager, id](const CancelToken& c) { return manager->Session(id, c).token; });
            game_dlls::RemoveDownloads(InstanceFolders::DataDirectory());
            fs::path root;
            bool prepared = false;
            std::vector<std::wstring> arguments;
            std::shared_lock playing(retention::PlayLock());
            std::vector<std::pair<std::wstring, std::wstring>> environment;
            if (!prepared) {
                step("Preparing game folder",
                     "Linking " + profile.displayName + "'s copy. The first launch can take a minute.");
                root = InstanceFolders::Prepare(source, Instances(), id);
            }
            try {
                first_run::LinkIntoCopy(root, source, first_run::ParseSize(Prefs().downloadSize));
            } catch (const Error& e) {
                if (e.kind == ErrorKind::Canceled) throw;
            }
            RefreshLogKey(root, login, cancel);
            log_vault::Seal(root, "leftover");
            HANDLE gate = CreateFileW(LongPath(root / L".RWND-Parabellum-launch.lock").c_str(),
                                      GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
            if (gate == INVALID_HANDLE_VALUE) ThrowLastError("Another launch is using this account's game copy");
            struct Gate {
                HANDLE h;
                ~Gate() { CloseHandle(h); }
            } gateHolder{gate};
            retention::CleanCopy(root, crash_reports::NeededBundles(CrashQueueDirectory()));
            if (!prepared) {
                step("Installing startup files", "Checking verified RWND components.");
                play_content::ContentLog content;
                content.Line("play launcher=" + DisplayVersion() + " outbreak=" +
                             (outbreakOn        ? "yes"
                              : outbreakPending ? "pending"
                                                : "no") +
                             " source=" + play_content::SourceName(outbreakAnswer.source) +
                             " checks=" + std::to_string(outbreakAnswer.attempts) +
                             (outbreakAnswer.last.empty() ? "" : " last=" + outbreakAnswer.last));
                content.Line(play_content::SaveFolderLine(root));
                play_content::Outcome outcome;
                try {
                    outcome =
                        play_content::Prepare(outbreakOn, play_content::RealSteps(root, source, step, cancel), content);
                } catch (const Error& e) {
                    content.Line("launch=failed step error " + play_content::ErrorText(e));
                    content.Write(root);
                    throw;
                }
                content.Write(root);
                if (!outcome.launch) {
                    log_vault::Seal(root, "content-refused");
                    Throw(ErrorKind::InvalidOperation, play_content::RepairMessage);
                }
            }
            auto refuseIfBlocked = [&](const char* when, std::optional<uint32_t> exitCode = std::nullopt) {
                auto file = app_control::Refused(root);
                if (!file && exitCode != app_control::PolicyViolation) return;
                auto state = app_control::SmartAppControlState();
                play_content::ContentLog blocked;
                blocked.Line(std::string("launch=blocked app-control ") + when + " file=" + file.value_or("unknown") +
                             " sac=" + (state ? std::to_string(*state) : "none"));
                blocked.Write(root);
                log_vault::Seal(root, "launch-blocked", exitCode);
                Throw(ErrorKind::InvalidOperation, app_control::Message(state));
            };
            refuseIfBlocked("before-start");
            {
                play_content::ContentLog saveLog;
                saveLog.Line(save_recovery::BeforeRun(root, save_recovery::Clock::now()));
                saveLog.Write(root);
            }
            {
                Preferences prefs = Prefs();
                first_run::Size size = first_run::ParseSize(prefs.downloadSize);
                if (size != first_run::Size::Full || prefs.mouseMultiplier || prefs.fpsLimit) {
                    try {
                        fs::path settingsIni = KnownFolder(&FOLDERID_Documents) / L"My Games" / L"Rainbow Six - Siege" /
                                               Widen(id.D()) / L"GameSettings.ini";
                        std::string before = FileExists(settingsIni) ? ReadAllText(settingsIni) : std::string();
                        std::string after = first_run::CapTexturePreset(before, size);
                        if (prefs.mouseMultiplier)
                            after = first_run::SetIniValue(after, "INPUT", "MouseSensitivityMultiplierUnit",
                                                           first_run::FormatMouseMultiplier(*prefs.mouseMultiplier));
                        if (prefs.fpsLimit)
                            after =
                                first_run::SetIniValue(after, "DISPLAY", "FPSLimit", std::to_string(*prefs.fpsLimit));
                        if (after != before) {
                            CreateDirectories(settingsIni.parent_path());
                            WriteAllText(settingsIni, after);
                        }
                    } catch (const Error& e) {
                        if (e.kind == ErrorKind::Canceled) throw;
                    }
                }
            }
            step("Starting Parabellum", "Waiting for the game to connect to RWND...");
            LaunchTicket ticket = api_->Ticket(login, cancel);
            StartedGame game;
            try {
                game = game_launcher::Start(root, profile, ticket.ticket, api_->ApiBaseUrl(), port, cancel, arguments,
                                            environment);
            } catch (const game_launcher::ExitedBeforeConnection& e) {
                refuseIfBlocked("exit", e.code);
                if (e.code >= 0x0AC00001u && e.code <= 0x0AC00003u) {
                    log_vault::Seal(root, "anticheat-missing", e.code);
                    Throw(ErrorKind::InvalidOperation,
                          "RWND protection is missing or damaged. Restart RWND and press Play again.");
                }
                if (crash_reports::UnexpectedExit(e.code, FileExists(root / log_vault::DumpName)))
                    QueueGameCrash(id, profile.displayName, root, e.code);
                else
                    log_vault::Seal(root, "launch-failed", e.code);
                throw;
            } catch (...) {
                log_vault::Seal(root, "launch-failed");
                throw;
            }
            playing.unlock();
            std::thread([this, game, id, name = profile.displayName, root] {
                Observe(game.process, id, name, game.sessionId, root);
            }).detach();
            return {{"displayName", profile.displayName}};
        } catch (const Error& e) {
            if (IsDiskFull(e)) Throw(e.kind, play_content::DiskFullMessage, e.status);
            if (e.kind == ErrorKind::Http && (e.status == 401 || e.status == 403)) {
                accounts_->Invalidate(id);
                ThrowLoginRequired();
            }
            throw;
        }
    });
#ifndef RWND_RELEASE
    s.On("dev.prepare", [this](const Json& p, const CancelToken& cancel) -> Json {
        Guid id = RequireId(p);
        fs::path source = FullPath(Trim(Prefs().gamePath));
        accounts_->Session(id, cancel);
        AccountManager* manager = accounts_.get();
        PatchFiles::Configure(api_->ApiBaseUrl(),
                              [manager, id](const CancelToken& c) { return manager->Session(id, c).token; });
        int64_t started = UtcNowTicks();
        fs::path root = InstanceFolders::Prepare(source, Instances(), id);
        port_installer::Install(root);
        Json hashes = Json::object();
        for (auto name :
             {L"rewind.dll", L"defaultargs.dll", L"uplay_r1_loader64.dll", L"steam_api64.dll", L"rwnd_overlay.dll", L"rwnd_ac.dll"})
            if (FileExists(root / name)) hashes[Narrow(name)] = Sha256File(root / name);
        return {{"root", Narrow(root.wstring())},
                {"seconds", (UtcNowTicks() - started) / TicksPerSecond},
                {"hashes", hashes}};
    });
#endif
    s.On("game.stop", [this](const Json& p, const CancelToken&) -> Json {
        Guid id = RequireId(p);
        auto running = Running();
        if (auto game = running.find(id); game != running.end()) GameInstanceGuard::Stop(game->second);
        return nullptr;
    });

    s.On("prefs.update", [this](const Json& p, const CancelToken&) -> Json {
        std::optional<std::optional<double>> mouse;
        if (const Json* m = Field(p, "mouseMultiplier")) {
            if (m->is_null())
                mouse = std::optional<double>();
            else {
                auto value = first_run::ParseMouseMultiplier(m->is_string() ? m->get<std::string>() : std::string());
                if (!value) Throw(ErrorKind::Argument, "Enter a number from 0.001 to 0.1.");
                mouse = value;
            }
        }
        std::optional<int> fps;
        if (const Json* f = Field(p, "fpsLimit"); f && !f->is_null()) {
            std::string text = f->is_number_integer() ? std::to_string(f->get<int64_t>())
                               : f->is_string()       ? f->get<std::string>()
                                                      : std::string("x");
            if (Trim(text) == "0")
                fps = 0;
            else if (auto value = first_run::ParseFpsLimit(text))
                fps = *value;
            else
                Throw(ErrorKind::Argument, "Enter a whole number from 30 to 1000.");
        }
        SavePrefs([&](Preferences& prefs) {
            if (mouse) prefs.mouseMultiplier = *mouse;
            if (fps) prefs.fpsLimit = *fps;
            if (auto v = GetBool(p, "friendNotifications")) prefs.friendNotifications = *v;
            if (auto v = GetBool(p, "messageNotifications")) prefs.messageNotifications = *v;
            if (auto v = GetBool(p, "keepInTray")) prefs.keepInTray = *v;
            if (auto v = GetBool(p, "updateNotifications")) prefs.updateNotifications = *v;
        });
        return Snapshot();
    });
    s.On("startup.get", [](const Json&, const CancelToken&) -> Json {
        bool installed = Installation::IsReleaseBuild() && Installation::IsInstalledHere();
        return {{"available", installed}, {"enabled", installed && Installation::StartsWithWindows()}};
    });
    s.On("startup.set", [](const Json& p, const CancelToken&) -> Json {
        try {
            Installation::SetStartsWithWindows(Bool(p, "enabled"));
        } catch (const Error& e) {
            if (e.kind != ErrorKind::UnauthorizedAccess) throw;
            Throw(ErrorKind::UnauthorizedAccess, "Windows didn't allow changing startup apps.");
        }
        return {{"enabled", Installation::StartsWithWindows()}};
    });
    s.On("uninstall.start", [this](const Json&, const CancelToken&) -> Json {
        process::Start(Installation::InstalledExe(),
                       {L"--after", std::to_wstring(GetCurrentProcessId()), L"--uninstall"}, ExeDirectory());
        PostToUi([this] { Quit(); });
        return nullptr;
    });

    s.On("gameSetup.verify", [this](const Json& p, const CancelToken& cancel) -> Json {
        BuildCheck result = game_build::Verify(Widen(Str(p, "folder").value_or("")), cancel);
        fs::path full;
        try {
            full = FullPath(TrimW(Widen(Str(p, "folder").value_or(""))));
        } catch (const Error&) {
        }
        bool unfinished = false;
        try {
            unfinished = !full.empty() && DirExists(full / L".DepotDownloader");
        } catch (const Error&) {
        }
        auto stop = first_run::StopFor(Prefs().downloadStop, full);
        std::string kind = first_run::CheckKind(result, unfinished, stop);
        Json out = {
            {"ok", result.ok}, {"title", result.title}, {"reason", result.reason}, {"folder", nullptr}, {"kind", kind}};
        if (kind == "partial") {
            out["stopTitle"] = first_run::StopTitle;
            out["stopReason"] = first_run::StopReason(stop, SteamDownloadRunning());
        }
        if (result.ok) out["folder"] = Narrow(full.wstring());
        if (result.ok && stop && stop->kind != "done" && !SteamDownloadRunning()) try {
                SavePrefs([](Preferences& prefs) {
                    if (prefs.downloadStop) {
                        prefs.downloadStop->kind = "done";
                        prefs.downloadStop->text.clear();
                    }
                });
            } catch (const Error&) {
            }
        return out;
    });
    s.On("gameSetup.finish", [this](const Json& p, const CancelToken&) -> Json {
        std::string folder = Str(p, "folder").value_or("");
        SavePrefs([&](Preferences& prefs) {
            auto unfinished = prefs.steamDownloadFolder;
            prefs.gamePath = folder;
            prefs.gameSetupFor = Preferences::SetupKey();
            prefs.steamDownloadFolder = unfinished && IEquals(FullPath(Widen(*unfinished)).wstring(), Widen(folder))
                                            ? std::nullopt
                                            : unfinished;
        });
        return Snapshot();
    });
    s.On("gameSetup.detect", [this](const Json&, const CancelToken& cancel) -> Json {
        Preferences prefs = Prefs();
        auto candidates = first_run::FolderCandidates(prefs.gamePath, prefs.steamDownloadFolder,
                                                      DefaultDownloadFolder(), first_run::FixedDriveRoots());
        auto found = first_run::DetectGameFolder(
            candidates, [&cancel](const fs::path& folder) { return game_build::Verify(folder.wstring(), cancel).ok; });
        return {{"folder", found ? Json(Narrow(found->wstring())) : Json(nullptr)}};
    });
    s.On("gameFiles.verify", [this](const Json& p, const CancelToken& cancel) -> Json {
        std::string folder = Str(p, "folder").value_or(Prefs().gamePath);
        fs::path root = FullPath(TrimW(Widen(folder)));
        auto report = game_files::Verify(
            root, Instances(),
            [this](const std::string& stage, std::optional<double> fraction, const std::string& detail) {
                shell_->Emit(
                    "gameFiles.progress",
                    {{"stage", stage}, {"fraction", fraction ? Json(*fraction) : Json(nullptr)}, {"detail", detail}});
            },
            cancel,
            [size = first_run::ParseSize(Prefs().downloadSize)](std::wstring_view path) {
                return first_run::LeftOut(path, size);
            });
        Json problems = Json::array();
        for (auto& problem : report.folder) problems.push_back({{"path", problem.path}, {"issue", problem.issue}});
        return {{"folder", Narrow(root.wstring())},
                {"checked", report.checked},
                {"problems", problems},
                {"copies", report.copies.copies},
                {"fixed", report.copies.fixed},
                {"fixedFiles", report.copies.fixedFiles}};
    });
    s.On("gameFiles.setSize", [this](const Json& p, const CancelToken& cancel) -> Json {
        fs::path game = FullPath(TrimW(Widen(Prefs().gamePath)));
        if (Prefs().gamePath.empty() || !DirExists(game))
            Throw(ErrorKind::InvalidOperation, "The game folder isn't there. Choose it again.");
        if (!Running().empty()) Throw(ErrorKind::InvalidOperation, "Close Para Bellum first.");
        first_run::Size from = first_run::ParseSize(Prefs().downloadSize),
                        to = first_run::ParseSize(Str(p, "size").value_or(""));
        std::vector<fs::path> roots = {Instances(), fs::path(InstanceFolders::DataDirectory()) / L"Instances"};
        if (static_cast<int>(to) >= static_cast<int>(from)) {
#ifdef RWND_RELEASE
            constexpr bool release = true;
            constexpr bool devData = false;
#else
            constexpr bool release = false;
            const bool devData = settings::Env(L"RWND_DEV_DATA_DIR").has_value();
#endif
            const bool gameFolder = first_run::FreesGameFolder(release, devData);
            auto files = first_run::SwitchDownFiles(game, roots, to, gameFolder);
            if (!Bool(p, "confirm"))
                return {{"result", "confirm"}, {"bytes", first_run::FreedBytes(files)}, {"gameFolder", gameFolder}};
            auto freed = first_run::RemoveFiles(files);
            SavePrefs([to](Preferences& prefs) { prefs.downloadSize = first_run::SizeName(to); });
            return {{"result", "switched"}, {"freed", freed.bytes}, {"removed", freed.removed}};
        }
        auto missing = first_run::MissingFiles(game, game_files::ManifestFor(game), to);
        int64_t needed = 0;
        for (auto& e : missing) needed += static_cast<int64_t>(e.size);
        if (!missing.empty()) {
            if (!Bool(p, "steam")) return {{"result", "steam"}, {"bytes", needed}};
            bool qr = Bool(p, "qr");
            std::optional<std::string> user = qr ? std::nullopt : std::optional(Trim(Str(p, "user").value_or("")));
            if (user && user->empty()) Throw(ErrorKind::Argument, "Enter your Steam account name.");
            ULARGE_INTEGER freeBytes{};
            GetDiskFreeSpaceExW(PathRoot(game).c_str(), &freeBytes, nullptr, nullptr);
            if (needed > static_cast<int64_t>(freeBytes.QuadPart))
                Throw(ErrorKind::Argument, "Not enough space on " + DriveText(PathRoot(game)) + ". The textures need " +
                                               Gigabytes(needed) + ".");
            SteamJob job = SteamJob::ParaBellum();
            job.fileList = fs::path(InstanceFolders::DataDirectory()) / L"steam-switch-filelist.txt";
            CreateDirectories(job.fileList->parent_path());
            WriteAllText(*job.fileList, first_run::MissingFileList(missing));
            SteamDownload download;
            WireSteam(download);
            {
                std::lock_guard lock(steamMutex_);
                download_ = &download;
            }
            struct Clear {
                Controller* c;
                ~Clear() {
                    std::lock_guard lock(c->steamMutex_);
                    c->download_ = nullptr;
                }
            } clear{this};
            busy_ = true;
            struct Idle {
                Controller* c;
                ~Idle() { c->busy_ = false; }
            } idle{this};
            download.Run(job, user, Str(p, "password"), game, Bool(p, "remember"), cancel);
        }
        first_run::LinkIntoCopies(game, roots, to);
        for (auto& account : accounts_->Entries()) {
            try {
                fs::path ini = KnownFolder(&FOLDERID_Documents) / L"My Games" / L"Rainbow Six - Siege" /
                               Widen(account.playerId.D()) / L"GameSettings.ini";
                if (!FileExists(ini)) continue;
                std::string before = ReadAllText(ini), after = first_run::LiftTexturePreset(before, from, to);
                if (after != before) WriteAllText(ini, after);
            } catch (const Error&) {
            }
        }
        SavePrefs([to](Preferences& prefs) { prefs.downloadSize = first_run::SizeName(to); });
        return {{"result", missing.empty() ? "switched" : "downloaded"}};
    });
    s.On("gameFiles.sizes", [](const Json&, const CancelToken&) -> Json {
        Json out = Json::object();
        for (auto size : {first_run::Size::Full, first_run::Size::Medium, first_run::Size::Small})
            out[first_run::SizeName(size)] =
                game_build::SteamBytes - first_run::LeftOutBytes(game_files::Manifest(), size);
        return out;
    });
    s.On("gameSetup.resolveDownload", [](const Json& p, const CancelToken&) -> Json {
        try {
            return {{"target", Narrow(ResolveDownloadTarget(Widen(Str(p, "folder").value_or(""))).wstring())}};
        } catch (const Error&) {
            return {{"target", nullptr}};
        }
    });
    s.On("steam.download", [this](const Json& p, const CancelToken& cancel) -> Json {
        bool qr = Bool(p, "qr");
        std::optional<std::string> user = qr ? std::nullopt : std::optional(Trim(Str(p, "user").value_or("")));
        std::optional<std::string> password = Str(p, "password");
        fs::path target;
        try {
            target = ResolveDownloadTarget(Widen(Str(p, "folder").value_or("")));
        } catch (const Error&) {
            Throw(ErrorKind::Argument, "Choose a valid download folder.");
        }
        if (user && user->empty()) Throw(ErrorKind::Argument, "Enter your Steam account name.");
        const std::string typed = password.value_or("");
        auto run = [&]() -> Json {
            fs::path drive = PathRoot(target);
            if (!InstanceFolders::SupportsHardLinks(drive))
                Throw(ErrorKind::Argument,
                      "Drive " + DriveText(drive) + " can't hold the game: it isn't NTFS. Choose another drive.");
            bool repair = Bool(p, "repair");
            if (!repair && DirExists(target) && FileExists(target / L"RainbowSix.exe") &&
                game_build::Verify(target.wstring()).ok) {
                if (first_run::StopFor(Prefs().downloadStop, target)) RememberDownload(target, "done");
                return {{"result", "alreadyGood"}, {"target", Narrow(target.wstring())}};
            }
            first_run::Size size =
                first_run::ParseSize(repair ? Prefs().downloadSize : Str(p, "size").value_or(Prefs().downloadSize));
            int64_t needed = game_build::SteamBytes - first_run::LeftOutBytes(game_files::Manifest(), size) -
                             static_cast<int64_t>(FolderBytes(target));
            ULARGE_INTEGER freeBytes{};
            GetDiskFreeSpaceExW(drive.c_str(), &freeBytes, nullptr, nullptr);
            int64_t free = static_cast<int64_t>(freeBytes.QuadPart);
            if (needed > free)
                Throw(ErrorKind::Argument, "Not enough space on " + DriveText(drive) + ". The download needs " +
                                               Gigabytes(needed) + " more and " + Gigabytes(free) + " is free.");
            if (!repair)
                SavePrefs([&](Preferences& prefs) {
                    prefs.steamDownloadFolder = Narrow(target.wstring());
                    prefs.downloadSize = first_run::SizeName(size);
                });
            shell_->Emit("steam.started", {{"target", Narrow(target.wstring())}});
            RememberDownload(target, "running", repair ? "repair" : "");
            SteamDownload download;
            WireSteam(download);
            {
                std::lock_guard lock(steamMutex_);
                download_ = &download;
            }
            struct Clear {
                Controller* c;
                ~Clear() {
                    std::lock_guard lock(c->steamMutex_);
                    c->download_ = nullptr;
                }
            } clear{this};
            try {
                if (!repair) FirstRunOutbreak(download, user, password, Bool(p, "remember"), target, needed, cancel);
                download.Run(GameJob(size), user, password, target, Bool(p, "remember"), cancel);
            } catch (const Error& e) {
                if (e.kind == ErrorKind::Canceled && !closing_.Token().IsCanceled()) {
                    bool partial = DirExists(target / L".DepotDownloader");
                    if (!DirExists(target) || IsDirectoryEmpty(target)) {
                        if (DirExists(target)) RemoveDirectoryW(LongPath(target).c_str());
                        SavePrefs([](Preferences& prefs) {
                            prefs.steamDownloadFolder.reset();
                            prefs.downloadStop.reset();
                        });
                        download_log::Append(download_log::File(), download_log::Line(UtcNowTicks(), "stopped by you",
                                                                                      target, "nothing downloaded"));
                        return {{"result", "cancelledEmpty"}, {"target", Narrow(target.wstring())}, {"partial", false}};
                    }
                    RememberDownload(target, "stopped");
                    return {{"result", "paused"}, {"target", Narrow(target.wstring())}, {"partial", partial}};
                }
                if (e.kind == ErrorKind::Http && e.status == 0)
                    Throw(ErrorKind::Http,
                          "Couldn't get the Steam downloader from GitHub. Check your connection and try again.");
                if (e.kind == ErrorKind::Http)
                    Throw(ErrorKind::Http,
                          "Couldn't get the Steam downloader from GitHub. Check your connection and try again.",
                          e.status);
                if (e.kind == ErrorKind::Canceled) throw;
                Throw(e.kind, download.Redact(e.what()), e.status);
            }
            RememberDownload(target, "done");
            return {{"result", "done"},
                    {"target", Narrow(target.wstring())},
                    {"partial", DirExists(target / L".DepotDownloader")}};
        };
        try {
            return run();
        } catch (const Error& e) {
            if (e.kind != ErrorKind::Canceled)
                RememberDownload(target, "error", download_log::Clean(e.what(), {typed}), e.status);
            throw;
        }
    });
    s.On("outbreak.install", [this](const Json& p, const CancelToken& cancel) -> Json {
        auto id = accounts_->Selected();
        if (!id || !accounts_->CanSignInSilently(*id)) ThrowLoginRequired();
        if (outbreakInstalling_) Throw(ErrorKind::InvalidOperation, "The Outbreak update is already running.");
        bool needsInvite = false;
        try {
            needsInvite = api_->NeedsInvite(accounts_->Session(*id, cancel), cancel);
        } catch (const Error& e) {
            if (e.kind == ErrorKind::Canceled || e.kind == ErrorKind::LoginRequired) throw;
        }
        if (needsInvite) Throw(ErrorKind::InvalidOperation, AccountClient::InviteOnlyMessage);
        if (outbreakInstalling_.exchange(true))
            Throw(ErrorKind::InvalidOperation, "The Outbreak update is already running.");
        struct Done {
            Controller* c;
            ~Done() { c->outbreakInstalling_ = false; }
        } done{this};
        fs::path drive = PathRoot(InstanceFolders::StorageDirectory());
        int64_t needed = outbreak::InstallBytes - outbreak::StoreBytes();
        ULARGE_INTEGER freeBytes{};
        GetDiskFreeSpaceExW(drive.c_str(), &freeBytes, nullptr, nullptr);
        int64_t free = static_cast<int64_t>(freeBytes.QuadPart);
        if (needed > free)
            Throw(ErrorKind::Argument, "Not enough space on " + DriveText(drive) + ". Outbreak needs " +
                                           Gigabytes(needed) + " more and " + Gigabytes(free) + " is free.");
        AccountManager* manager = accounts_.get();
        outbreak::Remote remote{api_->ApiBaseUrl(), [manager, account = *id](const CancelToken& c) {
                                    return manager->Session(account, c).token;
                                }};
        auto progress = [this](const std::string& stage, std::optional<double> fraction, const std::string& detail) {
            shell_->Emit(
                "outbreak.progress",
                {{"stage", stage}, {"fraction", fraction ? Json(*fraction) : Json(nullptr)}, {"detail", detail}});
        };
        if (!Bool(p, "steam"))
            return {{"result", outbreak::Install(nullptr, std::nullopt, std::nullopt, false, remote, progress, cancel)
                                   ? "done"
                                   : "steam"}};
        bool qr = Bool(p, "qr");
        std::optional<std::string> user = qr ? std::nullopt : std::optional(Trim(Str(p, "user").value_or("")));
        if (user && user->empty()) Throw(ErrorKind::Argument, "Enter your Steam account name.");
        SteamDownload download;
        download.onPrompt = [this](const SteamPrompt& prompt) {
            static const char* kinds[] = {"password", "appCode", "emailCode", "mobileApproval"};
            shell_->Emit("steam.prompt", {{"kind", kinds[static_cast<int>(prompt.kind)]},
                                          {"email", prompt.email ? Json(*prompt.email) : Json(nullptr)},
                                          {"retry", prompt.retry}});
        };
        download.onProgress = [progress](const SteamProgress& s) {
            progress(s.stage == "Downloading" ? "Downloading Outbreak" : s.stage, s.fraction, s.detail.value_or(""));
        };
        download.onQrCode = [this](const std::vector<std::vector<bool>>& modules) {
            Json rows = Json::array();
            for (auto& row : modules) {
                std::string line;
                for (bool dark : row) line += dark ? '1' : '0';
                rows.push_back(line);
            }
            shell_->Emit("steam.qr", {{"rows", rows}});
        };
        {
            std::lock_guard lock(steamMutex_);
            download_ = &download;
        }
        struct Clear {
            Controller* c;
            ~Clear() {
                std::lock_guard lock(c->steamMutex_);
                c->download_ = nullptr;
            }
        } clear{this};
        outbreak::Install(&download, user, Str(p, "password"), Bool(p, "remember"), remote, progress, cancel);
        return {{"result", "done"}};
    });
    s.On("steam.answer", [this](const Json& p, const CancelToken&) -> Json {
        std::lock_guard lock(steamMutex_);
        if (download_) download_->Answer(Str(p, "text").value_or(""));
        return nullptr;
    });

    s.On("update.check", [](const Json&, const CancelToken& cancel) -> Json {
        if (!Installation::IsReleaseBuild() || !Installation::IsInstalledHere()) return nullptr;
        auto release = updater::Check(cancel);
        return release ? Release(*release) : Json(nullptr);
    });
    s.On("update.install", [this](const Json& p, const CancelToken& cancel) -> Json {
        auto release = updater::Check(cancel);
        if (!release) return {{"restarting", false}};
        std::string bearer;
        if (accounts_ && updater::SendsSignIn(release->url)) {
            if (auto id = accounts_->Selected(); id && accounts_->CanSignInSilently(*id)) {
                try {
                    bearer = accounts_->Session(*id, cancel).token;
                } catch (const Error& e) {
                    if (e.kind == ErrorKind::Canceled) throw;
                }
            }
        }
        busy_ = true;
        shell_->Emit("update.downloading", Release(*release));
        try {
            fs::path file = updater::Download(
                *release, [this](double fraction) { shell_->Emit("update.progress", {{"fraction", fraction}}); },
                cancel, nullptr, bearer);
            shell_->Emit("update.progress", {{"fraction", 1.0}, {"restarting", true}});
            updater::ApplyAndRestart(file, Bool(p, "stayInTray"));
        } catch (...) {
            busy_ = false;
            throw;
        }
        PostToUi([this] { Quit(); });
        return {{"restarting", true}};
    });

    s.On(
        "preview.start",
        [this](const Json&, const CancelToken&) -> Json {
            PreviewNext();
            return nullptr;
        },
        true);
    s.On("preview.session", [this](const Json& p, const CancelToken&) -> Json {
        NewSession(static_cast<int>(GetInt(p, "accounts").value_or(0)), Str(p, "gamePath").value_or(""));
        const Json* sample = Field(p, "outbreak");
        previewOutbreak_ = sample && sample->is_object() ? *sample : Json(nullptr);
        previewSteam_ = Bool(p, "steam");
        if (auto size = Str(p, "downloadSize")) {
            std::lock_guard lock(sessionMutex_);
            prefs_.downloadSize = *size;
        }
        if (auto mouse = GetDouble(p, "mouseMultiplier")) {
            std::lock_guard lock(sessionMutex_);
            prefs_.mouseMultiplier = *mouse;
        }
        if (auto fps = GetInt(p, "fpsLimit")) {
            std::lock_guard lock(sessionMutex_);
            prefs_.fpsLimit = static_cast<int>(*fps);
        }
        {
            const Json* servers = Field(p, "servers");
            std::lock_guard lock(sessionMutex_);
            previewServers_ = servers && servers->is_object() ? AccountClient::ServerChoice(*servers) : Json(nullptr);
        }
        return Snapshot();
    });
    s.On(
        "preview.rendered",
        [this](const Json& p, const CancelToken&) -> Json {
            std::string screen = Str(p, "screen").value_or("screen");
            fs::path file = fs::path(previewDirectory_) / (Widen(screen) + L".png");
            shell_->CapturePng(file.wstring(), [this](bool) {
                previewIndex_++;
                PreviewNext();
            });
            return nullptr;
        },
        true);
}

void Controller::QueueGameCrash(Guid account, const std::string& name, const fs::path& root,
                                std::optional<uint32_t> code) {
    crash_reports::Pending report{Guid::New(), account, name, code, {}};
    auto bundles = log_vault::SealCrash(root, code);
    if (bundles) report.bundles = std::move(*bundles);
    {
        std::lock_guard lock(crashMutex_);
        try {
            crash_reports::Save(CrashQueueDirectory(), report);
        } catch (const Error&) {
            unsavedCrashReports_.emplace(report.id, report);
        }
        crash_reports::Trim(CrashQueueDirectory(), retention::Keep);
    }
    PostToUi([this] {
        if (closing_.Token().IsCanceled()) return;
        shell_->ShowFromTray();
        if (!shell_->IsActive()) {
            FLASHWINFO attention{sizeof(FLASHWINFO), shell_->Hwnd(), FLASHW_TRAY, 3, 0};
            FlashWindowEx(&attention);
        }
    });
    shell_->Emit("game.crashReport", report.ForUi());
}

Json Controller::ResolveCrash(const Json& params, const CancelToken& cancel) {
    auto id = RequireId(params);
    const bool send = Bool(params, "send");

    std::optional<crash_reports::Pending> report;
    const auto directory = CrashQueueDirectory();
    {
        std::lock_guard lock(crashMutex_);
        if (const auto item = unsavedCrashReports_.find(id); item != unsavedCrashReports_.end())
            report = item->second;
        else
            report = crash_reports::Load(directory, id);
        Require(report.has_value() && resolvingCrashReports_.insert(id).second,
                "This crash report is no longer pending.");
    }
    auto release = [&] {
        std::lock_guard lock(crashMutex_);
        resolvingCrashReports_.erase(id);
    };
    try {
        std::optional<std::string> receipt;
        if (send) {
            const auto ui = report->ForUi();
            Require(Bool(ui, "canSend"), "The requested crash report is no longer available.");
            const bool includeDump = Bool(ui, "hasDump");
            const auto& path = includeDump ? report->bundles.withDump : report->bundles.log;
            File file(path, File::Mode::Read, true);
            const uint64_t length = file.Length();
            Require(length > 0 && length <= crash_reports::MaxUpload, "The crash report exceeds the upload limit.");
            std::string body(static_cast<size_t>(length), '\0');
            file.ReadExactly(body.data(), body.size());
            LoginSession login = accounts_->Session(report->account, cancel);
            receipt = api_->SubmitCrashReport(login, std::move(body), cancel);
        }
        {
            std::lock_guard lock(crashMutex_);
            DeleteFileIfExists(directory / (Widen(id.N()) + L".json"));
            unsavedCrashReports_.erase(id);
        }
        release();
        return {{"sent", receipt.has_value()}, {"reportId", receipt ? Json(*receipt) : Json(nullptr)}};
    } catch (...) {
        release();
        throw;
    }
}
void Controller::Observe(void* process, Guid id, std::string displayName, Guid sessionId, fs::path root) {
    HANDLE handle = static_cast<HANDLE>(process);
    bool released = false, canceled = false;
    std::optional<uint32_t> exitCode;
    try {
        while (WaitForSingleObject(handle, 1000) == WAIT_TIMEOUT)
            if (closing_.Token().IsCanceled()) {
                canceled = true;
                break;
            }
        if (!canceled) {
            if (DWORD code = 0; GetExitCodeProcess(handle, &code) && code != STILL_ACTIVE) exitCode = code;
            {
                std::string route;
                try {
                    if (FileExists(root / L"rewind-native-route.log"))
                        route = ReadAllText(root / L"rewind-native-route.log");
                } catch (...) {
                }
                const bool abnormal = crash_reports::UnexpectedExit(exitCode, FileExists(root / log_vault::DumpName));
                play_content::ContentLog saveLog;
                saveLog.Line(save_recovery::AfterRun(root, abnormal, route, save_recovery::Clock::now()));
                saveLog.Write(root);
            }
            if (crash_reports::UnexpectedExit(exitCode, FileExists(root / log_vault::DumpName)))
                QueueGameCrash(id, displayName, root, exitCode);
            else {
                log_vault::Seal(root, "exit", exitCode);
                try {
                    DeleteFileIfExists(root / log_vault::DumpName);
                } catch (...) {
                }
            }
            LoginSession login = accounts_->Session(id, closing_.Token());
            api_->RevokeGameSession(login, sessionId, closing_.Token());
            released = true;
        }
    } catch (const Error& e) {
        canceled = e.kind == ErrorKind::Canceled || closing_.Token().IsCanceled();
    }
    CloseHandle(handle);
    if (!canceled)
        shell_->Emit("game.exited", {{"displayName", displayName},
                                     {"released", released},
                                     {"exitCode", exitCode && *exitCode != 0 ? Json(log_vault::ExitCodeText(*exitCode))
                                                                             : Json(nullptr)}});
}

void Controller::PreviewNext() {
    static std::vector<std::string> screens = {"1-setup",
                                               "1b-game-folder",
                                               "1c-game-folder-wrong",
                                               "1d-steam",
                                               "1d2-steam-password",
                                               "1e-steam-guard",
                                               "1f-steam-progress",
                                               "2-login",
                                               "2b-login-steam",
                                               "3-login-known",
                                               "4-home",
                                               "5-home-menu",
                                               "7-update-available",
                                               "6-settings",
                                               "8-outbreak-update",
                                               "8b-outbreak-steam",
                                               "8c-outbreak-progress",
                                               "00-installer-unchanged",
                                               "01-sign-in",
                                               "02-game-folder-not-installed",
                                               "03-game-folder-found",
                                               "04-game-folder-other-siege-version",
                                               "05-download-size",
                                               "05b-download-size-steam-sign-in",
                                               "06-download-size-steam-password",
                                               "07-download-outbreak-first",
                                               "07b-download-steam-guard-code",
                                               "08-download-second-steam-sign-in",
                                               "09-download-game",
                                               "10-home-play-gear",
                                               "11-home-play-gear-hover",
                                               "12-gear-menu",
                                               "13-gear-download-size-page",
                                               "14-confirm-medium-to-small",
                                               "15-small-to-full-steam-sign-in",
                                               "16-small-to-full-progress",
                                               "17-home-outbreak-background",
                                               "18-home-outbreak-get",
                                               "19-outbreak-steam-sign-in",
                                               "20-settings",
                                               "22-gear-mouse-multiplier",
                                               "23-gear-fps-limit-on",
                                               "23-gear-fps-limit-off",
                                               "9-messages",
                                               "9b-home-unread",
                                               "25-servers-global",
                                               "26-servers-open",
                                               "27-servers-eu-only",
                                               "28-servers-launch-progress",
                                               "02b-download-stopped-error",
                                               "02c-download-stopped-by-you",
                                               "29-home-season-vote",
                                               "23z-legal",
                                               "24-cant-reach-rwnd"};
    if (previewIndex_ >= screens.size()) {
        Quit();
        return;
    }
    shell_->Emit("preview.show", {{"screen", screens[previewIndex_]}});
}

int RunPreview(const std::wstring& directory) {
    CreateDirectories(directory);
    Controller controller(StartMode::Preview, false, FullPath(directory).wstring());
    return controller.Run();
}

}
