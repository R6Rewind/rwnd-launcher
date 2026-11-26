#pragma once
#include <optional>
#include <string>
#include <vector>

#include "core/util.h"

namespace rwnd::process {

uint32_t Start(const fs::path& exe, const std::vector<std::wstring>& arguments, const fs::path& workingDirectory);
int RunHidden(const fs::path& exe, const std::vector<std::wstring>& arguments, const fs::path& workingDirectory = {});
std::optional<int> RunElevated(const fs::path& exe, const std::vector<std::wstring>& arguments,
                               const fs::path& workingDirectory, bool wait);
void Open(const std::wstring& target);
fs::path SystemTool(const wchar_t* name);

}
