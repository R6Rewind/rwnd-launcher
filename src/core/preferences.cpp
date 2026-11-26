#include "core/preferences.h"

#include "core/installation.h"
#include "core/instance_folders.h"

namespace rwnd {

static fs::path FilePath() {
    return fs::path(InstanceFolders::DataDirectory()) / L"preferences.json";
}

std::string Preferences::SetupKey() {
    if (Installation::IsReleaseBuild() && Installation::IsInstalledHere())
        return Installation::InstallId().value_or("installed");
    return "development";
}

Preferences Preferences::FromJson(const Json& json) {
    Preferences p;
    p.gamePath = GetString(json, "GamePath").value_or("");
    p.trayHintShown = GetBool(json, "TrayHintShown").value_or(false);
    p.friendNotifications = GetBool(json, "FriendNotifications").value_or(true);
    p.keepInTray = GetBool(json, "KeepInTray").value_or(true);
    p.gameSetupFor = GetString(json, "GameSetupFor");
    p.steamDownloadFolder = GetString(json, "SteamDownloadFolder");
    p.updateNotifications = GetBool(json, "UpdateNotifications").value_or(true);
    std::string size = GetString(json, "DownloadSize").value_or("full");
    p.mouseMultiplier = GetDouble(json, "MouseSensitivityMultiplier");
    if (auto fps = GetInt(json, "FpsLimit")) p.fpsLimit = static_cast<int>(*fps);
    p.downloadSize = size == "medium" || size == "small" ? size : "full";
    p.messageNotifications = GetBool(json, "MessageNotifications").value_or(true);
    p.legalAccepted = GetString(json, "LegalAccepted").value_or("");
    if (auto it = json.find("DownloadStop"); it != json.end() && it->is_object()) {
        DownloadStop stop{GetString(*it, "Folder").value_or(""), GetString(*it, "Kind").value_or(""),
                          GetString(*it, "Text").value_or(""), GetString(*it, "At").value_or("")};
        if (!stop.folder.empty() && !stop.kind.empty()) p.downloadStop = stop;
    }
    return p;
}

Json Preferences::ToJson() const {
    Json j = Json::object();
    j["GamePath"] = gamePath;
    j["TrayHintShown"] = trayHintShown;
    j["FriendNotifications"] = friendNotifications;
    j["KeepInTray"] = keepInTray;
    j["GameSetupFor"] = gameSetupFor ? Json(*gameSetupFor) : Json(nullptr);
    j["SteamDownloadFolder"] = steamDownloadFolder ? Json(*steamDownloadFolder) : Json(nullptr);
    j["UpdateNotifications"] = updateNotifications;
    j["DownloadSize"] = downloadSize;
    j["MouseSensitivityMultiplier"] = mouseMultiplier ? Json(*mouseMultiplier) : Json(nullptr);
    j["FpsLimit"] = fpsLimit ? Json(*fpsLimit) : Json(nullptr);
    j["MessageNotifications"] = messageNotifications;
    if (!legalAccepted.empty()) j["LegalAccepted"] = legalAccepted;
    if (downloadStop)
        j["DownloadStop"] = Json{{"Folder", downloadStop->folder},
                                 {"Kind", downloadStop->kind},
                                 {"Text", downloadStop->text},
                                 {"At", downloadStop->at}};
    return j;
}

Preferences Preferences::Load() {
    try {
        if (FileExists(FilePath())) {
            Json json = ParseJson(ReadAllText(FilePath()));
            if (json.is_object()) return FromJson(json);
        }
    } catch (const Error& e) {
        if (e.kind != ErrorKind::IO && e.kind != ErrorKind::InvalidData && e.kind != ErrorKind::UnauthorizedAccess)
            throw;
    }
    return Preferences();
}

void Preferences::Save() const {
    CreateDirectories(InstanceFolders::DataDirectory());
    WriteAllText(FilePath(), rwnd::ToJson(ToJson()));
}

}
