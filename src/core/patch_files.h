#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/http.h"

namespace rwnd {

class PatchFiles {
public:
    using TokenProvider = std::function<std::string(const CancelToken&)>;
    static void Configure(const std::string& apiBaseUrl, TokenProvider token,
                          std::shared_ptr<HttpHandler> http = nullptr);
    static void Reset();
    static Bytes Get(const std::string& sha256, const CancelToken& cancel = CancelToken::None());

    static const std::vector<std::wstring>& RetiredFiles();
    static int RemoveRetired(const fs::path& directory);
};

}
