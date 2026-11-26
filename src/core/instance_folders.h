#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/util.h"

namespace rwnd {

enum class InstanceAction { Copy, Hardlink };
struct InstanceFile {
    std::wstring relativePath;
    InstanceAction action;
    uint64_t length;
};
struct InstancePlan {
    fs::path source, destination;
    std::vector<InstanceFile> files;
    uint64_t LinkedBytes() const;
    uint64_t CopiedBytes() const;
};

class InstanceFolders {
public:
    static std::wstring DataDirectory();
    static std::wstring StorageDirectory();
    static std::wstring StorageFor(const std::wstring& dataDirectory, const std::wstring& gameFolder);
    static std::wstring UsualStorageFor(const std::wstring& dataDirectory, const std::wstring& gameFolder);
    static bool MoveStoreOutOfGame(const std::wstring& dataDirectory, const std::wstring& gameFolder);
    static bool MoveStore(const fs::path& fromStorage, const fs::path& toStorage);
    static void SetGameFolder(const std::wstring& gameFolder);
    static bool SupportsHardLinks(const fs::path& anyPathOnDrive);
    static InstancePlan Plan(const fs::path& source, const fs::path& destination);
    static fs::path Prepare(const fs::path& source, const fs::path& instancesRoot, const Guid& playerId);
    static bool Within(const std::wstring& path, const std::wstring& parent);
    static void RequireNoReparse(const fs::path& path);
    static void RequirePrivateMutableFiles(const fs::path& directory);
    static void MakeHardLink(const fs::path& newFile, const fs::path& existingFile);
};

struct InstanceProcess {
    uint32_t pid = 0;
    int64_t startUtcTicks = 0;
    Guid playerId;
    int port = 0;
    std::string ToJson() const;
};

inline constexpr std::array<int, 8> GamePorts = {3074, 3076, 3078, 3080, 3082, 3084, 3086, 3088};

class GameInstanceGuard {
public:
    static std::map<Guid, InstanceProcess> Running(const fs::path& instancesRoot);
    static void Stop(const InstanceProcess& game);
    static int CheckAndChoosePort(const fs::path& instancesRoot, const Guid& playerId,
                                  const std::optional<fs::path>& original);
    static int ChoosePort(const std::map<Guid, InstanceProcess>& running, const Guid& playerId,
                          const std::vector<int>& listeners);
    static std::vector<int> UdpListeners();
};

}
