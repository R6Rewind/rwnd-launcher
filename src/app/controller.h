#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include "app/shell.h"
#include "app/tray.h"
#include "core/account_manager.h"
#include "core/chat_client.h"
#include "core/crash_reports.h"
#include "core/first_run.h"
#include "core/instance_folders.h"
#include "core/preferences.h"
#include "core/steam_download.h"

namespace rwnd::app {

class Controller {
public:
    Controller(StartMode mode, bool startInTray, std::wstring previewDirectory = {});
    ~Controller();
    int Run();

private:
    void Register();
    void NewSession(int previewAccounts = -1, const std::string& previewGamePath = {});
    Json Snapshot();
    Json AccountJson(const RememberedAccount& a, const std::map<Guid, InstanceProcess>& running);
    std::map<Guid, InstanceProcess> Running();
    void RefreshLogKey(const fs::path& root, const LoginSession& login, const CancelToken& cancel);
    bool OutbreakLive(const LoginSession& login, const CancelToken& cancel);
    play_content::LiveAnswer OutbreakAnswer(const LoginSession& login, const CancelToken& cancel);
    void RefreshOutbreakLater();
    bool OutbreakLiveSignedOut(const CancelToken& cancel);
    void FirstRunOutbreak(SteamDownload& game, const std::optional<std::string>& user,
                          const std::optional<std::string>& password, bool remember, const fs::path& target,
                          int64_t gameStillNeeded, const CancelToken& cancel);
    Json OutbreakJson();
    void WireSteam(SteamDownload& download);
    SteamJob GameJob(first_run::Size size);
    void RememberDownload(const fs::path& folder, const std::string& kind, const std::string& text = {},
                          int status = 0);
    bool SteamDownloadRunning();
    void Notify(const std::string& title, const std::string& text);
    void Toast(const std::string& text);
    bool CloseRequested();
    void Quit();
    Preferences Prefs();
    void SavePrefs(const std::function<void(Preferences&)>& change);
    Guid RequireId(const Json& params);
    void Observe(void* process, Guid id, std::string displayName, Guid sessionId, fs::path root);
    void QueueGameCrash(Guid account, const std::string& name, const fs::path& root, std::optional<uint32_t> exitCode);
    Json ResolveCrash(const Json& params, const CancelToken& cancel);
    void PreviewNext();

    StartMode mode_;
    bool startInTray_;
    std::unique_ptr<Shell> shell_;
    std::unique_ptr<Tray> tray_;
    CancelSource closing_;
    std::atomic<bool> busy_{false}, quitting_{false};

    std::recursive_mutex sessionMutex_;
    std::unique_ptr<AccountClient> api_;
    std::unique_ptr<CredentialStore> store_;
    std::unique_ptr<AccountManager> accounts_;
    Preferences prefs_;
    std::vector<std::unique_ptr<AccountManager>> retired_;
    std::vector<std::unique_ptr<CredentialStore>> retiredStores_;
    std::vector<std::unique_ptr<AccountClient>> retiredApis_;
    fs::path Instances() const;

    std::mutex crashMutex_;
    std::map<Guid, crash_reports::Pending> unsavedCrashReports_;
    std::set<Guid> resolvingCrashReports_;

    std::mutex steamMutex_;
    SteamDownload* download_ = nullptr;

    std::atomic<int> outbreakLive_{-1};
    std::atomic<int64_t> outbreakChecked_{0};
    std::atomic<bool> outbreakRefreshing_{false}, outbreakInstalling_{false};
    std::mutex chatMutex_;
    std::unique_ptr<ChatClient> chat_;
    Guid chatAccount_;
    ChatClient& Chat(const Guid& id);
    void StopChat();
    Json previewOutbreak_;
    std::atomic<bool> previewSteam_{false};
    Json previewServers_;

    std::wstring previewDirectory_;
    fs::path previewVault_;
    std::vector<std::string> previewScreens_;
    size_t previewIndex_ = 0;
};

}
