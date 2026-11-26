#include "core/installation.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <numeric>

#include "core/json.h"
#include "core/pak.h"
#include "core/patch_files.h"
#include "core/process.h"

using Microsoft::WRL::ComPtr;

namespace rwnd {

namespace payload {

static const Pak& Embedded() {
    static const Pak pak = Pak::FromResource(L"PAYLOAD");
    return pak;
}

bool IsEmbedded() {
    return !Embedded().Empty();
}

static bool Unpacked(const std::string& name) {
    return name.find('/') == std::string::npos;
}

std::optional<std::span<const uint8_t>> Find(const std::string& name) {
    if (IsEmbedded()) {
        auto entry = Embedded().Find(name);
        return entry ? std::optional(entry->bytes) : std::nullopt;
    }
    static std::mutex mutex;
    static std::map<std::string, Bytes> loaded;
    std::lock_guard lock(mutex);
    if (auto it = loaded.find(name); it != loaded.end()) return std::span<const uint8_t>(it->second);
    std::wstring relative = Widen(name);
    std::replace(relative.begin(), relative.end(), L'/', L'\\');
    fs::path file = ExeDirectory() / relative;
    if (name.find("..") != std::string::npos || !FileExists(file)) return std::nullopt;
    auto& bytes = loaded[name] = ReadAllBytes(file);
    return std::span<const uint8_t>(bytes);
}

std::vector<std::string> Names(const std::string& folder) {
    std::vector<std::string> out;
    if (IsEmbedded()) {
        for (auto& entry : Embedded().Entries())
            if (entry.name.starts_with(folder + "/")) out.push_back(entry.name);
    } else if (DirExists(ExeDirectory() / Widen(folder))) {
        for (auto& f : ListFiles(ExeDirectory() / Widen(folder)))
            out.push_back(folder + "/" + Narrow(f.filename().wstring()));
    }
    return out;
}

int RemoveUnpacked(const fs::path& directory,
                   const std::function<bool(const std::string&, const std::string&)>& shipped) {
    return IsEmbedded() ? RemoveUnpacked(Embedded(), directory, shipped) : 0;
}

int RemoveUnpacked(const Pak& pak, const fs::path& directory,
                   const std::function<bool(const std::string&, const std::string&)>& shipped) {
    int removed = 0;
    std::vector<fs::path> folders;
    for (auto& entry : pak.Entries()) {
        if (Unpacked(entry.name)) continue;
        std::wstring relative = Widen(entry.name);
        std::replace(relative.begin(), relative.end(), L'/', L'\\');
        fs::path target = directory / relative;
        if (std::find(folders.begin(), folders.end(), target.parent_path()) == folders.end())
            folders.push_back(target.parent_path());
        if (!FileExists(target)) continue;
        try {
            std::string hash = Sha256File(target);
            if (hash != Sha256Hex(entry.bytes) && !(shipped && shipped(entry.name, hash))) continue;
            DeleteFileIfExists(target);
            removed++;
        } catch (const Error&) {
        }
    }
    fs::path patches = directory / L"Patches";
    if (shipped && DirExists(patches)) {
        if (std::find(folders.begin(), folders.end(), patches) == folders.end()) folders.push_back(patches);
        for (auto& file : ListFiles(patches)) {
            std::string name = "Patches/" + Narrow(file.filename().wstring());
            if (!ToLowerAscii(name).ends_with(".dll") || pak.Find(name)) continue;
            try {
                if (shipped(name, Sha256File(file))) {
                    DeleteFileIfExists(file);
                    removed++;
                }
            } catch (const Error&) {
            }
        }
    }
    for (auto& folder : folders)
        if (DirExists(folder) && IsDirectoryEmpty(folder)) RemoveDirectoryW(LongPath(folder).c_str());
    return removed;
}

int ExtractTo(const fs::path& directory) {
    fs::path root = FullPath(directory);
    std::wstring rootText = TrimEndSeparator(root.wstring()) + L"\\";
    int written = 0;
    for (auto& entry : Embedded().Entries()) {
        if (!Unpacked(entry.name)) continue;
        std::wstring relative = Widen(entry.name);
        std::replace(relative.begin(), relative.end(), L'/', L'\\');
        fs::path target = FullPath(root / relative);
        if (!IStartsWithW(target.wstring(), rootText))
            Throw(ErrorKind::InvalidData, "Invalid payload path: " + entry.name);
        if (FileExists(target) && Sha256File(target) == Sha256Hex(entry.bytes)) continue;
        CreateDirectories(target.parent_path());
        fs::path temporary = target.wstring() + L".tmp";
        WriteAllBytes(temporary, entry.bytes);
        MoveReplace(temporary, target);
        written++;
    }
    return written;
}

std::vector<std::wstring> Files() {
    std::vector<std::wstring> out;
    for (auto& entry : Embedded().Entries()) {
        std::wstring relative = Widen(entry.name);
        std::replace(relative.begin(), relative.end(), L'/', L'\\');
        out.push_back(relative);
    }
    return out;
}

}

static constexpr const wchar_t* FolderName = L"RWND";
static constexpr const wchar_t* UninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RWND.Parabellum";
static constexpr const wchar_t* RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

fs::path Installation::ExeIn(const fs::path& directory) {
    return FileExists(directory / LegacyExeName) && !FileExists(directory / ExeName) ? directory / LegacyExeName
                                                                                     : directory / ExeName;
}

fs::path Installation::DefaultDirectory() {
    return KnownFolder(&FOLDERID_ProgramFiles) / FolderName;
}

bool Installation::InProtectedFolder(const fs::path& directory) {
    const std::wstring path = TrimEndSeparator(FullPath(directory).wstring()) + L"\\";
    bool under = false;
    for (auto id : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86}) {
        const std::wstring root = TrimEndSeparator(FullPath(KnownFolder(id)).wstring()) + L"\\";
        under = under || (root.size() > 3 && path.size() > root.size() && IStartsWithW(path, root));
    }
    return under && !(DirExists(directory) && IsReparsePoint(directory));
}

