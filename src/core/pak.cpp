#include "core/pak.h"

#include <windows.h>

#include <cstring>

namespace rwnd {

Pak::Pak(std::span<const uint8_t> blob) {
    size_t at = 0;
    auto take = [&](size_t n) -> const uint8_t* {
        Require(at + n <= blob.size() && at + n >= at, "Truncated resource pack.");
        const uint8_t* p = blob.data() + at;
        at += n;
        return p;
    };
    Require(blob.size() >= 12 && std::memcmp(take(8), "RWNDPAK1", 8) == 0, "Invalid resource pack.");
    uint32_t count;
    std::memcpy(&count, take(4), 4);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t nameLength;
        std::memcpy(&nameLength, take(4), 4);
        Require(nameLength > 0 && nameLength < 1024, "Invalid resource pack name.");
        std::string name(reinterpret_cast<const char*>(take(nameLength)), nameLength);
        uint64_t size;
        std::memcpy(&size, take(8), 8);
        Require(size <= blob.size(), "Invalid resource pack size.");
        const uint8_t* data = take(static_cast<size_t>(size));
        entries_.push_back({std::move(name), std::span(data, static_cast<size_t>(size))});
    }
}

Pak Pak::FromResource(const wchar_t* name) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC info = FindResourceW(module, name, RT_RCDATA);
    if (!info) return Pak();
    HGLOBAL loaded = LoadResource(module, info);
    if (!loaded) return Pak();
    auto data = static_cast<const uint8_t*>(LockResource(loaded));
    return Pak(std::span(data, SizeofResource(module, info)));
}

const PakEntry* Pak::Find(std::string_view name) const {
    for (auto& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

}
