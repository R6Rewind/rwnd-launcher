#include "core/log_vault.h"

#include "core/crash_reports.h"
#include "core/retention.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rwnd::log_vault {

namespace {

constexpr uint64_t MaxFile = retention::MaxLogBytes;
constexpr char Prepend[] = "RWND log bundle v2";

void Put32(Bytes& b, uint32_t v) {
    uint8_t x[4];
    std::memcpy(x, &v, 4);
    b.insert(b.end(), x, x + 4);
}
void Put64(Bytes& b, uint64_t v) {
    uint8_t x[8];
    std::memcpy(x, &v, 8);
    b.insert(b.end(), x, x + 8);
}
void Put16(Bytes& b, uint16_t v) {
    uint8_t x[2];
    std::memcpy(x, &v, 2);
    b.insert(b.end(), x, x + 2);
}

struct Alg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~Alg() {
        if (h) BCryptCloseAlgorithmProvider(h, 0);
    }
};
struct Key {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~Key() {
        if (h) BCryptDestroyKey(h);
    }
};
struct Secret {
    BCRYPT_SECRET_HANDLE h = nullptr;
    ~Secret() {
        if (h) BCryptDestroySecret(h);
    }
};
void Check(NTSTATUS status, const char* what) {
    if (status < 0) Throw(ErrorKind::InvalidOperation, std::string("Log encryption failed (") + what + ").");
}

Bytes AgreeKey(const ProfileKey& recipient, uint8_t ephemeralOut[65]) {
    Alg alg;
    Check(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0), "ecdh");
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), recipient.x.data(), 32);
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB) + 32, recipient.y.data(), 32);
    Key theirs;
    Check(BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &theirs.h, blob.data(),
                              static_cast<ULONG>(blob.size()), 0),
          "public key");
    Key mine;
    Check(BCryptGenerateKeyPair(alg.h, &mine.h, 256, 0), "ephemeral");
    Check(BCryptFinalizeKeyPair(mine.h, 0), "ephemeral");
    Bytes exported(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    ULONG size = 0;
    Check(BCryptExportKey(mine.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, exported.data(), static_cast<ULONG>(exported.size()),
                          &size, 0),
          "export");
    ephemeralOut[0] = 4;
    std::memcpy(ephemeralOut + 1, exported.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    Secret secret;
    Check(BCryptSecretAgreement(mine.h, theirs.h, &secret.h, 0), "agreement");
    BCryptBuffer buffers[3] = {
        {static_cast<ULONG>(sizeof(BCRYPT_SHA256_ALGORITHM)), KDF_HASH_ALGORITHM,
         const_cast<wchar_t*>(BCRYPT_SHA256_ALGORITHM)},
        {static_cast<ULONG>(sizeof(Prepend)), KDF_SECRET_PREPEND, const_cast<char*>(Prepend)},
        {65, KDF_SECRET_APPEND, ephemeralOut},
    };
    BCryptBufferDesc parameters{BCRYPTBUFFER_VERSION, 3, buffers};
    Bytes aesKey(32);
    ULONG derived = 0;
    Check(BCryptDeriveKey(secret.h, BCRYPT_KDF_HASH, &parameters, aesKey.data(), 32, &derived, 0), "derive");
    Require(derived == 32, "Log encryption failed (key length).");
    return aesKey;
}

void Gcm(std::span<const uint8_t> aesKey, std::span<const uint8_t> nonce, std::span<const uint8_t> aad, Bytes& data,
         uint8_t tag[16]) {
    Alg alg;
    Check(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_AES_ALGORITHM, nullptr, 0), "aes");
    Check(BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE,
                            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                            static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0),
          "gcm");
    Key key;
    Check(BCryptGenerateSymmetricKey(alg.h, &key.h, nullptr, 0, const_cast<PUCHAR>(aesKey.data()),
                                     static_cast<ULONG>(aesKey.size()), 0),
          "key");
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO mode;
    BCRYPT_INIT_AUTH_MODE_INFO(mode);
    mode.pbNonce = const_cast<PUCHAR>(nonce.data());
    mode.cbNonce = static_cast<ULONG>(nonce.size());
    mode.pbAuthData = const_cast<PUCHAR>(aad.data());
    mode.cbAuthData = static_cast<ULONG>(aad.size());
    mode.pbTag = tag;
    mode.cbTag = 16;
    ULONG written = 0;
    Bytes out(data.size());
    Check(BCryptEncrypt(key.h, data.empty() ? nullptr : data.data(), static_cast<ULONG>(data.size()), &mode, nullptr, 0,
                        out.empty() ? nullptr : out.data(), static_cast<ULONG>(out.size()), &written, 0),
          "encrypt");
    SecureZeroMemory(data.data(), data.size());
    data = std::move(out);
}