bool Installation::HasOwnPermissions(const fs::path& directory) {
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(directory.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl,
                              nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    bool own = acl == nullptr;
    for (DWORD i = 0; acl && i < acl->AceCount; i++) {
        LPVOID ace = nullptr;
        if (GetAce(acl, i, &ace) && !(static_cast<ACE_HEADER*>(ace)->AceFlags & INHERITED_ACE)) own = true;
    }
    if (descriptor) LocalFree(descriptor);
    return own;
}

bool Installation::InheritPermissions(const fs::path& directory) {
    ACL empty;
    if (!InitializeAcl(&empty, sizeof empty, ACL_REVISION)) return false;
    return SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                 &empty, nullptr) == ERROR_SUCCESS;
}

bool Installation::IsReleaseBuild() {
#ifdef RWND_RELEASE
    return payload::IsEmbedded();
#else
    return false;
#endif
}

bool Installation::IsInstalledHere() {
    return FileExists(ExeDirectory() / MarkerName);
}
fs::path Installation::InstalledExe() {
    return ExeDirectory() / ExeName;
}

struct Marker {
    std::optional<std::string> version, installId;
};
static std::optional<Marker> ReadMarker(const fs::path& file) {
    if (!FileExists(file)) return std::nullopt;
    Json j = ParseJson(ReadAllText(file));
    if (!j.is_object()) return Marker{};
    return Marker{GetString(j, "version"), GetString(j, "installId")};
}

std::string Installation::MarkerJson(const std::string& installId) {
    Json j = Json::object();
    j["version"] = DisplayVersion();
    j["installedAt"] = FormatIsoTicks(UtcNowTicks());
    j["installId"] = installId;
    return ToJson(j);
}

std::optional<std::string> Installation::InstallId() {
    try {
        auto marker = ReadMarker(ExeDirectory() / MarkerName);
        return marker ? marker->installId : std::nullopt;
    } catch (const Error&) {
        return std::nullopt;
    }
}

static std::optional<std::wstring> RegString(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buffer[2048];
    DWORD size = sizeof buffer, type = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, &type, buffer, &size) != ERROR_SUCCESS) return std::nullopt;
    return std::wstring(buffer);
}

std::optional<fs::path> Installation::RegisteredDirectory() {
    auto path = RegString(HKEY_CURRENT_USER, UninstallKey, L"InstallLocation");
    if (path && FileExists(fs::path(*path) / MarkerName)) return fs::path(*path);
    return std::nullopt;
}

