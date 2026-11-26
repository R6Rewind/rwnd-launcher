#pragma once
#include <optional>
#include <span>
#include <string>

#include "core/util.h"

namespace rwnd::game_dlls {

Bytes Read(const std::string& name);
inline constexpr size_t MaxSize = 32u << 20;
Bytes Unpack(std::span<const uint8_t> packed);
void RemoveDownloads(const fs::path& dataDirectory);
void SetDirectory(const std::optional<fs::path>& directory);

}
