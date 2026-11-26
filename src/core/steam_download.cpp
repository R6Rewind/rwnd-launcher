#include "core/steam_download.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <regex>
#include <thread>

#include "core/download_log.h"
#include "core/game.h"
#include "core/http.h"
#include "core/instance_folders.h"
#include "core/zip.h"

namespace rwnd {

namespace {
constexpr const char* ToolUrl =
    "https://github.com/SteamRE/DepotDownloader/releases/download/DepotDownloader_3.4.0/DepotDownloader-windows-x64.zip";
constexpr const char* ToolZipSha256 = "41c9e9f0df54b3ad02e67a11726756e5c73283bd7c2e1b04acfa5ae4c2ed3767";
constexpr const char* ToolExeSha256 = "6281279efce8f1e20db9532a58e42382f81afb9e3827a8b965ffcb43fbe4531f";
constexpr int64_t ToolZipSize = 33'474'005;

bool Contains(const std::wstring& s, const wchar_t* part) {
    return s.find(part) != std::wstring::npos;
}
bool Starts(const std::wstring& s, const wchar_t* part) {
    return s.rfind(part, 0) == 0;
}
bool IContains(const std::wstring& s, const wchar_t* part) {
    return ToLowerW(s).find(ToLowerW(part)) != std::wstring::npos;
}

std::string SignInError(const std::wstring& line) {
    std::wstring reason = TrimW(line.substr(line.find(L':') + 1));
    if (IContains(reason, L"InvalidPassword")) return "Steam didn't accept that username and password.";
    if (IContains(reason, L"RateLimit")) return "Steam is limiting sign-in attempts. Wait a while, then try again.";
    if (IContains(reason, L"TwoFactorCodeMismatch") || IContains(reason, L"InvalidLoginAuthCode"))
        return "Steam didn't accept the Steam Guard code. Try again with a new code.";
    if (IContains(reason, L"AccountLoginDeniedNeedTwoFactor") || IContains(reason, L"FileNotFound"))
        return "Steam didn't finish the sign-in. Try again and approve it in the Steam Mobile app.";
    while (!reason.empty() && reason.back() == L'.') reason.pop_back();
    return "Steam sign-in failed (" + Narrow(reason) + ").";
}
}

fs::path SteamDownload::ToolDirectory() {
    return fs::path(InstanceFolders::DataDirectory()) / L"Tools" /
           (std::wstring(L"DepotDownloader-") + Widen(ToolVersion));
}

fs::path SteamDownload::EnsureTool(const std::function<void(double)>& progress, const CancelToken& cancel) {
    fs::path exe = ToolDirectory() / L"DepotDownloader.exe";
    if (FileExists(exe) && Sha256File(exe, &cancel) == ToolExeSha256) return exe;
    fs::path parent = ToolDirectory().parent_path();
    CreateDirectories(parent);
    fs::path zip = parent / (std::wstring(L"DepotDownloader-") + Widen(ToolVersion) + L".zip.download");
    fs::path staging = ToolDirectory().wstring() + L".staging-" + Widen(Guid::New().N());
    auto cleanup = [&] {
        try {
            DeleteFileIfExists(zip);
        } catch (...) {
        }
        try {
            if (DirExists(staging)) DeleteTree(staging);
        } catch (...) {
        }
    };
    try {
        {
            HttpRequest request;
            request.url = ToolUrl;
            request.followRedirects = true;
            request.timeoutMs = 10 * 60 * 1000;
            std::unique_ptr<File> out;
            int64_t done = 0;
            WinHttp()->Stream(
                request,
                [&](const HttpResponse& response) {
                    if (!response.IsSuccess()) ThrowStatus(response.status);
                    if (response.contentLength && static_cast<int64_t>(*response.contentLength) != ToolZipSize)
                        Throw(ErrorKind::InvalidData, "The DepotDownloader download has an unexpected size.");
                    out = std::make_unique<File>(zip, File::Mode::Create);
                },
                [&](const uint8_t* data, size_t size) {
                    out->Write(data, size);
                    done += static_cast<int64_t>(size);
                    if (done > ToolZipSize) Throw(ErrorKind::InvalidData, "The DepotDownloader download is too large.");
                    progress(static_cast<double>(done) / static_cast<double>(ToolZipSize));
                },
                cancel);
        }
        if (Sha256File(zip, &cancel) != ToolZipSha256)
            Throw(ErrorKind::InvalidData, "The DepotDownloader download failed its checksum.");
        CreateDirectories(staging);
        try {
            rwnd::zip::ExtractAll(zip, staging, &cancel);
        } catch (const Error& e) {
            if (e.kind == ErrorKind::Canceled) throw;
            Throw(ErrorKind::InvalidData, "Couldn't unpack the Steam downloader. Try again.");
        }
        if (Sha256File(staging / L"DepotDownloader.exe", &cancel) != ToolExeSha256)
            Throw(ErrorKind::InvalidData, "DepotDownloader failed its checksum.");
        if (DirExists(ToolDirectory())) DeleteTree(ToolDirectory());
        MoveNoReplace(staging, ToolDirectory());
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    return exe;
}

SteamDownload::~SteamDownload() {
    Kill();
    password_.reset();
    for (auto& s : secrets_) SecureZeroMemory(s.data(), s.size());
    if (stdin_) CloseHandle(stdin_);
    if (process_) CloseHandle(process_);
    if (job_) CloseHandle(job_);
}

std::string SteamDownload::Redact(std::string_view text) {
    std::lock_guard lock(mutex_);
    return download_log::Clean(text, secrets_);
}

void SteamDownload::Kill() {
    if (!process_) return;
    DWORD code = 0;
    if (GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE) {
        if (job_)
            TerminateJobObject(job_, 1);
        else
            TerminateProcess(process_, 1);
    }
}

void SteamDownload::Answer(const std::string& text) {
    std::lock_guard lock(mutex_);
    if (!stdin_ || !process_) return;
    DWORD code = 0;
    if (!GetExitCodeProcess(process_, &code) || code != STILL_ACTIVE) return;
    std::string line = Trim(text) + "\r\n";
    if (Trim(text).size() >= 4) secrets_.push_back(Trim(text));
    DWORD written = 0;
    WriteFile(stdin_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    SecureZeroMemory(line.data(), line.size());
}

bool SteamDownload::QrLine(const std::wstring& text) {
    if (Contains(text, L"sign in with this QR code")) {
        state_.qr = std::vector<std::wstring>();
        return true;
    }
    if (!state_.qr) return false;
    auto& rows = *state_.qr;
    if (rows.empty() && text.size() < 2) return true;
    rows.push_back(text);
    size_t size = rows[0].size() / 2;
    if (rows.size() < size) return true;
    std::vector<std::vector<bool>> modules(size, std::vector<bool>(size));
    for (size_t y = 0; y < size; y++)
        for (size_t x = 0; x < size; x++) modules[y][x] = 2 * x < rows[y].size() && rows[y][2 * x] != L' ';
    state_.qr.reset();
    if (onQrCode) onQrCode(modules);
    if (onProgress) onProgress({"Scan the QR code with the Steam app", 0, 0, std::nullopt, std::nullopt});
    return true;
}

bool SteamDownload::Prompt(const std::wstring& text) {
    auto raise = [&](SteamPromptKind kind, std::optional<std::string> email, bool retry) {
        if (onPrompt) onPrompt({kind, std::move(email), retry});
    };
    auto consume = [&] {
        bool rejected = codeRejected_;
        codeRejected_ = false;
        return rejected;
    };
    if (Starts(text, L"Enter account password for")) {
        if (password_) {
            std::string known = *password_;
            password_.reset();
            mutex_.unlock();
            Answer(known);
            mutex_.lock();
            SecureZeroMemory(known.data(), known.size());
        } else
            raise(SteamPromptKind::Password, std::nullopt, false);
        return true;
    }
    if (Contains(text, L"2-factor auth code") || Contains(text, L"2 factor auth code")) {
        raise(SteamPromptKind::AppCode, std::nullopt, consume());
        return true;
    }
    static const std::wregex email(LR"(auth code sent to the email at (.+?):\s*$)");
    std::wsmatch match;
    if (std::regex_search(text, match, email)) {
        raise(SteamPromptKind::EmailCode, Narrow(match[1].str()), consume());
        return true;
    }
    if (Contains(text, L"authentication code sent to your email")) {
        raise(SteamPromptKind::EmailCode, std::nullopt, consume());
        return true;
    }
    return false;
}

void SteamDownload::Line(const std::wstring& text) {
    if (QrLine(text)) return;
    std::wstring trimmed = TrimW(text);
    if (trimmed.empty()) return;
    state_.lastLine = Narrow(trimmed);
    if (Contains(trimmed, L"previous 2-factor auth code you have provided is incorrect")) {
        codeRejected_ = true;
        return;
    }
    if (Prompt(text + L" ")) return;
    auto report = [&](SteamProgress p) {
        if (onProgress) onProgress(p);
    };
    auto fail = [&](const std::string& message) {
        if (!state_.error) state_.error = message;
    };
    if (Contains(trimmed, L"Use the Steam Mobile App to confirm")) {
        if (onPrompt) onPrompt({SteamPromptKind::MobileApproval, std::nullopt, false});
        return;
    }
    if (Starts(trimmed, L"Logging '")) {
        report({"Signing in to Steam"});
        return;
    }
    if (Starts(trimmed, L"Logging in with QR code")) {
        report({"Getting a sign-in code from Steam"});
        return;
    }
    if (Starts(trimmed, L"Got ") && trimmed.ends_with(L" licenses for account!")) {
        report({"Signed in. Getting the game files list"});
        return;
    }
    if (Starts(trimmed, L"Failed to authenticate with Steam:") || Starts(trimmed, L"Unable to login to Steam3:")) {
        fail(SignInError(trimmed));
        return;
    }
    if (Starts(trimmed, L"Access token was rejected")) {
        fail("Your saved Steam sign-in expired. Sign in again with your password.");
        return;
    }
    static const std::wregex notOwned(LR"(^Depot (\d+) is not available from this account)"),
        noKey(LR"(^No valid depot key for (\d+))"), refused(LR"(^Encountered (401|403) for depot manifest)"),
        stageRx(LR"(^(Processing|Downloading) depot (\d+)$)"), fileRx(LR"(^(\d{1,3}\.\d+)% (.+)$)");
    if (depots_.empty())
        for (auto& d : game_build::SteamDepots) depots_.push_back(d.depot);
    std::wsmatch m;
    if (std::regex_search(trimmed, m, notOwned) || std::regex_search(trimmed, m, noKey)) {
        if (Refused(static_cast<uint32_t>(std::stoul(m[1].str()))))
            fail("This Steam account doesn't own Rainbow Six Siege, so Steam won't let it download the game.");
        return;
    }
    if (std::regex_search(trimmed, refused)) {
        fail("Steam refused this game version for your account. Make sure the account owns Rainbow Six Siege.");
        return;
    }
    if (Starts(trimmed, L"Unable to download manifest") || Starts(trimmed, L"Failed to find any server with chunk")) {
        fail("Steam's servers didn't send the game files. Check your connection, then start again to resume.");
        return;
    }
    if (Starts(trimmed, L"Error: Unable to create install directories")) {
        fail("Couldn't create the download folder. Pick a folder you can write to.");
        return;
    }
    if (Starts(trimmed, L"Could not connect to Steam")) {
        fail("Couldn't connect to Steam. Check your connection and try again.");
        return;
    }
    const int parts = Parts();
    if (std::regex_match(trimmed, m, stageRx)) {
        int index = 0, counted = 0;
        for (auto depot : depots_) {
            if (std::ranges::find(state_.refused, depot) != state_.refused.end()) continue;
            counted++;
            if (std::to_wstring(depot) == m[2].str()) index = counted;
        }
        if (m[1].str() == L"Downloading") {
            state_.part = index;
            state_.fraction = 0.0;
            report({"Downloading", index, parts, 0.0});
        } else
            report({"Getting the game files list"});
        return;
    }
    auto name = [](const std::wstring& path) { return Narrow(fs::path(path).filename().wstring()); };
    if (Starts(trimmed, L"Validating ")) {
        state_.file = name(trimmed.substr(11));
        report({"Checking files already downloaded", state_.part, parts, std::nullopt, state_.file});
        return;
    }
    if (Starts(trimmed, L"Pre-allocating ")) {
        state_.file = name(trimmed.substr(15));
        report({"Making space on disk", state_.part, parts, std::nullopt, state_.file});
        return;
    }
    if (std::regex_match(trimmed, m, fileRx)) {
        double percent = _wtof(m[1].str().c_str());
        state_.fraction = std::clamp(percent / 100, 0.0, 1.0);
        state_.file = name(m[2].str());
        report({"Downloading", state_.part, parts, state_.fraction, state_.file});
        return;
    }
    if (Starts(trimmed, L"Total downloaded:")) {
        state_.finished = true;
        report({"Download finished", parts, parts, 1.0});
    }
}

bool SteamDownload::Refused(uint32_t depot) {
    if (std::ranges::find(state_.refused, depot) == state_.refused.end()) state_.refused.push_back(depot);
    if (!game_build::IsRegionDepot(depot)) return true;
    bool regionLeft = false;
    for (auto d : depots_)
        if (game_build::IsRegionDepot(d) && std::ranges::find(state_.refused, d) == state_.refused.end())
            regionLeft = true;
    return !regionLeft;
}

int SteamDownload::Parts() const {
    int parts = 0;
    for (auto d : depots_)
        if (std::ranges::find(state_.refused, d) == state_.refused.end()) parts++;
    return parts;
}

void SteamDownload::FeedLine(const std::wstring& text) {
    std::lock_guard lock(mutex_);
    Line(text);
}

std::string SteamDownload::DownloadedText(uint64_t bytes) {
    char buffer[48];
    double gb = static_cast<double>(bytes) / (1024.0 * 1024 * 1024);
    if (gb < 1)
        snprintf(buffer, sizeof buffer, "%.0f MB downloaded", static_cast<double>(bytes) / (1024.0 * 1024));
    else
        snprintf(buffer, sizeof buffer, "%.1f GB downloaded", gb);
    return buffer;
}

void SteamDownload::ReportWritten(uint64_t bytes) {
    std::lock_guard lock(mutex_);
    if (state_.part <= 0 || state_.finished || state_.error || !onProgress) return;
    std::string written = DownloadedText(bytes);
    onProgress({"Downloading", state_.part, Parts(), state_.fraction,
                state_.file ? *state_.file + "  \xC2\xB7  " + written : written});
}
bool SteamDownload::FeedPartial(const std::wstring& text) {
    std::lock_guard lock(mutex_);
    return text.ends_with(L": ") && Prompt(text);
}

void SteamDownload::Pump(void* pipe) {
    std::string pending;
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof buffer, &read, nullptr) || read == 0) break;
        for (DWORD i = 0; i < read; i++) {
            if (buffer[i] == '\n') {
                std::wstring line = Widen(pending);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                FeedLine(line);
                pending.clear();
            } else
                pending += buffer[i];
        }
        if (!pending.empty() && pending.size() >= 2 && pending.ends_with(": ") && FeedPartial(Widen(pending)))
            pending.clear();
    }
    if (!pending.empty()) FeedLine(Widen(pending));
}

