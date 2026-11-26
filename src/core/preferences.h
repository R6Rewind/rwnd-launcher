#pragma once
#include <optional>
#include <string>

#include "core/json.h"

namespace rwnd {

struct DownloadStop {
    std::string folder, kind, text, at;
};

struct Preferences {
    std::string gamePath;
    bool trayHintShown = false;
    bool friendNotifications = true;
    bool messageNotifications = true;
    bool keepInTray = true;
    std::optional<std::string> gameSetupFor;
    std::optional<std::string> steamDownloadFolder;
    bool updateNotifications = true;
    std::optional<int> fpsLimit;
    std::optional<double> mouseMultiplier;
    std::string legalAccepted;
    std::string downloadSize = "full";
    std::optional<DownloadStop> downloadStop;

    static Preferences Load();
    void Save() const;
    static std::string SetupKey();
    bool NeedsGameSetup() const { return gamePath.empty() || gameSetupFor != SetupKey(); }

    Json ToJson() const;
    static Preferences FromJson(const Json& json);
};

}
