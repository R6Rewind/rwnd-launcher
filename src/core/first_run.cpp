#include "core/first_run.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include "core/instance_folders.h"
#include "core/json.h"

namespace rwnd::first_run {

std::vector<fs::path> FolderCandidates(const std::string& gamePath, const std::optional<std::string>& steamFolder,
                                       const fs::path& defaultFolder, const std::vector<fs::path>& driveRoots) {
    std::vector<fs::path> out;
    std::vector<std::wstring> seen;
    auto add = [&](const std::wstring& raw) {
        std::wstring trimmed = TrimEndSeparator(TrimW(raw));
        if (trimmed.empty()) return;
        std::wstring key = ToLowerW(trimmed);
        for (auto& s : seen)
            if (s == key) return;
        seen.push_back(key);
        out.emplace_back(trimmed);
    };
    add(Widen(gamePath));
    if (steamFolder) add(Widen(*steamFolder));
    add(defaultFolder.wstring());
    for (auto& root : driveRoots) {
        add((root / L"Games" / L"RWND" / L"ParaBellum").wstring());
        add((root / L"Games" / L"ParaBellum").wstring());
        add((root / L"ParaBellum").wstring());
    }
    return out;
}

std::optional<fs::path> DetectGameFolder(const std::vector<fs::path>& candidates,
                                         const std::function<bool(const fs::path&)>& good) {
    for (auto& folder : candidates) {
        try {
            if (FileExists(folder / L"RainbowSix.exe") && good(folder)) return folder;
        } catch (const Error& e) {
            if (e.kind == ErrorKind::Canceled) throw;
        }
    }
    return std::nullopt;
}

std::vector<fs::path> FixedDriveRoots() {
    std::vector<fs::path> roots;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_FIXED) roots.emplace_back(root);
    }
    return roots;
}

bool FetchOutbreak(bool live, bool ready, int64_t freeBytes, int64_t gameStillNeeded, int64_t outbreakStillNeeded) {
    if (!live || ready) return false;
    return std::max<int64_t>(0, gameStillNeeded) + std::max<int64_t>(0, outbreakStillNeeded) < freeBytes;
}

std::string CheckKind(const BuildCheck& check, bool unfinishedDownload) {
    return CheckKind(check, unfinishedDownload, std::nullopt);
}

std::optional<DownloadStop> StopFor(const std::optional<DownloadStop>& saved, const fs::path& folder) {
    if (!saved || saved->folder.empty() || folder.empty()) return std::nullopt;
    if (!IEquals(TrimEndSeparator(TrimW(Widen(saved->folder))), TrimEndSeparator(TrimW(folder.wstring()))))
        return std::nullopt;
    return saved;
}

std::string CheckKind(const BuildCheck& check, bool unfinishedDownload, const std::optional<DownloadStop>& stop) {
    if (check.ok) return "ok";
    if (unfinishedDownload && check.unfinished && !(stop && stop->kind == "done")) return "partial";
    return check.kind;
}

std::string StopReason(const std::optional<DownloadStop>& stop, bool running) {
    const std::string resume = "Picks up where it stopped.";
    if (!stop) return resume;
    if (stop->kind == "stopped") return "You stopped it. " + resume;
    if (stop->kind == "running") return running ? resume : "RWND closed before it finished. " + resume;
    if (stop->kind == "error") {
        std::string text = Trim(stop->text);
        if (text.empty()) return resume;
        if (text.size() > 240) {
            size_t cut = 237;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;
            text = text.substr(0, cut) + "...";
        }
        return text;
    }
    return resume;
}

}