SteamJob SteamJob::ParaBellum() {
    SteamJob job;
    job.app = game_build::SteamApp;
    for (auto& d : game_build::SteamDepots) job.depots.emplace_back(d.depot, d.manifest);
    return job;
}

void SteamDownload::Run(const std::optional<std::string>& username, const std::optional<std::string>& password,
                        const fs::path& folder, bool remember, const CancelToken& cancel) {
    Run(SteamJob::ParaBellum(), username, password, folder, remember, cancel);
}

void SteamDownload::Run(const SteamJob& job, const std::optional<std::string>& username,
                        const std::optional<std::string>& password, const fs::path& folder, bool remember,
                        const CancelToken& cancel) {
    Require(job.app != 0 && !job.depots.empty(), "Invalid Steam download job.");
    fs::path exe = EnsureTool(
        [this](double p) {
            if (onProgress) onProgress({"Getting the Steam downloader", 0, 0, p});
        },
        cancel);
    password_ = password && !password->empty() ? password : std::nullopt;
    if (password_) {
        std::lock_guard lock(mutex_);
        secrets_.push_back(*password_);
    }
    CreateDirectories(folder);
    {
        std::lock_guard lock(mutex_);
        depots_.clear();
        for (auto& [depot, manifest] : job.depots) depots_.push_back(depot);
    }
    std::vector<std::wstring> arguments = {L"-app", std::to_wstring(job.app), L"-depot"};
    for (auto& [depot, manifest] : job.depots) arguments.push_back(std::to_wstring(depot));
    arguments.push_back(L"-manifest");
    for (auto& [depot, manifest] : job.depots) arguments.push_back(std::to_wstring(manifest));
    if (job.fileList) {
        Require(FileExists(*job.fileList), "The Steam file list is missing.");
        arguments.push_back(L"-filelist");
        arguments.push_back(job.fileList->wstring());
    }
    if (!username)
        arguments.push_back(L"-qr");
    else {
        arguments.push_back(L"-username");
        arguments.push_back(Widen(*username));
    }
    arguments.push_back(L"-dir");
    arguments.push_back(folder.wstring());
    arguments.push_back(L"-validate");
    if (remember) arguments.push_back(L"-remember-password");

    if (onProgress) onProgress({"Connecting to Steam"});
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inRead, inWrite, outRead, outWrite, errRead, errWrite;
    CreatePipe(&inRead, &inWrite, &sa, 0);
    CreatePipe(&outRead, &outWrite, &sa, 0);
    CreatePipe(&errRead, &errWrite, &sa, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    PROCESS_INFORMATION pi{};
    std::wstring command = QuoteArgument(exe.wstring()) + L" " + JoinArguments(arguments);
    std::wstring directory = ToolDirectory().wstring();
    SetEnvironmentVariableW(L"DOTNET_SYSTEM_CONSOLE_ALLOW_ANSI_COLOR_REDIRECTION", nullptr);
    BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.c_str(), &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (!started) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        CloseHandle(errRead);
        Throw(ErrorKind::SteamDownload, "Windows didn't start the Steam downloader.");
    }
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof info))
            AssignProcessToJobObject(job_, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard lock(mutex_);
        process_ = pi.hProcess;
        stdin_ = inWrite;
        state_ = State();
    }
    CancelRegistration stop(cancel, [this] {
        std::lock_guard lock(mutex_);
        Kill();
    });
    std::thread errors([this, errRead] { Pump(errRead); });
    std::atomic<bool> pumping = true;
    std::thread ticker([this, &pumping] {
        for (int tick = 0; pumping; tick++) {
            Sleep(100);
            if (tick % 15 != 14 || !job_) continue;
            JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION info{};
            if (QueryInformationJobObject(job_, JobObjectBasicAndIoAccountingInformation, &info, sizeof info, nullptr))
                ReportWritten(info.IoInfo.WriteTransferCount);
        }
    });
    Pump(outRead);
    errors.join();
    pumping = false;
    ticker.join();
    CloseHandle(outRead);
    CloseHandle(errRead);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    if (password_) {
        SecureZeroMemory(password_->data(), password_->size());
        password_.reset();
    }
    cancel.ThrowIfCanceled();
    std::lock_guard lock(mutex_);
    if (state_.error) Throw(ErrorKind::SteamDownload, *state_.error);
    if (exitCode != 0 || !state_.finished) {
        std::string last = state_.lastLine.value_or("");
        while (!last.empty() && last.back() == '.') last.pop_back();
        Throw(ErrorKind::SteamDownload, "The download stopped" + (last.empty() ? std::string(".") : ": " + last + ".") +
                                            " Start it again to resume.");
    }
}

}
