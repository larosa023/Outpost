// Outpost - one-click game servers for Windows
//
//   Outpost.exe                     open Outpost (or bring the running one to the front)
//   Outpost.exe --tray [--boot]     start in the tray (Start with Windows); --boot also starts "start on boot" servers
//   Outpost.exe --quit              close the running Outpost window (servers keep running)
//   Outpost.exe --cli <command>     command line, see cli.cpp
//   Outpost.exe --host <folder>     (internal) run one server
//   Outpost.exe --firewall add|remove <folder>   (internal, elevated) Windows Firewall rules
//   Outpost.exe --restore-state <file>           (Hangar live update) reopen where you were
#include "common.h"
#include "ui.h"
#include "util.h"
#include "config.h"
#include "net.h"
#include "host.h"
#include "cli.h"
#include "netx.h"
#include "template.h"
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

static HANDLE g_mutex;
static const wchar_t* kMutexName = L"Local\\Outpost.larosa023.SingleInstance";

void ReleaseInstanceMutex() { if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; } }
void ReacquireInstanceMutex() { if (!g_mutex) g_mutex = CreateMutexW(nullptr, TRUE, kMutexName); }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto has = [&](const wchar_t* k) { for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], k)) return i; return 0; };
    if (has(L"--version")) return 0;
    if (int i = has(L"--data"); i && i + 1 < argc) SetDataDir(argv[i + 1]);

    if (int i = has(L"--host"); i && i + 1 < argc) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        LogInit();
        net::Init();
        int code = host::Main(argv[i + 1]);
        net::Shutdown();
        return code;
    }
    if (int i = has(L"--firewall"); i && i + 2 < argc) {
        LogInit();
        std::wstring rest = std::wstring(argv[i + 1]) + L" \"" + argv[i + 2] + L"\"";
        return netx::firewall::RunElevatedHelper(rest);
    }
    if (has(L"--cli")) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        LogInit();
        ConfigLoad();
        net::Init();
        templates::Load();
        int code = cli::Main(argc, argv);
        net::Shutdown();
        return code;
    }
    if (has(L"--quit")) {
        if (HWND w = FindWindowW(L"Outpost.larosa023.Main", nullptr)) PostMessageW(w, WM_CLOSE, 1, 0x0D17);
        return 0;
    }

    UiStart start;
    start.after = selfupdate::ParseArgs(GetCommandLineW());
    start.tray = has(L"--tray") != 0;
    start.boot = has(L"--boot") != 0;
    if (int i = has(L"--restore-state"); i && i + 1 < argc) start.restoreState = argv[i + 1];
    LocalFree(argv);

    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS && (start.after.active || !start.restoreState.empty())) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        for (int i = 0; i < 100 && !g_mutex; ++i) {
            Sleep(100);
            HANDLE m = CreateMutexW(nullptr, TRUE, kMutexName);
            if (GetLastError() != ERROR_ALREADY_EXISTS) g_mutex = m; else CloseHandle(m);
        }
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"Outpost.larosa023.Main", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            if (!start.tray) PostMessageW(existing, WM_OP_ACTIVATE, 0, 0);
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetCurrentProcessExplicitAppUserModelID(L"larosa023.Outpost");
    LogInit();
    Log(L"==== Outpost %ls starting (data: %ls%ls)", OUTPOST_VERSION, DataDir().c_str(), IsPortable() ? L", portable" : L"");
    ConfigLoad();
    net::Init();
    templates::Load();

    HWND hwnd = UiCreate(inst, show, start);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Outpost couldn't start. Details are in outpost.log.", L"Outpost", MB_ICONERROR);
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    net::Shutdown();
    Log(L"Outpost window closed");
    CoUninitialize();
    if (g_mutex) CloseHandle(g_mutex);
    return 0;
}
