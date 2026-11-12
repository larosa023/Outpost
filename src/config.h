// Outpost - app settings (DataDir()\settings.json). Each server's own settings live in its server.json.
#pragma once
#include "common.h"

struct Config {
    // look (the suite's shared theme format; Hangar can push a theme live)
    std::wstring theme = L"dark";        // built-in id (dark, light, system, midnight, glass…) or custom:<id>
    std::wstring accent = L"theme";      // theme | windows | #RRGGBB
    std::wstring backdrop = L"theme";    // theme | solid | mica | acrylic
    double fontScale = 1.0;              // 0.9 – 1.3
    std::wstring density = L"comfortable";   // compact | comfortable | spacious
    int radius = 8;                      // corner radius, 0 – 16
    std::wstring animation = L"normal";  // off | fast | normal | slow
    bool hangarThemes = true;            // accept live theme changes from Hangar
    // updates
    bool beta = false;
    bool autoUpdate = false;             // install updates silently (never stops servers)
    int checkHours = 3;
    std::wstring skipVersion;
    // system
    bool startWithWindows = false;
    bool closeToTray = true;
    bool notifyJoin = true, notifyCrash = true, notifyBackupFail = true;
    bool hidePublicIp = false;
    std::wstring curseForgeKey;
    std::wstring duckDomain, duckToken;
    // window
    int windowX = CW_USEDEFAULT, windowY = CW_USEDEFAULT, windowW = 0, windowH = 0;
    bool maximized = false;
    std::wstring lastServer;
    int lastTab = 0;
};

extern Config g_cfg;
void ConfigLoad();
void ConfigSave();
bool IsSkippedVersion(const std::wstring& v);
void SkipVersion(const std::wstring& v);
void PruneSkippedVersions(const std::wstring& current);
