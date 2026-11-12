// Outpost - themes: built-in presets, custom theme files, accent colors.
//
// A theme is a small JSON file in the suite's shared format (docs/THEMES.md), so the same file
// can theme every app once the SDK lands (Phase 4). Custom themes live in DataDir()\themes\.
#pragma once
#include "common.h"
#include "gfx.h"

struct ThemeDef {
    std::wstring id, name, description;
    bool dark = true;
    bool followWindows = false;     // "Windows": light or dark to match Windows
    std::wstring backdrop = L"mica"; // mica | acrylic | none
    float transparency = 0.20f;     // how much of the backdrop shows through the background (0-1)
    uint32_t bg = 0, sidebar = 0, surface = 0, surfaceHover = 0, surfaceStrong = 0;
    uint32_t text = 0, subtext = 0, faint = 0;
    uint32_t accent = 0;            // 0: use the Windows accent color
    float surfaceOpacity = 1.0f;    // < 1 for see-through cards (Glass)
    bool custom = false;
    std::wstring file;
};

const std::vector<ThemeDef>& BuiltinThemes();
std::vector<ThemeDef> LoadCustomThemes();                 // DataDir()\themes\*.json
std::wstring ThemesDir();
bool ParseThemeJson(const std::wstring& text, ThemeDef& out, std::wstring* error);
std::wstring ThemeToJson(const ThemeDef& t, const std::wstring& accentSetting);

// Finds a theme by id among the built-in and custom ones (falls back to Dark)
ThemeDef FindTheme(const std::wstring& id, const std::vector<ThemeDef>& custom);

// accentSetting: "theme" (the theme's own), "windows", or "#RRGGBB"
gfx::Theme MakeTheme(const ThemeDef& def, const std::wstring& accentSetting);
uint32_t ResolveAccent(const ThemeDef& def, const std::wstring& accentSetting);

extern const uint32_t kAccentSwatches[];
extern const int kAccentSwatchCount;
