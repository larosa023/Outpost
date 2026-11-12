// Outpost - folder helpers
#include "fsutil.h"
#include "util.h"
#include <ctime>

namespace fs {

bool DeleteTree(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path.c_str()) != 0;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(path.c_str()) != 0;   // a junction: never follow
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((path + L"\\*").c_str(), &fd);
    bool ok = true;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            ok &= DeleteTree(path + L"\\" + n);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    for (int i = 0; i < 5; ++i) { if (RemoveDirectoryW(path.c_str())) return ok; Sleep(100); }
    return false;
}

bool CopyTree(const std::wstring& from, const std::wstring& to, const std::function<bool(uint64_t)>& progress) {
    DWORD a = GetFileAttributesW(from.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        EnsureDir(PathParent(to));
        if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) return false;
        if (progress) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            GetFileAttributesExW(to.c_str(), GetFileExInfoStandard, &fa);
            return progress(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
        }
        return true;
    }
    EnsureDir(to);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        ok = CopyTree(from + L"\\" + n, to + L"\\" + n, progress);
    } while (ok && FindNextFileW(f, &fd));
    FindClose(f);
    return ok;
}

uint64_t TreeSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += TreeSize(path + L"\\" + n);
        else total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return total;
}

uint64_t FreeSpace(const std::wstring& path) {
    ULARGE_INTEGER free{};
    std::wstring p = path;
    while (!p.empty() && !DirExists(p)) p = PathParent(p);
    if (GetDiskFreeSpaceExW(p.empty() ? nullptr : p.c_str(), &free, nullptr, nullptr)) return free.QuadPart;
    return 0;
}

bool IsAbsolute(const std::wstring& p) { return p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\')); }
std::wstring Join(const std::wstring& base, const std::wstring& rel) {
    std::wstring r = ExpandEnv(rel);
    for (auto& c : r) if (c == L'/') c = L'\\';
    return IsAbsolute(r) ? r : base + L"\\" + r;
}

std::wstring TimeStamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t b[32]; swprintf(b, 32, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}
std::wstring IsoNow() {
    SYSTEMTIME st; GetSystemTime(&st);
    wchar_t b[32]; swprintf(b, 32, L"%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}
long long UnixNow() { return (long long)time(nullptr); }

} // namespace fs
