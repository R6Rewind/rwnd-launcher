#include "core/game.h"

#include <windows.h>

#include <algorithm>

#include "core/instance_folders.h"
#include "core/installation.h"
#include "core/log_vault.h"
#include "core/settings.h"

namespace rwnd {

namespace game_build {

static BuildCheck Fail(std::string title, std::string reason, const char* kind = "fix") {
    return {false, std::move(title), std::move(reason), kind};
}
static BuildCheck Unfinished(BuildCheck check) {
    check.unfinished = true;
    return check;
}

BuildCheck Verify(const std::wstring& folder, const CancelToken& cancel) {
    if (TrimW(folder).empty())
        return Fail("Choose a folder", "Pick the folder where Para Bellum is installed.", "missing");
    fs::path root;
    try {
        root = FullPath(TrimW(folder));
    } catch (const Error&) {
        return Fail("Not a valid folder", "Check the folder path and try again.");
    }
    if (!DirExists(root)) return Fail("Folder not found", "That folder doesn't exist.", "missing");
    try {
        InstanceFolders::RequireNoReparse(root);
    } catch (const Error& e) {
        if (e.kind == ErrorKind::InvalidData)
            return Fail("Linked folder", "Pick the real game folder, not a shortcut, link or junction.");
        throw;
    }
    fs::path exe = root / L"RainbowSix.exe";
    if (!FileExists(exe)) {
        std::vector<std::string> nested;
        for (auto& d : ListDirectories(root))
            if (FileExists(d / L"RainbowSix.exe")) nested.push_back(Narrow(d.filename().wstring()));
        if (nested.empty())
            return Unfinished(Fail("RainbowSix.exe not found",
                                   "This folder doesn't have RainbowSix.exe. Pick the folder that does.", "missing"));
        if (nested.size() == 1)
            return Fail("RainbowSix.exe not found",
                        "RainbowSix.exe is one folder down, in \"" + nested[0] + "\". Pick that folder.");
        std::string list;
        for (auto& n : nested) list += (list.empty() ? "" : ", ") + n;
        return Fail("RainbowSix.exe not found",
                    "Game folders are one level down (" + list + "). Pick the Para Bellum one.");
    }
    try {
        std::string exeHash = Sha256File(exe, &cancel);
        if (exeHash == "89620adbbad8996faa0dc75bbd6b334cde2ee35df260f42bed233bee926e6ca3")
            return Fail("Wrong Siege version", std::string("This is Burnt Horizon (Y3S1). RWND needs ") + Name + ".",
                        "other");
    } catch (const Error& e) {
        if (e.kind == ErrorKind::IO || e.kind == ErrorKind::UnauthorizedAccess)
            return Fail("Can't read the game",
                        "Windows didn't let RWND read the game files. Close the game and try again.");
        throw;
    }
    if (!InstanceFolders::SupportsHardLinks(root)) {
        std::string drive = Narrow(TrimEndSeparator(PathRoot(root).wstring()));
        if (!drive.empty() && drive.back() == '\\') drive.pop_back();
        return Fail("Wrong drive",
                    "Drive " + drive +
                        " isn't NTFS, so RWND can't link each account's copy there. Move the game to an NTFS drive.");
    }
    return {true, std::string(Name) + " found", "RainbowSix.exe and the game data match the supported build.", "ok"};
}

}

namespace windowed_optimizations {

std::string Merge(const std::string& value) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= value.size()) {
        size_t semi = value.find(';', at);
        std::string part = Trim(value.substr(at, semi == std::string::npos ? std::string::npos : semi - at));
        if (!part.empty() && !(part.size() >= 24 && IEqualsA(part.substr(0, 24), "SwapEffectUpgradeEnable=")))
            parts.push_back(part);
        if (semi == std::string::npos) break;
        at = semi + 1;
    }
    parts.push_back("SwapEffectUpgradeEnable=0");
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) out += (i ? ";" : "") + parts[i];
    return out + ";";
}

void TurnOff(const fs::path& gameExe) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        Throw(ErrorKind::UnauthorizedAccess, "Cannot open the graphics preferences.");
    std::wstring name = gameExe.wstring(), current;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_SZ &&
        size > 0) {
        current.resize(size / sizeof(wchar_t));
        RegQueryValueExW(key, name.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(current.data()), &size);
        while (!current.empty() && current.back() == L'\0') current.pop_back();
    }
    std::wstring updated = Widen(Merge(Narrow(current)));
    if (updated != current)
        RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(updated.c_str()),
                       static_cast<DWORD>((updated.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

}

}