namespace rwnd::first_run {

Size ParseSize(const std::string& name) {
    if (IEqualsA(name, "medium")) return Size::Medium;
    if (IEqualsA(name, "small")) return Size::Small;
    return Size::Full;
}

const char* SizeName(Size size) {
    return size == Size::Medium ? "medium" : size == Size::Small ? "small" : "full";
}

namespace {

const wchar_t* const KeptArchives[] = {L"datapc64_merged_bnk_011965738_textures1.forge",
                                       L"datapc64_merged_bnk_011965738_textures2.forge",
                                       L"datapc64_merged_bnk_011965738_textures3.forge"};

std::wstring FileName(std::wstring_view relativePath) {
    std::wstring path = ToLowerW(relativePath);
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::pair<std::wstring, int> Tier(const std::wstring& name) {
    static const std::wstring marker = L"_textures";
    static const std::wstring forge = L".forge";
    if (name.size() < marker.size() + 1 + forge.size() ||
        name.compare(name.size() - forge.size(), forge.size(), forge) != 0)
        return {L"", -1};
    size_t digit = name.size() - forge.size() - 1;
    wchar_t c = name[digit];
    if (c < L'0' || c > L'9' || name.compare(digit - marker.size(), marker.size(), marker) != 0) return {L"", -1};
    return {name.substr(0, digit - marker.size()), c - L'0'};
}

const std::set<std::wstring>& BanksWithoutBase() {
    static const std::set<std::wstring> banks = [] {
        std::set<std::wstring> withTiers, withBase;
        for (auto& e : game_files::Manifest()) {
            auto [bank, tier] = Tier(FileName(e.path));
            if (tier < 0) continue;
            withTiers.insert(bank);
            if (tier == 0) withBase.insert(bank);
        }
        std::set<std::wstring> out;
        for (auto& b : withTiers)
            if (!withBase.contains(b)) out.insert(b);
        return out;
    }();
    return banks;
}

bool Kept(const std::wstring& name) {
    for (auto kept : KeptArchives)
        if (name == kept) return true;
    return false;
}

}

bool LeftOut(std::wstring_view relativePath, Size size) {
    if (size == Size::Full) return false;
    std::wstring name = FileName(relativePath);
    auto [bank, tier] = Tier(name);
    if (tier < 0 || Kept(name) || name.find(L"_fury_") != std::wstring::npos) return false;
    if (tier >= 2) return true;
    return tier == 1 && size == Size::Small && !BanksWithoutBase().contains(bank);
}

std::string FileList(Size size) {
    if (size == Size::Full) return "";
    std::string list =
        size == Size::Medium ? "regex:^(?!.*_textures[23]\\.forge$).*\n" : "regex:^(?!.*_textures[123]\\.forge$).*\n";
    std::set<std::string> names;
    for (auto& e : game_files::Manifest()) {
        std::wstring name = FileName(e.path);
        if (Tier(name).second >= (size == Size::Medium ? 2 : 1) && !LeftOut(e.path, size)) names.insert(Narrow(e.path));
    }
    for (auto kept : KeptArchives) names.insert(Narrow(kept));
    for (auto& n : names) list += n + "\n";
    return list;
}

std::vector<game_files::Entry> Kept(std::span<const game_files::Entry> manifest, Size size) {
    std::vector<game_files::Entry> out;
    for (auto& e : manifest)
        if (!LeftOut(e.path, size)) out.push_back(e);
    return out;
}

int64_t LeftOutBytes(std::span<const game_files::Entry> manifest, Size size) {
    int64_t total = 0;
    for (auto& e : manifest)
        if (LeftOut(e.path, size)) total += static_cast<int64_t>(e.size);
    return total;
}

std::vector<fs::path> LeftOutFiles(const fs::path& root, Size size) {
    std::vector<fs::path> out;
    if (!DirExists(root)) return out;
    for (auto& file : ListFiles(root))
        if (LeftOut(file.filename().wstring(), size)) out.push_back(file);
    return out;
}

namespace {

bool IsCopyOf(const fs::path& copy, const fs::path& source) {
    if (InstanceFolders::Within(copy.wstring(), source.wstring()) ||
        InstanceFolders::Within(source.wstring(), copy.wstring()))
        return false;
    fs::path marker = copy / L"instance-source.json";
    if (!FileExists(marker)) return false;
    try {
        Json recorded = ParseJson(ReadAllText(marker));
        return recorded.is_string() &&
               IEquals(TrimEndSeparator(Widen(recorded.get<std::string>())), TrimEndSeparator(source.wstring()));
    } catch (const Error&) {
        return false;
    }
}

std::optional<std::string> IniValue(const std::string& ini, const std::string& section, const std::string& key) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    bool inside = false;
    for (size_t at = 0; at < ini.size();) {
        size_t end = ini.find('\n', at);
        std::string line = Trim(ini.substr(at, (end == std::string::npos ? ini.size() : end) - at));
        if (!line.empty() && line[0] == '[')
            inside = lower(line) == "[" + lower(section) + "]";
        else if (size_t eq = line.find('=');
                 inside && eq != std::string::npos && lower(Trim(line.substr(0, eq))) == lower(key))
            return Trim(line.substr(eq + 1));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return std::nullopt;
}

}

std::vector<fs::path> AccountCopies(const fs::path& game, const std::vector<fs::path>& roots) {
    std::vector<fs::path> out;
    if (game.empty()) return out;
    fs::path source = FullPath(game);
    std::set<std::wstring> seen;
    for (auto& root : roots) {
        if (root.empty() || !DirExists(root)) continue;
        for (auto& dir : ListDirectories(root)) {
            fs::path copy = FullPath(dir);
            if (seen.insert(ToLowerW(copy.wstring())).second && IsCopyOf(copy, source)) out.push_back(copy);
        }
    }
    return out;
}

int LeaveOutOfCopies(const fs::path& game, const std::vector<fs::path>& roots, Size size) {
    int removed = 0;
    for (auto& copy : AccountCopies(game, roots)) {
        InstanceFolders::RequireNoReparse(copy);
        for (auto& file : LeftOutFiles(copy, size)) try {
                DeleteFileIfExists(file);
                removed++;
            } catch (const Error&) {
            }
    }
    return removed;
}

bool FreesGameFolder(bool releaseBuild, bool devDataDir) {
    return releaseBuild && !devDataDir;
}

std::vector<fs::path> SwitchDownFiles(const fs::path& game, const std::vector<fs::path>& roots, Size size,
                                      bool gameFolder) {
    std::vector<fs::path> files;
    for (auto& copy : AccountCopies(game, roots)) {
        InstanceFolders::RequireNoReparse(copy);
        for (auto& file : LeftOutFiles(copy, size)) files.push_back(file);
    }
    if (gameFolder && !game.empty() && DirExists(game)) {
        InstanceFolders::RequireNoReparse(FullPath(game));
        for (auto& file : LeftOutFiles(FullPath(game), size)) files.push_back(file);
    }
    return files;
}

namespace {
struct FileIdentity {
    bool ok = false;
    uint64_t key[2]{};
    DWORD links = 0;
    int64_t size = 0;
};
FileIdentity Identify(const fs::path& file) {
    FileIdentity out;
    HANDLE h = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h, &info)) {
        out.ok = true;
        out.key[0] = info.dwVolumeSerialNumber;
        out.key[1] = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        out.links = info.nNumberOfLinks;
        out.size = static_cast<int64_t>((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
    }
    CloseHandle(h);
    return out;
}
}

int64_t FreedBytes(const std::vector<fs::path>& files) {
    struct Seen {
        DWORD links = 0, listed = 0;
        int64_t size = 0;
    };
    std::map<std::pair<uint64_t, uint64_t>, Seen> seen;
    std::set<std::wstring> paths;
    for (auto& file : files) {
        if (!paths.insert(ToLowerW(FullPath(file).wstring())).second) continue;
        auto id = Identify(file);
        if (!id.ok) continue;
        auto& entry = seen[{id.key[0], id.key[1]}];
        entry.links = id.links;
        entry.size = id.size;
        entry.listed++;
    }
    int64_t total = 0;
    for (auto& [key, entry] : seen)
        if (entry.listed >= entry.links) total += entry.size;
    return total;
}

Freed RemoveFiles(const std::vector<fs::path>& files) {
    Freed out;
    out.bytes = FreedBytes(files);
    for (auto& file : files) try {
            if (FileExists(file)) {
                DeleteFileIfExists(file);
                out.removed++;
            }
        } catch (const Error&) {
        }
    return out;
}

int LinkIntoCopy(const fs::path& copyIn, const fs::path& game, Size size) {
    fs::path copy = FullPath(copyIn), source = FullPath(game);
    if (!IsCopyOf(copy, source)) return 0;
    InstanceFolders::RequireNoReparse(copy);
    int linked = 0;
    for (auto& e : game_files::Manifest()) {
        if (!LeftOut(e.path, Size::Small) || LeftOut(e.path, size)) continue;
        fs::path target = copy / e.path, original = source / e.path;
        if (FileExists(target) || !FileExists(original)) continue;
        InstanceFolders::MakeHardLink(target, original);
        linked++;
    }
    return linked;
}

int LinkIntoCopies(const fs::path& game, const std::vector<fs::path>& roots, Size size) {
    int linked = 0;
    for (auto& copy : AccountCopies(game, roots)) linked += LinkIntoCopy(copy, game, size);
    return linked;
}

std::vector<game_files::Entry> MissingFiles(const fs::path& game, std::span<const game_files::Entry> manifest,
                                            Size size) {
    std::vector<game_files::Entry> out;
    for (auto& e : manifest) {
        if (LeftOut(e.path, size)) continue;
        fs::path file = game / e.path;
        if (!FileExists(file) || FileLength(file) != e.size) out.push_back(e);
    }
    return out;
}

std::string MissingFileList(std::span<const game_files::Entry> missing) {
    std::string list;
    for (auto& e : missing) {
        std::string name = Narrow(e.path);
        for (auto& c : name)
            if (c == '\\') c = '/';
        list += name + "\n";
    }
    return list;
}

std::string LiftTexturePreset(const std::string& ini, Size from, Size to) {
    if (static_cast<int>(to) >= static_cast<int>(from)) return ini;
    auto value = IniValue(ini, "QUALITY_SETTINGS", "TexturePresetName");
    if (!value || !IEqualsA(*value, from == Size::Small ? "Low" : "Medium")) return ini;
    return SetIniValue(ini, "QUALITY_SETTINGS", "TexturePresetName", to == Size::Full ? "Very High" : "Medium");
}

std::string CapTexturePreset(const std::string& ini, Size size) {
    if (size == Size::Full) return ini;
    const std::string ceiling = size == Size::Small ? "Low" : "Medium";
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto allowed = [&](const std::string& value) {
        return value == "low" || (size == Size::Medium && value == "medium");
    };
    std::string text = ini;
    std::string newline = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
    size_t section = std::string::npos;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, (end == std::string::npos ? text.size() : end) - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lower(Trim(line)) == "[quality_settings]") {
            section = end == std::string::npos ? text.size() : end + 1;
            break;
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    if (section == std::string::npos) {
        if (!text.empty() && text.back() != '\n') text += newline;
        return text + "[QUALITY_SETTINGS]" + newline + "TexturePresetName=" + ceiling + newline;
    }
    for (size_t at = section; at < text.size();) {
        size_t end = text.find('\n', at);
        size_t stop = end == std::string::npos ? text.size() : end;
        std::string line = text.substr(at, stop - at);
        bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        std::string trimmed = Trim(line);
        if (!trimmed.empty() && trimmed[0] == '[') break;
        size_t eq = trimmed.find('=');
        if (eq != std::string::npos && lower(Trim(trimmed.substr(0, eq))) == "texturepresetname") {
            if (allowed(lower(Trim(trimmed.substr(eq + 1))))) return text;
            return text.substr(0, at) + "TexturePresetName=" + ceiling + (cr ? "\r" : "") + text.substr(stop);
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return text.substr(0, section) + "TexturePresetName=" + ceiling + newline + text.substr(section);
}

}

namespace rwnd::first_run {

std::optional<double> ParseMouseMultiplier(const std::string& text) {
    std::string t = Trim(text);
    if (t.empty() || t.size() > 12) return std::nullopt;
    int dots = 0, decimals = 0, digits = 0;
    for (char c : t) {
        if (c == '.') {
            if (++dots > 1) return std::nullopt;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        digits++;
        if (dots) decimals++;
    }
    if (digits == 0 || decimals > 6) return std::nullopt;
    double value = std::strtod(t.c_str(), nullptr);
    if (!(value >= 0.001 - 1e-12 && value <= 0.1 + 1e-12)) return std::nullopt;
    return value;
}

std::string FormatMouseMultiplier(double value) {
    char buffer[32];
    snprintf(buffer, sizeof buffer, "%.6f", value);
    return buffer;
}

std::string SetIniValue(const std::string& ini, const std::string& section, const std::string& key,
                        const std::string& value) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string header = "[" + lower(section) + "]", name = lower(key);
    std::string text = ini;
    std::string newline = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
    size_t start = std::string::npos;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, (end == std::string::npos ? text.size() : end) - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lower(Trim(line)) == header) {
            start = end == std::string::npos ? text.size() : end + 1;
            break;
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    if (start == std::string::npos) {
        if (!text.empty() && text.back() != '\n') text += newline;
        return text + "[" + section + "]" + newline + key + "=" + value + newline;
    }
    if (start == text.size() && !text.empty() && text.back() != '\n') {
        text += newline;
        start = text.size();
    }
    for (size_t at = start; at < text.size();) {
        size_t end = text.find('\n', at);
        size_t stop = end == std::string::npos ? text.size() : end;
        std::string line = text.substr(at, stop - at);
        bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        std::string trimmed = Trim(line);
        if (!trimmed.empty() && trimmed[0] == '[') break;
        size_t eq = trimmed.find('=');
        if (eq != std::string::npos && lower(Trim(trimmed.substr(0, eq))) == name)
            return text.substr(0, at) + key + "=" + value + (cr ? "\r" : "") + text.substr(stop);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return text.substr(0, start) + key + "=" + value + newline + text.substr(start);
}

}

namespace rwnd::first_run {

std::optional<int> ParseFpsLimit(const std::string& text) {
    std::string t = Trim(text);
    if (t.empty() || t.size() > 4) return std::nullopt;
    for (char c : t)
        if (c < '0' || c > '9') return std::nullopt;
    int value = std::atoi(t.c_str());
    if (value < 30 || value > 1000) return std::nullopt;
    return value;
}

}
