#pragma once
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

namespace rwnd::settings {

const Json& Root();
bool Flag(std::string_view name);
std::optional<std::string> String(std::string_view name);
std::vector<std::string> Strings(std::string_view name);
std::string RawText();

std::optional<std::string> Env(const wchar_t* name);

std::optional<std::string> Endpoint(const wchar_t* name);
void SetEndpoint(const wchar_t* name, std::optional<std::string> value);

}
