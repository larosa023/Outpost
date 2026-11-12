// Outpost - RCON, telnet and small HTTP
#include "remote.h"
#include "util.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>

namespace sock {

void Init() {
    static bool done = false;
    if (done) return;
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    done = true;
}

uintptr_t Connect(const std::string& host, int port, int timeoutMs) {
    Init();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return kInvalid;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return kInvalid; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    fd_set w, e;
    FD_ZERO(&w); FD_SET(s, &w); FD_ZERO(&e); FD_SET(s, &e);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, nullptr, &w, &e, &tv) <= 0 || FD_ISSET(s, &e)) { closesocket(s); return kInvalid; }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    return (uintptr_t)s;
}

void Close(uintptr_t s) { if (s != kInvalid) closesocket((SOCKET)s); }

bool SendAll(uintptr_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send((SOCKET)s, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

int Recv(uintptr_t s, char* buf, int n, int timeoutMs) {
    fd_set r;
    FD_ZERO(&r); FD_SET((SOCKET)s, &r);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, &r, nullptr, nullptr, &tv) <= 0) return -1;
    int got = recv((SOCKET)s, buf, n, 0);
    return got < 0 ? -1 : got;
}

} // namespace sock

namespace http {

std::string Base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; bits += 8;
        while (bits >= 0) { out.push_back(t[(val >> bits) & 0x3F]); bits -= 6; }
    }
    if (bits > -6) out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

Result Request(const std::string& method, const std::string& url, const std::string& body,
               const std::vector<std::string>& headers, int timeoutMs) {
    Result r;
    if (url.compare(0, 7, "http://") != 0) { r.error = "only http:// is supported here"; return r; }
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string host = hostport;
    int port = 80;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) { host = hostport.substr(0, colon); port = atoi(hostport.c_str() + colon + 1); }
    uintptr_t s = sock::Connect(host, port, std::min(timeoutMs, 3000));
    if (s == sock::kInvalid) { r.error = "couldn't connect"; return r; }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + hostport + "\r\nConnection: close\r\nUser-Agent: Outpost\r\n";
    for (auto& h : headers) req += h + "\r\n";
    if (!body.empty() || method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    if (!sock::SendAll(s, req)) { sock::Close(s); r.error = "send failed"; return r; }
    std::string resp;
    char buf[8192];
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)timeoutMs) {
        int n = sock::Recv(s, buf, sizeof(buf), 500);
        if (n == 0) break;
        if (n > 0) resp.append(buf, n);
        if (resp.size() > 4 * 1024 * 1024) break;
    }
    sock::Close(s);
    size_t he = resp.find("\r\n\r\n");
    if (resp.compare(0, 5, "HTTP/") != 0 || he == std::string::npos) { r.error = "no reply"; return r; }
    r.status = atoi(resp.c_str() + resp.find(' ') + 1);
    std::string head = resp.substr(0, he), b = resp.substr(he + 4);
    std::string lh = head;
    for (auto& c : lh) c = (char)tolower((unsigned char)c);
    if (lh.find("transfer-encoding: chunked") != std::string::npos) {
        std::string out;
        size_t p = 0;
        while (p < b.size()) {
            size_t e = b.find("\r\n", p);
            if (e == std::string::npos) break;
            size_t len = strtoul(b.substr(p, e - p).c_str(), nullptr, 16);
            if (!len) break;
            out += b.substr(e + 2, len);
            p = e + 2 + len + 2;
        }
        b = out;
    }
    r.body = b;
    return r;
}

} // namespace http

// ---------------------------------------------------------------- Source RCON
bool Rcon::Configure(int port, const std::string& password) {
    std::lock_guard<std::mutex> g(m_);
    if (port == port_ && password == password_) return false;
    port_ = port; password_ = password;
    sock::Close(s_); s_ = sock::kInvalid;
    return true;
}

void Rcon::Disconnect() { std::lock_guard<std::mutex> g(m_); sock::Close(s_); s_ = sock::kInvalid; }

bool Rcon::SendPacket(int id, int type, const std::string& body) {
    std::string p;
    int32_t size = (int32_t)(4 + 4 + body.size() + 2);
    p.append((const char*)&size, 4);
    p.append((const char*)&id, 4);
    p.append((const char*)&type, 4);
    p += body;
    p.push_back(0); p.push_back(0);
    return sock::SendAll(s_, p);
}

