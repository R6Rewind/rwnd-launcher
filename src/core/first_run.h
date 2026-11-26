#pragma once
#include <functional>
#include <span>
#include <optional>
#include <string>
#include <vector>

#include "core/game.h"
#include "core/preferences.h"
#include "core/util.h"

namespace rwnd::first_run {

std::vector<fs::path> FolderCandidates(const std::string& gamePath, const std::optional<std::string>& steamFolder,
                                       const fs::path& defaultFolder, const std::vector<fs::path>& driveRoots);

std::optional<fs::path> DetectGameFolder(const std::vector<fs::path>& candidates,
                                         const std::function<bool(const fs::path&)>& good);

std::vector<fs::path> FixedDriveRoots();

bool FetchOutbreak(bool live, bool ready, int64_t freeBytes, int64_t gameStillNeeded, int64_t outbreakStillNeeded);

std::string CheckKind(const BuildCheck& check, bool unfinishedDownload);

std::optional<DownloadStop> StopFor(const std::optional<DownloadStop>& saved, const fs::path& folder);
std::string CheckKind(const BuildCheck& check, bool unfinishedDownload, const std::optional<DownloadStop>& stop);
inline constexpr const char* StopTitle = "Download stopped";
std::string StopReason(const std::optional<DownloadStop>& stop, bool running);

enum class Size { Full, Medium, Small };
Size ParseSize(const std::string& name);
const char* SizeName(Size size);
bool LeftOut(std::wstring_view relativePath, Size size);
std::string FileList(Size size);
std::vector<game_files::Entry> Kept(std::span<const game_files::Entry> manifest, Size size);
int64_t LeftOutBytes(std::span<const game_files::Entry> manifest, Size size);
std::vector<fs::path> LeftOutFiles(const fs::path& root, Size size);

std::vector<fs::path> AccountCopies(const fs::path& game, const std::vector<fs::path>& roots);
int LeaveOutOfCopies(const fs::path& game, const std::vector<fs::path>& roots, Size size);
int LinkIntoCopy(const fs::path& copy, const fs::path& game, Size size);
int LinkIntoCopies(const fs::path& game, const std::vector<fs::path>& roots, Size size);

bool FreesGameFolder(bool releaseBuild, bool devDataDir);
std::vector<fs::path> SwitchDownFiles(const fs::path& game, const std::vector<fs::path>& roots, Size size,
                                      bool gameFolder);
int64_t FreedBytes(const std::vector<fs::path>& files);
struct Freed {
    int removed = 0;
    int64_t bytes = 0;
};
Freed RemoveFiles(const std::vector<fs::path>& files);
std::vector<game_files::Entry> MissingFiles(const fs::path& game, std::span<const game_files::Entry> manifest,
                                            Size size);
std::string MissingFileList(std::span<const game_files::Entry> missing);
std::string LiftTexturePreset(const std::string& ini, Size from, Size to);
std::string CapTexturePreset(const std::string& ini, Size size);

inline constexpr double DefaultMouseMultiplier = 0.02;
std::optional<double> ParseMouseMultiplier(const std::string& text);
std::string FormatMouseMultiplier(double value);
inline constexpr int DefaultFpsLimit = 144;
std::optional<int> ParseFpsLimit(const std::string& text);

std::string SetIniValue(const std::string& ini, const std::string& section, const std::string& key,
                        const std::string& value);

}
