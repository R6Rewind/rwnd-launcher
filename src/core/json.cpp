#include "core/json.h"

namespace rwnd {

Json ParseJson(std::string_view text) {
    try {
        if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB &&
            static_cast<uint8_t>(text[2]) == 0xBF)
            text.remove_prefix(3);
        return Json::parse(text);
    } catch (const Json::exception& e) {
        Throw(ErrorKind::InvalidData, std::string("Invalid JSON: ") + e.what());
    }
}

Json ParseJson(std::span<const uint8_t> bytes) {
    return ParseJson(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

const Json* Field(const Json& object, std::string_view name) {
    if (!object.is_object()) return nullptr;
    auto exact = object.find(std::string(name));
    if (exact != object.end()) return &*exact;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (IEqualsA(it.key(), name)) return &it.value();
    return nullptr;
}

static const Json* Present(const Json& object, std::string_view name) {
    const Json* v = Field(object, name);
    return v && !v->is_null() ? v : nullptr;
}

std::optional<std::string> GetString(const Json& object, std::string_view name) {
    const Json* v = Present(object, name);
    if (!v) return std::nullopt;
    Require(v->is_string(), "Expected text for " + std::string(name) + ".");
    return v->get<std::string>();
}

std::optional<int64_t> GetInt(const Json& object, std::string_view name) {
    const Json* v = Present(object, name);
    if (!v) return std::nullopt;
    Require(v->is_number_integer(), "Expected a whole number for " + std::string(name) + ".");
    if (v->is_number_unsigned()) {
        uint64_t u = v->get<uint64_t>();
        Require(u <= static_cast<uint64_t>(INT64_MAX), "Number out of range for " + std::string(name) + ".");
        return static_cast<int64_t>(u);
    }
    return v->get<int64_t>();
}

std::optional<uint64_t> GetUInt(const Json& object, std::string_view name) {
    const Json* v = Present(object, name);
    if (!v) return std::nullopt;
    Require(v->is_number_unsigned() || (v->is_number_integer() && v->get<int64_t>() >= 0),
            "Expected a positive number for " + std::string(name) + ".");
    return v->is_number_unsigned() ? v->get<uint64_t>() : static_cast<uint64_t>(v->get<int64_t>());
}

std::optional<double> GetDouble(const Json& object, std::string_view name) {
    const Json* v = Present(object, name);
    if (!v) return std::nullopt;
    Require(v->is_number(), "Expected a number for " + std::string(name) + ".");
    return v->get<double>();
}

std::optional<bool> GetBool(const Json& object, std::string_view name) {
    const Json* v = Present(object, name);
    if (!v) return std::nullopt;
    Require(v->is_boolean(), "Expected true or false for " + std::string(name) + ".");
    return v->get<bool>();
}

std::optional<Guid> GetGuid(const Json& object, std::string_view name) {
    auto s = GetString(object, name);
    if (!s) return std::nullopt;
    auto g = Guid::Parse(*s);
    Require(g.has_value(), "Invalid id for " + std::string(name) + ".");
    return g;
}

std::optional<int64_t> GetTicks(const Json& object, std::string_view name) {
    auto s = GetString(object, name);
    if (!s) return std::nullopt;
    auto t = ParseIsoTicks(*s);
    Require(t.has_value(), "Invalid date for " + std::string(name) + ".");
    return t;
}

std::string RequireString(const Json& object, std::string_view name) {
    auto v = GetString(object, name);
    Require(v.has_value(), "Missing " + std::string(name) + ".");
    return *v;
}
int64_t RequireInt(const Json& object, std::string_view name) {
    auto v = GetInt(object, name);
    Require(v.has_value(), "Missing " + std::string(name) + ".");
    return *v;
}
Guid RequireGuid(const Json& object, std::string_view name) {
    auto v = GetGuid(object, name);
    Require(v.has_value(), "Missing " + std::string(name) + ".");
    return *v;
}
int64_t RequireTicks(const Json& object, std::string_view name) {
    auto v = GetTicks(object, name);
    Require(v.has_value(), "Missing " + std::string(name) + ".");
    return *v;
}
const Json& RequireArray(const Json& object, std::string_view name) {
    const Json* v = Present(object, name);
    Require(v && v->is_array(), "Missing " + std::string(name) + ".");
    return *v;
}

std::string ToJson(const Json& value) {
    return value.dump(-1, ' ', true, Json::error_handler_t::replace);
}

}
