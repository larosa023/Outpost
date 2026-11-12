// Outpost - JSON-lines over named pipes (overlapped, so reading and writing never block each other).
// Used between the window and each server's host process, and for Hangar's IPC protocol.
#pragma once
#include "common.h"
#include <functional>
#include <mutex>
#include <deque>
#include <atomic>
#include <memory>

namespace pipe {

std::wstring SessionSuffix();     // "<sessionId>", so two signed-in users never share a pipe

class Conn {
public:
    explicit Conn(HANDLE h);
    ~Conn();
    // Reads lines and calls onLine until the other side goes away or Close() is called.
    void Run(const std::function<void(const std::string&)>& onLine);
    void Send(const std::string& line);           // thread-safe; '\n' appended
    void Close();
    bool Open() const { return !closed_; }
private:
    HANDLE h_;
    HANDLE wake_;
    std::mutex m_;
    std::deque<std::string> out_;
    std::atomic<bool> closed_{false};
};

// Server: accepts clients on `name` forever (until Stop), one thread per client.
class Server {
public:
    using OnClient = std::function<void(std::shared_ptr<Conn>)>;   // runs on the client's own thread; call conn->Run()
    bool Start(const std::wstring& name, OnClient onClient);
    void Stop();
private:
    std::wstring name_;
    std::atomic<bool> stop_{false};
};

// Client: connects (waiting up to timeoutMs for a free instance). nullptr if nobody is listening.
std::shared_ptr<Conn> Connect(const std::wstring& name, DWORD timeoutMs);

// One request, one reply (used for quick calls such as Hangar's health ping)
bool Call(const std::wstring& name, const std::string& request, std::string* reply, DWORD timeoutMs);
}
