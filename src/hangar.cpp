// Outpost - Hangar IPC
#include "hangar.h"
#include "util.h"
#include <shlobj.h>
#include <thread>

void HangarRequest::Reply(json::Value v) {
    v.Set(L"id", json::Value::Num(req.GetNum(L"id", 0)));
    std::wstring o;
    json::WriteCompact(v, o);
    if (conn) conn->Send(WideToUtf8(o));
}

namespace hangar {

static pipe::Server s_server;
static std::wstring PipeName() { return L"\\\\.\\pipe\\larosa023.outpost." + pipe::SessionSuffix(); }
static std::wstring HangarPipe() { return L"\\\\.\\pipe\\larosa023.hangar." + pipe::SessionSuffix(); }
static std::wstring RunFile() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Hangar\\run\\outpost.json"; }

void Start(HWND notify) {
    s_server.Start(PipeName(), [notify](std::shared_ptr<pipe::Conn> c) {
        c->Run([notify, c](const std::string& line) {
            auto* r = new HangarRequest;
            r->conn = c;
            if (!json::Parser(Utf8ToWide(line)).Parse(r->req)) { delete r; return; }
            if (!PostMessageW(notify, WM_OP_HANGAR, 0, (LPARAM)r)) delete r;
        });
    });
    json::Value v = json::Value::MakeObject();
    v.Set(L"pid", json::Value::Num(GetCurrentProcessId()));
    v.Set(L"pipe", json::Value::Str(PipeName()));
    v.Set(L"version", json::Value::Str(OUTPOST_VERSION));
    v.Set(L"protocol", json::Value::Num(1));
    std::wstring o;
    json::Write(v, o);
    EnsureDir(PathParent(RunFile()));
    WriteTextFileAtomic(RunFile(), o);
}

void Stop() {
    s_server.Stop();
    // only remove the discovery file if it's still ours
    std::wstring text;
    json::Value v;
    if (ReadTextFile(RunFile(), text) && json::Parser(text).Parse(v) && (DWORD)v.GetNum(L"pid", 0) == GetCurrentProcessId()) DeleteFileW(RunFile().c_str());
}

static void Send(const std::string& msg) {
    std::thread([msg] {
        if (auto c = pipe::Connect(HangarPipe(), 1500)) {
            c->Send(msg);
            std::thread([c] { Sleep(1500); c->Close(); }).detach();
            c->Run([](const std::string&) {});
        }
    }).detach();
}

void SendHealth(const std::wstring& restored) {
    std::string base = "\"app\":\"outpost\",\"version\":\"" + WideToUtf8(OUTPOST_VERSION) + "\",\"pid\":" + std::to_string(GetCurrentProcessId());
    Send("{\"type\":\"health\"," + base + "}");
    if (!restored.empty()) Send("{\"type\":\"restored\"," + base + ",\"summary\":\"" + WideToUtf8(restored) + "\"}");
}

void SendBusy(bool busy, const std::wstring& reason) {
    Send(std::string("{\"type\":\"") + (busy ? "busy" : "idle") + "\",\"app\":\"outpost\",\"reason\":\"" + WideToUtf8(reason) + "\"}");
}

} // namespace hangar
