// Outpost - one-click game servers for Windows (part of the larosa023 suite)
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

#ifndef OUTPOST_VERSION
#define OUTPOST_VERSION L"1.0.1"
#endif
#define OUTPOST_REPO_OWNER L"larosa023"
#define OUTPOST_REPO_NAME L"Outpost"
#define OUTPOST_REPO L"larosa023/Outpost"
#define OUTPOST_TEMPLATES_URL L"https://raw.githubusercontent.com/larosa023/Outpost/main/templates/"

// Messages posted from worker threads to the main window. lParam owns a heap object the window takes over.
enum : UINT {
    WM_OP_HOSTEVENT = WM_APP + 1,   // lParam: HostEvent*      (a running server's console line / status)
    WM_OP_JOB,                      // lParam: JobUpdate*      (install, backup, restore, update progress)
    WM_OP_ACTIVATE,                 // another Outpost.exe asked us to come to the front
    WM_OP_SELF,                     // lParam: SelfUpdateState*
    WM_OP_NET,                      // lParam: NetReport*
    WM_OP_MODS,                     // lParam: ModsResult*
    WM_OP_VERSIONS,                 // lParam: VersionsResult*
    WM_OP_TRAY,                     // tray icon callback
    WM_OP_HANGAR,                   // lParam: HangarRequest*  (Hangar IPC, answered on the UI thread)
    WM_OP_TEMPLATES,                // new templates were downloaded
    WM_OP_SYS,                      // lParam: SysStats*
};
