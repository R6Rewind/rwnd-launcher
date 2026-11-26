#include "core/save_recovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <vector>

#include "core/json.h"

namespace rwnd::save_recovery {
namespace {

fs::path Saves(const fs::path& root) {
    return root / L"uplay-saves";
}
fs::path Store(const fs::path& root) {
    return root / L"RWND-save-backups";
}
fs::path StateFile(const fs::path& root) {
    return Store(root) / L"state.json";
}
fs::path Good(const fs::path& root, int n) {
    return Store(root) / (L"good-" + std::to_wstring(n));
}
fs::path RunStart(const fs::path& root) {
    return Store(root) / L"run-start";
}

int64_t Seconds(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

std::vector<fs::path> SaveFiles(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path p = it->path();
        if (it->is_regular_file(ec) && _wcsicmp(p.extension().c_str(), L".sav") == 0) out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool HasSaves(const fs::path& dir) {
    return !SaveFiles(dir).empty();
}

bool CopySaves(const fs::path& from, const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    for (const auto& old : SaveFiles(dir)) {
        fs::remove(old, ec);
        if (ec) return false;
    }
    for (const auto& src : SaveFiles(from)) {
        const fs::path tmp = dir / (src.filename().wstring() + L".tmp");
        fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
        fs::rename(tmp, dir / src.filename(), ec);
        if (ec) return false;
    }
    return true;
}

bool MoveSaves(const fs::path& from, const fs::path& dir) {
    std::error_code ec;
    if (SaveFiles(from).empty()) return true;
    fs::create_directories(dir, ec);
    if (ec) return false;
    for (const auto& src : SaveFiles(from)) {
        fs::rename(src, dir / src.filename(), ec);
        if (ec) return false;
    }
    return true;
}

void ReplaceDir(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::rename(from, to, ec);
}

void TrimCrashed(const fs::path& root) {
    std::error_code ec;
    const fs::path dir = Store(root) / L"crashed";
    std::vector<fs::path> all;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) all.push_back(it->path());
    std::sort(all.begin(), all.end());
    while (static_cast<int>(all.size()) > KeepCrashed) {
        fs::remove_all(all[1], ec);
        all.erase(all.begin() + 1);
    }
}

std::wstring Stamp(Clock::time_point now) {
    const std::time_t t = Clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &t);
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buf;
}

Json LoadState(const fs::path& root) {
    try {
        if (FileExists(StateFile(root))) {
            Json j = ParseJson(ReadAllText(StateFile(root)));
            if (j.is_object()) return j;
        }
    } catch (...) {
    }
    return Json::object();
}

void SaveState(const fs::path& root, const Json& state) {
    try {
        CreateDirectories(Store(root));
        const fs::path tmp = StateFile(root).wstring() + L".tmp";
        WriteAllText(tmp, ToJson(state));
        std::error_code ec;
        fs::rename(tmp, StateFile(root), ec);
    } catch (...) {
    }
}

std::string Str(const Json& j, const char* key, const char* fallback) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}
int64_t Int(const Json& j, const char* key, int64_t fallback) {
    auto it = j.find(key);
    return it != j.end() && it->is_number_integer() ? it->get<int64_t>() : fallback;
}

uint32_t Crc32(const uint8_t* data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t U32(const Bytes& b, size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

void TrimFolders(const fs::path& dir, int keep) {
    std::error_code ec;
    std::vector<fs::path> all;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) all.push_back(it->path());
    std::sort(all.begin(), all.end());
    while (static_cast<int>(all.size()) > keep) {
        fs::remove_all(all.front(), ec);
        all.erase(all.begin());
    }
}

std::string StripSaves(const fs::path& root, Clock::time_point now) {
    int total = 0;
    std::string skipped;
    for (const auto& file : SaveFiles(Saves(root))) {
        try {
            Bytes bytes = ReadAllBytes(file);
            const Bytes original = bytes;
            int removed = 0;
            std::string reason;
            if (!StripPackRecords(bytes, removed, reason)) {
                skipped = reason;
                continue;
            }
            if (removed == 0) continue;
            const fs::path keep = Store(root) / L"pack-records" / Stamp(now);
            CreateDirectories(keep);
            WriteAllBytes(keep / file.filename(), original);
            const fs::path tmp = file.wstring() + L".tmp";
            WriteAllBytes(tmp, bytes);
            std::error_code ec;
            fs::rename(tmp, file, ec);
            if (ec) {
                fs::remove(tmp, ec);
                skipped = "write";
                continue;
            }
            total += removed;
        } catch (...) {
            skipped = "error";
        }
    }
    if (total) TrimFolders(Store(root) / L"pack-records", KeepCrashed);
    if (total) return "removed-" + std::to_string(total);
    return skipped.empty() ? "none" : "skipped-" + skipped;
}

}

bool SaveLooksValid(const fs::path& file) {
    try {
        const Bytes b = ReadAllBytes(file);
        if (b.size() < 16) return false;
        return U32(b, 12) == b.size() - 16 && U32(b, 8) == Crc32(b.data() + 16, b.size() - 16);
    } catch (...) {
        return false;
    }
}

bool IsPleaseWaitCrash(const std::string& routeLog) {
    auto lower = routeLog;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("rainbowsix.exe+0x5a7260") != std::string::npos ||
        lower.find("rainbowsix.exe+0x125f7fd") != std::string::npos)
        return true;
    const std::string module = "rainbowsix.exe+0x";
    for (size_t at = lower.find(module); at != std::string::npos; at = lower.find(module, at + 1)) {
        uint64_t rva = 0;
        size_t i = at + module.size(), digits = 0;
        for (; i < lower.size() && digits < 12 && std::isxdigit(static_cast<unsigned char>(lower[i])); ++i, ++digits)
            rva = rva * 16 + static_cast<uint64_t>(lower[i] <= '9' ? lower[i] - '0' : lower[i] - 'a' + 10);
        if (digits && rva >= StartupRewardFunction && rva < StartupRewardFunction + StartupRewardFunctionSize)
            return true;
    }
    return false;
}

