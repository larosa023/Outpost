// Outpost - folder helpers
#pragma once
#include "common.h"
#include <functional>

namespace fs {
bool DeleteTree(const std::wstring& path);                    // file or folder; clears read-only flags
bool CopyTree(const std::wstring& from, const std::wstring& to, const std::function<bool(uint64_t)>& progress = nullptr);
uint64_t TreeSize(const std::wstring& path);
uint64_t FreeSpace(const std::wstring& path);
bool IsAbsolute(const std::wstring& p);
std::wstring Join(const std::wstring& base, const std::wstring& rel);   // rel may be absolute
std::wstring TimeStamp();                                     // 20261112-153000 (local time)
std::wstring IsoNow();                                        // 2026-11-12T15:30:00Z
long long UnixNow();
}
