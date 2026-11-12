// Outpost - Direct2D / DirectWrite helpers and the theme
#pragma once
#include "common.h"
#include <d2d1.h>
#include <dwrite.h>

namespace gfx {

extern ID2D1Factory* d2d;
extern IDWriteFactory* dw;
bool Init();
const wchar_t* IconFont();      // Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets
const wchar_t* UiFont();        // Segoe UI Variable Text, else Segoe UI

// Cached text formats (never released; there are only a handful)
IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false, bool icon = false);

inline D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}
float Luma(D2D1_COLOR_F c);
inline D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
inline D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Theme {
    bool dark = true;
    D2D1_COLOR_F bg, sidebar, surface, surfaceHover, surfaceStrong, divider, text, subtext, faint;
    D2D1_COLOR_F accent, accentHover, onAccent, good, warn, bad, info;
    float radius = 8.0f;
    bool backdrop = false;          // a DWM backdrop shows through (Windows 11)
    int backdropType = 1;           // DWMSBT_*: 1 none, 2 Mica, 3 acrylic
    uint32_t captionRgb = 0x16161A; // title bar color when there's no backdrop
};
bool SystemUsesLightTheme();
uint32_t SystemAccent();                      // 0xRRGGBB
bool BackdropSupported();
void ApplyWindowTheme(HWND hwnd, const Theme& t);

struct Canvas {
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;

    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c) { br->SetColor(c); rt->FillRectangle(r, br); }
    void FillRound(const D2D1_RECT_F& r, float rad, D2D1_COLOR_F c) {
        br->SetColor(c);
        if (rad <= 0.01f) rt->FillRectangle(r, br);
        else rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    }
    void StrokeRound(const D2D1_RECT_F& r, float rad, D2D1_COLOR_F c, float w = 1.0f) {
        br->SetColor(c);
        D2D1_RECT_F rr = D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
        if (rad <= 0.01f) rt->DrawRectangle(rr, br, w);
        else rt->DrawRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), br, w);
    }
    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w = 1.0f) {
        br->SetColor(c);
        rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), br, w);
    }
    void Circle(float cx, float cy, float r, D2D1_COLOR_F c) {
        br->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br);
    }
    void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, D2D1_COLOR_F c) {
        if (!f || s.empty()) return;
        br->SetColor(c);
        rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Glyph(wchar_t g, IDWriteTextFormat* f, const D2D1_RECT_F& r, D2D1_COLOR_F c) {
        wchar_t s[2] = {g, 0};
        Text(s, f, r, c);
    }
};

IDWriteTextFormat* MonoFormat(float size);   // Cascadia Mono / Consolas, for consoles
float MeasureWidth(const std::wstring& s, IDWriteTextFormat* f);
float MeasureHeight(const std::wstring& s, IDWriteTextFormat* f, float width);

// Segoe Fluent Icons / MDL2 code points
namespace icon {
constexpr wchar_t Search = 0xE721, Grid = 0xF0E2, List = 0xE8FD, Refresh = 0xE72C, Back = 0xE72B, Settings = 0xE713,
    Download = 0xE896, Play = 0xE768, Folder = 0xE8B7, Delete = 0xE74D, Repair = 0xE90F, Cancel = 0xE711,
    Library = 0xE8F1, Update = 0xE895, Check = 0xE73E, Warning = 0xE7BA, Apps = 0xE71D, Link = 0xE71B,
    Windows = 0xE782, Audio = 0xE767, Game = 0xE7FC, Media = 0xE8B2, Work = 0xE821, Installed = 0xE7B8,
    ChevronRight = 0xE76C, Color = 0xE790, Info = 0xE946, Close = 0xE8BB, Globe = 0xE774, Star = 0xE734,
    Stop = 0xE71A, Pause = 0xE769, Add = 0xE710, Copy = 0xE8C8, People = 0xE716, Puzzle = 0xEA86, Save = 0xE74E,
    History = 0xE81C, Console = 0xE756, Home = 0xE80F, Server = 0xE968, Network = 0xE968, Shield = 0xEA18, Edit = 0xE70F,
    Upload = 0xE898, Sleep = 0xE708, Cpu = 0xE950, Memory = 0xEEA0, Eye = 0xE7B3, EyeOff = 0xED1A, Send = 0xE724,
    Ban = 0xF140, Crown = 0xE734, Kick = 0xE8F8, Clock = 0xE823, Restore = 0xE777, Filter = 0xE71C, More = 0xE712,
    Wrench = 0xE90F, Plug = 0xE83E, Lock = 0xE72E, Up = 0xE70E, Down = 0xE70D, Bell = 0xEA8F, Wifi = 0xE701;
}

} // namespace gfx