bool Rcon::ReadPacket(int* id, int* type, std::string* body, int timeoutMs) {
    auto readN = [&](char* dst, int n) {
        int got = 0;
        while (got < n) { int r = sock::Recv(s_, dst + got, n - got, timeoutMs); if (r <= 0) return false; got += r; }
        return true;
    };
    int32_t size = 0;
    if (!readN((char*)&size, 4) || size < 10 || size > 1 << 20) return false;
    std::string buf(size, 0);
    if (!readN(&buf[0], size)) return false;
    memcpy(id, buf.data(), 4);
    memcpy(type, buf.data() + 4, 4);
    *body = buf.substr(8, size - 10);
    return true;
}

bool Rcon::Ensure() {
    if (s_ != sock::kInvalid) return true;
    if (!port_ || password_.empty()) return false;
    if (failedAt_ && GetTickCount() - failedAt_ < 3000) return false;
    s_ = sock::Connect("127.0.0.1", port_, 1500);
    if (s_ == sock::kInvalid) { failedAt_ = GetTickCount(); return false; }
    int id = nextId_++, rid = 0, type = 0;
    std::string body;
    bool ok = SendPacket(id, 3, password_);
    // some servers send an empty RESPONSE_VALUE before the AUTH_RESPONSE
    for (int i = 0; ok && i < 2; ++i) {
        ok = ReadPacket(&rid, &type, &body, 3000);
        if (ok && type == 2) break;
    }
    if (!ok || rid == -1 || type != 2) {
        if (ok && rid == -1) Log(L"RCON: the password was refused");
        sock::Close(s_); s_ = sock::kInvalid; failedAt_ = GetTickCount();
        return false;
    }
    failedAt_ = 0;
    return true;
}

bool Rcon::Exec(const std::string& cmd, std::string* reply, int timeoutMs) {
    std::lock_guard<std::mutex> g(m_);
    if (!Ensure()) return false;
    int id = nextId_++;
    if (nextId_ > 1000000) nextId_ = 10;
    if (!SendPacket(id, 2, cmd)) { sock::Close(s_); s_ = sock::kInvalid; return false; }
    std::string all;
    int rid = 0, type = 0;
    std::string body;
    bool got = false;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)timeoutMs) {
        if (!ReadPacket(&rid, &type, &body, got ? 150 : timeoutMs)) break;   // after the first packet, wait briefly for more
        if (rid == id) { all += body; got = true; }
    }
    if (!got) { sock::Close(s_); s_ = sock::kInvalid; return false; }
    if (reply) *reply = all;
    return true;
}

// ---------------------------------------------------------------- telnet (7 Days to Die)
bool Telnet::Configure(int port, const std::string& password) {
    std::lock_guard<std::mutex> g(m_);
    if (port == port_ && password == password_) return false;
    port_ = port; password_ = password;
    sock::Close(s_); s_ = sock::kInvalid;
    return true;
}
void Telnet::Disconnect() { std::lock_guard<std::mutex> g(m_); sock::Close(s_); s_ = sock::kInvalid; }

static std::string Drain(uintptr_t s, int firstMs, int quietMs) {
    std::string out;
    char buf[4096];
    int n = sock::Recv(s, buf, sizeof(buf), firstMs);
    while (n > 0) {
        out.append(buf, n);
        n = sock::Recv(s, buf, sizeof(buf), quietMs);
    }
    return out;
}

bool Telnet::Ensure() {
    if (s_ != sock::kInvalid) return true;
    if (!port_) return false;
    if (failedAt_ && GetTickCount() - failedAt_ < 3000) return false;
    s_ = sock::Connect("127.0.0.1", port_, 1500);
    if (s_ == sock::kInvalid) { failedAt_ = GetTickCount(); return false; }
    std::string hello = Drain(s_, 2000, 300);
    if (hello.find("password") != std::string::npos || hello.find("Password") != std::string::npos) {
        sock::SendAll(s_, password_ + "\r\n");
        std::string r = Drain(s_, 2000, 300);
        if (r.find("incorrect") != std::string::npos || r.find("wrong") != std::string::npos) {
            sock::Close(s_); s_ = sock::kInvalid; failedAt_ = GetTickCount();
            return false;
        }
    }
    return true;
}

bool Telnet::Exec(const std::string& cmd, std::string* reply, int timeoutMs) {
    std::lock_guard<std::mutex> g(m_);
    if (!Ensure()) return false;
    Drain(s_, 1, 1);
    if (!sock::SendAll(s_, cmd + "\r\n")) { sock::Close(s_); s_ = sock::kInvalid; return false; }
    std::string r = Drain(s_, timeoutMs, 400);
    if (reply) *reply = r;
    return true;
}
