#include "core/util.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <sddl.h>

#include <algorithm>
#include <cwctype>
#include <thread>

namespace rwnd {

void Throw(ErrorKind kind, const std::string& message, int status) {
    throw Error(kind, message, status);
}

bool IsDiskFull(const Error& error) {
    return (error.kind == ErrorKind::IO || error.kind == ErrorKind::Win32) &&
           (error.status == ERROR_DISK_FULL || error.status == ERROR_HANDLE_DISK_FULL);
}

static std::string SystemMessage(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = text ? Narrow(text) : "Windows error " + std::to_string(code);
    if (text) LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' '))
        message.pop_back();
    return message;
}

void ThrowLastError(const std::string& what) {
    DWORD code = GetLastError();
    auto kind = code == ERROR_ACCESS_DENIED ? ErrorKind::UnauthorizedAccess : ErrorKind::IO;
    Throw(kind, what + ": " + SystemMessage(code), static_cast<int>(code));
}

[[noreturn]] static void ThrowFile(const std::string& what, const fs::path& path, DWORD code) {
    auto kind = code == ERROR_ACCESS_DENIED ? ErrorKind::UnauthorizedAccess : ErrorKind::IO;
    Throw(kind, what + " " + Narrow(path.wstring()) + ": " + SystemMessage(code), static_cast<int>(code));
}

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string Narrow(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::wstring ToLowerW(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, out.data(), static_cast<int>(out.size()), out.data(),
                      static_cast<int>(out.size()), nullptr, nullptr, 0);
    return out;
}

bool IEquals(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}
bool IEqualsA(std::string_view a, std::string_view b) {
    return IEquals(Widen(a), Widen(b));
}
bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}
bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
bool IStartsWithW(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

std::string Trim(std::string_view s) {
    auto space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    };
    size_t a = 0, b = s.size();
    while (a < b && space(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && space(static_cast<unsigned char>(s[b - 1]))) b--;
    return std::string(s.substr(a, b - a));
}
std::wstring TrimW(std::wstring_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) a++;
    while (b > a && std::iswspace(s[b - 1])) b--;
    return std::wstring(s.substr(a, b - a));
}

std::string FormatThousands(int64_t value) {
    bool negative = value < 0;
    std::string digits = std::to_string(negative ? -value : value), out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return negative ? "-" + out : out;
}

std::string HexLower(std::span<const uint8_t> bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (size_t i = 0; i < bytes.size(); i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    return out;
}

bool IsHexHash(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(),
                                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string TrackedText(std::string_view value) {
    std::wstring w = Widen(value), out;
    for (size_t i = 0; i < w.size(); i++) {
        if (i > 0) out += L'\u200A';
        out += w[i] == L' ' ? L'\u2002' : w[i];
    }
    return Narrow(out);
}

Sha256::Sha256() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        Throw(ErrorKind::Win32, "SHA-256 is unavailable.");
    alg_ = alg;
    DWORD objectSize = 0, written = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof objectSize, &written, 0);
    object_.resize(objectSize);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, object_.data(), objectSize, nullptr, 0, 0) < 0)
        Throw(ErrorKind::Win32, "SHA-256 is unavailable.");
    hash_ = hash;
}
Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
}
void Sha256::Update(const void* data, size_t size) {
    auto p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        ULONG chunk = static_cast<ULONG>(std::min<size_t>(size, 1u << 30));
        BCryptHashData(hash_, const_cast<PUCHAR>(p), chunk, 0);
        p += chunk;
        size -= chunk;
    }
}
Bytes Sha256::Final() {
    Bytes out(32);
    BCryptFinishHash(hash_, out.data(), 32, 0);
    return out;
}
std::string Sha256::FinalHex() {
    return HexLower(Final());
}