fs::path Installation::ResolveTarget(const std::wstring& chosen) {
    (void)chosen;
    if (auto registered = RegisteredDirectory(); registered && InProtectedFolder(*registered)) return *registered;
    return DefaultDirectory();
}

void CreateShortcut(const fs::path& link, const fs::path& target) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Uninit {
        HRESULT hr;
        ~Uninit() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } uninit{init};
    ComPtr<IShellLinkW> shell;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))))
        Throw(ErrorKind::InvalidOperation, "Windows Script Host is unavailable.");
    CreateDirectories(link.parent_path());
    shell->SetPath(target.c_str());
    shell->SetWorkingDirectory(target.parent_path().c_str());
    shell->SetIconLocation(target.c_str(), 0);
    shell->SetDescription(L"Play Parabellum on RWND");
    ComPtr<IPersistFile> file;
    shell.As(&file);
    if (!file || FAILED(file->Save(link.c_str(), TRUE)))
        Throw(ErrorKind::IO, "Cannot create the shortcut " + Narrow(link.wstring()));
}

std::vector<fs::path> Installation::Shortcuts() {
    return {KnownFolder(&FOLDERID_Programs) / (std::wstring(ProductName) + L".lnk"),
            KnownFolder(&FOLDERID_Desktop) / (std::wstring(ProductName) + L".lnk")};
}

static uint64_t DirectoryBytes(const fs::path& directory) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(directory, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

void Installation::Register(const fs::path& directory, const fs::path& exe) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        Throw(ErrorKind::UnauthorizedAccess, "Windows did not allow registering RWND Parabellum.");
    auto setString = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    auto setDword = [&](const wchar_t* name, DWORD value) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
    };
    setString(L"DisplayName", ProductName);
    setString(L"DisplayVersion", Widen(DisplayVersion()));
    setString(L"Publisher", L"RWND");
    setString(L"DisplayIcon", exe.wstring() + L",0");
    setString(L"InstallLocation", directory.wstring());
    setString(L"UninstallString", L"\"" + exe.wstring() + L"\" --uninstall");
    setDword(L"NoModify", 1);
    setDword(L"NoRepair", 1);
    setDword(L"EstimatedSize", static_cast<DWORD>(DirectoryBytes(directory) / 1024));
    RegCloseKey(key);
}

bool Installation::CanWrite(const fs::path& directory) {
    try {
        CreateDirectories(directory);
        fs::path probe = directory / (L".write-test-" + Widen(Guid::New().N()));
        WriteAllText(probe, "");
        DeleteFileIfExists(probe);
        return true;
    } catch (const Error& e) {
        if (e.kind == ErrorKind::UnauthorizedAccess) return false;
        throw;
    }
}

void Installation::PrepareFolderElevated(const fs::path& directory) {
    std::wstring sid = CurrentUserSid();
    if (sid.empty()) Throw(ErrorKind::InvalidOperation, "Cannot identify the Windows user.");
    auto code = process::RunElevated(ExePath(), {L"--prepare-folder", directory.wstring(), sid}, {}, true);
    if (!code)
        Throw(ErrorKind::UnauthorizedAccess,
              "That folder needs administrator permission. Allow the Windows prompt, or use a folder like " +
                  Narrow(DefaultDirectory().wstring()) + ".");
    if (*code != 0)
        Throw(ErrorKind::UnauthorizedAccess, "Windows could not prepare that folder. Choose another folder.");
}

int Installation::PrepareFolder(const std::wstring& directory, const std::wstring& userSid) {
    fs::path full = FullPath(directory);
    if (PathRoot(full) == full || (DirExists(full) && !IsDirectoryEmpty(full) && !FileExists(full / MarkerName)))
        return 2;
    CreateDirectories(full);
    if (InProtectedFolder(full)) return 0;
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(userSid.c_str(), &sid)) return 1;
    PACL current = nullptr, updated = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    int result = 1;
    if (GetNamedSecurityInfoW(full.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &current,
                              nullptr, &descriptor) == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        if (SetEntriesInAclW(1, &access, current, &updated) == ERROR_SUCCESS &&
            SetNamedSecurityInfoW(const_cast<LPWSTR>(full.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                  nullptr, updated, nullptr) == ERROR_SUCCESS)
            result = 0;
    }
    if (updated) LocalFree(updated);
    if (descriptor) LocalFree(descriptor);
    LocalFree(sid);
    return result;
}

