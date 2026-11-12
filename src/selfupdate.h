// Outpost - Outpost's own updater (same model as Hangar's).
//   1. a background job finds the newest release of larosa023/Outpost on your channel (Stable, or Beta for
//      pre-releases), downloads Outpost.exe and verifies its SHA-256 (GitHub's digest + SHA256SUMS.txt)
//   2. Outpost swaps its exe (rename the running one to .old, copy the new one in), hides its window and starts
//      the new copy with --after-update. Game servers are NOT touched: they run in host processes from their
//      own copy of the exe, and the new window reconnects to them.
//   3. the new copy signals "healthy" once its window is up; if it doesn't within 20 s, the old one puts
//      itself back and skips that version.
#pragma once
#include "common.h"

struct SelfUpdateState {
    enum Kind { Idle, Checking, UpToDate, Downloading, Ready, Error } kind = Idle;
    std::wstring version, notes, staged, message;
    int percent = -1;
};

namespace selfupdate {
void Check(HWND notify, bool force);
bool Apply(HWND hwnd, const std::wstring& exe, const std::wstring& version, const std::wstring& restoreArgs, std::wstring* error);

struct AfterUpdate { bool active = false; std::wstring fromVersion, healthyEvent; DWORD oldPid = 0; };
AfterUpdate ParseArgs(const wchar_t* cmdLine);
void SignalHealthy(const AfterUpdate& a);
void FinishAfterUpdate(const AfterUpdate& a);
struct Previous { std::wstring version, exe; };
std::vector<Previous> PreviousVersions();
std::wstring ExpectedHashFromSums(const std::string& sums, const std::wstring& assetName);
}

void ReleaseInstanceMutex();
void ReacquireInstanceMutex();
