// Outpost - HTTP(S) via WinHTTP
#include "net.h"
#include "util.h"
#include <winhttp.h>
#include <algorithm>

namespace net {

static HINTERNET s_session = nullptr;

std::wstring UserAgent() { return std::wstring(L"Outpost/") + OUTPOST_VERSION + L" (+https://github.com/larosa023/Outpost)"; }

std::wstring ApiBase() {
    wchar_t buf[512];
    DWORD n = GetEnvironmentVariableW(L"OUTPOST_API_BASE", buf, 512);
    if (n > 0 && n < 512) return buf;
    return L"https://api.github.com";
}

void Init() {
    if (s_session) return;
    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (4) needs Windows 8.1+; fall back to the default proxy setting
    s_session = WinHttpOpen(UserAgent().c_str(), 4, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s_session) s_session = WinHttpOpen(UserAgent().c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s_session) {
        WinHttpSetTimeouts(s_session, 10000, 10000, 20000, 30000);
        DWORD protocols = 0x00000800 /*TLS1.2*/ | 0x00002000 /*TLS1.3*/;
        if (!WinHttpSetOption(s_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            protocols = 0x00000800;
            WinHttpSetOption(s_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
        DWORD decompress = 3; // WINHTTP_DECOMPRESSION_FLAG_ALL
        WinHttpSetOption(s_session, 118 /*WINHTTP_OPTION_DECOMPRESSION*/, &decompress, sizeof(decompress));
    }
}

void Shutdown() {
    if (s_session) WinHttpCloseHandle(s_session);
    s_session = nullptr;
}

struct Request {
    HINTERNET conn = nullptr, req = nullptr;
    ~Request() {
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
    }
};

static std::wstring LastErrorText(const wchar_t* what) {
    DWORD e = GetLastError();
    std::wstring msg;
    switch (e) {
        case 12007: msg = L"Couldn't find the server. Are you online?"; break;          // NAME_NOT_RESOLVED
        case 12029: msg = L"Couldn't connect to the server. Are you online?"; break;    // CANNOT_CONNECT
        case 12002: msg = L"The connection timed out."; break;                          // TIMEOUT
        case 12030: msg = L"The connection was interrupted."; break;                    // CONNECTION_ERROR
        case 12175: case 12045: case 12169: msg = L"Secure connection failed."; break;
        default: msg = std::wstring(what) + L" failed (error " + std::to_wstring(e) + L")."; break;
    }
    return msg;
}

static bool Open(const std::wstring& url, const std::wstring& extraHeaders, Request& r, std::wstring* error,
                 const wchar_t* method = L"GET", const std::string* body = nullptr) {
    if (!s_session) { if (error) *error = L"Networking isn't available."; return false; }
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[4096];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    wchar_t extra[2048];
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { if (error) *error = L"Bad address: " + url; return false; }
    std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    r.conn = WinHttpConnect(s_session, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    if (!r.conn) { if (error) *error = LastErrorText(L"Connecting"); return false; }
    r.req = WinHttpOpenRequest(r.conn, method, object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               https ? WINHTTP_FLAG_SECURE : 0);
    if (!r.req) { if (error) *error = LastErrorText(L"Request"); return false; }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;   // GitHub downloads redirect to its CDN
    WinHttpSetOption(r.req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    if (!WinHttpSendRequest(r.req, extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str(),
                            extraHeaders.empty() ? 0 : (DWORD)-1L, body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA,
                            body ? (DWORD)body->size() : 0, body ? (DWORD)body->size() : 0, 0) ||
        !WinHttpReceiveResponse(r.req, nullptr)) {
        if (error) *error = LastErrorText(L"Request");
        return false;
    }
    return true;
}

static std::wstring Header(HINTERNET req, const wchar_t* name) {
    wchar_t buf[512];
    DWORD len = sizeof(buf);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, buf, &len, WINHTTP_NO_HEADER_INDEX)) return std::wstring(buf, len / sizeof(wchar_t));
    return L"";
}

static int Status(HINTERNET req) {
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    return (int)status;
}

bool Send(const std::wstring& method, const std::wstring& url, const std::string& body, const std::wstring& headers, Response& r) {
    Request q;
    if (!Open(url, headers, q, &r.error, method.c_str(), &body)) return false;
    r.status = Status(q.req);
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(q.req, &avail) && avail) {
        std::string chunk(avail, 0);
        DWORD read = 0;
        if (!WinHttpReadData(q.req, &chunk[0], avail, &read) || !read) break;
        r.body.append(chunk.data(), read);
        if (r.body.size() > 16 * 1024 * 1024) break;
    }
    return true;
}

bool Get(const std::wstring& url, Response& r, const std::wstring& etag, bool githubApi) {
    std::wstring headers;
    if (githubApi) headers += L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!etag.empty()) headers += L"If-None-Match: " + etag + L"\r\n";
    Request q;
    if (!Open(url, headers, q, &r.error)) return false;
    r.status = Status(q.req);
    r.etag = Header(q.req, L"ETag");
    std::wstring rem = Header(q.req, L"x-ratelimit-remaining"), reset = Header(q.req, L"x-ratelimit-reset");
    if (!rem.empty()) r.rateRemaining = _wtoi(rem.c_str());
    if (!reset.empty()) r.rateReset = _wtoi64(reset.c_str());
    DWORD avail = 0;
    char buf[16384];
    while (WinHttpQueryDataAvailable(q.req, &avail) && avail > 0) {
        DWORD got = 0;
        if (!WinHttpReadData(q.req, buf, std::min<DWORD>(avail, sizeof(buf)), &got) || got == 0) break;
        r.body.append(buf, got);
        if (r.body.size() > 32u * 1024 * 1024) { r.error = L"Response too large."; return false; }
    }
    return true;
}

bool Download(const std::wstring& url, const std::wstring& path,
              const std::function<bool(uint64_t, uint64_t)>& progress, std::wstring* error) {
    Request q;
    if (!Open(url, L"Accept: application/octet-stream\r\n", q, error)) return false;
    int status = Status(q.req);
    if (status != 200) { if (error) *error = L"The download failed (HTTP " + std::to_wstring(status) + L")."; return false; }
    uint64_t total = 0;
    std::wstring cl = Header(q.req, L"Content-Length");
    if (!cl.empty()) total = _wcstoui64(cl.c_str(), nullptr, 10);
    EnsureDir(PathParent(path));
    std::wstring part = path + L".part";
    HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (error) *error = L"Couldn't write to " + part + L"."; return false; }
    uint64_t done = 0;
    bool ok = true, cancelled = false;
    std::vector<char> buf(256 * 1024);
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(q.req, &avail)) { ok = false; if (error) *error = LastErrorText(L"Download"); break; }
        if (avail == 0) break;
        DWORD got = 0;
        if (!WinHttpReadData(q.req, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &got)) { ok = false; if (error) *error = LastErrorText(L"Download"); break; }
        if (got == 0) break;
        DWORD w = 0;
        if (!WriteFile(h, buf.data(), got, &w, nullptr) || w != got) { ok = false; if (error) *error = L"Couldn't write the download (disk full?)."; break; }
        done += got;
        if (progress && !progress(done, total)) { ok = false; cancelled = true; if (error) *error = L"Cancelled."; break; }
    }
    CloseHandle(h);
    if (ok && total && done != total) { ok = false; if (error) *error = L"The download was incomplete."; }
    if (!ok) { DeleteFileW(part.c_str()); (void)cancelled; return false; }
    if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (error) *error = L"Couldn't save the download.";
        DeleteFileW(part.c_str());
        return false;
    }
    return true;
}

} // namespace net