fs::path Installation::Install(const std::wstring& chosen, bool desktopShortcut, bool startMenuShortcut,
                               const std::function<void(const std::string&)>& progress) {
    fs::path directory = ResolveTarget(chosen);
    progress("Preparing " + Narrow(directory.wstring()));
    if (!CanWrite(directory)) {
        progress("Waiting for Windows permission");
        PrepareFolderElevated(directory);
        if (!CanWrite(directory))
            Throw(ErrorKind::UnauthorizedAccess,
                  "Windows did not allow writing to that folder. Choose another folder.");
    }
    fs::path source = ExePath(), target = directory / ExeName;
    if (!IEquals(FullPath(source).wstring(), target.wstring())) {
        progress("Copying launcher");
        fs::path temporary = target.wstring() + L".download";
        CopyFileTo(source, temporary, true);
        try {
            MoveReplace(temporary, target);
        } catch (const Error&) {
            try {
                DeleteFileIfExists(temporary);
            } catch (...) {
            }
            Throw(ErrorKind::IO, "RWND Parabellum is running from that folder. Close it and try again.");
        }
    }
    try {
        DeleteFileIfExists(directory / LegacyExeName);
    } catch (const Error&) {
    }
    progress("Unpacking game components");
    payload::ExtractTo(directory);
    payload::RemoveUnpacked(directory, port_installer::IsShipped);
    PatchFiles::RemoveRetired(directory);
    Json marker = ParseJson(MarkerJson(Guid::New().N()));
    if (auto earlier = RegisteredDirectory();
        earlier && !IEquals(FullPath(*earlier).wstring(), FullPath(directory).wstring()))
        marker["movedFrom"] = Narrow(earlier->wstring());
    WriteAllText(directory / MarkerName, ToJson(marker));
    progress("Creating shortcuts");
    auto links = Shortcuts();
    if (startMenuShortcut) CreateShortcut(links[0], target);
    if (desktopShortcut) CreateShortcut(links[1], target);
    Register(directory, target);
    return target;
}

bool Installation::RefreshInstalled(bool tray) {
    fs::path directory = ExeDirectory();
    for (auto pattern : {L"*.old", L"*.download"})
        for (auto& stale : ListFiles(directory, pattern)) try {
                DeleteFileIfExists(stale);
            } catch (const Error&) {
            }
    payload::ExtractTo(directory);
    payload::RemoveUnpacked(directory, port_installer::IsShipped);
    PatchFiles::RemoveRetired(directory);
    try {
        if (MigrateExeName(directory, tray)) return true;
    } catch (const Error&) {
    }
    try {
        if (MoveToProgramFiles(tray)) return true;
    } catch (const Error&) {
    }
    try {
        FinishMove();
    } catch (const Error&) {
    }
    if (IsElevated() && InProtectedFolder(directory) && HasOwnPermissions(directory)) InheritPermissions(directory);
    fs::path marker = directory / MarkerName;
    std::optional<Marker> current;
    try {
        current = ReadMarker(marker);
    } catch (const Error&) {
        current.reset();
    }
    if (!current || current->version != DisplayVersion() || !current->installId) {
        WriteAllText(marker, MarkerJson(current && current->installId ? *current->installId : Guid::New().N()));
        Register(directory, InstalledExe());
        for (auto& link : Shortcuts())
            if (FileExists(link)) try {
                    CreateShortcut(link, InstalledExe());
                } catch (const Error&) {
                }
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, InstalledExe().c_str(), nullptr);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    if (RegString(HKEY_CURRENT_USER, RunKey, ProductName)) try {
            SetStartsWithWindows(true);
        } catch (const Error&) {
        }
    return false;
}

