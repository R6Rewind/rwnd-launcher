#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/util.h"

namespace rwnd {

struct PakEntry {
    std::string name;
    std::span<const uint8_t> bytes;
};

class Pak {
public:
    Pak() = default;
    explicit Pak(std::span<const uint8_t> blob);
    static Pak FromResource(const wchar_t* name);
    const PakEntry* Find(std::string_view name) const;
    const std::vector<PakEntry>& Entries() const { return entries_; }
    bool Empty() const { return entries_.empty(); }

private:
    std::vector<PakEntry> entries_;
};

}
