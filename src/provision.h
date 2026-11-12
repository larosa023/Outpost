// Outpost - getting a game's server files: Minecraft (Mojang, Paper, Fabric, Forge, NeoForge) with an
// automatic Java runtime, Minecraft Bedrock, Terraria, and SteamCMD for everything on Steam.
#pragma once
#include "server.h"
#include <functional>
#include <atomic>

namespace provision {

using Progress = std::function<void(int percent, const std::wstring& text)>;   // percent -1: indeterminate

struct Versions {
    std::vector<std::wstring> list;      // newest first
    std::wstring latest;
    std::wstring error;
};
Versions ListVersions(const Template& t, const std::wstring& variant);

// Downloads/updates the server files into s.FilesDir() and fills s.installedVersion and s.vars.
bool Install(ServerDef& s, const Template& t, const Progress& progress, std::atomic<bool>* cancel, std::wstring* error);

// "" when up to date (or unknown); otherwise the newer version available.
std::wstring CheckUpdate(const ServerDef& s, const Template& t, std::wstring* error);

// Detects which game a folder holds (for "Import an existing server"). Returns template id and fills variant.
std::wstring DetectGame(const std::wstring& folder, std::wstring* variant);

std::wstring HashFile(const std::wstring& path, const wchar_t* alg);   // BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA512_ALGORITHM
bool EnsureJava(int major, const Progress& progress, std::atomic<bool>* cancel, std::wstring* javaExe, std::wstring* error);
bool EnsureSteamCmd(const Progress& progress, std::wstring* exe, std::wstring* error);
}