bool Installation::MoveToProgramFiles(bool tray) {
    const fs::path from = ExeDirectory();
    if (!IsReleaseBuild() || InProtectedFolder(from)) return false;
    const fs::path to = DefaultDirectory();
    CreateDirectories(to);
    const fs::path exe = to / ExeName, temporary = exe.wstring() + L".download";
    CopyFileTo(ExePath(), temporary, true);
    MoveReplace(temporary, exe);
    payload::ExtractTo(to);
    std::optional<Marker> current;
    try {
        current = ReadMarker(from / MarkerName);
    } catch (const Error&) {
    }
    Json marker = ParseJson(MarkerJson(current && current->installId ? *current->installId : Guid::New().N()));
    marker["movedFrom"] = Narrow(from.wstring());
    WriteAllText(to / MarkerName, ToJson(marker));
    for (auto& link : Shortcuts())
        if (FileExists(link)) try {
                CreateShortcut(link, exe);
            } catch (const Error&) {
            }
    Register(to, exe);
    std::vector<std::wstring> arguments = {L"--after", std::to_wstring(GetCurrentProcessId())};
    if (tray) arguments.push_back(L"--tray");
    process::Start(exe, arguments, to);
    return true;
}

void Installation::FinishMove() {
    const fs::path markerFile = ExeDirectory() / MarkerName;
    if (!FileExists(markerFile)) return;
    Json marker = ParseJson(ReadAllText(markerFile));
    auto movedFrom = GetString(marker, "movedFrom");
    if (!movedFrom) return;
    const fs::path old = FullPath(Widen(*movedFrom));
    if (StartsWithWindows()) try {
            SetStartsWithWindows(true);
        } catch (const Error&) {
        }
    if (!IEquals(old.wstring(), ExeDirectory().wstring()) && !RemoveOldInstall(old, payload::Files())) return;
    marker.erase("movedFrom");
    WriteAllText(markerFile, ToJson(marker));
}

bool Installation::RemoveOldInstall(const fs::path& old, const std::vector<std::wstring>& notices) {
    if (!DirExists(old) || IsReparsePoint(old) || !FileExists(old / MarkerName)) return true;
    for (auto name : {ExeName, LegacyExeName}) try {
            DeleteFileIfExists(old / name);
        } catch (const Error&) {
        }
    if (FileExists(old / ExeName) || FileExists(old / LegacyExeName)) return false;
    for (auto& relative : notices)
        if (relative.find_first_of(L"\\/") == std::wstring::npos) try {
                DeleteFileIfExists(old / relative);
            } catch (const Error&) {
            }
    try {
        DeleteFileIfExists(old / MarkerName);
    } catch (const Error&) {
    }
    if (IsDirectoryEmpty(old)) RemoveDirectoryW(LongPath(old).c_str());
    return true;
}

bool Installation::MigrateExeName(const fs::path& directory, bool tray) {
    fs::path legacy = directory / LegacyExeName;
    if (!IEquals(ExePath().wstring(), legacy.wstring())) {
        if (FileExists(legacy)) try {
                DeleteFileIfExists(legacy);
            } catch (const Error&) {
            }
        return false;
    }
    CopyFileTo(legacy, InstalledExe(), true);
    for (auto& link : Shortcuts())
        if (FileExists(link)) CreateShortcut(link, InstalledExe());
    Register(directory, InstalledExe());
    if (StartsWithWindows()) try {
            SetStartsWithWindows(true);
        } catch (const Error&) {
        }
    std::vector<std::wstring> arguments = {L"--after", std::to_wstring(GetCurrentProcessId())};
    if (tray) arguments.push_back(L"--tray");
    process::Start(InstalledExe(), arguments, directory);
    return true;
}

