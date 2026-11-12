// Outpost - the window's connection to each server's host. Reconnects by itself (after an Outpost update,
// or when a host starts), and posts everything to the window as WM_OP_HOSTEVENT.
#pragma once
#include "common.h"

struct HostEvent {
    std::wstring server;
    std::string line;           // one JSON message from the host
    bool connected = false, disconnected = false;
};

namespace hostlink {
void Init(HWND notify);
void Watch(const std::wstring& serverId);
void Unwatch(const std::wstring& serverId);
bool Send(const std::wstring& serverId, const std::string& json);
bool Connected(const std::wstring& serverId);
void Shutdown();
}
