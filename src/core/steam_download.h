#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/util.h"

namespace rwnd {

enum class SteamPromptKind { Password, AppCode, EmailCode, MobileApproval };
struct SteamPrompt {
    SteamPromptKind kind;
    std::optional<std::string> email;
    bool retry;
};
struct SteamProgress {
    std::string stage;
    int part = 0, parts = 0;
    std::optional<double> fraction;
    std::optional<std::string> detail;
};
struct SteamJob {
    uint32_t app = 0;
    std::vector<std::pair<uint32_t, uint64_t>> depots;
    std::optional<fs::path> fileList;
    static SteamJob ParaBellum();
};

class SteamDownload {
public:
    static constexpr const char* ToolVersion = "3.4.0";
    static constexpr const char* SourceUrl = "https://github.com/SteamRE/DepotDownloader/tree/DepotDownloader_3.4.0";
    static fs::path ToolDirectory();
    static fs::path EnsureTool(const std::function<void(double)>& progress, const CancelToken& cancel);

    SteamDownload() = default;
    ~SteamDownload();
    SteamDownload(const SteamDownload&) = delete;
    SteamDownload& operator=(const SteamDownload&) = delete;

    std::function<void(const SteamPrompt&)> onPrompt;
    std::function<void(const SteamProgress&)> onProgress;
    std::function<void(const std::vector<std::vector<bool>>&)> onQrCode;

    void Run(const std::optional<std::string>& username, const std::optional<std::string>& password,
             const fs::path& folder, bool remember, const CancelToken& cancel);
    void Run(const SteamJob& job, const std::optional<std::string>& username,
             const std::optional<std::string>& password, const fs::path& folder, bool remember,
             const CancelToken& cancel);
    void Answer(const std::string& text);
    std::string Redact(std::string_view text);

    void FeedLine(const std::wstring& text);
    bool FeedPartial(const std::wstring& text);
    struct State {
        int part = 0;
        bool finished = false;
        std::optional<std::string> error, lastLine;
        std::optional<std::vector<std::wstring>> qr;
        std::optional<double> fraction;
        std::optional<std::string> file;
        std::vector<uint32_t> refused;
    };
    void ReportWritten(uint64_t bytes);
    static std::string DownloadedText(uint64_t bytes);
    const State& RunState() const { return state_; }

private:
    bool Prompt(const std::wstring& text);
    bool QrLine(const std::wstring& text);
    void Line(const std::wstring& text);
    bool Refused(uint32_t depot);
    int Parts() const;
    void Kill();
    void Pump(void* pipe);

    std::mutex mutex_;
    State state_;
    std::vector<uint32_t> depots_ = {};
    std::optional<std::string> password_;
    std::vector<std::string> secrets_;
    bool codeRejected_ = false;
    void* process_ = nullptr;
    void* job_ = nullptr;
    void* stdin_ = nullptr;
};

}