void Installation::Uninstall() {
    fs::path directory = ExeDirectory();
    if (!FileExists(directory / MarkerName))
        Throw(ErrorKind::InvalidOperation, "This copy of RWND Parabellum is not installed.");
    for (auto& link : Shortcuts()) DeleteFileIfExists(link);
    RegDeleteTreeW(HKEY_CURRENT_USER, UninstallKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, UninstallKey);
    SetStartsWithWindows(false);
    PatchFiles::RemoveRetired(directory);
    payload::RemoveUnpacked(directory, port_installer::IsShipped);
    auto files = payload::Files();
    files.push_back(MarkerName);
    for (auto& relative : files) DeleteFileIfExists(directory / relative);
    for (auto pattern : {L"*.old", L"*.download"})
        for (auto& stale : ListFiles(directory, pattern)) try {
                DeleteFileIfExists(stale);
            } catch (const Error& e) {
                if (e.kind != ErrorKind::UnauthorizedAccess) throw;
            }
    std::vector<std::wstring> folders;
    for (auto& relative : payload::Files()) {
        auto parent = fs::path(relative).parent_path().wstring();
        if (!parent.empty() && std::find(folders.begin(), folders.end(), parent) == folders.end())
            folders.push_back(parent);
    }
    for (auto& sub : folders) {
        fs::path folder = directory / sub;
        if (DirExists(folder) && IsDirectoryEmpty(folder)) RemoveDirectoryW(LongPath(folder).c_str());
    }
    std::wstring command = L"/d /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + InstalledExe().wstring() + L"\" \"" +
                           (directory / LegacyExeName).wstring() + L"\" \"" + directory.wstring() +
                           L"\\*.old\" & rmdir \"" + directory.wstring() + L"\"";
    std::wstring line = QuoteArgument(process::SystemTool(L"cmd.exe").wstring()) + L" " + command;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring temp = TempDirectory();
    if (CreateProcessW(process::SystemTool(L"cmd.exe").c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, temp.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void Installation::EnsureWritable() {
    fs::path directory = ExeDirectory();
    if (CanWrite(directory)) return;
    PrepareFolderElevated(directory);
    if (!CanWrite(directory))
        Throw(ErrorKind::UnauthorizedAccess, "Windows did not allow updating " + Narrow(directory.wstring()) + ".");
}

static std::wstring TaskName() {
    wchar_t name[257];
    DWORD size = 257;
    GetUserNameW(name, &size);
    return L"RWND Parabellum (" + std::wstring(name) + L")";
}

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'&':
                out += L"&amp;";
                break;
            case L'<':
                out += L"&lt;";
                break;
            case L'>':
                out += L"&gt;";
                break;
            case L'"':
                out += L"&quot;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

bool Installation::StartsWithWindows() {
    return process::RunHidden(process::SystemTool(L"schtasks.exe"), {L"/Query", L"/TN", TaskName()}) == 0;
}

void Installation::SetStartsWithWindows(bool value) {
    HKEY run;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &run, nullptr) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(run, ProductName);
        RegCloseKey(run);
    }
    fs::path schtasks = process::SystemTool(L"schtasks.exe");
    if (!value) {
        process::RunHidden(schtasks, {L"/Delete", L"/TN", TaskName(), L"/F"});
        return;
    }
    std::wstring user = XmlEscape(CurrentUserName());
    std::wstring xml =
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <Triggers>\r\n    <LogonTrigger>\r\n      <UserId>" +
        user +
        L"</UserId>\r\n    </LogonTrigger>\r\n  </Triggers>\r\n"
        L"  <Principals>\r\n    <Principal id=\"Author\">\r\n      <UserId>" +
        user +
        L"</UserId>\r\n      <LogonType>InteractiveToken</LogonType>\r\n"
        L"      <RunLevel>HighestAvailable</RunLevel>\r\n    </Principal>\r\n  </Principals>\r\n"
        L"  <Settings>\r\n    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n    <Exec>\r\n      <Command>" +
        XmlEscape(InstalledExe().wstring()) +
        L"</Command>\r\n"
        L"      <Arguments>--tray</Arguments>\r\n      <WorkingDirectory>" +
        XmlEscape(ExeDirectory().wstring()) +
        L"</WorkingDirectory>\r\n"
        L"    </Exec>\r\n  </Actions>\r\n</Task>";
    fs::path file = fs::path(TempDirectory()) / (L"rwnd-startup-" + Widen(Guid::New().N()) + L".xml");
    {
        File out(file, File::Mode::Create);
        const uint8_t bom[] = {0xFF, 0xFE};
        out.Write(bom, 2);
        out.Write(xml.data(), xml.size() * sizeof(wchar_t));
    }
    int code = process::RunHidden(schtasks, {L"/Create", L"/TN", TaskName(), L"/XML", file.wstring(), L"/F"});
    DeleteFileIfExists(file);
    if (code != 0) Throw(ErrorKind::UnauthorizedAccess, "Windows did not create the startup task.");
}

}
