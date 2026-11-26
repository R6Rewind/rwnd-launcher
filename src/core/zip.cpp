#include "core/zip.h"

#include <miniz.h>

namespace rwnd::zip {

namespace {
size_t ReadAt(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
    auto* file = static_cast<File*>(opaque);
    try {
        file->Seek(offset);
        size_t total = 0;
        while (total < size) {
            size_t read = file->Read(static_cast<uint8_t*>(buffer) + total, size - total);
            if (read == 0) break;
            total += read;
        }
        return total;
    } catch (...) {
        return 0;
    }
}

struct Sink {
    File* out;
    const CancelToken* cancel;
    bool canceled = false;
};

size_t WriteNext(void* opaque, mz_uint64, const void* data, size_t size) {
    auto* sink = static_cast<Sink*>(opaque);
    if (sink->cancel && sink->cancel->IsCanceled()) {
        sink->canceled = true;
        return 0;
    }
    try {
        sink->out->Write(data, size);
    } catch (...) {
        return 0;
    }
    return size;
}

bool ReservedName(const std::wstring& part) {
    std::wstring stem = ToLowerW(part.substr(0, part.find(L'.')));
    for (const wchar_t* name : {L"con", L"prn", L"aux", L"nul"})
        if (stem == name) return true;
    if (stem.size() == 4 && (stem.starts_with(L"com") || stem.starts_with(L"lpt")) && stem[3] >= L'0' &&
        stem[3] <= L'9')
        return true;
    return false;
}

struct Reader {
    mz_zip_archive archive{};
    bool open = false;
    ~Reader() {
        if (open) mz_zip_reader_end(&archive);
    }
};
}

std::optional<fs::path> SafeEntryPath(const std::string& name) {
    if (name.empty() || name.find('\0') != std::string::npos) return std::nullopt;
    std::wstring wide = Widen(name);
    for (auto& c : wide)
        if (c == L'\\') c = L'/';
    if (wide.front() == L'/') return std::nullopt;
    fs::path result;
    size_t start = 0;
    while (start < wide.size()) {
        size_t end = wide.find(L'/', start);
        if (end == std::wstring::npos) end = wide.size();
        std::wstring part = wide.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == L".") continue;
        if (part == L"..") return std::nullopt;
        if (part.find_first_of(L":*?\"<>|") != std::wstring::npos) return std::nullopt;
        for (wchar_t c : part)
            if (c < 32) return std::nullopt;
        if (part.back() == L'.' || part.back() == L' ' || ReservedName(part)) return std::nullopt;
        result /= part;
    }
    if (result.empty()) return std::nullopt;
    return result;
}

int ExtractAll(const fs::path& zipFile, const fs::path& destination, const CancelToken* cancel) {
    File in(zipFile, File::Mode::Read);
    Reader reader;
    reader.archive.m_pRead = ReadAt;
    reader.archive.m_pIO_opaque = &in;
    if (!mz_zip_reader_init(&reader.archive, in.Length(), 0))
        Throw(ErrorKind::InvalidData, std::string("The zip file is damaged (") +
                                          mz_zip_get_error_string(mz_zip_get_last_error(&reader.archive)) + ").");
    reader.open = true;
    CreateDirectories(destination);
    int written = 0;
    mz_uint count = mz_zip_reader_get_num_files(&reader.archive);
    for (mz_uint i = 0; i < count; i++) {
        if (cancel) cancel->ThrowIfCanceled();
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&reader.archive, i, &stat))
            Throw(ErrorKind::InvalidData, "The zip file is damaged.");
        std::optional<fs::path> relative = SafeEntryPath(stat.m_filename);
        if (!relative)
            Throw(ErrorKind::InvalidData, std::string("The zip file has an unsafe file name: ") + stat.m_filename);
        fs::path target = destination / *relative;
        if (stat.m_is_directory) {
            CreateDirectories(target);
            continue;
        }
        if (stat.m_is_encrypted || !stat.m_is_supported)
            Throw(ErrorKind::InvalidData, std::string("The zip file uses an unsupported format: ") + stat.m_filename);
        CreateDirectories(target.parent_path());
        bool ok = false, canceled = false;
        {
            File out(target, File::Mode::Create);
            Sink sink{&out, cancel};
            ok = mz_zip_reader_extract_to_callback(&reader.archive, i, WriteNext, &sink, 0) != MZ_FALSE;
            canceled = sink.canceled;
            if (ok) out.Flush();
        }
        if (!ok) {
            mz_zip_error error = mz_zip_get_last_error(&reader.archive);
            try {
                DeleteFileIfExists(target);
            } catch (...) {
            }
            if (canceled && cancel) cancel->ThrowIfCanceled();
            Throw(ErrorKind::InvalidData, std::string("The zip file is damaged (") + stat.m_filename + ": " +
                                              mz_zip_get_error_string(error) + ").");
        }
        written++;
    }
    return written;
}

}