Bytes Sha256Of(std::span<const uint8_t> data) {
    Sha256 h;
    h.Update(data.data(), data.size());
    return h.Final();
}
std::string Sha256Hex(std::span<const uint8_t> data) {
    return HexLower(Sha256Of(data));
}
std::string Sha256Hex(std::string_view data) {
    Sha256 h;
    h.Update(data.data(), data.size());
    return h.FinalHex();
}

std::string Sha256File(const fs::path& path, const CancelToken* cancel) {
    File file(path, File::Mode::Read, true);
    Sha256 hash;
    std::vector<uint8_t> buffer(1 << 20);
    for (;;) {
        if (cancel) cancel->ThrowIfCanceled();
        size_t n = file.Read(buffer.data(), buffer.size());
        if (n == 0) break;
        hash.Update(buffer.data(), n);
    }
    return hash.FinalHex();
}

Bytes RandomBytes(size_t count) {
    Bytes out(count);
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        Throw(ErrorKind::Win32, "Random numbers are unavailable.");
    return out;
}

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(std::span<const uint8_t> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += B64[v >> 18];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += B64[v & 63];
    }
    if (i + 1 == data.size()) {
        uint32_t v = data[i] << 16;
        out += B64[v >> 18];
        out += B64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += B64[v >> 18];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

Bytes Base64Decode(std::string_view text) {
    static int8_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        std::fill(std::begin(table), std::end(table), int8_t(-1));
        for (int i = 0; i < 64; i++) table[static_cast<uint8_t>(B64[i])] = static_cast<int8_t>(i);
    });
    std::string clean;
    clean.reserve(text.size());
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') clean += c;
    if (clean.size() % 4 != 0) Throw(ErrorKind::InvalidData, "Invalid base64 data.");
    Bytes out;
    out.reserve(clean.size() / 4 * 3);
    for (size_t i = 0; i < clean.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            char c = clean[i + k];
            if (c == '=') {
                if (i + 4 != clean.size() || k < 2) Throw(ErrorKind::InvalidData, "Invalid base64 data.");
                v[k] = 0;
                pad++;
            } else {
                if (pad) Throw(ErrorKind::InvalidData, "Invalid base64 data.");
                v[k] = table[static_cast<uint8_t>(c)];
                if (v[k] < 0) Throw(ErrorKind::InvalidData, "Invalid base64 data.");
            }
        }
        uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
        out.push_back(static_cast<uint8_t>(n >> 16));
        if (pad < 2) out.push_back(static_cast<uint8_t>(n >> 8));
        if (pad < 1) out.push_back(static_cast<uint8_t>(n));
    }
    return out;
}

std::string Base64Url(std::span<const uint8_t> data) {
    std::string s = Base64Encode(data);
    while (!s.empty() && s.back() == '=') s.pop_back();
    for (auto& c : s) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }
    return s;
}

Guid Guid::New() {
    Bytes r = RandomBytes(16);
    Guid g;
    std::copy(r.begin(), r.end(), g.bytes);
    g.bytes[6] = static_cast<uint8_t>((g.bytes[6] & 0x0F) | 0x40);
    g.bytes[8] = static_cast<uint8_t>((g.bytes[8] & 0x3F) | 0x80);
    return g;
}

std::optional<Guid> Guid::Parse(std::string_view text) {
    std::string t = Trim(text);
    if (t.size() == 38 && t.front() == '{' && t.back() == '}') t = t.substr(1, 36);
    std::string hex;
    if (t.size() == 36) {
        if (t[8] != '-' || t[13] != '-' || t[18] != '-' || t[23] != '-') return std::nullopt;
        for (size_t i = 0; i < t.size(); i++)
            if (i != 8 && i != 13 && i != 18 && i != 23) hex += t[i];
    } else if (t.size() == 32)
        hex = t;
    else
        return std::nullopt;
    Guid g;
    for (int i = 0; i < 16; i++) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        g.bytes[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return g;
}

bool Guid::IsEmpty() const {
    return std::all_of(std::begin(bytes), std::end(bytes), [](uint8_t b) { return b == 0; });
}
std::string Guid::N() const {
    return HexLower(bytes);
}
std::string Guid::D() const {
    std::string n = N();
    return n.substr(0, 8) + "-" + n.substr(8, 4) + "-" + n.substr(12, 4) + "-" + n.substr(16, 4) + "-" + n.substr(20);
}
size_t GuidHash::operator()(const Guid& g) const noexcept {
    uint64_t a, b;
    memcpy(&a, g.bytes, 8);
    memcpy(&b, g.bytes + 8, 8);
    return std::hash<uint64_t>()(a ^ (b * 0x9E3779B97F4A7C15ull));
}

int64_t UtcNowTicks() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) +
           FileTimeEpochTicks;
}

