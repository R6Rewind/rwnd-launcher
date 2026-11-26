#include "core/game_dlls.h"

#include <windows.h>
#include <zstd.h>

#include <mutex>

#include "core/installation.h"

namespace rwnd::game_dlls {

namespace {
std::mutex g_gate;
std::optional<fs::path> g_directory;

std::optional<fs::path> Directory() {
    std::lock_guard lock(g_gate);
    return g_directory;
}
}

void SetDirectory(const std::optional<fs::path>& directory) {
    std::lock_guard lock(g_gate);
    g_directory = directory;
}

Bytes Unpack(std::span<const uint8_t> packed) {
    const unsigned long long size = ZSTD_getFrameContentSize(packed.data(), packed.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > MaxSize) return {};
    Bytes bytes(static_cast<size_t>(size));
    const size_t written = ZSTD_decompress(bytes.data(), bytes.size(), packed.data(), packed.size());
    if (ZSTD_isError(written) || written != bytes.size()) return {};
    return bytes;
}

Bytes Read(const std::string& name) {
    Bytes bytes;
    if (auto directory = Directory()) {
        const fs::path file = *directory / Widen(name);
        if (FileExists(file)) bytes = ReadAllBytes(file);
    } else if (auto carried = payload::Find("Patches/" + name + ".zst")) {
        bytes = Unpack(*carried);
    } else if (auto loose = payload::Find("Patches/" + name)) {
        bytes.assign(loose->begin(), loose->end());
    }
    for (auto& [known, sha256] : port_installer::PayloadHashes())
        if (IEqualsA(known, name) && !bytes.empty() && Sha256Hex(bytes) == sha256) return bytes;
    Throw(ErrorKind::InvalidData, "Payload checksum failed: " + name);
}

void RemoveDownloads(const fs::path& dataDirectory) {
    try {
        const fs::path cache = dataDirectory / L"Cache" / L"game-dlls";
        if (!DirExists(cache) || IsReparsePoint(cache)) return;
        for (auto pattern : {L"*.bin", L"*.bin.part"})
            for (auto& file : ListFiles(cache, pattern)) DeleteFileIfExists(file);
        if (IsDirectoryEmpty(cache)) RemoveDirectoryW(LongPath(cache).c_str());
    } catch (const Error&) {
    }
}

}
