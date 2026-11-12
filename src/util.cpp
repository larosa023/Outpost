// Outpost - small helpers
#include "util.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <algorithm>

// ---------------------------------------------------------------- logging
static std::mutex s_logLock;
static std::wstring s_logPath;

std::wstring LogPath() { return s_logPath; }

void LogInit() {
    EnsureDir(DataDir());
    s_logPath = DataDir() + L"\\outpost.log";
    // keep the log small: roll it over at 1 MB
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(s_logPath.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 1024 * 1024)
        MoveFileExW(s_logPath.c_str(), (s_logPath + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2200];
    swprintf(line, 2200, L"%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %ls\r\n", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), buf);
    std::string u = WideToUtf8(line);
    std::lock_guard<std::mutex> g(s_logLock);
    if (s_logPath.empty()) return;
    HANDLE h = CreateFileW(s_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------- paths
std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return std::wstring(buf, n);
}
std::wstring ExeDir() { return PathParent(ExePath()); }

bool IsPortable() {
    static int s = -1;
    if (s < 0) s = (FileExists(ExeDir() + L"\\portable") || FileExists(ExeDir() + L"\\portable.txt")) ? 1 : 0;
    return s == 1;
}

std::wstring KnownFolder(const GUID& id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static std::wstring s_dataOverride;
void SetDataDir(const std::wstring& dir) { s_dataOverride = dir; }

std::wstring DataDir() {
    static std::wstring d;
    if (!s_dataOverride.empty()) return s_dataOverride;
    if (d.empty()) {
        if (IsPortable()) d = ExeDir() + L"\\data";
        else d = KnownFolder(FOLDERID_LocalAppData) + L"\\Outpost";
    }
    return d;
}
std::wstring CacheDir() { return DataDir() + L"\\cache"; }
std::wstring DownloadDir() { return DataDir() + L"\\downloads"; }

std::wstring ExpandEnv(const std::wstring& s) {
    wchar_t buf[4096];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 4096);
    if (n == 0 || n > 4096) return s;
    return buf;
}

std::wstring PathParent(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}
std::wstring PathName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
void EnsureDir(const std::wstring& p) { if (!p.empty()) SHCreateDirectoryExW(nullptr, p.c_str(), nullptr); }

bool PathStartsWith(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty() || path.size() < dir.size()) return false;
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
    if (path.size() == dir.size()) return true;
    wchar_t c = path[dir.size()];
    return c == L'\\' || c == L'/' || dir.back() == L'\\';
}

// ---------------------------------------------------------------- strings
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &u[0], n, nullptr, nullptr);
    return u;
}
std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
bool IEquals(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool IStartsWith(const std::wstring& s, const std::wstring& p) {
    return s.size() >= p.size() && _wcsnicmp(s.c_str(), p.c_str(), p.size()) == 0;
}
bool IContains(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

bool WildMatch(const std::wstring& pat, const std::wstring& s) {
    size_t p = 0, i = 0, star = std::wstring::npos, mark = 0;
    auto eq = [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); };
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == L'?' || eq(pat[p], s[i]))) { ++p; ++i; }
        else if (p < pat.size() && pat[p] == L'*') { star = p++; mark = i; }
        else if (star != std::wstring::npos) { p = star + 1; i = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == L'*') ++p;
    return p == pat.size();
}

