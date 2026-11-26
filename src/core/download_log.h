#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "core/util.h"

namespace rwnd::download_log {

inline constexpr size_t MaxBytes = 256 * 1024;
inline constexpr size_t KeepBytes = 192 * 1024;
inline constexpr size_t MaxText = 600;

fs::path File();
std::string UtcText(int64_t utcTicks);
std::string Clean(std::string_view text, const std::vector<std::string>& secrets);
std::string Line(int64_t utcTicks, std::string_view what, const fs::path& folder, std::string_view text);
std::string Newest(const std::string& text, size_t keep);
void Append(const fs::path& file, const std::string& line);

}