std::string Stamp() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buffer[40];
    snprintf(buffer, sizeof buffer, "%04u%02u%02u-%02u%02u%02u-", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    return buffer + HexLower(RandomBytes(3));
}

struct Held {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Held() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

}

std::optional<ProfileKey> ProfileKey::FromJson(const Json& json) {
    try {
        if (!json.is_object()) return std::nullopt;
        ProfileKey key;
        key.keyId = GetString(json, "keyId").value_or("");
        auto id = GetGuid(json, "playerId");
        key.x = Base64Decode(GetString(json, "x").value_or(""));
        key.y = Base64Decode(GetString(json, "y").value_or(""));
        if (key.keyId.empty() || key.keyId.size() > 16 || !id || id->IsEmpty() || key.x.size() != 32 ||
            key.y.size() != 32)
            return std::nullopt;
        key.playerId = *id;
        return key;
    } catch (const Error&) {
        return std::nullopt;
    }
}

Json ProfileKey::ToJson() const {
    return {{"keyId", keyId}, {"playerId", playerId.D()}, {"x", Base64Encode(x)}, {"y", Base64Encode(y)}};
}

const std::vector<std::wstring>& Names() {
    static const std::vector<std::wstring> names = {
        L"rewind.log",        L"rewind-native-route.log",     L"rewind-overlay.log",
        L"rewind-steam.log",  L"RWND-Parabellum-physics.log", L"rewind-session.json",
        L"rewind-content.log"};
    return names;
}

void SaveKey(const fs::path& root, const ProfileKey& key) {
    fs::path folder = root / Folder;
    CreateDirectories(folder);
    fs::path file = folder / L"key.json", temporary = file.wstring() + L".tmp";
    WriteAllText(temporary, ToJson(key.ToJson()));
    MoveReplace(temporary, file);
}

std::optional<ProfileKey> LoadKey(const fs::path& root) {
    try {
        fs::path file = root / Folder / L"key.json";
        if (!FileExists(file)) return std::nullopt;
        return ProfileKey::FromJson(ParseJson(ReadAllText(file)));
    } catch (const Error&) {
        return std::nullopt;
    }
}

Bytes Encrypt(const std::vector<std::pair<std::string, Bytes>>& files, const Json& headerJson, const ProfileKey& key) {
    Bytes plain;
    for (auto& [name, data] : files) {
        Require(name.size() < 65536, "Log name too long.");
        Put16(plain, static_cast<uint16_t>(name.size()));
        plain.insert(plain.end(), name.begin(), name.end());
        Put64(plain, data.size());
        plain.insert(plain.end(), data.begin(), data.end());
    }
    std::string header = headerJson.dump();
    uint8_t ephemeral[65];
    Bytes aesKey = AgreeKey(key, ephemeral);
    Bytes nonce = RandomBytes(12);
    Bytes out = {'R', 'W', 'N', 'D', 'L', 'O', 'G', '2'};
    Put32(out, static_cast<uint32_t>(header.size()));
    out.insert(out.end(), header.begin(), header.end());
    out.push_back(65);
    out.insert(out.end(), ephemeral, ephemeral + 65);
    out.insert(out.end(), nonce.begin(), nonce.end());
    Bytes aad = out;
    uint8_t tag[16];
    Gcm(aesKey, nonce, aad, plain, tag);
    SecureZeroMemory(aesKey.data(), aesKey.size());
    Put64(out, plain.size());
    out.insert(out.end(), plain.begin(), plain.end());
    out.insert(out.end(), tag, tag + 16);
    return out;
}

std::string ExitCodeText(uint32_t code) {
    char hex[16];
    snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(code));
    return hex;
}