std::wstring FormatBytes(uint64_t n) {
    wchar_t b[64];
    if (n < 1024) swprintf(b, 64, L"%llu B", (unsigned long long)n);
    else if (n < 1024ull * 1024) swprintf(b, 64, L"%.0f KB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024) swprintf(b, 64, L"%.1f MB", n / (1024.0 * 1024));
    else swprintf(b, 64, L"%.2f GB", n / (1024.0 * 1024 * 1024));
    return b;
}

std::wstring FormatAgo(const std::wstring& iso) {
    int Y, M, D, h, m, s;
    if (swscanf(iso.c_str(), L"%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return L"";
    SYSTEMTIME st{};
    st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D; st.wHour = (WORD)h; st.wMinute = (WORD)m; st.wSecond = (WORD)s;
    FILETIME ft, now;
    if (!SystemTimeToFileTime(&st, &ft)) return L"";
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = ft.dwLowDateTime; a.HighPart = ft.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    if (b.QuadPart < a.QuadPart) return L"just now";
    long long secs = (long long)((b.QuadPart - a.QuadPart) / 10000000ull);
    wchar_t buf[64];
    if (secs < 3600) return L"just now";
    if (secs < 86400) { long long n = secs / 3600; swprintf(buf, 64, L"%lld hour%ls ago", n, n == 1 ? L"" : L"s"); return buf; }
    long long days = secs / 86400;
    if (days < 31) { swprintf(buf, 64, L"%lld day%ls ago", days, days == 1 ? L"" : L"s"); return buf; }
    static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    swprintf(buf, 64, L"%ls %d, %d", mon[(M - 1) % 12], D, Y);
    return buf;
}

// ---------------------------------------------------------------- files
bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 256ll * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD r = 0;
    bool ok = sz.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)sz.QuadPart, &r, nullptr) && r == (DWORD)sz.QuadPart);
    CloseHandle(h);
    return ok;
}

bool ReadTextFile(const std::wstring& path, std::wstring& out) {
    std::string b;
    if (!ReadFileBytes(path, b)) return false;
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) b.erase(0, 3);
    out = Utf8ToWide(b);
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    EnsureDir(PathParent(path));
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size());
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
bool WriteTextFileAtomic(const std::wstring& path, const std::wstring& text) { return WriteFileAtomic(path, WideToUtf8(text)); }

bool LoadResourceText(int id, std::wstring& out) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    DWORD n = SizeofResource(nullptr, r);
    const char* p = g ? (const char*)LockResource(g) : nullptr;
    if (!p || !n) return false;
    std::string s(p, n);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    out = Utf8ToWide(s);
    return true;
}

// ---------------------------------------------------------------- versions
std::wstring NormalizeVersion(const std::wstring& v0) {
    std::wstring v = Trim(v0);
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
    size_t plus = v.find(L'+');
    if (plus != std::wstring::npos) v.erase(plus);
    return v;
}

static void SplitVersion(const std::wstring& v, std::vector<long>& nums, std::wstring& pre) {
    std::wstring s = NormalizeVersion(v);
    size_t dash = s.find(L'-');
    std::wstring core = dash == std::wstring::npos ? s : s.substr(0, dash);
    pre = dash == std::wstring::npos ? L"" : s.substr(dash + 1);
    size_t i = 0;
    while (i <= core.size()) {
        size_t j = core.find(L'.', i);
        if (j == std::wstring::npos) j = core.size();
        nums.push_back(wcstol(core.substr(i, j - i).c_str(), nullptr, 10));
        i = j + 1;
    }
    while (nums.size() > 1 && nums.back() == 0) nums.pop_back();   // 1.2 == 1.2.0 == 1.2.0.0
}

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    std::vector<long> na, nb;
    std::wstring pa, pb;
    SplitVersion(a, na, pa);
    SplitVersion(b, nb, pb);
    for (size_t i = 0; i < std::max(na.size(), nb.size()); ++i) {
        long x = i < na.size() ? na[i] : 0, y = i < nb.size() ? nb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (pa.empty() != pb.empty()) return pa.empty() ? 1 : -1;   // 1.0.0 > 1.0.0-beta
    // compare pre-release identifiers dot by dot, numerically where both are numbers
    size_t i = 0, j = 0;
    while (i < pa.size() || j < pb.size()) {
        size_t ie = pa.find(L'.', i), je = pb.find(L'.', j);
        if (ie == std::wstring::npos) ie = pa.size();
        if (je == std::wstring::npos) je = pb.size();
        std::wstring x = i < pa.size() ? pa.substr(i, ie - i) : L"", y = j < pb.size() ? pb.substr(j, je - j) : L"";
        if (x.empty() != y.empty()) return x.empty() ? -1 : 1;
        bool xn = !x.empty() && x.find_first_not_of(L"0123456789") == std::wstring::npos;
        bool yn = !y.empty() && y.find_first_not_of(L"0123456789") == std::wstring::npos;
        if (xn && yn) {
            long u = wcstol(x.c_str(), nullptr, 10), w = wcstol(y.c_str(), nullptr, 10);
            if (u != w) return u < w ? -1 : 1;
        } else {
            int c = _wcsicmp(x.c_str(), y.c_str());
            if (c) return c < 0 ? -1 : 1;
        }
        i = ie + 1; j = je + 1;
    }
    return 0;
}

