#pragma once
#include <functional>
#include <optional>
#include <string>

#include "core/util.h"

namespace rwnd::app_control {

inline constexpr uint32_t PolicyViolation = 0xC0E90002u;
inline constexpr const char* SmartAppControlMessage =
    "Windows Smart App Control is blocking RWND. Turn it off to play.";
inline constexpr const char* PolicyMessage = "A Windows security policy on this PC is blocking RWND.";
inline constexpr const wchar_t* SettingsPage = L"windowsdefender://smartapp/";
inline constexpr const wchar_t* StartupFiles[] = {L"steam_api64.dll", L"uplay_r1_loader64.dll", L"rwnd_ac.dll",
                                                  L"rewind.dll"};

std::optional<uint32_t> LoadStatus(const fs::path& dll);
std::optional<uint32_t> SmartAppControlState();

using StatusOf = std::function<std::optional<uint32_t>(const fs::path&)>;
std::optional<std::string> Refused(const fs::path& root, const StatusOf& status = LoadStatus);
std::string Message(std::optional<uint32_t> smartAppControl);

}
