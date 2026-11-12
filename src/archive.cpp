// Outpost - zip files
#include "archive.h"
#include "util.h"
#include "miniz/miniz.h"
#include <io.h>
#include <fcntl.h>

namespace archive {

static std::wstring Slash(std::wstring s) { for (auto& c : s) if (c == L'\\') c = L'/'; return s; }

void Collect(const std::wstring& base, const std::wstring& path, const std::wstring& zipPrefix, std::vector<Entry>& out, uint64_t* bytes) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && bytes) *bytes += ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        out.push_back({path, Slash(zipPrefix)});
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // don't follow junctions
        Collect(base, path + L"\\" + n, zipPrefix + L"/" + n, out, bytes);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static FILE* OpenShared(const std::wstring& path, bool write) {
    HANDLE h = write ? CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
                     : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    int fd = _open_osfhandle((intptr_t)h, write ? _O_WRONLY | _O_BINARY : _O_RDONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(h); return nullptr; }
    FILE* f = _fdopen(fd, write ? "wb" : "rb");
    if (!f) _close(fd);
    return f;
}

bool Create(const std::wstring& zipPath, const std::vector<Entry>& entries, const std::string& manifestJson,
            const Progress& progress, std::wstring* error) {
    EnsureDir(PathParent(zipPath));
    std::wstring tmp = zipPath + L".part";
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file_v2(&z, WideToUtf8(tmp).c_str(), 0, MZ_ZIP_FLAG_WRITE_ZIP64)) { *error = L"Couldn't create " + zipPath; return false; }
    uint64_t total = 0, done = 0;
    for (auto& e : entries) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(e.source.c_str(), GetFileExInfoStandard, &fa)) total += ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    }
    bool ok = true;
    if (!manifestJson.empty())
        ok = mz_zip_writer_add_mem(&z, "outpost-backup.json", manifestJson.data(), manifestJson.size(), MZ_DEFAULT_LEVEL);
    for (size_t i = 0; ok && i < entries.size(); ++i) {
        FILE* f = OpenShared(entries[i].source, false);
        if (!f) { Log(L"backup: skipped %ls (can't open)", entries[i].source.c_str()); continue; }
        _fseeki64(f, 0, SEEK_END);
        uint64_t size = (uint64_t)_ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        // already-compressed files (Minecraft regions, Unreal saves) gain little: favor speed
        ok = mz_zip_writer_add_cfile(&z, WideToUtf8(entries[i].name).c_str(), f, size, nullptr, nullptr, 0, 3, nullptr, 0, nullptr, 0);
        fclose(f);
        if (!ok) *error = L"Couldn't add " + entries[i].name + L" to the backup.";
        done += size;
        if (progress && !progress(done, total)) { ok = false; *error = L"Cancelled."; }
    }
    if (ok && !mz_zip_writer_finalize_archive(&z)) { ok = false; *error = L"Couldn't finish the zip file (is the disk full?)."; }
    mz_zip_writer_end(&z);
    if (ok) ok = MoveFileExW(tmp.c_str(), zipPath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

static size_t WriteCb(void* opaque, mz_uint64, const void* buf, size_t n) {
    DWORD w = 0;
    return WriteFile((HANDLE)opaque, buf, (DWORD)n, &w, nullptr) ? w : 0;
}

bool Extract(const std::wstring& zipPath, const std::wstring& dest, const std::wstring& prefix0, const Progress& progress, std::wstring* error) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, WideToUtf8(zipPath).c_str(), 0)) { *error = L"The zip file is damaged: " + PathName(zipPath); return false; }
    std::wstring prefix = Slash(prefix0);
    if (!prefix.empty() && prefix.back() != L'/') prefix += L'/';
    mz_uint n = mz_zip_reader_get_num_files(&z);
    uint64_t total = 0, done = 0;
    for (mz_uint i = 0; i < n; ++i) { mz_zip_archive_file_stat st; if (mz_zip_reader_file_stat(&z, i, &st)) total += st.m_uncomp_size; }
    bool ok = true;
    for (mz_uint i = 0; ok && i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        std::wstring name = Slash(Utf8ToWide(st.m_filename));
        if (!prefix.empty()) { if (!IStartsWith(name, prefix)) continue; name = name.substr(prefix.size()); }
        if (name.empty() || name.find(L"../") != std::wstring::npos || name.find(L":") != std::wstring::npos || name[0] == L'/') continue;
        std::wstring out = dest + L"\\" + name;
        for (auto& c : out) if (c == L'/') c = L'\\';
        if (st.m_is_directory) { EnsureDir(out); continue; }
        EnsureDir(PathParent(out));
        SetFileAttributesW(out.c_str(), FILE_ATTRIBUTE_NORMAL);
        HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { *error = L"Couldn't write " + out + L" (is the server still running?)"; ok = false; break; }
        ok = mz_zip_reader_extract_to_callback(&z, i, WriteCb, h, 0);
        CloseHandle(h);
        if (!ok) *error = L"Couldn't extract " + name;
        done += st.m_uncomp_size;
        if (progress && !progress(done, total)) { ok = false; *error = L"Cancelled."; }
    }
    mz_zip_reader_end(&z);
    return ok;
}

bool ReadEntry(const std::wstring& zipPath, const std::wstring& name, std::string& out) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, WideToUtf8(zipPath).c_str(), 0)) return false;
    size_t size = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&z, WideToUtf8(name).c_str(), &size, 0);
    if (p) { out.assign((const char*)p, size); mz_free(p); }
    mz_zip_reader_end(&z);
    return p != nullptr;
}

std::vector<std::wstring> List(const std::wstring& zipPath) {
    std::vector<std::wstring> out;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, WideToUtf8(zipPath).c_str(), 0)) return out;
    mz_uint n = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < n; ++i) { mz_zip_archive_file_stat st; if (mz_zip_reader_file_stat(&z, i, &st)) out.push_back(Utf8ToWide(st.m_filename)); }
    mz_zip_reader_end(&z);
    return out;
}

} // namespace archive
