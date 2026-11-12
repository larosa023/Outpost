// Outpost - the Hangar SDK side of the suite's IPC protocol (Hangar's docs/IPC.md, protocol 1):
// a pipe server Hangar can talk to (hello, status, prepare-update, restart, apply-theme, apply-settings,
// export-settings), a discovery file in %LOCALAPPDATA%\Hangar\run\, and the health ping after a start.
#pragma once
#include "common.h"
#include "json.h"
#include "pipe.h"
#include <memory>

struct HangarRequest {
    json::Value req;
    std::shared_ptr<pipe::Conn> conn;
    void Reply(json::Value v);             // echoes the request id
};

namespace hangar {
void Start(HWND notify);                   // pipe server + discovery file
void Stop();
void SendHealth(const std::wstring& restoredSummary);   // tells Hangar we started fine (and what we restored)
void SendBusy(bool busy, const std::wstring& reason);
}