static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
static void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}
constexpr int64_t UnixEpochTicks = 621'355'968'000'000'000;

std::optional<int64_t> ParseIsoTicks(std::string_view text) {
    std::string s = Trim(text);
    auto digits = [&](size_t at, size_t count, int& out) {
        if (at + count > s.size()) return false;
        out = 0;
        for (size_t i = 0; i < count; i++) {
            char c = s[at + i];
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
        }
        return true;
    };
    int year, month, day, hour = 0, minute = 0, second = 0;
    if (!digits(0, 4, year) || s.size() < 10 || s[4] != '-' || !digits(5, 2, month) || s[7] != '-' ||
        !digits(8, 2, day))
        return std::nullopt;
    size_t at = 10;
    int64_t fraction = 0;
    if (at < s.size() && (s[at] == 'T' || s[at] == ' ')) {
        if (!digits(at + 1, 2, hour) || s.size() < at + 6 || s[at + 3] != ':' || !digits(at + 4, 2, minute))
            return std::nullopt;
        at += 6;
        if (at < s.size() && s[at] == ':') {
            if (!digits(at + 1, 2, second)) return std::nullopt;
            at += 3;
            if (at < s.size() && s[at] == '.') {
                at++;
                int n = 0;
                while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
                    if (n < 7) {
                        fraction = fraction * 10 + (s[at] - '0');
                        n++;
                    }
                    at++;
                }
                if (n == 0) return std::nullopt;
                for (; n < 7; n++) fraction *= 10;
            }
        }
    }
    int64_t offsetMinutes = 0;
    if (at < s.size()) {
        if (s[at] == 'Z' || s[at] == 'z')
            at++;
        else if (s[at] == '+' || s[at] == '-') {
            int oh, om;
            if (!digits(at + 1, 2, oh)) return std::nullopt;
            size_t next = at + 3;
            if (next < s.size() && s[next] == ':') next++;
            if (!digits(next, 2, om)) return std::nullopt;
            offsetMinutes = (oh * 60 + om) * (s[at] == '-' ? -1 : 1);
            at = next + 2;
        } else
            return std::nullopt;
    }
    if (at != s.size() || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
        return std::nullopt;
    int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second - offsetMinutes * 60;
    return UnixEpochTicks + seconds * TicksPerSecond + fraction;
}

std::string FormatIsoTicks(int64_t ticks) {
    int64_t unix = ticks - UnixEpochTicks;
    int64_t seconds = unix >= 0 ? unix / TicksPerSecond : -((-unix + TicksPerSecond - 1) / TicksPerSecond);
    int64_t fraction = unix - seconds * TicksPerSecond;
    int64_t days = seconds >= 0 ? seconds / 86400 : -((-seconds + 86399) / 86400);
    int64_t rest = seconds - days * 86400;
    int64_t y;
    unsigned m, d;
    CivilFromDays(days, y, m, d);
    char buffer[64];
    snprintf(buffer, sizeof buffer, "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%07lld+00:00", static_cast<long long>(y), m,
             d, static_cast<long long>(rest / 3600), static_cast<long long>(rest / 60 % 60),
             static_cast<long long>(rest % 60), static_cast<long long>(fraction));
    return buffer;
}