static std::optional<CrashBundles> SealFiles(const fs::path& root, const std::string& reason,
                                             std::optional<uint32_t> exitCode, bool crash) {
    try {
        if (!DirExists(root)) return std::nullopt;
        auto key = LoadKey(root);
        if (!key) return std::nullopt;
        std::vector<std::pair<std::string, Bytes>> files;
        std::vector<std::unique_ptr<Held>> held;
        Json list = Json::array();

        auto addFile = [&](const std::wstring& name, uint64_t cap, bool mayTruncate) {
            fs::path path = root / name;
            if (!FileExists(path) || IsReparsePoint(path)) return false;
            auto file = std::make_unique<Held>();
            file->h = CreateFileW(LongPath(path).c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file->h == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file->h, &size) || size.QuadPart < 0) return false;
            const auto length = static_cast<uint64_t>(size.QuadPart);
            if (!mayTruncate && (length > cap || length == 0)) return false;
            const uint64_t skip = length > cap ? length - cap : 0;
            Bytes data(static_cast<size_t>(length - skip));
            LARGE_INTEGER at{};
            at.QuadPart = static_cast<LONGLONG>(skip);
            if (!SetFilePointerEx(file->h, at, nullptr, FILE_BEGIN)) return false;
            for (size_t done = 0; done < data.size();) {
                DWORD n = 0;
                if (!ReadFile(file->h, data.data() + done,
                              static_cast<DWORD>(std::min<size_t>(data.size() - done, 1 << 24)), &n, nullptr) ||
                    n == 0)
                    return false;
                done += n;
            }
            list.push_back(
                {{"name", Narrow(name)}, {"size", length}, {"truncated", skip > 0}, {"sha256", Sha256Hex(data)}});
            files.emplace_back(Narrow(name), std::move(data));
            held.push_back(std::move(file));
            return true;
        };

        for (const auto& name : Names()) {
            if (crash && name == L"rewind-session.json") continue;
            addFile(name, crash ? 512ULL * 1024 : MaxFile, true);
        }

        if (crash && files.empty()) {
            auto text = "RWND recorded an unexpected game exit";
            Bytes data(text, text + std::strlen(text));
            list.push_back({{"name", "rewind-native-route.log"},
                            {"size", data.size()},
                            {"truncated", false},
                            {"sha256", Sha256Hex(data)}});
            files.emplace_back("rewind-native-route.log", std::move(data));
        }
        if (files.empty()) return std::nullopt;

        Json header = {{"format", 2},
                       {"keyId", key->keyId},
                       {"playerId", key->playerId.D()},
                       {"created", FormatIsoTicks(UtcNowTicks())},
                       {"launcher", DisplayVersion()},
                       {"reason", reason}};
        if (exitCode) header["exitCode"] = ExitCodeText(*exitCode);
        fs::path folder = root / Folder;
        CreateDirectories(folder);
        const auto stamp = Widen(Stamp());
        auto write = [&](const fs::path& target) {
            header["files"] = list;
            Bytes bundle = Encrypt(files, header, *key);
            fs::path temporary = target.wstring() + L".tmp";
            WriteAllBytes(temporary, bundle);
            MoveReplace(temporary, target);
        };
        CrashBundles out{folder / (stamp + L".rwndlog"), {}};
        write(out.log);
        if (crash && addFile(DumpName, 8ULL * 1024 * 1024, false)) {
            const auto withDump = folder / (stamp + L"-with-dump.rwndlog");
            try {
                write(withDump);
                out.withDump = withDump;
            } catch (...) {
                held.pop_back();
            }
        }
        for (auto& [name, data] : files) SecureZeroMemory(data.data(), data.size());

        for (auto& file : held) {
            FILE_DISPOSITION_INFO dispose{TRUE};
            SetFileInformationByHandle(file->h, FileDispositionInfo, &dispose, sizeof dispose);
        }
        held.clear();
        retention::TrimBundles(root, crash_reports::NeededBundles(crash_reports::QueueDirectory()));
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<fs::path> Seal(const fs::path& root, const std::string& reason, std::optional<uint32_t> exitCode) {
    auto saved = SealFiles(root, reason, exitCode, false);
    return saved ? std::optional(saved->log) : std::nullopt;
}

std::optional<CrashBundles> SealCrash(const fs::path& root, std::optional<uint32_t> exitCode) {
    return SealFiles(root, "crash", exitCode, true);
}
}
