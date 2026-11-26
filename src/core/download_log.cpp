#include "core/download_log.h"

#include <windows.h>

#include "core/instance_folders.h"

namespace rwnd::download_log {

fs::path File() {
    return fs::path(InstanceFolders::DataDirectory()) / L"launcher-download.log";
}

namespace {
std::string Utf8Cut(std::string text, size_t max) {
    if (text.size() <= max) return text;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;
    return text.substr(0, cut);
}
}

std::string Clean(std::string_view text, const std::vector<std::string>& secrets) {
    std::string out(text);
    for (auto& secret : secrets) {
        std::string s = Trim(secret);
        if (s.size() < 4) continue;
        for (size_t at = out.find(s); at != std::string::npos; at = out.find(s, at + 3))
            out.replace(at, s.size(), "***");
    }
    for (auto& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    return Utf8Cut(Trim(out), MaxText);
}

std::string UtcText(int64_t utcTicks) {
    std::string time = FormatIsoTicks(utcTicks);
    return time.size() >= 19 ? time.substr(0, 19) + "Z" : time;
}

std::string Line(int64_t utcTicks, std::string_view what, const fs::path& folder, std::string_view text) {
    std::string line = UtcText(utcTicks) + "  " + std::string(what) + "  " + Clean(Narrow(folder.wstring()), {});
    if (!text.empty()) line += "  " + std::string(text);
    return line + "\r\n";
}

std::string Newest(const std::string& text, size_t keep) {
    if (text.size() <= keep) return text;
    size_t from = text.size() - keep;
    if (text[from - 1] == '\n') return text.substr(from);
    size_t newline = text.find('\n', from);
    return newline == std::string::npos ? std::string() : text.substr(newline + 1);
}

void Append(const fs::path& file, const std::string& line) {
    try {
        CreateDirectories(file.parent_path());
        if (FileExists(file) && FileLength(file) + line.size() > MaxBytes) {
            std::string kept = Newest(ReadAllText(file), KeepBytes);
            WriteAllText(file, kept + line);
            return;
        }
        HANDLE h = CreateFileW(LongPath(file).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(h);
    } catch (...) {
    }
}

}