std::string FormatLocalDate(int64_t utcTicks) {
    int64_t ft = utcTicks - FileTimeEpochTicks;
    FILETIME utc{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)}, local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &st);
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buffer[32];
    snprintf(buffer, sizeof buffer, "%02u %s %04u", st.wDay, months[(st.wMonth + 11) % 12], st.wYear);
    return buffer;
}

void CancelToken::ThrowIfCanceled() const {
    if (IsCanceled()) Throw(ErrorKind::Canceled, "The operation was canceled.");
}

uint64_t CancelToken::Register(std::function<void()> callback) const {
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->canceled) {
            uint64_t id = state_->next++;
            state_->callbacks.emplace_back(id, std::move(callback));
            return id;
        }
    }
    callback();
    return 0;
}

void CancelToken::Unregister(uint64_t id) const {
    if (id == 0) return;
    std::lock_guard lock(state_->mutex);
    auto& list = state_->callbacks;
    list.erase(std::remove_if(list.begin(), list.end(), [id](auto& p) { return p.first == id; }), list.end());
}

bool CancelToken::Sleep(int milliseconds) const {
    std::unique_lock lock(state_->mutex);
    return !state_->wake.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                  [&] { return state_->canceled.load(); });
}

CancelToken CancelToken::None() {
    return CancelToken();
}

static void CancelState(const std::shared_ptr<CancelToken::State>& state);

CancelSource::CancelSource(const CancelToken& parent) : parent_(parent) {
    auto state = token_.state_;
    parentRegistration_ = parent.Register([state] { CancelState(state); });
}

CancelSource::~CancelSource() {
    if (parent_) parent_->Unregister(parentRegistration_);
}

static void CancelState(const std::shared_ptr<CancelToken::State>& state) {
    std::vector<std::pair<uint64_t, std::function<void()>>> callbacks;
    {
        std::lock_guard lock(state->mutex);
        if (state->canceled.exchange(true)) return;
        callbacks.swap(state->callbacks);
    }
    state->wake.notify_all();
    for (auto& [id, callback] : callbacks) {
        try {
            callback();
        } catch (...) {
        }
    }
}

void CancelSource::Cancel() {
    CancelState(token_.state_);
}

void CancelSource::CancelAfter(int milliseconds) {
    auto state = token_.state_;
    std::thread([state, milliseconds] {
        std::unique_lock lock(state->mutex);
        if (!state->wake.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                  [&] { return state->canceled.load(); })) {
            lock.unlock();
            CancelState(state);
        }
    }).detach();
}

std::wstring LongPath(const fs::path& path) {
    std::wstring p = path.wstring();
    if (p.empty() || p.starts_with(LR"(\\?\)")) return p;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n == 0) return p;
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, full.data(), nullptr);
    full.resize(n);
    if (full.starts_with(LR"(\\?\)")) return full;
    if (full.starts_with(LR"(\\)")) return LR"(\\?\UNC\)" + full.substr(2);
    return LR"(\\?\)" + full;
}

Bytes ReadAllBytes(const fs::path& path) {
    File file(path, File::Mode::Read, true);
    uint64_t length = file.Length();
    if (length > 0x7FFFFFFFull) Throw(ErrorKind::IO, "File too large: " + Narrow(path.wstring()));
    Bytes out(static_cast<size_t>(length));
    if (length) file.ReadExactly(out.data(), out.size());
    return out;
}

std::string ReadAllText(const fs::path& path) {
    Bytes bytes = ReadAllBytes(path);
    size_t skip = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
    return std::string(bytes.begin() + skip, bytes.end());
}

void WriteAllBytes(const fs::path& path, std::span<const uint8_t> data) {
    File file(path, File::Mode::Create);
    if (!data.empty()) file.Write(data.data(), data.size());
    file.Close();
}

