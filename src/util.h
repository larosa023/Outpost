// Outpost - small helpers: logging, paths, strings, files, versions, hashing
#pragma once
#include "common.h"
#include <functional>

void LogInit();
void Log(const wchar_t* fmt, ...);
std::wstring LogPath();

// Paths
std::wstring ExePath();
std::wstring ExeDir();
std::wstring DataDir();            // %LOCALAPPDATA%\Outpost, or .\data next to Outpost.exe in portable mode
std::wstring CacheDir();           // DataDir()\cache
std::wstring DownloadDir();        // DataDir()\downloads
bool IsPortable();
void SetDataDir(const std::wstring& dir);   // the host of a portable copy is told where the data is
std::wstring KnownFolder(const GUID& id);
std::wstring ExpandEnv(const std::wstring& s);
std::wstring PathParent(const std::wstring& p);
std::wstring PathName(const std::wstring& p);
bool FileExists(const std::wstring& p);
bool DirExists(const std::wstring& p);
void EnsureDir(const std::wstring& p);
bool PathStartsWith(const std::wstring& path, const std::wstring& dir);   // case-insensitive, dir boundary

// Strings
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::wstring Lower(std::wstring s);
std::wstring Trim(const std::wstring& s);
bool IEquals(const std::wstring& a, const std::wstring& b);
bool IStartsWith(const std::wstring& s, const std::wstring& prefix);
bool IContains(const std::wstring& hay, const std::wstring& needle);
bool WildMatch(const std::wstring& pattern, const std::wstring& s);     // '*' and '?', case-insensitive
std::wstring FormatBytes(uint64_t n);
std::wstring FormatAgo(const std::wstring& isoTime);                    // "2026-11-10T08:00:00Z" -> "3 days ago"

// Files
bool ReadFileBytes(const std::wstring& path, std::string& out);
bool ReadTextFile(const std::wstring& path, std::wstring& out);         // UTF-8 (BOM tolerated)
bool WriteFileAtomic(const std::wstring& path, const std::string& data);
bool WriteTextFileAtomic(const std::wstring& path, const std::wstring& text);
bool LoadResourceText(int id, std::wstring& out);                       // RCDATA resource, UTF-8

// Versions: "1.2.3", "v1.2.3", "1.2.3-beta.1". Pre-release suffixes sort before the release.
int CompareVersions(const std::wstring& a, const std::wstring& b);
std::wstring NormalizeVersion(const std::wstring& v);                   // strips a leading 'v' and build metadata

// SHA-256 of a file as lowercase hex ("" on failure)
std::wstring Sha256File(const std::wstring& path);

// Exe version resource (ProductVersion string, else the fixed file version)
std::wstring ExeVersion(const std::wstring& exe);

// Version-resource strings (CompanyName, ProductName, FileDescription); empty when absent
void ExeStrings(const std::wstring& exe, std::wstring& company, std::wstring& product, std::wstring& description);

// Processes
bool RunAndWait(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs, DWORD* exitCode, bool hidden);
bool LaunchDetached(const std::wstring& exe, const std::wstring& args, DWORD* pid = nullptr);