bool StripPackRecords(Bytes& b, int& removed, std::string& reason) {
    removed = 0;
    reason.clear();
    static const uint8_t magic[4] = {0xac, 0xdb, 0xfe, 0x00};
    if (b.size() < 16 || U32(b, 0) != 13 || !std::equal(magic, magic + 4, b.begin() + 4) ||
        U32(b, 12) != b.size() - 16 || U32(b, 8) != Crc32(b.data() + 16, b.size() - 16)) {
        reason = "not-a-valid-save";
        return false;
    }
    static const uint8_t marker[4] = {0x7d, 0x3e, 0x68, 0xc1};
    const auto first = std::search(b.begin() + 16, b.end(), marker, marker + 4);
    if (first == b.end()) {
        reason = "no-container";
        return false;
    }
    if (std::search(first + 1, b.end(), marker, marker + 4) != b.end()) {
        reason = "container-twice";
        return false;
    }
    const size_t p = static_cast<size_t>(first - b.begin()) + 4;
    if (p + PackPrefix + 4 > b.size() || b[p] != 0x01) {
        reason = "layout";
        return false;
    }
    const uint32_t count = U32(b, p + PackPrefix);
    if (count > 64) {
        reason = "count";
        return false;
    }
    const size_t records = p + PackPrefix + 4, after = records + static_cast<size_t>(count) * PackRecordSize;
    static const uint8_t anchor[7] = {0x03, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01};
    if (after + PackSuffix + sizeof anchor > b.size() ||
        !std::equal(anchor, anchor + sizeof anchor, b.begin() + after + PackSuffix)) {
        reason = "anchor";
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = records + static_cast<size_t>(i) * PackRecordSize;
        if (U32(b, e) != 1 || U32(b, e + 4) != PackRecordKey) {
            reason = "record";
            return false;
        }
    }
    if (count == 0) return true;
    Bytes out(b.begin(), b.begin() + records);
    for (int i = 0; i < 4; ++i) out[p + PackPrefix + i] = 0;
    out.insert(out.end(), b.begin() + after, b.end());
    const uint32_t len = static_cast<uint32_t>(out.size() - 16), crc = Crc32(out.data() + 16, out.size() - 16);
    for (int i = 0; i < 4; ++i) {
        out[12 + i] = static_cast<uint8_t>(len >> (8 * i));
        out[8 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
    b = std::move(out);
    removed = static_cast<int>(count);
    return true;
}

std::string BeforeRun(const fs::path& root, Clock::time_point now) {
    try {
        Json state = LoadState(root);
        const std::string last = Str(state, "lastOutcome", "none");
        int64_t step = Int(state, "recoveryStep", 0);
        std::string line = "save ";
        const fs::path saves = Saves(root);
        if (last == "please-wait-crash") {
            const fs::path quarantine = Store(root) / L"crashed" / Stamp(now);
            if (!MoveSaves(saves, quarantine)) {
                line += "recovery=failed-quarantine";
            } else {
                TrimCrashed(root);
                int restored = 0;
                for (int n = static_cast<int>(step) + 1; n <= Generations && !restored; ++n)
                    if (HasSaves(Good(root, n)) && CopySaves(Good(root, n), saves)) restored = n;
                step = restored ? restored : Generations + 1;
                line += restored ? "recovery=restored good-" + std::to_string(restored) : std::string("recovery=fresh");
                line += " quarantined=" +
                        (fs::exists(quarantine) ? Narrow(quarantine.filename().wstring()) : std::string("none"));
            }
        } else {
            step = 0;
            line += "recovery=none";
        }
        line += " packrecords=" + StripSaves(root, now);
        std::error_code ec;
        fs::remove_all(RunStart(root), ec);
        const auto files = SaveFiles(saves);
        bool valid = true;
        for (const auto& f : files) valid = valid && SaveLooksValid(f);
        const bool snap = files.empty() ? false : CopySaves(saves, RunStart(root));
        state["lastOutcome"] = "running";
        state["runStartedAt"] = Seconds(now);
        state["recoveryStep"] = step;
        state["runSnapshot"] = snap;
        state["runSnapshotValid"] = snap && valid;
        SaveState(root, state);
        return line + " files=" + std::to_string(files.size()) + " valid=" + (valid ? "1" : "0");
    } catch (...) {
        return "save recovery=error";
    }
}

std::string AfterRun(const fs::path& root, bool abnormalExit, const std::string& routeLog, Clock::time_point now) {
    try {
        Json state = LoadState(root);
        if (Str(state, "lastOutcome", "none") != "running") return "save after=no-run";
        const int64_t runtime = Seconds(now) - Int(state, "runStartedAt", Seconds(now));
        if (abnormalExit && runtime >= 0 && runtime < EarlyExitSeconds && IsPleaseWaitCrash(routeLog)) {
            state["lastOutcome"] = "please-wait-crash";
            SaveState(root, state);
            return "save after=please-wait-crash runtime=" + std::to_string(runtime) + "s next=recover";
        }
        if (abnormalExit && runtime >= 0 && runtime < EarlyExitSeconds) {
            state["lastOutcome"] = "early-crash";
            state["recoveryStep"] = 0;
            SaveState(root, state);
            return "save after=early-crash runtime=" + std::to_string(runtime) + "s promoted=no";
        }
        std::string promoted = "no";
        auto snap = state.find("runSnapshotValid");
        if (snap != state.end() && snap->is_boolean() && snap->get<bool>() && HasSaves(RunStart(root))) {
            std::error_code ec;
            fs::remove_all(Good(root, Generations), ec);
            for (int n = Generations - 1; n >= 1; --n)
                if (fs::exists(Good(root, n), ec)) fs::rename(Good(root, n), Good(root, n + 1), ec);
            ReplaceDir(RunStart(root), Good(root, 1));
            promoted = HasSaves(Good(root, 1)) ? "good-1" : "failed";
        }
        state["lastOutcome"] = "ok";
        state["recoveryStep"] = 0;
        SaveState(root, state);
        return "save after=ok runtime=" + std::to_string(runtime) + "s promoted=" + promoted;
    } catch (...) {
        return "save after=error";
    }
}

}