void WriteAllText(const fs::path& path, std::string_view text) {
    WriteAllBytes(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

bool FileExists(const fs::path& path) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool DirExists(const fs::path& path) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool PathExists(const fs::path& path) {
    return GetFileAttributesW(LongPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

void MoveReplace(const fs::path& from, const fs::path& to) {
    if (!MoveFileExW(LongPath(from).c_str(), LongPath(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        ThrowFile("Cannot move to", to, GetLastError());
}
void MoveNoReplace(const fs::path& from, const fs::path& to) {
    if (!MoveFileExW(LongPath(from).c_str(), LongPath(to).c_str(), 0)) ThrowFile("Cannot move to", to, GetLastError());
}
void CopyFileTo(const fs::path& from, const fs::path& to, bool overwrite) {
    if (!CopyFileW(LongPath(from).c_str(), LongPath(to).c_str(), overwrite ? FALSE : TRUE))
        ThrowFile("Cannot copy to", to, GetLastError());
}
void DeleteFileIfExists(const fs::path& path) {
    if (!DeleteFileW(LongPath(path).c_str())) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) ThrowFile("Cannot delete", path, e);
    }
}
void CreateDirectories(const fs::path& path) {
    std::wstring full = LongPath(path);
    if (DirExists(full)) return;
    size_t first = full.starts_with(LR"(\\?\UNC\)") ? full.find(L'\\', full.find(L'\\', 8) + 1) : full.find(L'\\', 4);
    for (size_t at = full.find(L'\\', first + 1);; at = full.find(L'\\', at + 1)) {
        std::wstring part = at == std::wstring::npos ? full : full.substr(0, at);
        if (!DirExists(part) && !CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            ThrowFile("Cannot create", path, GetLastError());
        if (at == std::wstring::npos) break;
    }
}
void DeleteTree(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(fs::path(LongPath(path)), ec);
    if (ec)
        Throw(ErrorKind::IO,
              "Cannot delete " + Narrow(path.wstring()) + ": " + SystemMessage(static_cast<DWORD>(ec.value())),
              ec.value());
}
uint64_t FileLength(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &data))
        ThrowFile("Cannot read", path, GetLastError());
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}
bool IsReparsePoint(const fs::path& path) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) ThrowFile("Cannot read", path, GetLastError());
    return (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static std::vector<fs::path> Enumerate(const fs::path& directory, std::wstring_view pattern, int which) {
    std::vector<fs::path> out;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW((LongPath(directory) + L"\\" + std::wstring(pattern)).c_str(), FindExInfoBasic,
                                   &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES) return out;
        ThrowFile("Cannot list", directory, e);
    }
    do {
        std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        bool dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (which == 2 || (which == 1) == dir) out.push_back(directory / name);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return out;
}
std::vector<fs::path> ListFiles(const fs::path& directory, std::wstring_view pattern) {
    auto all = Enumerate(directory, pattern, 0);
    return all;
}
std::vector<fs::path> ListDirectories(const fs::path& directory) {
    return Enumerate(directory, L"*", 1);
}
std::vector<fs::path> ListEntries(const fs::path& directory) {
    return Enumerate(directory, L"*", 2);
}
bool IsDirectoryEmpty(const fs::path& directory) {
    return Enumerate(directory, L"*", 2).empty();
}

fs::path FullPath(const fs::path& path) {
    std::wstring in = path.wstring();
    DWORD n = GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
    if (n == 0) Throw(ErrorKind::Argument, "Invalid path: " + Narrow(in));
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(in.c_str(), n, out.data(), nullptr);
    out.resize(n);
    return out;
}

fs::path PathRoot(const fs::path& path) {
    return path.root_name().wstring() + path.root_directory().wstring();
}

std::wstring TrimEndSeparator(std::wstring_view path) {
    std::wstring s(path);
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 2, buffer);
    return TrimEndSeparator(std::wstring(buffer, n));
}

fs::path ExePath() {
    std::wstring buffer(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(n);
    return buffer;
}
fs::path ExeDirectory() {
    return ExePath().parent_path();
}

fs::path KnownFolder(const void* folderId) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(*static_cast<const KNOWNFOLDERID*>(folderId), KF_FLAG_DONT_VERIFY, nullptr, &path)))
        Throw(ErrorKind::IO, "Windows did not report a user folder.");
    fs::path out = path;
    CoTaskMemFree(path);
    return out;
}
fs::path LocalAppData() {
    return KnownFolder(&FOLDERID_LocalAppData);
}

File::File(const fs::path& path, Mode mode, bool shareDelete) : path_(path) {
    DWORD access = GENERIC_READ, share = FILE_SHARE_READ, disposition = OPEN_EXISTING;
    switch (mode) {
        case Mode::Read:
            share = FILE_SHARE_READ | (shareDelete ? FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0);
            break;
        case Mode::CreateNew:
            access = GENERIC_READ | GENERIC_WRITE;
            share = 0;
            disposition = CREATE_NEW;
            break;
        case Mode::Create:
            access = GENERIC_READ | GENERIC_WRITE;
            share = 0;
            disposition = CREATE_ALWAYS;
            break;
        case Mode::ReadWriteExisting:
            access = GENERIC_READ | GENERIC_WRITE;
            share = 0;
            disposition = OPEN_EXISTING;
            break;
    }
    handle_ = CreateFileW(LongPath(path).c_str(), access, share, nullptr, disposition,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
        ThrowFile(mode == Mode::Read ? "Cannot read" : "Cannot write", path, GetLastError());
}
File::~File() {
    Close();
}
void File::Close() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = nullptr;
}
uint64_t File::Length() const {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_, &size)) ThrowFile("Cannot read", path_, GetLastError());
    return static_cast<uint64_t>(size.QuadPart);
}
void File::Seek(uint64_t position) {
    LARGE_INTEGER to;
    to.QuadPart = static_cast<LONGLONG>(position);
    if (!SetFilePointerEx(handle_, to, nullptr, FILE_BEGIN)) ThrowFile("Cannot seek", path_, GetLastError());
}
uint64_t File::Position() const {
    LARGE_INTEGER zero{}, at;
    SetFilePointerEx(handle_, zero, &at, FILE_CURRENT);
    return static_cast<uint64_t>(at.QuadPart);
}
size_t File::Read(void* buffer, size_t size) {
    DWORD read = 0;
    if (!ReadFile(handle_, buffer, static_cast<DWORD>(std::min<size_t>(size, 1u << 30)), &read, nullptr))
        ThrowFile("Cannot read", path_, GetLastError());
    return read;
}
void File::ReadExactly(void* buffer, size_t size) {
    auto p = static_cast<uint8_t*>(buffer);
    while (size > 0) {
        size_t n = Read(p, size);
        if (n == 0) Throw(ErrorKind::IO, "Unexpected end of " + Narrow(path_.wstring()));
        p += n;
        size -= n;
    }
}
void File::Write(const void* data, size_t size) {
    auto p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(handle_, p, static_cast<DWORD>(std::min<size_t>(size, 1u << 30)), &written, nullptr))
            ThrowFile("Cannot write", path_, GetLastError());
        p += written;
        size -= written;
    }
}
void File::Flush() {
    FlushFileBuffers(handle_);
}

