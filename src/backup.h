// Outpost - world backups: zip the template's save paths, keep the newest N, restore with a safety backup first
#pragma once
#include "server.h"
#include "archive.h"

struct BackupInfo {
    std::wstring path, label;
    long long time = 0;      // unix seconds
    uint64_t size = 0;
};

namespace backup {
std::wstring Dir(const ServerDef& s);
std::vector<BackupInfo> List(const ServerDef& s);          // newest first
// label: "manual", "automatic", "before restore", "before update"…
bool Create(const ServerDef& s, const Template& t, const std::wstring& label, const archive::Progress& progress,
            std::wstring* outPath, std::wstring* error);
void Prune(const ServerDef& s);                            // keeps s.keepBackups (safety backups from the last day are never removed)
// The server must be stopped. Takes a "before restore" backup first, then replaces the saves.
bool Restore(const ServerDef& s, const Template& t, const std::wstring& zip, const archive::Progress& progress, std::wstring* error);
}
