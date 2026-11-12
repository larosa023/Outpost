// Outpost - the big operations, shared by the window and the command line: create, install, import,
// update the game files (with a backup first), delete.
#pragma once
#include "server.h"
#include "provision.h"

namespace ops {
// A new server folder with defaults filled in (nothing downloaded yet)
bool Create(const std::wstring& game, const std::wstring& variant, const std::wstring& version, const std::wstring& name,
            const std::map<std::wstring, std::wstring>& settings, int ramMb, const std::wstring& location, ServerDef* out, std::wstring* error);
// Download the game files, write config, save. Used for new servers, Repair, and updates.
bool Install(ServerDef& s, const provision::Progress& progress, std::atomic<bool>* cancel, std::wstring* error);
// Update the game files: backup first ("before update"), then Install. The server must be stopped.
bool UpdateFiles(ServerDef& s, const provision::Progress& progress, std::atomic<bool>* cancel, std::wstring* error);
// Copy an existing server folder (or just a world folder) into a new Outpost server
bool Import(const std::wstring& folder, const std::wstring& name, const std::wstring& gameHint, const provision::Progress& progress,
            ServerDef* out, std::wstring* error);
bool Delete(const ServerDef& s, bool backupsToo, std::wstring* error);
// Ports already used by other Outpost servers, so new ones don't clash
void PickFreePorts(ServerDef& s, const Template& t);
}
