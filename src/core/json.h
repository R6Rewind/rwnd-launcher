#pragma once
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

#include "core/util.h"

namespace rwnd {

using Json = nlohmann::ordered_json;

Json ParseJson(std::string_view text);
Json ParseJson(std::span<const uint8_t> bytes);

const Json* Field(const Json& object, std::string_view name);
std::optional<std::string> GetString(const Json& object, std::string_view name);
std::optional<int64_t> GetInt(const Json& object, std::string_view name);
std::optional<uint64_t> GetUInt(const Json& object, std::string_view name);
std::optional<double> GetDouble(const Json& object, std::string_view name);
std::optional<bool> GetBool(const Json& object, std::string_view name);
std::optional<Guid> GetGuid(const Json& object, std::string_view name);
std::optional<int64_t> GetTicks(const Json& object, std::string_view name);
std::string RequireString(const Json& object, std::string_view name);
int64_t RequireInt(const Json& object, std::string_view name);
Guid RequireGuid(const Json& object, std::string_view name);
int64_t RequireTicks(const Json& object, std::string_view name);
const Json& RequireArray(const Json& object, std::string_view name);

std::string ToJson(const Json& value);

}
