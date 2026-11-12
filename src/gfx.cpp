// Outpost - Direct2D / DirectWrite helpers and the theme
#include "gfx.h"
#include <unordered_map>
#include "util.h"
#include <dwmapi.h>
#include <map>
#include <tuple>

namespace gfx {

ID2D1Factory* d2d = nullptr;
IDWriteFactory* dw = nullptr;
static std::wstring s_iconFont = L"Segoe MDL2 Assets";
static std::wstring s_uiFont = L"Segoe UI";

static bool FontExists(IDWriteFontCollection* sys, const wchar_t* name) {
    UINT32 idx;
    BOOL exists = FALSE;
    return sys && SUCCEEDED(sys->FindFamilyName(name, &idx, &exists)) && exists;
}

bool Init() {
    if (d2d && dw) return true;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw));
    if (!d2d || !dw) return false;
    IDWriteFontCollection* sys = nullptr;
    dw->GetSystemFontCollection(&sys, FALSE);
    if (FontExists(sys, L"Segoe Fluent Icons")) s_iconFont = L"Segoe Fluent Icons";
    if (FontExists(sys, L"Segoe UI Variable Text")) s_uiFont = L"Segoe UI Variable Text";
    else if (!FontExists(sys, L"Segoe UI") && FontExists(sys, L"Tahoma")) s_uiFont = L"Tahoma";   // Wine test runs
    SafeRelease(sys);
    return true;
}

const wchar_t* IconFont() { return s_iconFont.c_str(); }
const wchar_t* UiFont() { return s_uiFont.c_str(); }

IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, bool wrap, bool icon) {
    static std::map<std::tuple<int, int, int, bool, bool>, IDWriteTextFormat*> cache;
    auto key = std::make_tuple((int)(size * 10), (int)weight, (int)align, wrap, icon);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    dw->CreateTextFormat(icon ? s_iconFont.c_str() : s_uiFont.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (f) {
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            IDWriteInlineObject* ell = nullptr;
            dw->CreateEllipsisTrimmingSign(f, &ell);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            f->SetTrimming(&trim, ell);
            SafeRelease(ell);
        } else {
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.42f, size * 1.12f);
        }
    }
    cache[key] = f;
    return f;
}

IDWriteTextFormat* MonoFormat(float size) {
    static std::map<int, IDWriteTextFormat*> cache;
    auto it = cache.find((int)(size * 10));
    if (it != cache.end()) return it->second;
    static std::wstring mono;
    if (mono.empty()) {
        IDWriteFontCollection* sys = nullptr;
        dw->GetSystemFontCollection(&sys, FALSE);
        mono = FontExists(sys, L"Cascadia Mono") ? L"Cascadia Mono" : FontExists(sys, L"Consolas") ? L"Consolas" : L"Courier New";
        SafeRelease(sys);
    }
    IDWriteTextFormat* f = nullptr;
    dw->CreateTextFormat(mono.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (f) {
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        IDWriteInlineObject* ell = nullptr;
        dw->CreateEllipsisTrimmingSign(f, &ell);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, ell);
        SafeRelease(ell);
    }
    cache[(int)(size * 10)] = f;
    return f;
}

// Measuring text means building a DirectWrite layout, which is slow to do hundreds of times a frame:
// remember the answers (keyed by text, format and width) and start over when the cache gets big.
static std::unordered_map<std::wstring, float> s_measure;
static float* Cached(const std::wstring& s, IDWriteTextFormat* f, float width) {
    if (s_measure.size() > 6000) s_measure.clear();
    wchar_t k[48];
    swprintf(k, 48, L"%p|%.1f|", (void*)f, width);
    return &s_measure.emplace(std::wstring(k) + s, -1.f).first->second;
}

float MeasureWidth(const std::wstring& s, IDWriteTextFormat* f) {
    if (!f || s.empty()) return 0;
    float* c = Cached(s, f, -1);
    if (*c >= 0) return *c;
    IDWriteTextLayout* l = nullptr;
    float w = 0;
    if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000.f, 1000.f, &l))) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    *c = w;
    return w;
}

float MeasureHeight(const std::wstring& s, IDWriteTextFormat* f, float width) {
    if (!f || s.empty()) return 0;
    float* c = Cached(s, f, width);
    if (*c >= 0) return *c;
    IDWriteTextLayout* l = nullptr;
    float h = 0;
    if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, width, 100000.f, &l))) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) h = m.height;
        l->Release();
    }
    *c = h;
    return h;
}

// ---------------------------------------------------------------- theme
bool SystemUsesLightTheme() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    return v != 0;
}

uint32_t SystemAccent() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) {
        // stored as 0xAABBGGRR
        return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
    }
    return 0x4F8CFF;
}

bool BackdropSupported() {
    static int s = -1;
    if (s < 0) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        OSVERSIONINFOW vi{sizeof(vi)};
        s = (fn && fn(&vi) == 0 && vi.dwBuildNumber >= 22621) ? 1 : 0;   // Windows 11 22H2
        // Wine reports a Windows version but has no DWM backdrops
        if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) s = 0;
    }
    return s == 1;
}

float Luma(D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

void ApplyWindowTheme(HWND hwnd, const Theme& t) {
    BOOL dark = t.dark;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corner = 2; /*DWMWCP_ROUND*/
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    if (BackdropSupported()) {
        int type = t.backdrop ? t.backdropType : 1 /*DWMSBT_NONE*/;
        DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &type, sizeof(type));
        MARGINS m = t.backdrop ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(hwnd, &m);
    }
    // a solid title bar that matches the theme (Midnight gets a black one); default over a backdrop
    COLORREF cap = t.backdrop ? 0xFFFFFFFF /*DWMWA_COLOR_DEFAULT*/
                              : RGB((t.captionRgb >> 16) & 0xFF, (t.captionRgb >> 8) & 0xFF, t.captionRgb & 0xFF);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
}

} // namespace gfx