// ---------------------------------------------------------------- hashing
std::wstring Sha256File(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD r = 0;
        bool ok = true;
        while (ReadFile(h, buf.data(), (DWORD)buf.size(), &r, nullptr) && r > 0) {
            if (BCryptHashData(hash, buf.data(), r, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
            static const wchar_t* d = L"0123456789abcdef";
            for (unsigned char c : digest) { hex += d[c >> 4]; hex += d[c & 15]; }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return hex;
}

// ---------------------------------------------------------------- exe version
std::wstring ExeVersion(const std::wstring& exe) {
    DWORD dummy = 0;
    DWORD n = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (!n) return L"";
    std::vector<BYTE> buf(n);
    if (!GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) return L"";
    struct LangCp { WORD lang, cp; } *tr = nullptr;
    UINT len = 0;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && len >= sizeof(LangCp)) {
        wchar_t key[128];
        swprintf(key, 128, L"\\StringFileInfo\\%04x%04x\\ProductVersion", tr[0].lang, tr[0].cp);
        wchar_t* val = nullptr;
        UINT vl = 0;
        if (VerQueryValueW(buf.data(), key, (void**)&val, &vl) && vl > 1) {
            std::wstring v = Trim(val);
            // "0.3.0" or "0.3.0.0"; keep pre-release text like "1.2.0-beta.1"
            if (!v.empty() && iswdigit(v[0])) return NormalizeVersion(v);
        }
    }
    VS_FIXEDFILEINFO* fi = nullptr;
    if (VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) && fi && fi->dwProductVersionMS) {
        wchar_t v[64];
        swprintf(v, 64, L"%u.%u.%u", HIWORD(fi->dwProductVersionMS), LOWORD(fi->dwProductVersionMS), HIWORD(fi->dwProductVersionLS));
        return v;
    }
    return L"";
}

void ExeStrings(const std::wstring& exe, std::wstring& company, std::wstring& product, std::wstring& description) {
    company.clear(); product.clear(); description.clear();
    DWORD dummy = 0;
    DWORD n = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (!n) return;
    std::vector<BYTE> buf(n);
    if (!GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) return;
    struct LangCp { WORD lang, cp; } *tr = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) || len < sizeof(LangCp)) return;
    auto get = [&](const wchar_t* name) {
        wchar_t key[128];
        swprintf(key, 128, L"\\StringFileInfo\\%04x%04x\\%ls", tr[0].lang, tr[0].cp, name);
        wchar_t* val = nullptr;
        UINT vl = 0;
        return (VerQueryValueW(buf.data(), key, (void**)&val, &vl) && vl > 1) ? Trim(val) : std::wstring();
    };
    company = get(L"CompanyName");
    product = get(L"ProductName");
    description = get(L"FileDescription");
}

// ---------------------------------------------------------------- processes
static bool Spawn(const std::wstring& exe, const std::wstring& args, bool hidden, PROCESS_INFORMATION* pi) {
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) cmd += L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    if (hidden) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
    std::wstring dir = PathParent(exe);
    return CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          dir.empty() ? nullptr : dir.c_str(), &si, pi) != 0;
}

bool RunAndWait(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs, DWORD* exitCode, bool hidden) {
    PROCESS_INFORMATION pi{};
    if (!Spawn(exe, args, hidden, &pi)) return false;
    CloseHandle(pi.hThread);
    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = code;
    return w == WAIT_OBJECT_0;
}

bool LaunchDetached(const std::wstring& exe, const std::wstring& args, DWORD* pid) {
    PROCESS_INFORMATION pi{};
    if (!Spawn(exe, args, false, &pi)) return false;
    if (pid) *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
