// Outpost - JSON-lines over named pipes
#include "pipe.h"
#include "util.h"
#include <thread>

namespace pipe {

std::wstring SessionSuffix() {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    return std::to_wstring(sid);
}

Conn::Conn(HANDLE h) : h_(h) { wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
Conn::~Conn() { Close(); if (wake_) CloseHandle(wake_); }

void Conn::Close() {
    if (closed_.exchange(true)) return;
    SetEvent(wake_);
    CancelIoEx(h_, nullptr);
}

void Conn::Send(const std::string& line) {
    if (closed_) return;
    { std::lock_guard<std::mutex> g(m_); out_.push_back(line + "\n"); }
    SetEvent(wake_);
}

void Conn::Run(const std::function<void(const std::string&)>& onLine) {
    OVERLAPPED ro{}, wo{};
    ro.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wo.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    char buf[16384];
    std::string pending;
    bool reading = false, writing = false;
    std::string writeBuf;
    while (!closed_) {
        if (!reading) {
            ResetEvent(ro.hEvent);
            DWORD n = 0;
            if (ReadFile(h_, buf, sizeof(buf), &n, &ro)) {
                pending.append(buf, n);
            } else if (GetLastError() == ERROR_IO_PENDING) {
                reading = true;
            } else if (GetLastError() == ERROR_MORE_DATA) {
                GetOverlappedResult(h_, &ro, &n, FALSE);
                pending.append(buf, n);
            } else break;
            size_t p;
            while ((p = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, p);
                pending.erase(0, p + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) onLine(line);
            }
            if (pending.size() > 4 * 1024 * 1024) break;
            if (!reading) continue;
        }
        if (!writing) {
            std::lock_guard<std::mutex> g(m_);
            if (!out_.empty()) {
                writeBuf.clear();
                while (!out_.empty() && writeBuf.size() < 256 * 1024) { writeBuf += out_.front(); out_.pop_front(); }
                ResetEvent(wo.hEvent);
                DWORD w = 0;
                if (WriteFile(h_, writeBuf.data(), (DWORD)writeBuf.size(), &w, &wo)) writing = false;
                else if (GetLastError() == ERROR_IO_PENDING) writing = true;
                else { closed_ = true; break; }
                if (!writing && !out_.empty()) SetEvent(wake_);
            }
        }
        HANDLE waits[3] = {ro.hEvent, wake_, wo.hEvent};
        DWORD r = WaitForMultipleObjects(writing ? 3 : 2, waits, FALSE, 30000);
        if (r == WAIT_OBJECT_0) {
            DWORD n = 0;
            BOOL ok = GetOverlappedResult(h_, &ro, &n, FALSE);
            reading = false;
            if (!ok && GetLastError() != ERROR_MORE_DATA) break;
            pending.append(buf, n);
            size_t p;
            while ((p = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, p);
                pending.erase(0, p + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) onLine(line);
            }
        } else if (r == WAIT_OBJECT_0 + 2) {
            DWORD w = 0;
            if (!GetOverlappedResult(h_, &wo, &w, FALSE)) break;
            writing = false;
            std::lock_guard<std::mutex> g(m_);
            if (!out_.empty()) SetEvent(wake_);
        }
    }
    closed_ = true;
    CancelIoEx(h_, nullptr);
    if (reading) { DWORD n; GetOverlappedResult(h_, &ro, &n, TRUE); }
    if (writing) { DWORD n; GetOverlappedResult(h_, &wo, &n, TRUE); }
    CloseHandle(ro.hEvent);
    CloseHandle(wo.hEvent);
    DisconnectNamedPipe(h_);
    CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
}

bool Server::Start(const std::wstring& name, OnClient onClient) {
    name_ = name;
    // make sure we can create the first instance before reporting success
    HANDLE first = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    if (first == INVALID_HANDLE_VALUE) { Log(L"pipe %ls: can't create (%lu)", name.c_str(), GetLastError()); return false; }
    std::thread([this, first, onClient] {
        HANDLE h = first;
        while (!stop_) {
            if (h == INVALID_HANDLE_VALUE)
                h = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
            OVERLAPPED o{};
            o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL c = ConnectNamedPipe(h, &o);
            bool connected = c || GetLastError() == ERROR_PIPE_CONNECTED;
            if (!connected && GetLastError() == ERROR_IO_PENDING) {
                while (!stop_) {
                    if (WaitForSingleObject(o.hEvent, 500) == WAIT_OBJECT_0) { DWORD n; connected = GetOverlappedResult(h, &o, &n, FALSE) != 0; break; }
                }
            }
            CloseHandle(o.hEvent);
            if (stop_) { CancelIoEx(h, nullptr); CloseHandle(h); break; }
            if (!connected) { CloseHandle(h); h = INVALID_HANDLE_VALUE; continue; }
            auto conn = std::make_shared<Conn>(h);
            std::thread([conn, onClient] { onClient(conn); }).detach();
            h = INVALID_HANDLE_VALUE;
        }
    }).detach();
    return true;
}

void Server::Stop() {
    stop_ = true;
    // poke the accept loop
    HANDLE h = CreateFileW(name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

std::shared_ptr<Conn> Connect(const std::wstring& name, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    while (true) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) return std::make_shared<Conn>(h);
        DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY) return nullptr;
        DWORD left = GetTickCount() - start;
        if (left >= timeoutMs) return nullptr;
        WaitNamedPipeW(name.c_str(), std::min<DWORD>(timeoutMs - left, 1000));
    }
}

bool Call(const std::wstring& name, const std::string& request, std::string* reply, DWORD timeoutMs) {
    auto c = Connect(name, timeoutMs);
    if (!c) return false;
    std::string got;
    bool have = false;
    c->Send(request);
    std::thread killer([c, timeoutMs] { Sleep(timeoutMs); c->Close(); });
    killer.detach();
    c->Run([&](const std::string& line) { got = line; have = true; c->Close(); });
    if (reply) *reply = got;
    return have;
}

} // namespace pipe
