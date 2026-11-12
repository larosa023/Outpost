// Outpost - zip files (miniz): backups, and unpacking SteamCMD, Java, Bedrock and Terraria downloads
#pragma once
#include "common.h"
#include <functional>

namespace archive {
using Progress = std::function<bool(uint64_t done, uint64_t total)>;   // return false to cancel

struct Entry { std::wstring source, name; };   // file on disk, name inside the zip ("world/level.dat")
// Lists every file under each path (file or folder), naming them relative to base.
// Paths outside base are stored under "_external/<n>/" (see backup.cpp).
void Collect(const std::wstring& base, const std::wstring& path, const std::wstring& zipPrefix, std::vector<Entry>& out, uint64_t* bytes);
bool Create(const std::wstring& zipPath, const std::vector<Entry>& entries, const std::string& manifestJson,
            const Progress& progress, std::wstring* error);
// Extracts everything under `prefix` (or all when empty) into dest, dropping the prefix.
bool Extract(const std::wstring& zipPath, const std::wstring& dest, const std::wstring& prefix,
             const Progress& progress, std::wstring* error);
bool ReadEntry(const std::wstring& zipPath, const std::wstring& name, std::string& out);
std::vector<std::wstring> List(const std::wstring& zipPath);
}