std::wstring QuoteArgument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring out = L"\"";
    for (size_t i = 0;; i++) {
        size_t slashes = 0;
        while (i < argument.size() && argument[i] == L'\\') {
            i++;
            slashes++;
        }
        if (i == argument.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(slashes, L'\\');
            out += argument[i];
        }
    }
    return out + L"\"";
}

std::wstring JoinArguments(const std::vector<std::wstring>& arguments) {
    std::wstring out;
    for (auto& a : arguments) {
        if (!out.empty()) out += L' ';
        out += QuoteArgument(a);
    }
    return out;
}

std::optional<int64_t> ProcessStartTicks(void* processHandle) {
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(processHandle, &creation, &exit, &kernel, &user)) return std::nullopt;
    return static_cast<int64_t>((static_cast<uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime) +
           FileTimeEpochTicks;
}

std::vector<ProcessInfo> ProcessesByName(std::wstring_view exeName, bool requirePath) {
    std::vector<ProcessInfo> out;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) ThrowLastError("Cannot list processes");
    PROCESSENTRY32W entry{sizeof entry};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (!IEquals(entry.szExeFile, exeName)) continue;
        ProcessInfo info{entry.th32ProcessID, L"", 0};
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process) {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(process, &exitCode) && exitCode != STILL_ACTIVE) {
                CloseHandle(process);
                continue;
            }
            std::wstring path(32768, L'\0');
            DWORD n = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &n)) {
                path.resize(n);
                info.exePath = path;
            }
            info.startUtcTicks = ProcessStartTicks(process).value_or(0);
            CloseHandle(process);
        }
        if (info.exePath.empty() && requirePath) {
            CloseHandle(snapshot);
            Throw(ErrorKind::Win32, "Access is denied.");
        }
        out.push_back(std::move(info));
    }
    CloseHandle(snapshot);
    return out;
}

