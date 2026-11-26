#include "core/settings.h"

#include <windows.h>

#include <map>
#include <mutex>

#include "settings_data.h"

namespace rwnd::settings {

const Json& Root() {
    static const Json root = [] { return ParseJson(std::string_view(generated::LauncherSettingsJson)); }();
    return root;
}

static const Json* Exact(std::string_view name) {
    const Json& root = Root();
    if (!root.is_object()) return nullptr;
    auto it = root.find(std::string(name));
    return it == root.end() ? nullptr : &*it;
}

bool Flag(std::string_view name) {
    const Json* v = Exact(name);
    return v && v->is_boolean() && v->get<bool>();
}

std::optional<std::string> String(std::string_view name) {
    const Json* v = Exact(name);
    return v && v->is_string() ? std::optional(v->get<std::string>()) : std::nullopt;
}

std::vector<std::string> Strings(std::string_view name) {
    std::vector<std::string> out;
    const Json* v = Exact(name);
    if (v && v->is_array())
        for (auto& item : *v)
            if (item.is_string()) out.push_back(item.get<std::string>());
    return out;
}

std::string RawText() {
    return generated::LauncherSettingsJson;
}

std::optional<std::string> Env(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(name, value.data(), n);
    value.resize(n);
    return Narrow(value);
}

static std::mutex g_endpointsLock;
static std::map<std::wstring, std::string> g_endpoints;

std::optional<std::string> Endpoint(const wchar_t* name) {
    {
        std::lock_guard lock(g_endpointsLock);
        if (auto it = g_endpoints.find(name); it != g_endpoints.end()) return it->second;
    }
#ifdef RWND_RELEASE
    return std::nullopt;
#else
    return Env(name);
#endif
}

void SetEndpoint(const wchar_t* name, std::optional<std::string> value) {
    std::lock_guard lock(g_endpointsLock);
    if (value)
        g_endpoints[name] = std::move(*value);
    else
        g_endpoints.erase(name);
}

}
