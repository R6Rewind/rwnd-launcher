#pragma once
#include <string>

#include "core/account_client.h"

namespace rwnd {

struct BuildCheck {
    bool ok;
    std::string title, reason;
    std::string kind = "fix";
    bool unfinished = false;
};

namespace game_build {
inline constexpr const char* Name = "Para Bellum (Y3S2)";
inline constexpr uint32_t SteamApp = 359550;
struct Depot {
    uint32_t depot;
    uint64_t manifest;
};
inline constexpr uint32_t SkuWw = 377237, SkuRus = 377238;
inline constexpr uint64_t SkuRusManifest = 7612896943642733404ull;
inline constexpr Depot SteamDepots[] = {
    {SkuRus, SkuRusManifest}, {SkuWw, 7727030782821676747ull}, {359551, 4479992811705865482ull}};
inline constexpr bool IsRegionDepot(uint32_t depot) {
    return depot == SkuWw || depot == SkuRus;
}
inline constexpr int64_t SteamBytes = 68'516'085'340;
BuildCheck Verify(const std::wstring& folder, const CancelToken& cancel = CancelToken::None());
}

struct StartedGame {
    void* process = nullptr;
    uint32_t pid = 0;
    Guid sessionId;
};

namespace game_launcher {
class ExitedBeforeConnection final : public Error {
public:
    ExitedBeforeConnection(uint32_t exitCode, const std::string& message)
        : Error(ErrorKind::InvalidOperation, message), code(exitCode) {}
    uint32_t code;
};
}

namespace windowed_optimizations {
std::string Merge(const std::string& value);
void TurnOff(const fs::path& gameExe);
}

}