static std::vector<uint8_t> TokenUser() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) ThrowLastError("Cannot read the Windows user");
    DWORD size = 0;
    GetTokenInformation(token, ::TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> buffer(size);
    BOOL ok = GetTokenInformation(token, ::TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok) ThrowLastError("Cannot read the Windows user");
    return buffer;
}

std::wstring CurrentUserSid() {
    auto buffer = TokenUser();
    LPWSTR text = nullptr;
    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text);
    std::wstring sid = text ? text : L"";
    if (text) LocalFree(text);
    return sid;
}

std::wstring CurrentUserName() {
    auto buffer = TokenUser();
    wchar_t name[256], domain[256];
    DWORD nameSize = 256, domainSize = 256;
    SID_NAME_USE use;
    if (!LookupAccountSidW(nullptr, reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, name, &nameSize, domain,
                           &domainSize, &use))
        return L"";
    return std::wstring(domain) + L"\\" + name;
}

bool IsElevated() {
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &admins))
        return false;
    BOOL member = FALSE;
    CheckTokenMembership(nullptr, admins, &member);
    FreeSid(admins);
    return member != FALSE;
}

uint32_t CurrentProcessId() {
    return GetCurrentProcessId();
}

std::optional<Version> Version::Parse(std::string_view text) {
    std::string s = Trim(text);
    std::vector<int> parts;
    size_t at = 0;
    while (true) {
        size_t dot = s.find('.', at);
        std::string piece = s.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
        if (piece.empty() || piece.size() > 9 ||
            !std::all_of(piece.begin(), piece.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        parts.push_back(std::stoi(piece));
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
    if (parts.size() < 2 || parts.size() > 4) return std::nullopt;
    Version v;
    v.major = parts[0];
    v.minor = parts[1];
    if (parts.size() > 2) v.build = parts[2];
    if (parts.size() > 3) v.revision = parts[3];
    return v;
}

int Version::Compare(const Version& o) const {
    for (auto [a, b] : {std::pair{major, o.major}, {minor, o.minor}, {build, o.build}, {revision, o.revision}})
        if (a != b) return a < b ? -1 : 1;
    return 0;
}

Version LauncherVersion() {
    return Version{RWND_VERSION_MAJOR, RWND_VERSION_MINOR, RWND_VERSION_PATCH, 0};
}
std::string DisplayVersion() {
    return std::to_string(RWND_VERSION_MAJOR) + "." + std::to_string(RWND_VERSION_MINOR) + "." +
           std::to_string(RWND_VERSION_PATCH);
}

}
