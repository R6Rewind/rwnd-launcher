#pragma once
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/util.h"

namespace rwnd {

class Pak;

namespace payload {
bool IsEmbedded();
std::optional<std::span<const uint8_t>> Find(const std::string& name);
std::vector<std::string> Names(const std::string& folder);
int ExtractTo(const fs::path& directory);
int RemoveUnpacked(const fs::path& directory,
                   const std::function<bool(const std::string&, const std::string&)>& shipped = nullptr);
int RemoveUnpacked(const Pak& pak, const fs::path& directory,
                   const std::function<bool(const std::string&, const std::string&)>& shipped);
std::vector<std::wstring> Files();
}

class Installation {
public:
    static constexpr const wchar_t* ExeName = L"RWND.exe";
    static constexpr const wchar_t* LegacyExeName = L"RWND Parabellum.exe";
    static constexpr const wchar_t* ProductName = L"RWND Parabellum";
    static constexpr const wchar_t* MarkerName = L".rwnd-install";

    static fs::path ExeIn(const fs::path& directory);
    static fs::path DefaultDirectory();
    static bool InProtectedFolder(const fs::path& directory);
    static bool HasOwnPermissions(const fs::path& directory);
    static bool InheritPermissions(const fs::path& directory);
    static bool RemoveOldInstall(const fs::path& old, const std::vector<std::wstring>& notices);
    static bool IsReleaseBuild();
    static bool IsInstalledHere();
    static fs::path InstalledExe();
    static std::optional<std::string> InstallId();
    static std::optional<fs::path> RegisteredDirectory();
    static fs::path ResolveTarget(const std::wstring& chosen);

    static fs::path Install(const std::wstring& chosen, bool desktopShortcut, bool startMenuShortcut,
                            const std::function<void(const std::string&)>& progress);
    static bool RefreshInstalled(bool tray);
    static void Uninstall();
    static void EnsureWritable();
    static int PrepareFolder(const std::wstring& directory, const std::wstring& userSid);

    static bool StartsWithWindows();
    static void SetStartsWithWindows(bool value);

    static std::string MarkerJson(const std::string& installId);

private:
    static bool MigrateExeName(const fs::path& directory, bool tray);
    static bool MoveToProgramFiles(bool tray);
    static void FinishMove();
    static void Register(const fs::path& directory, const fs::path& exe);
    static std::vector<fs::path> Shortcuts();
    static bool CanWrite(const fs::path& directory);
    static void PrepareFolderElevated(const fs::path& directory);
};

void CreateShortcut(const fs::path& link, const fs::path& target);

}
