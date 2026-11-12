// Outpost - mods and plugins: Modrinth (and CurseForge with your API key) for Minecraft Java,
// a plain mods folder for other games. Disabled mods move to a ".disabled" subfolder, not the bin.
#pragma once
#include "server.h"

struct ModHit {
    std::wstring source, projectId, slug, title, description, author, icon, kind;   // kind: mod | plugin
    double downloads = 0;
};
struct ModVersion {
    std::wstring id, name, number, file, url, sha1, sha512;
    std::vector<std::wstring> gameVersions, loaders;
    uint64_t size = 0;
};
struct ModFile { std::wstring file, title, version, source; bool enabled = true, tracked = false; uint64_t size = 0; int index = -1; };

namespace mods {
struct Target { std::wstring loader, folder, kind, mc; bool ok = false; };   // where mods go for this server
Target TargetFor(const ServerDef& s, const Template& t);
std::wstring FolderFor(const ServerDef& s, const Template& t);           // full path ("" = no mods for this game)

std::vector<ModHit> Search(const ServerDef& s, const Template& t, const std::wstring& query, const std::wstring& source, std::wstring* error);
std::vector<ModVersion> Versions(const ServerDef& s, const Template& t, const ModHit& hit, std::wstring* error);   // compatible, newest first
bool Install(ServerDef& s, const Template& t, const ModHit& hit, const ModVersion& v, std::wstring* error);
// Newer compatible versions for installed Modrinth mods: index into s.mods -> version
std::vector<std::pair<int, ModVersion>> CheckUpdates(const ServerDef& s, const Template& t, std::wstring* error);
bool Update(ServerDef& s, const Template& t, int index, const ModVersion& v, std::wstring* error);
std::vector<ModFile> List(const ServerDef& s, const Template& t);         // what's in the folder (tracked or not)
bool SetEnabled(ServerDef& s, const Template& t, const std::wstring& file, bool enabled, std::wstring* error);
bool Remove(ServerDef& s, const Template& t, const std::wstring& file, std::wstring* error);
bool AddFile(ServerDef& s, const Template& t, const std::wstring& path, std::wstring* error);
}
