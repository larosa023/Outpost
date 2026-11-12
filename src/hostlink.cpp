// Outpost - the window's connection to each host
#include "hostlink.h"
#include "host.h"
#include "pipe.h"
#include "util.h"
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>

namespace hostlink {

struct Link {
    std::wstring id;
    std::shared_ptr<pipe::Conn> conn;
    std::atomic<bool> stop{false};
    long long lastSeq = 0;
};

static HWND s_notify;
static std::mutex s_m;
static std::map<std::wstring, std::shared_ptr<Link>> s_links;

void Init(HWND notify) { s_notify = notify; }

static void Post(HostEvent* e) { if (!s_notify || !PostMessageW(s_notify, WM_OP_HOSTEVENT, 0, (LPARAM)e)) delete e; }

static void Loop(std::shared_ptr<Link> l) {
    bool wasConnected = false;
    while (!l->stop) {
        if (!host::IsRunning(l->id)) {
            if (wasConnected) { auto* e = new HostEvent; e->server = l->id; e->disconnected = true; Post(e); wasConnected = false; }
            for (int i = 0; i < 6 && !l->stop; ++i) Sleep(100);
            continue;
        }
        auto c = pipe::Connect(host::PipeName(l->id), 2000);
        if (!c) { Sleep(300); continue; }
        { std::lock_guard<std::mutex> g(s_m); l->conn = c; }
        wasConnected = true;
        auto* e = new HostEvent; e->server = l->id; e->connected = true; Post(e);
        c->Send("{\"type\":\"subscribe\",\"since\":" + std::to_string(l->lastSeq) + "}");
        c->Run([l](const std::string& line) {
            // remember the newest console line so a reconnect only sends what we missed
            size_t p = line.find("\"n\":");
            if (line.compare(0, 12, "{\"ev\":\"line\"") == 0 && p != std::string::npos) l->lastSeq = std::max(l->lastSeq, atoll(line.c_str() + p + 4));
            auto* ev = new HostEvent; ev->server = l->id; ev->line = line; Post(ev);
        });
        { std::lock_guard<std::mutex> g(s_m); l->conn.reset(); }
        if (!l->stop) { auto* d = new HostEvent; d->server = l->id; d->disconnected = true; Post(d); }
        wasConnected = false;
        Sleep(300);
    }
}

void Watch(const std::wstring& id) {
    std::lock_guard<std::mutex> g(s_m);
    if (s_links.count(id)) return;
    auto l = std::make_shared<Link>();
    l->id = id;
    s_links[id] = l;
    std::thread([l] { Loop(l); }).detach();
}

void Unwatch(const std::wstring& id) {
    std::lock_guard<std::mutex> g(s_m);
    auto it = s_links.find(id);
    if (it == s_links.end()) return;
    it->second->stop = true;
    if (it->second->conn) it->second->conn->Close();
    s_links.erase(it);
}

bool Send(const std::wstring& id, const std::string& json) {
    std::lock_guard<std::mutex> g(s_m);
    auto it = s_links.find(id);
    if (it == s_links.end() || !it->second->conn) return false;
    it->second->conn->Send(json);
    return true;
}

bool Connected(const std::wstring& id) {
    std::lock_guard<std::mutex> g(s_m);
    auto it = s_links.find(id);
    return it != s_links.end() && it->second->conn != nullptr;
}

void Shutdown() {
    std::lock_guard<std::mutex> g(s_m);
    for (auto& kv : s_links) { kv.second->stop = true; if (kv.second->conn) kv.second->conn->Close(); }
    s_links.clear();
}

} // namespace hostlink
