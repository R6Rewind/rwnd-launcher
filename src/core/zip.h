#pragma once
#include "core/util.h"

namespace rwnd::zip {

int ExtractAll(const fs::path& zipFile, const fs::path& destination, const CancelToken* cancel = nullptr);

std::optional<fs::path> SafeEntryPath(const std::string& name);

}
