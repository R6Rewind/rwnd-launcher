#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rwnd {

using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;

enum class ErrorKind {
    IO,
    InvalidData,
    InvalidOperation,
    UnauthorizedAccess,
    Http,
    Canceled,
    LoginRequired,
    Argument,
    SteamDownload,
    Timeout,
    Win32
};

class Error : public std::runtime_error {
public:
    Error(ErrorKind kind, const std::string& message, int status = 0)
        : std::runtime_error(message), kind(kind), status(status) {}
    ErrorKind kind;
    int status;
};
[[noreturn]] void Throw(ErrorKind kind, const std::string& message, int status = 0);
bool IsDiskFull(const Error& error);
inline void Require(bool condition, const std::string& message) {
    if (!condition) Throw(ErrorKind::InvalidData, message);
}
[[noreturn]] void ThrowLastError(const std::string& what);

std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view utf16);
std::string ToLowerAscii(std::string_view s);
std::wstring ToLowerW(std::wstring_view s);
bool IEquals(std::wstring_view a, std::wstring_view b);
bool IEqualsA(std::string_view a, std::string_view b);
bool StartsWith(std::string_view s, std::string_view prefix);
bool EndsWith(std::string_view s, std::string_view suffix);
bool IStartsWithW(std::wstring_view s, std::wstring_view prefix);
std::string Trim(std::string_view s);
std::wstring TrimW(std::wstring_view s);
std::string FormatThousands(int64_t value);
std::string HexLower(std::span<const uint8_t> bytes);
bool IsHexHash(std::string_view value);
std::string TrackedText(std::string_view value);

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void Update(const void* data, size_t size);
    Bytes Final();
    std::string FinalHex();

private:
    void* alg_ = nullptr;
    void* hash_ = nullptr;
    Bytes object_;
};
Bytes Sha256Of(std::span<const uint8_t> data);
std::string Sha256Hex(std::span<const uint8_t> data);
std::string Sha256Hex(std::string_view data);
class CancelToken;
std::string Sha256File(const fs::path& path, const CancelToken* cancel = nullptr);
Bytes RandomBytes(size_t count);
std::string Base64Encode(std::span<const uint8_t> data);
Bytes Base64Decode(std::string_view text);
std::string Base64Url(std::span<const uint8_t> data);

struct Guid {
    uint8_t bytes[16]{};
    static Guid New();
    static std::optional<Guid> Parse(std::string_view text);
    bool IsEmpty() const;
    std::string D() const;
    std::string N() const;
    bool operator==(const Guid&) const = default;
    auto operator<=>(const Guid&) const = default;
};
struct GuidHash {
    size_t operator()(const Guid& g) const noexcept;
};

constexpr int64_t TicksPerSecond = 10'000'000;
constexpr int64_t FileTimeEpochTicks = 504'911'232'000'000'000;
int64_t UtcNowTicks();
std::optional<int64_t> ParseIsoTicks(std::string_view text);
std::string FormatIsoTicks(int64_t ticks);
std::string FormatLocalDate(int64_t utcTicks);

class CancelToken {
public:
    bool IsCanceled() const { return state_->canceled.load(); }
    void ThrowIfCanceled() const;
    uint64_t Register(std::function<void()> callback) const;
    void Unregister(uint64_t id) const;
    bool Sleep(int milliseconds) const;
    static CancelToken None();
    struct State {
        std::atomic<bool> canceled{false};
        std::mutex mutex;
        std::condition_variable wake;
        uint64_t next = 1;
        std::vector<std::pair<uint64_t, std::function<void()>>> callbacks;
    };

private:
    friend class CancelSource;
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

class CancelSource {
public:
    CancelSource() = default;
    explicit CancelSource(const CancelToken& parent);
    ~CancelSource();
    CancelSource(const CancelSource&) = delete;
    CancelSource& operator=(const CancelSource&) = delete;
    void Cancel();
    void CancelAfter(int milliseconds);
    CancelToken Token() const { return token_; }

private:
    CancelToken token_;
    std::optional<CancelToken> parent_;
    uint64_t parentRegistration_ = 0;
    std::shared_ptr<std::atomic<bool>> timerAlive_;
};

class CancelRegistration {
public:
    CancelRegistration(const CancelToken& token, std::function<void()> callback)
        : token_(token), id_(token.Register(std::move(callback))) {}
    ~CancelRegistration() { token_.Unregister(id_); }
    CancelRegistration(const CancelRegistration&) = delete;
    CancelRegistration& operator=(const CancelRegistration&) = delete;

private:
    CancelToken token_;
    uint64_t id_;
};

Bytes ReadAllBytes(const fs::path& path);
std::string ReadAllText(const fs::path& path);
void WriteAllBytes(const fs::path& path, std::span<const uint8_t> data);
void WriteAllText(const fs::path& path, std::string_view text);
bool FileExists(const fs::path& path);
bool DirExists(const fs::path& path);
bool PathExists(const fs::path& path);
void MoveReplace(const fs::path& from, const fs::path& to);
void MoveNoReplace(const fs::path& from, const fs::path& to);
void CopyFileTo(const fs::path& from, const fs::path& to, bool overwrite);
void DeleteFileIfExists(const fs::path& path);
void CreateDirectories(const fs::path& path);
void DeleteTree(const fs::path& path);
uint64_t FileLength(const fs::path& path);
bool IsReparsePoint(const fs::path& path);
std::vector<fs::path> ListFiles(const fs::path& directory, std::wstring_view pattern = L"*");
std::vector<fs::path> ListDirectories(const fs::path& directory);
std::vector<fs::path> ListEntries(const fs::path& directory);
bool IsDirectoryEmpty(const fs::path& directory);
fs::path FullPath(const fs::path& path);
std::wstring LongPath(const fs::path& path);
fs::path PathRoot(const fs::path& path);
std::wstring TrimEndSeparator(std::wstring_view path);
std::wstring TempDirectory();
fs::path ExePath();
fs::path ExeDirectory();
fs::path LocalAppData();
fs::path KnownFolder(const void* folderId);

class File {
public:
    enum class Mode { Read, CreateNew, Create, ReadWriteExisting };
    File(const fs::path& path, Mode mode, bool shareDelete = false);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    uint64_t Length() const;
    void Seek(uint64_t position);
    uint64_t Position() const;
    size_t Read(void* buffer, size_t size);
    void ReadExactly(void* buffer, size_t size);
    void Write(const void* data, size_t size);
    void Flush();
    void Close();
    void* Handle() const { return handle_; }

private:
    void* handle_;
    fs::path path_;
};

std::wstring QuoteArgument(std::wstring_view argument);
std::wstring JoinArguments(const std::vector<std::wstring>& arguments);
struct ProcessInfo {
    uint32_t pid;
    std::wstring exePath;
    int64_t startUtcTicks;
};
std::vector<ProcessInfo> ProcessesByName(std::wstring_view exeName, bool requirePath);
std::optional<int64_t> ProcessStartTicks(void* processHandle);
std::wstring CurrentUserSid();
std::wstring CurrentUserName();
bool IsElevated();
uint32_t CurrentProcessId();

struct Version {
    int major = 0, minor = 0, build = -1, revision = -1;
    static std::optional<Version> Parse(std::string_view text);
    int Compare(const Version& other) const;
};
Version LauncherVersion();
std::string DisplayVersion();

}
