// Outpost - talking to running servers over the network: Source RCON (Minecraft, ARK, Rust, CS2),
// telnet (7 Days to Die), and plain HTTP for local admin APIs (Palworld) and UPnP routers.
#pragma once
#include "common.h"
#include <mutex>

namespace sock {
void Init();                                         // WSAStartup once
uintptr_t Connect(const std::string& host, int port, int timeoutMs);   // INVALID (~0) on failure
void Close(uintptr_t s);
bool SendAll(uintptr_t s, const std::string& data);
int Recv(uintptr_t s, char* buf, int n, int timeoutMs);   // bytes, 0 closed, -1 timeout/error
constexpr uintptr_t kInvalid = ~(uintptr_t)0;
}

// Minimal HTTP/1.1 client for plain-http endpoints on the LAN / this PC
namespace http {
struct Result { int status = 0; std::string body; std::string error; };
Result Request(const std::string& method, const std::string& url, const std::string& body = "",
               const std::vector<std::string>& headers = {}, int timeoutMs = 5000);
std::string Base64(const std::string& s);
}

class Rcon {
public:
    bool Configure(int port, const std::string& password);   // returns true if changed
    // Runs a command; empty optional on failure. Thread-safe.
    bool Exec(const std::string& cmd, std::string* reply, int timeoutMs = 4000);
    void Disconnect();
    bool Connected() const { return s_ != sock::kInvalid; }
private:
    bool Ensure();
    bool SendPacket(int id, int type, const std::string& body);
    bool ReadPacket(int* id, int* type, std::string* body, int timeoutMs);
    std::mutex m_;
    uintptr_t s_ = sock::kInvalid;
    int port_ = 0, nextId_ = 10;
    std::string password_;
    unsigned long failedAt_ = 0;
};

class Telnet {
public:
    bool Configure(int port, const std::string& password);
    bool Exec(const std::string& cmd, std::string* reply, int timeoutMs = 2500);
    void Disconnect();
private:
    bool Ensure();
    std::mutex m_;
    uintptr_t s_ = sock::kInvalid;
    int port_ = 0;
    std::string password_;
    unsigned long failedAt_ = 0;
};
