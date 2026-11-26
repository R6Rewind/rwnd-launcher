#pragma once
#include <string>

namespace rwnd::app {

bool EnsureWebView2Runtime(bool quiet);

int RunPreview(const std::wstring& directory);

}
