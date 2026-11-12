// Outpost - the window: Direct2D painting, widgets, input, tray, and the glue to hosts, Hangar and the updater
#include "ui.h"
#include "ui_internal.h"
#include "util.h"
#include "net.h"
#include "hostlink.h"
#include "host.h"
#include "hangar.h"
#include "proc.h"
#include "fsutil.h"
#include "json.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <thread>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <ctime>

namespace ui {

std::vector<ServerView> g_servers;
int g_sel = -1;
Page g_page = Page::Welcome;
int g_tab = 0;
HWND g_hwnd = nullptr;
Theme g_t;
float g_s = 1, g_d = 1, g_W = 0, g_H = 0, g_mx = -1, g_my = -1;
bool g_mdown = false;
Canvas g_c;
SelfUpdateState g_self;
std::vector<ThemeDef> g_customThemes;
SysView g_sys;
D2D1_RECT_F g_viewport = {0, 0, 0, 0};

static ID2D1HwndRenderTarget* g_rt = nullptr;
static ID2D1SolidColorBrush* g_br = nullptr;
static UiStart g_start;
static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded = false;
static UINT g_taskbarCreated = 0;
static bool g_reallyQuit = false;
static DWORD g_lastInput = 0;
static DWORD g_selfCheckedTick = 0;
static bool g_restoredFromState = false;
static std::wstring g_restoreSummary;

// ---------------------------------------------------------------- plumbing
void Async(std::function<void()> work) { std::thread(std::move(work)).detach(); }
void Ui(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_OP_JOB, 0, (LPARAM)p)) delete p;
}
void Invalidate() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }
static DWORD g_animUntil = 0;
static bool g_animTimer = false;
void StartAnim() {
    if (g_cfg.animation == L"off") { Invalidate(); return; }
    g_animUntil = GetTickCount() + 400;
    if (!g_animTimer && g_hwnd) { SetTimer(g_hwnd, 2, 15, nullptr); g_animTimer = true; }
    Invalidate();
}
float AnimMs(float ms) {
    if (g_cfg.animation == L"off") return 0;
    if (g_cfg.animation == L"fast") return ms * 0.5f;
    if (g_cfg.animation == L"slow") return ms * 1.8f;
    return ms;
}
ServerView* FindServer(const std::wstring& id) { for (auto& v : g_servers) if (v.def.id == id) return &v; return nullptr; }
ServerView* Sel() { return g_sel >= 0 && g_sel < (int)g_servers.size() ? &g_servers[g_sel] : nullptr; }
int ActiveJobs() { int n = 0; for (auto& v : g_servers) n += v.job.active; return n; }

static std::wstring g_toast;
static bool g_toastBad = false;
static DWORD g_toastAt = 0;
void Toast(const std::wstring& text, bool bad) {
    g_toast = text; g_toastBad = bad; g_toastAt = GetTickCount();
    Log(L"toast: %ls", text.c_str());
    StartAnim();
}

struct Modal { bool open = false; std::wstring title, text, ok; std::function<void()> onOk; bool danger = false; };
static Modal g_modal;
static size_t g_modalHotStart = 0;
void Confirm(const std::wstring& title, const std::wstring& text, const std::wstring& ok, std::function<void()> onOk, bool danger) {
    g_modal = {true, title, text, ok, std::move(onOk), danger};
    Invalidate();
}

void SetClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), text.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}
static std::wstring GetClipboard() {
    std::wstring out;
    if (!OpenClipboard(g_hwnd)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) { if (auto* p = (const wchar_t*)GlobalLock(h)) { out = p; GlobalUnlock(h); } }
    CloseClipboard();
    return out;
}

static std::wstring ShellPick(const std::wstring& title, bool folder, const wchar_t* filter) {
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    d->SetTitle(title.c_str());
    if (!folder && filter) {
        COMDLG_FILTERSPEC spec[2] = {{L"Files", filter}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, spec);
    }
    if (SUCCEEDED(d->Show(g_hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    return out;
}
std::wstring PickFolder(const std::wstring& title) { return ShellPick(title, true, nullptr); }
std::wstring PickFile(const std::wstring& title, const wchar_t* filter) { return ShellPick(title, false, filter); }
void OpenPath(const std::wstring& path) {
    EnsureDir(DirExists(path) || !FileExists(path) ? path : PathParent(path));
    ShellExecuteW(g_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void OpenUrl(const std::wstring& url) { ShellExecuteW(g_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

std::wstring Duration(long long s) {
    if (s < 0) s = 0;
    wchar_t b[64];
    if (s < 60) swprintf(b, 64, L"%llds", s);
    else if (s < 3600) swprintf(b, 64, L"%lldm", s / 60);
    else if (s < 86400) swprintf(b, 64, L"%lldh %lldm", s / 3600, (s % 3600) / 60);
    else swprintf(b, 64, L"%lldd %lldh", s / 86400, (s % 86400) / 3600);
    return b;
}
std::wstring Ago(long long t) {
    if (!t) return L"never";
    long long d = fs::UnixNow() - t;
    if (d < 45) return L"just now";
    if (d < 3600) return std::to_wstring(std::max(1LL, d / 60)) + L" min ago";
    if (d < 86400) return std::to_wstring(d / 3600) + (d / 3600 == 1 ? L" hour ago" : L" hours ago");
    return std::to_wstring(d / 86400) + (d / 86400 == 1 ? L" day ago" : L" days ago");
}
std::wstring ShortTime(long long t) {
    time_t tt = (time_t)t;
    struct tm lt;
    localtime_s(&lt, &tt);
    wchar_t b[64];
    wcsftime(b, 64, L"%b %d, %I:%M %p", &lt);
    return b;
}

// ---------------------------------------------------------------- theme
void ApplyTheme() {
    g_customThemes = LoadCustomThemes();
    ThemeDef def = FindTheme(g_cfg.theme, g_customThemes);
    if (def.followWindows) def = BuiltinThemes()[SystemUsesLightTheme() ? 1 : 0];
    if (g_cfg.backdrop == L"solid") def.backdrop = L"none";
    else if (g_cfg.backdrop == L"mica" || g_cfg.backdrop == L"acrylic") {
        if (def.backdrop == L"none") def.transparency = 0.2f;
        def.backdrop = g_cfg.backdrop;
    }
    g_t = MakeTheme(def, g_cfg.accent);
    HIGHCONTRASTW hc{sizeof(hc)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON)) {
        // Windows contrast themes: use the system's colors
        auto sys = [](int i) { COLORREF c = GetSysColor(i); return Rgb((GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c)); };
        g_t.bg = g_t.sidebar = g_t.surface = sys(COLOR_WINDOW);
        g_t.surfaceHover = g_t.surfaceStrong = sys(COLOR_BTNFACE);
        g_t.text = g_t.subtext = sys(COLOR_WINDOWTEXT);
        g_t.faint = sys(COLOR_GRAYTEXT);
        g_t.accent = g_t.accentHover = sys(COLOR_HIGHLIGHT);
        g_t.onAccent = sys(COLOR_HIGHLIGHTTEXT);
        g_t.divider = sys(COLOR_WINDOWTEXT);
        g_t.backdrop = false;
    }
    g_t.radius = (float)g_cfg.radius;
    g_d = g_cfg.density == L"compact" ? 0.86f : g_cfg.density == L"spacious" ? 1.14f : 1.0f;
    if (g_hwnd) ApplyWindowTheme(g_hwnd, g_t);
    Invalidate();
}

// ---------------------------------------------------------------- drawing helpers
float TextW(const std::wstring& s, float size, DWRITE_FONT_WEIGHT wt) { return MeasureWidth(s, Format(size, wt)); }
float Text(const std::wstring& s, float x, float y, float w, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT wt, DWRITE_TEXT_ALIGNMENT al, float h) {
    if (h <= 0) h = size * 1.5f;
    g_c.Text(s, Format(size, wt, al), R(x, y, x + w, y + h), c);
    return std::min(w, MeasureWidth(s, Format(size, wt)));
}
float Para(const std::wstring& s, float x, float y, float w, float size, D2D1_COLOR_F c) {
    IDWriteTextFormat* f = Format(size, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    float h = MeasureHeight(s, f, w);
    g_c.Text(s, f, R(x, y, x + w, y + h + 2), c);
    return h;
}
void Glyph(wchar_t g, float cx, float cy, float size, D2D1_COLOR_F c) {
    g_c.Glyph(g, Format(size, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), R(cx - size, cy - size, cx + size, cy + size), c);
}
void Card(const D2D1_RECT_F& r, bool hover) {
    g_c.FillRound(r, S(g_t.radius + 2), hover ? g_t.surfaceHover : g_t.surface);
    g_c.StrokeRound(r, S(g_t.radius + 2), g_t.divider, 1);
}
float Pill(float x, float y, const std::wstring& text, D2D1_COLOR_F color) {
    float fs = F(11.5f);
    float w = TextW(text, fs, DWRITE_FONT_WEIGHT_SEMI_BOLD) + S(18);
    D2D1_RECT_F r = RW(x, y, w, S(22));
    g_c.FillRound(r, S(11), Alpha(color, 0.16f));
    g_c.Circle(x + S(10), y + S(11), S(3.2f), color);
    Text(text, x + S(16), y, w - S(16), fs, color, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, S(22));
    return w + S(4);
}
void GameTile(const Template& t, const D2D1_RECT_F& r, float textSize) {
    D2D1_COLOR_F base = Rgb(t.color);
    float rad = std::min(S(g_t.radius + 2), (r.right - r.left) * 0.28f);
    g_c.FillRound(r, rad, base);
    // a soft highlight on top
    g_c.FillRound(R(r.left, r.top, r.right, r.top + (r.bottom - r.top) * 0.5f), rad, Rgb(0xFFFFFF, 0.10f));
    g_c.FillRound(R(r.left, (r.top + r.bottom) / 2 - 1, r.right, (r.top + r.bottom) / 2 + rad), 0, Rgb(0, 0));
    Text(t.shortName, r.left, r.top, r.right - r.left, textSize, Rgb(0xFFFFFF), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, r.bottom - r.top);
}
D2D1_COLOR_F StateColor(const std::wstring& s) {
    if (s == L"running") return g_t.good;
    if (s == L"starting" || s == L"stopping" || s == L"restarting" || s == L"installing") return g_t.warn;
    if (s == L"sleeping") return g_t.info;
    if (s == L"error") return g_t.bad;
    return g_t.faint;
}
std::wstring StateLabel(const std::wstring& s) {
    if (s == L"running") return L"Running";
    if (s == L"starting") return L"Starting";
    if (s == L"stopping") return L"Stopping";
    if (s == L"restarting") return L"Restarting";
    if (s == L"sleeping") return L"Sleeping";
    if (s == L"error") return L"Error";
    if (s == L"installing") return L"Installing";
    return L"Stopped";
}

// ---------------------------------------------------------------- hit testing
struct HotRect { D2D1_RECT_F r; std::function<void()> click; bool enabled, hand; };
static std::vector<HotRect> g_hot;
static float g_downX = -1, g_downY = -1;
void Hot(const D2D1_RECT_F& r0, std::function<void()> click, bool enabled, bool hand) {
    D2D1_RECT_F r = r0;
    // clip to the current scroll viewport
    if (g_viewport.right > g_viewport.left) {
        r.top = std::max(r.top, g_viewport.top); r.bottom = std::min(r.bottom, g_viewport.bottom);
        r.left = std::max(r.left, g_viewport.left); r.right = std::min(r.right, g_viewport.right);
        if (r.bottom <= r.top || r.right <= r.left) return;
    }
    g_hot.push_back({r, std::move(click), enabled, hand});
}
static bool Blocked(float x, float y);
bool Hover(const D2D1_RECT_F& r) {
    if (g_viewport.right > g_viewport.left && !In(g_viewport, g_mx, g_my)) return false;
    return In(r, g_mx, g_my) && !Blocked(g_mx, g_my);
}
bool Pressed(const D2D1_RECT_F& r) { return g_mdown && Hover(r) && In(r, g_downX, g_downY); }

// ---------------------------------------------------------------- widgets
float ButtonW(const std::wstring& label, wchar_t glyph) {
    float w = (label.empty() ? 0 : TextW(label, F(13), DWRITE_FONT_WEIGHT_SEMI_BOLD)) + D(28);
    if (glyph) w += label.empty() ? S(8) : S(24);
    return w;
}
void Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph, Btn style, std::function<void()> click, bool enabled) {
    bool hov = enabled && Hover(r), pr = enabled && Pressed(r);
    D2D1_COLOR_F bg, fg;
    switch (style) {
        case Btn::Primary: bg = hov ? g_t.accentHover : g_t.accent; fg = g_t.onAccent; break;
        case Btn::Danger: bg = hov ? Mix(g_t.bad, Rgb(0xFFFFFF), 0.1f) : g_t.bad; fg = Rgb(0xFFFFFF); break;
        case Btn::Secondary: bg = hov ? g_t.surfaceStrong : g_t.surfaceHover; fg = g_t.text; break;
        default: bg = hov ? g_t.surfaceHover : Rgb(0, 0); fg = g_t.text; break;
    }
    if (!enabled) { bg = Alpha(bg, 0.45f); fg = Alpha(fg, 0.45f); }
    D2D1_RECT_F rr = r;
    if (pr) { rr.top += 1; rr.bottom += 1; }
    g_c.FillRound(rr, S(g_t.radius), bg);
    if (style == Btn::Secondary || style == Btn::Ghost) g_c.StrokeRound(rr, S(g_t.radius), Alpha(g_t.divider, style == Btn::Ghost && !hov ? 0 : 1));
    float tw = label.empty() ? 0 : TextW(label, F(13), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float gw = glyph ? S(16) + (label.empty() ? 0 : S(8)) : 0;
    float x = (rr.left + rr.right - tw - gw) / 2;
    float cy = (rr.top + rr.bottom) / 2;
    if (glyph) { Glyph(glyph, x + S(8), cy, F(13), fg); x += gw; }
    if (!label.empty()) Text(label, x, rr.top, tw + 2, F(13), fg, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, rr.bottom - rr.top);
    Hot(r, std::move(click), enabled);
}
void IconButton(const D2D1_RECT_F& r, wchar_t glyph, std::function<void()> click, bool enabled, const D2D1_COLOR_F* color) {
    bool hov = enabled && Hover(r);
    if (hov) g_c.FillRound(r, S(g_t.radius), Pressed(r) ? g_t.surfaceStrong : g_t.surfaceHover);
    D2D1_COLOR_F c = color ? *color : g_t.subtext;
    if (!enabled) c = Alpha(c, 0.4f);
    Glyph(glyph, (r.left + r.right) / 2, (r.top + r.bottom) / 2, F(14), hov ? g_t.text : c);
    Hot(r, std::move(click), enabled);
}

static std::map<std::wstring, float> g_knob;   // animated toggle positions, keyed by place
void Toggle(float x, float y, bool on, std::function<void(bool)> changed, bool enabled) {
    D2D1_RECT_F r = RW(x, y, S(40), S(22));
    wchar_t key[64]; swprintf(key, 64, L"%d:%d", (int)x, (int)y);
    float& k = g_knob.emplace(key, on ? 1.f : 0.f).first->second;
    float target = on ? 1.f : 0.f;
    if (std::fabs(k - target) > 0.01f) { k += (target - k) * (AnimMs(150) ? 0.35f : 1.0f); StartAnim(); } else k = target;
    D2D1_COLOR_F track = Mix(g_t.surfaceStrong, g_t.accent, k);
    if (!enabled) track = Alpha(track, 0.45f);
    g_c.FillRound(r, S(11), track);
    if (k < 0.5f) g_c.StrokeRound(r, S(11), g_t.divider);
    float kx = x + S(11) + k * S(18);
    g_c.Circle(kx, y + S(11), S(Hover(r) ? 8.f : 7.f), k > 0.5f ? g_t.onAccent : g_t.subtext);
    Hot(r, [changed, on] { changed(!on); }, enabled);
}

float Segmented(float x, float y, const std::vector<std::wstring>& opts, int sel, std::function<void(int)> pick) {
    float h = D(32), pad = S(3);
    std::vector<float> ws;
    float total = pad * 2;
    for (auto& o : opts) { ws.push_back(TextW(o, F(12.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD) + D(24)); total += ws.back(); }
    g_c.FillRound(RW(x, y, total, h), S(g_t.radius), g_t.surfaceHover);
    float cx = x + pad;
    for (size_t i = 0; i < opts.size(); ++i) {
        D2D1_RECT_F r = RW(cx, y + pad, ws[i], h - pad * 2);
        bool on = (int)i == sel;
        if (on) g_c.FillRound(r, S(g_t.radius - 1), g_t.accent);
        else if (Hover(r)) g_c.FillRound(r, S(g_t.radius - 1), g_t.surfaceStrong);
        Text(opts[i], r.left, r.top, ws[i], F(12.5f), on ? g_t.onAccent : g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, r.bottom - r.top);
        int idx = (int)i;
        Hot(r, [pick, idx] { pick(idx); });
        cx += ws[i];
    }
    return total;
}

void ProgressBar(const D2D1_RECT_F& r, int pct, D2D1_COLOR_F color) {
    float rad = (r.bottom - r.top) / 2;
    g_c.FillRound(r, rad, g_t.surfaceStrong);
    if (pct < 0) {
        // indeterminate: a sliding segment
        float w = (r.right - r.left) * 0.3f;
        float t = (float)(GetTickCount() % 1400) / 1400.f;
        float x = r.left - w + t * (r.right - r.left + w);
        D2D1_RECT_F seg = R(std::max(r.left, x), r.top, std::min(r.right, x + w), r.bottom);
        if (seg.right > seg.left) g_c.FillRound(seg, rad, color);
        StartAnim();
        return;
    }
    float w = (r.right - r.left) * std::min(100, std::max(0, pct)) / 100.f;
    if (w > 0) g_c.FillRound(R(r.left, r.top, r.left + std::max(w, rad * 2), r.bottom), rad, color);
}

// ---- text fields
static std::wstring g_focus, g_edit, g_editOriginal;
static bool g_selectAll = false;
static std::function<void(const std::wstring&)> g_commit;
static FieldOpts g_fieldOpts;
static DWORD g_caretAt = 0;
bool Focused(const std::wstring& key) { return !g_focus.empty() && g_focus == key; }
std::wstring& EditText() { return g_edit; }
static void Blur(bool commit) {
    if (g_focus.empty()) return;
    auto c = g_commit;
    std::wstring v = g_edit;
    bool changed = v != g_editOriginal;
    g_focus.clear();
    g_commit = nullptr;
    if (commit && c && changed) c(v);
    Invalidate();
}
void FocusField(const std::wstring& key, const std::wstring& value) {
    if (g_focus == key) return;
    Blur(true);
    g_focus = key; g_edit = g_editOriginal = value; g_selectAll = true; g_caretAt = GetTickCount();
    Invalidate();
}
void TextField(const D2D1_RECT_F& r, const std::wstring& key, const std::wstring& value, std::function<void(const std::wstring&)> commit, const FieldOpts& o) {
    bool focused = Focused(key);
    if (focused) { g_commit = commit; g_fieldOpts = o; }
    bool hov = Hover(r);
    g_c.FillRound(r, S(g_t.radius), focused ? g_t.surface : hov ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceHover, 0.5f));
    g_c.StrokeRound(r, S(g_t.radius), focused ? g_t.accent : g_t.divider, focused ? S(1.5f) : 1.f);
    std::wstring shown = focused ? g_edit : value;
    if (o.password && !shown.empty()) shown = std::wstring(std::min<size_t>(shown.size(), 24), L'\x2022');
    float pad = D(10), fs = F(13);
    float x = r.left + pad, w = r.right - r.left - pad * 2;
    if (shown.empty() && !o.placeholder.empty()) Text(o.placeholder, x, r.top, w, fs, g_t.faint, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, r.bottom - r.top);
    else {
        float tw = TextW(shown, fs);
        float shift = focused && tw > w - S(4) ? tw - (w - S(4)) : 0;   // keep the caret in view
        g_rt->PushAxisAlignedClip(R(x, r.top, x + w, r.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        if (focused && g_selectAll && !shown.empty()) g_c.FillRound(RW(x - shift, r.top + S(7), tw, r.bottom - r.top - S(14)), S(2), Alpha(g_t.accent, 0.35f));
        g_c.Text(shown, Format(fs), R(x - shift, r.top, x - shift + tw + 4, r.bottom), g_t.text);
        if (focused && ((GetTickCount() - g_caretAt) / 530) % 2 == 0) {
            float cx = x - shift + tw + 1;
            g_c.Line(cx, r.top + S(8), cx, r.bottom - S(8), g_t.text, S(1.2f));
        }
        g_rt->PopAxisAlignedClip();
    }
    Hot(r, [key, value] { FocusField(key, value); }, true, false);
}
static void FieldChar(wchar_t c) {
    if (g_selectAll && c >= 32) { g_edit.clear(); }
    g_selectAll = false;
    if (c == 8) { if (!g_edit.empty()) g_edit.pop_back(); }
    else if (c == 127) {   // Ctrl+Backspace: delete a word
        while (!g_edit.empty() && g_edit.back() == L' ') g_edit.pop_back();
        while (!g_edit.empty() && g_edit.back() != L' ') g_edit.pop_back();
    } else if (c >= 32) {
        if (g_fieldOpts.numeric && !(iswdigit(c) || c == L'.' || c == L'-')) return;
        if (g_edit.size() < 4000) g_edit += c;
    }
    g_caretAt = GetTickCount();
    if (g_fieldOpts.change) g_fieldOpts.change(g_edit);
    Invalidate();
}

// ---- dropdowns and menus
struct Menu { bool open = false; D2D1_RECT_F anchor, box; std::vector<std::wstring> items; int sel = -1; std::function<void(int)> pick; float scroll = 0; };
static Menu g_menu;
void OpenMenu(const D2D1_RECT_F& anchor, const std::vector<std::wstring>& items, int sel, std::function<void(int)> pick) {
    Blur(true);
    g_menu = Menu();
    g_menu.open = true; g_menu.anchor = anchor; g_menu.items = items; g_menu.sel = sel; g_menu.pick = std::move(pick);
    float rowH = D(32), w = anchor.right - anchor.left;
    for (auto& it : items) w = std::max(w, TextW(it, F(13)) + D(40));
    float h = std::min((float)items.size() * rowH + S(8), std::max(S(160), g_H * 0.6f));
    float top = anchor.bottom + S(4);
    if (top + h > g_H - S(8)) top = std::max(S(8), anchor.top - S(4) - h);
    float left = std::min(anchor.left, g_W - w - S(8));
    g_menu.box = RW(left, top, w, h);
    if (sel > 0) g_menu.scroll = std::max(0.f, std::min(sel * rowH - h / 2, items.size() * rowH + S(8) - h));
    StartAnim();
}
void Dropdown(const D2D1_RECT_F& r, const std::vector<std::wstring>& labels, int sel, std::function<void(int)> pick, bool enabled) {
    bool hov = enabled && Hover(r);
    g_c.FillRound(r, S(g_t.radius), hov ? g_t.surfaceStrong : g_t.surfaceHover);
    g_c.StrokeRound(r, S(g_t.radius), g_t.divider);
    std::wstring cur = sel >= 0 && sel < (int)labels.size() ? labels[sel] : L"";
    Text(cur, r.left + D(10), r.top, r.right - r.left - D(34), F(13), enabled ? g_t.text : g_t.faint, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, r.bottom - r.top);
    Glyph(0xE70D, r.right - D(16), (r.top + r.bottom) / 2, F(10), g_t.subtext);
    Hot(r, [r, labels, sel, pick] { OpenMenu(r, labels, sel, pick); }, enabled);
}
static void DrawMenu() {
    if (!g_menu.open) return;
    float rowH = D(32);
    D2D1_RECT_F b = g_menu.box;
    g_c.FillRound(R(b.left + 2, b.top + 4, b.right + 2, b.bottom + 6), S(g_t.radius + 2), Rgb(0, 0.25f));
    g_c.FillRound(b, S(g_t.radius + 2), Mix(g_t.surface, g_t.surfaceHover, 0.4f).a < 0.99f ? Rgb(g_t.dark ? 0x26262C : 0xFFFFFF) : Mix(g_t.surface, g_t.surfaceHover, 0.4f));
    g_c.StrokeRound(b, S(g_t.radius + 2), g_t.divider);
    g_rt->PushAxisAlignedClip(R(b.left, b.top + S(4), b.right, b.bottom - S(4)), D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < g_menu.items.size(); ++i) {
        float y = b.top + S(4) + i * rowH - g_menu.scroll;
        if (y + rowH < b.top || y > b.bottom) continue;
        D2D1_RECT_F r = R(b.left + S(4), y, b.right - S(4), y + rowH);
        bool hov = In(r, g_mx, g_my) && In(b, g_mx, g_my);
        if (hov || (int)i == g_menu.sel) g_c.FillRound(r, S(g_t.radius - 1), hov ? g_t.surfaceStrong : Alpha(g_t.accent, 0.18f));
        Text(g_menu.items[i], r.left + D(12), r.top, r.right - r.left - D(20), F(13), g_t.text, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, rowH);
    }
    g_rt->PopAxisAlignedClip();
}

// ---- sliders
static std::wstring g_dragKey;
static std::function<void(double)> g_dragLive, g_dragCommit;
static D2D1_RECT_F g_dragRect;
static double g_dragLo, g_dragHi, g_dragStep, g_dragValue;
static double SliderValueAt(float x) {
    double t = (x - g_dragRect.left) / std::max(1.f, g_dragRect.right - g_dragRect.left);
    t = std::max(0.0, std::min(1.0, t));
    double v = g_dragLo + t * (g_dragHi - g_dragLo);
    if (g_dragStep > 0) v = std::round((v - g_dragLo) / g_dragStep) * g_dragStep + g_dragLo;
    return std::max(g_dragLo, std::min(g_dragHi, v));
}
void Slider(const D2D1_RECT_F& r, const std::wstring& key, double value, double lo, double hi, double step,
            std::function<void(double)> live, std::function<void(double)> commit) {
    bool dragging = g_dragKey == key;
    if (dragging) { value = g_dragValue; g_dragLive = live; g_dragCommit = commit; g_dragRect = r; }
    float cy = (r.top + r.bottom) / 2;
    float t = hi > lo ? (float)((value - lo) / (hi - lo)) : 0;
    t = std::max(0.f, std::min(1.f, t));
    float x = r.left + t * (r.right - r.left);
    g_c.FillRound(R(r.left, cy - S(2), r.right, cy + S(2)), S(2), g_t.surfaceStrong);
    g_c.FillRound(R(r.left, cy - S(2), x, cy + S(2)), S(2), g_t.accent);
    bool hov = Hover(r) || dragging;
    g_c.Circle(x, cy, S(hov ? 9.f : 8.f), g_t.accent);
    g_c.Circle(x, cy, S(3.5f), g_t.onAccent);
    Hot(r, [] {}, true);
    // drag starts on mouse down (see WM_LBUTTONDOWN): remember what this slider is
    if (g_mdown && g_dragKey.empty() && In(r, g_downX, g_downY)) {
        g_dragKey = key; g_dragRect = r; g_dragLo = lo; g_dragHi = hi; g_dragStep = step; g_dragLive = live; g_dragCommit = commit;
        g_dragValue = SliderValueAt(g_mx);
        if (live) live(g_dragValue);
    }
}

// ---- scrolling
static std::map<std::wstring, ScrollArea> g_scrolls;
ScrollArea& PageScroll() {
    std::wstring key = g_page == Page::Server && Sel() ? Sel()->def.id + L":" + std::to_wstring(g_tab) : std::to_wstring((int)g_page);
    return g_scrolls[key];
}
static ScrollArea* g_activeScroll = nullptr;
static D2D1_RECT_F g_activeView;
float BeginScroll(ScrollArea& sa, const D2D1_RECT_F& vp) {
    sa.view = vp.bottom - vp.top;
    g_viewport = vp;
    g_activeScroll = &sa;
    g_activeView = vp;
    g_rt->PushAxisAlignedClip(vp, D2D1_ANTIALIAS_MODE_ALIASED);
    return vp.top - sa.pos;
}
void EndScroll(ScrollArea& sa, float bottom) {
    g_rt->PopAxisAlignedClip();
    sa.content = bottom + sa.pos - g_activeView.top;
    float maxPos = std::max(0.f, sa.content - sa.view);
    if (sa.target > maxPos) sa.target = maxPos;
    if (sa.pos > maxPos) sa.pos = maxPos;
    // scrollbar
    if (sa.content > sa.view + 1) {
        float trackH = sa.view - S(8);
        float h = std::max(S(30), trackH * sa.view / sa.content);
        float y = g_activeView.top + S(4) + (trackH - h) * (sa.pos / maxPos);
        g_c.FillRound(R(g_activeView.right - S(7), y, g_activeView.right - S(3), y + h), S(2), Alpha(g_t.subtext, In(g_activeView, g_mx, g_my) ? 0.45f : 0.2f));
    }
    g_viewport = {0, 0, 0, 0};
}
bool Visible(float top, float bottom) { return bottom >= g_viewport.top && top <= g_viewport.bottom; }

// ---------------------------------------------------------------- device and paint
static void DiscardDevice() { SafeRelease(g_br); SafeRelease(g_rt); }
static bool EnsureDevice() {
    if (g_rt) return true;
    RECT rc; GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt))) return false;
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0), &g_br);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_c.rt = g_rt; g_c.br = g_br;
    return true;
}

static bool Blocked(float x, float y) {
    if (g_modal.open) return true;
    if (g_menu.open && !In(g_menu.box, x, y)) return true;
    return false;
}

static void DrawToast() {
    if (g_toast.empty()) return;
    DWORD age = GetTickCount() - g_toastAt;
    if (age > 4500) { g_toast.clear(); return; }
    float a = age < 200 ? age / 200.f : age > 4000 ? (4500 - age) / 500.f : 1.f;
    float fs = F(13);
    float w = std::min(g_W - S(80), TextW(g_toast, fs) + S(48));
    float h = S(42);
    D2D1_RECT_F r = RW((g_W - w) / 2 + S(120), g_H - h - S(24), w, h);
    g_c.FillRound(r, S(g_t.radius + 4), Alpha(g_t.dark ? Rgb(0x2C2C33) : Rgb(0x1C1C22), 0.97f * a));
    g_c.Circle(r.left + S(18), r.top + h / 2, S(4), Alpha(g_toastBad ? g_t.bad : g_t.good, a));
    Text(g_toast, r.left + S(30), r.top, w - S(40), fs, Alpha(Rgb(0xF2F2F5), a), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, h);
    StartAnim();
}

static void DrawModal() {
    if (!g_modal.open) return;
    g_c.Fill(R(0, 0, g_W, g_H), Rgb(0, 0.45f));
    float w = std::min(S(460), g_W - S(40));
    float th = MeasureHeight(g_modal.text, Format(F(13), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), w - S(48));
    float h = S(56) + th + S(84);
    D2D1_RECT_F r = RW((g_W - w) / 2, (g_H - h) / 2, w, h);
    g_c.FillRound(r, S(g_t.radius + 6), g_t.dark ? Rgb(0x222228) : Rgb(0xFFFFFF));
    g_c.StrokeRound(r, S(g_t.radius + 6), g_t.divider);
    Text(g_modal.title, r.left + S(24), r.top + S(20), w - S(48), F(17), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Para(g_modal.text, r.left + S(24), r.top + S(56), w - S(48), F(13), g_t.subtext);
    float bw = ButtonW(g_modal.ok), cw = ButtonW(L"Cancel");
    float by = r.bottom - S(56);
    D2D1_RECT_F save = g_viewport;
    g_viewport = {0, 0, 0, 0};
    g_modalHotStart = g_hot.size();
    bool wasOpen = true;
    g_modal.open = false;   // let the buttons themselves be hit
    Button(RW(r.right - S(24) - bw, by, bw, D(36)), g_modal.ok, 0, g_modal.danger ? Btn::Danger : Btn::Primary, [] {
        auto f = g_modal.onOk; g_modal = Modal(); if (f) f(); Invalidate();
    });
    Button(RW(r.right - S(32) - bw - cw, by, cw, D(36)), L"Cancel", 0, Btn::Secondary, [] { g_modal = Modal(); Invalidate(); });
    g_modal.open = wasOpen;
    g_viewport = save;
}

static void DrawSelfUpdateBar(float x0) {
    (void)x0;
}

static void Paint() {
    if (!EnsureDevice()) return;
    g_hot.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(g_t.backdrop ? D2D1::ColorF(0, 0) : g_t.bg);
    if (g_t.backdrop) g_c.Fill(R(0, 0, g_W, g_H), g_t.bg);
    if (RawEditorOpen() && g_page != Page::Server) CloseRawEditor(false);
    DrawSidebar();
    float x0 = S(264);
    switch (g_page) {
        case Page::Server: if (Sel()) DrawServer(x0); else DrawWelcome(x0); break;
        case Page::Wizard: DrawWizard(x0); break;
        case Page::AppSettings: DrawAppSettings(x0); break;
        default: DrawWelcome(x0); break;
    }
    DrawSelfUpdateBar(x0);
    // overlays are drawn last; their hot rects go last so they win
    if (g_menu.open) {
        DrawMenu();
        float rowH = D(32);
        for (size_t i = 0; i < g_menu.items.size(); ++i) {
            float y = g_menu.box.top + S(4) + i * rowH - g_menu.scroll;
            D2D1_RECT_F r = R(g_menu.box.left, std::max(y, g_menu.box.top), g_menu.box.right, std::min(y + rowH, g_menu.box.bottom));
            if (r.bottom <= r.top) continue;
            int idx = (int)i;
            g_hot.push_back({r, [idx] { auto p = g_menu.pick; g_menu.open = false; if (p) p(idx); Invalidate(); }, true, true});
        }
    }
    DrawModal();
    DrawToast();
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDevice();
}

static const HotRect* HitAt(float x, float y) {
    if (g_modal.open) {
        // only the modal's own buttons (registered last)
        for (size_t i = g_hot.size(); i-- > g_modalHotStart;) if (In(g_hot[i].r, x, y)) return &g_hot[i];
        return nullptr;
    }
    for (auto it = g_hot.rbegin(); it != g_hot.rend(); ++it) if (In(it->r, x, y)) return &*it;
    return nullptr;
}

// ---------------------------------------------------------------- tray and notifications
static void TrayTip() {
    if (!g_trayAdded) return;
    int running = 0, players = 0;
    for (auto& v : g_servers) if (v.state == L"running") { ++running; players += (int)v.players.size(); }
    std::wstring tip = running ? L"Outpost: " + std::to_wstring(running) + (running == 1 ? L" server running, " : L" servers running, ") + std::to_wstring(players) + (players == 1 ? L" player" : L" players")
                               : L"Outpost: no servers running";
    g_nid.uFlags = NIF_TIP;
    wcsncpy(g_nid.szTip, tip.c_str(), 127);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void AddTray() {
    g_nid = NOTIFYICONDATAW{};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_OP_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy(g_nid.szTip, L"Outpost");
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != 0;
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    TrayTip();
}
static void Notify(const std::wstring& title, const std::wstring& text) {
    if (!g_trayAdded) return;
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title.c_str(), 63);
    wcsncpy(g_nid.szInfo, text.c_str(), 255);
    g_nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void ShowWindowNow() {
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}
static void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, 1, L"Open Outpost");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < g_servers.size() && i < 40; ++i) {
        auto& v = g_servers[i];
        HMENU sub = CreatePopupMenu();
        bool on = v.Running();
        AppendMenuW(sub, MF_STRING | (on || v.job.active ? MF_GRAYED : 0), 1000 + i * 4 + 0, L"Start");
        AppendMenuW(sub, MF_STRING | (on ? 0 : MF_GRAYED), 1000 + i * 4 + 1, L"Stop");
        AppendMenuW(sub, MF_STRING | (v.state == L"running" ? 0 : MF_GRAYED), 1000 + i * 4 + 2, L"Restart");
        AppendMenuW(sub, MF_STRING, 1000 + i * 4 + 3, L"Show");
        std::wstring label = v.def.name + L"  \x2014  " + StateLabel(v.state) + (v.state == L"running" ? L" (" + std::to_wstring(v.players.size()) + L")" : L"");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, label.c_str());
    }
    if (!g_servers.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, L"Quit Outpost (servers keep running)");
    AppendMenuW(m, MF_STRING, 3, L"Stop all servers and quit");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) ShowWindowNow();
    else if (cmd == 2) { g_reallyQuit = true; PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
    else if (cmd == 3) {
        for (auto& v : g_servers) if (v.Running()) StopServer(v);
        g_reallyQuit = true;
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    } else if (cmd >= 1000) {
        size_t i = (cmd - 1000) / 4;
        int a = (cmd - 1000) % 4;
        if (i < g_servers.size()) {
            if (a == 0) StartServer(g_servers[i]);
            else if (a == 1) StopServer(g_servers[i]);
            else if (a == 2) RestartServer(g_servers[i]);
            else { SelectServer((int)i); ShowWindowNow(); }
        }
    }
}

// ---------------------------------------------------------------- start with Windows
void ApplyStartup() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (g_cfg.startWithWindows) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --tray --boot";
        RegSetValueExW(k, L"Outpost", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, L"Outpost");
    RegCloseKey(k);
}

// ---------------------------------------------------------------- restore state (updates and Hangar)
static std::wstring StateJson() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"page", json::Value::Num((int)g_page));
    v.Set(L"server", json::Value::Str(Sel() ? Sel()->def.id : L""));
    v.Set(L"tab", json::Value::Num(g_tab));
    v.Set(L"visible", json::Value::Boolean(IsWindowVisible(g_hwnd) != 0));
    std::wstring o;
    json::Write(v, o);
    return o;
}
static void RestoreFrom(const std::wstring& text) {
    json::Value v;
    if (!json::Parser(text).Parse(v)) return;
    std::wstring id = v.GetStr(L"server");
    for (size_t i = 0; i < g_servers.size(); ++i) if (g_servers[i].def.id == id) g_sel = (int)i;
    g_page = (Page)(int)v.GetNum(L"page", (int)Page::Server);
    if (g_page == Page::Wizard) StartWizard();
    g_tab = (int)v.GetNum(L"tab", 0);
    g_restoredFromState = true;
    int running = 0;
    for (auto& s : g_servers) running += host::IsRunning(s.def.id);
    g_restoreSummary = std::to_wstring(running) + (running == 1 ? L" running server" : L" running servers");
}

static void ApplySelf(bool userAsked) {
    if (g_self.kind != SelfUpdateState::Ready) return;
    if (ActiveJobs()) { if (userAsked) Toast(L"Outpost will update when the current download finishes.", false); return; }
    std::wstring stateFile = DataDir() + L"\\restore.json";
    WriteTextFileAtomic(stateFile, StateJson());
    ConfigSave();
    std::wstring err;
    hangar::Stop();
    hostlink::Shutdown();
    if (selfupdate::Apply(g_hwnd, g_self.staged, g_self.version, L"--restore-state " + proc::Quote(stateFile), &err)) {
        g_reallyQuit = true;
        if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
        DestroyWindow(g_hwnd);
        return;
    }
    SkipVersion(g_self.version);
    g_self = SelfUpdateState();
    hangar::Start(g_hwnd);
    for (auto& v : g_servers) hostlink::Watch(v.def.id);
    Toast(err, true);
}
void UpdateNow() { ApplySelf(true); }
void CheckSelf(bool force) { g_selfCheckedTick = GetTickCount(); selfupdate::Check(g_hwnd, force); }

// ---------------------------------------------------------------- Hangar requests
static void OnHangar(HangarRequest* r) {
    std::wstring type = r->req.GetStr(L"type");
    json::Value rep = json::Value::MakeObject();
    if (type == L"hello") {
        rep.Set(L"protocol", json::Value::Num(1));
        rep.Set(L"app", json::Value::Str(L"outpost"));
        rep.Set(L"version", json::Value::Str(OUTPOST_VERSION));
        json::Value caps = json::Value::MakeArray();
        for (auto c : {L"state", L"theme", L"settings"}) caps.arr.push_back(json::Value::Str(c));
        rep.Set(L"capabilities", caps);
    } else if (type == L"status") {
        int jobs = ActiveJobs();
        rep.Set(L"state", json::Value::Str(jobs ? L"busy" : L"idle"));
        if (jobs) rep.Set(L"reason", json::Value::Str(L"downloading game files"));
    } else if (type == L"prepare-update") {
        if (ActiveJobs()) { rep.Set(L"ok", json::Value::Boolean(false)); rep.Set(L"reason", json::Value::Str(L"downloading game files")); }
        else {
            std::wstring f = DataDir() + L"\\hangar-state.json";
            WriteTextFileAtomic(f, StateJson());
            ConfigSave();
            rep.Set(L"ok", json::Value::Boolean(true));
            rep.Set(L"stateFile", json::Value::Str(f));
        }
    } else if (type == L"restart") {
        r->Reply(rep);
        // the window closes; the game servers keep running and the new version reconnects to them
        g_reallyQuit = true;
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        delete r;
        return;
    } else if (type == L"cancel-update") {
        rep.Set(L"ok", json::Value::Boolean(true));
    } else if (type == L"apply-theme") {
        bool ok = false;
        if (const json::Value* th = r->req.Get(L"theme"); th && g_cfg.hangarThemes) {
            std::wstring text;
            json::Write(*th, text);
            ThemeDef def;
            std::wstring err;
            if (ParseThemeJson(text, def, &err)) {
                EnsureDir(ThemesDir());
                WriteTextFileAtomic(ThemesDir() + L"\\hangar-live.json", text);
                g_cfg.theme = def.id;
                if (const json::Value* a = r->req.Get(L"accent")) g_cfg.accent = a->s;
                ConfigSave();
                ApplyTheme();
                ok = true;
            }
        }
        rep.Set(L"ok", json::Value::Boolean(ok));
    } else if (type == L"apply-settings") {
        ConfigLoad();
        ApplyTheme();
        ApplyStartup();
        rep.Set(L"ok", json::Value::Boolean(true));
        rep.Set(L"restartRequired", json::Value::Boolean(false));
    } else if (type == L"export-settings") {
        ConfigSave();
        rep.Set(L"file", json::Value::Str(DataDir() + L"\\settings.json"));
    } else {
        rep.Set(L"ok", json::Value::Boolean(false));
        rep.Set(L"reason", json::Value::Str(L"unknown request"));
    }
    r->Reply(rep);
    delete r;
}

// ---------------------------------------------------------------- input
static void Scroll(float delta) {
    if (g_menu.open && In(g_menu.box, g_mx, g_my)) {
        float maxS = std::max(0.f, g_menu.items.size() * D(32) + S(8) - (g_menu.box.bottom - g_menu.box.top));
        g_menu.scroll = std::max(0.f, std::min(maxS, g_menu.scroll - delta));
        Invalidate();
        return;
    }
    if (ConsoleHit(g_mx, g_my)) { ConsoleWheel(delta); return; }
    if (g_mx < S(264)) { SidebarWheel(delta); return; }
    ScrollArea& sa = PageScroll();
    float maxPos = std::max(0.f, sa.content - sa.view);
    sa.target = std::max(0.f, std::min(maxPos, sa.target - delta));
    if (AnimMs(1) == 0) sa.pos = sa.target;
    StartAnim();
}

static bool Animate() {
    bool more = false;
    for (auto& kv : g_scrolls) {
        ScrollArea& sa = kv.second;
        if (std::fabs(sa.pos - sa.target) > 0.5f) { sa.pos += (sa.target - sa.pos) * 0.3f; more = true; }
        else sa.pos = sa.target;
    }
    return more || GetTickCount() < g_animUntil;
}

static void OnKey(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (g_modal.open) {
        if (vk == VK_ESCAPE) { g_modal = Modal(); Invalidate(); }
        if (vk == VK_RETURN) { auto f = g_modal.onOk; g_modal = Modal(); if (f) f(); Invalidate(); }
        return;
    }
    if (g_menu.open) {
        if (vk == VK_ESCAPE) { g_menu.open = false; Invalidate(); }
        if (vk == VK_DOWN) { g_menu.sel = std::min((int)g_menu.items.size() - 1, g_menu.sel + 1); Invalidate(); }
        if (vk == VK_UP) { g_menu.sel = std::max(0, g_menu.sel - 1); Invalidate(); }
        if (vk == VK_RETURN && g_menu.sel >= 0) { auto p = g_menu.pick; int i = g_menu.sel; g_menu.open = false; if (p) p(i); Invalidate(); }
        return;
    }
    if (!g_focus.empty()) {
        if (g_fieldOpts.key && g_fieldOpts.key((UINT)vk, g_edit)) { g_selectAll = false; Invalidate(); return; }
        if (vk == VK_ESCAPE) { g_edit = g_editOriginal; Blur(false); return; }
        if (vk == VK_RETURN) { Blur(true); return; }
        if (vk == VK_TAB) { Blur(true); return; }
        if (ctrl && vk == 'V') { std::wstring c = GetClipboard(); for (wchar_t ch : c) if (ch >= 32) FieldChar(ch); return; }
        if (ctrl && vk == 'A') { g_selectAll = true; Invalidate(); return; }
        if (ctrl && vk == 'C') { SetClipboard(g_edit); return; }
        return;
    }
    if (ctrl && vk == 'N') { StartWizard(); return; }
    if (ctrl && vk >= '1' && vk <= '6' && Sel()) { g_page = Page::Server; SetTab((int)(vk - '1')); return; }
    if (ctrl && vk == VK_OEM_COMMA) { g_page = Page::AppSettings; StartAnim(); return; }
    if (vk == VK_F5) { ReloadServers(Sel() ? Sel()->def.id : L""); return; }
    if ((vk == VK_UP || vk == VK_DOWN) && !g_servers.empty() && g_page == Page::Server) {
        int n = (int)g_servers.size();
        SelectServer(((g_sel < 0 ? 0 : g_sel) + (vk == VK_DOWN ? 1 : n - 1)) % n);
        return;
    }
    if (vk == VK_ESCAPE && g_page != Page::Server) { g_page = Page::Server; StartAnim(); }
}

// ---------------------------------------------------------------- window
static void Layout() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    g_W = (float)rc.right; g_H = (float)rc.bottom;
    if (g_rt) g_rt->Resize(D2D1::SizeU(rc.right, rc.bottom));
    LayoutRawEditor();
}

static void SaveWindowPlacement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    g_cfg.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    RECT r = wp.rcNormalPosition;
    g_cfg.windowX = r.left; g_cfg.windowY = r.top; g_cfg.windowW = r.right - r.left; g_cfg.windowH = r.bottom - r.top;
    if (Sel()) g_cfg.lastServer = Sel()->def.id;
    g_cfg.lastTab = g_tab;
    ConfigSave();
}

static void SampleSystem() {
    proc::SysStats s = proc::SampleSystem();
    g_sys.cpu = s.cpu; g_sys.ramUsed = s.ramUsed; g_sys.ramTotal = s.ramTotal;
}
static void RefreshNetwork() {
    Async([] {
        std::wstring router;
        bool upnp = netx::upnp::Available(&router);
        std::wstring pub = netx::PublicIp(), local = netx::LocalIp();
        if (!g_cfg.duckDomain.empty() && !g_cfg.duckToken.empty()) netx::UpdateDuckDns(g_cfg.duckDomain, g_cfg.duckToken, nullptr);
        Ui([=] { g_sys.upnp = upnp ? 1 : 0; g_sys.router = router; g_sys.publicIp = pub; g_sys.localIp = local; Invalidate(); });
    });
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) { AddTray(); return 0; }
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_s = GetDpiForWindow(hwnd) / 96.f;
        ApplyTheme();
        return 0;
    }
    case WM_SIZE: Layout(); Invalidate(); return 0;
    case WM_DPICHANGED: {
        g_s = HIWORD(wp) / 96.f;
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Invalidate();
        return 0;
    }
    case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize = {(LONG)(900 * g_s), (LONG)(580 * g_s)}; return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); Paint(); EndPaint(hwnd, &ps); return 0; }
    case WM_MOUSEMOVE: {
        g_mx = (float)GET_X_LPARAM(lp); g_my = (float)GET_Y_LPARAM(lp);
        g_lastInput = GetTickCount();
        if (!g_dragKey.empty()) { g_dragValue = SliderValueAt(g_mx); if (g_dragLive) g_dragLive(g_dragValue); }
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
        Invalidate();
        return 0;
    }
    case WM_MOUSELEAVE: g_mx = g_my = -1; Invalidate(); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            const HotRect* h = HitAt(g_mx, g_my);
            SetCursor(LoadCursorW(nullptr, h && h->enabled && h->hand ? IDC_HAND : h && !h->hand ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        g_mx = (float)GET_X_LPARAM(lp); g_my = (float)GET_Y_LPARAM(lp);
        g_downX = g_mx; g_downY = g_my; g_mdown = true;
        SetCapture(hwnd);
        const HotRect* h = HitAt(g_mx, g_my);
        if (g_menu.open && !In(g_menu.box, g_mx, g_my)) { g_menu.open = false; g_mdown = false; ReleaseCapture(); Invalidate(); return 0; }
        // clicking outside the focused field commits it
        if (!g_focus.empty() && (!h || h->hand)) Blur(true);
        Invalidate();
        // sliders pick up the drag in the next paint
        UpdateWindow(hwnd);
        return 0;
    }
    case WM_LBUTTONUP: {
        g_mx = (float)GET_X_LPARAM(lp); g_my = (float)GET_Y_LPARAM(lp);
        ReleaseCapture();
        bool wasDown = g_mdown;
        g_mdown = false;
        if (!g_dragKey.empty()) {
            auto commit = g_dragCommit;
            double v = g_dragValue;
            g_dragKey.clear(); g_dragLive = nullptr; g_dragCommit = nullptr;
            if (commit) commit(v);
            Invalidate();
            return 0;
        }
        if (!wasDown) return 0;
        const HotRect* h = HitAt(g_mx, g_my);
        if (h && h->enabled && In(h->r, g_downX, g_downY)) { auto f = h->click; if (f) f(); }
        Invalidate();
        return 0;
    }
    case WM_LBUTTONDBLCLK: return 0;
    case WM_RBUTTONUP: return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        g_mx = (float)p.x; g_my = (float)p.y;
        Scroll(GET_WHEEL_DELTA_WPARAM(wp) / 120.f * S(80));
        return 0;
    }
    case WM_KEYDOWN: g_lastInput = GetTickCount(); OnKey(wp); return 0;
    case WM_CHAR:
        if (!g_focus.empty() && wp != VK_RETURN && wp != VK_ESCAPE && wp != VK_TAB && !(GetKeyState(VK_CONTROL) < 0 && wp < 32 && wp != 127 && wp != 8)) FieldChar((wchar_t)wp);
        return 0;
    case WM_TIMER:
        if (wp == 2) {
            if (!Animate()) { KillTimer(hwnd, 2); g_animTimer = false; }
            Invalidate();
        } else if (wp == 1) {
            // every second: caret, uptime labels, system stats, idle updates
            static int n = 0;
            ++n;
            if (n % 2 == 0) SampleSystem();
            if (n % 600 == 0) RefreshNetwork();
            if (n % 300 == 0) host::CleanupRuntimeCopies();
            if (GetTickCount() - g_selfCheckedTick > (DWORD)g_cfg.checkHours * 3600000u) CheckSelf(false);
            if (g_self.kind == SelfUpdateState::Ready && g_cfg.autoUpdate && !ActiveJobs() &&
                (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetTickCount() - g_lastInput > 120000)) ApplySelf(false);
            TrayTip();
            Invalidate();
        }
        return 0;
    case WM_OP_JOB: { auto* f = (std::function<void()>*)lp; (*f)(); delete f; Invalidate(); return 0; }
    case WM_OP_HOSTEVENT: {
        auto* e = (HostEvent*)lp;
        if (ServerView* v = FindServer(e->server)) {
            if (e->connected) v->connected = true;
            if (e->disconnected) {
                v->connected = false;
                if (!host::IsRunning(v->def.id)) {
                    // the host finished: read its last words
                    std::wstring st;
                    json::Value sj;
                    if (ReadTextFile(v->def.root + L"\\state.json", st) && json::Parser(st).Parse(sj) && sj.GetStr(L"state") == L"error") { v->state = L"error"; v->message = sj.GetStr(L"message"); }
                    else if (v->state != L"error") { v->state = L"stopped"; v->message.clear(); }
                    v->players.clear(); v->cpu = 0; v->ram = 0; v->perf = -1;
                }
            }
            if (!e->line.empty()) {
                std::string line = e->line;
                if (line.find("\"ev\":\"notify\"") != std::string::npos) {
                    json::Value n;
                    if (json::Parser(Utf8ToWide(line)).Parse(n)) {
                        std::wstring kind = n.GetStr(L"kind");
                        bool fg = GetForegroundWindow() == hwnd;
                        if ((kind == L"crash" && g_cfg.notifyCrash) || (kind == L"backup-failed" && g_cfg.notifyBackupFail) || (kind == L"join" && g_cfg.notifyJoin && !fg))
                            Notify(kind == L"crash" ? L"Server crashed" : kind == L"join" ? L"Player joined" : L"Backup failed", n.GetStr(L"text"));
                        if (kind == L"backup" || kind == L"backup-failed") { v->backupsLoaded = false; if (kind == L"backup-failed") Toast(n.GetStr(L"text"), true); }
                    }
                }
                OnHostEvent(*v, line);
            }
            Invalidate();
        }
        delete e;
        return 0;
    }
    case WM_OP_SELF: {
        auto* s = (SelfUpdateState*)lp;
        if (s->kind == SelfUpdateState::Ready || s->kind == SelfUpdateState::Downloading || g_self.kind != SelfUpdateState::Ready) g_self = *s;
        if (s->kind == SelfUpdateState::Ready && !g_cfg.autoUpdate) Notify(L"Outpost " + s->version + L" is ready", L"Open Outpost to update. Your servers keep running.");
        delete s;
        Invalidate();
        return 0;
    }
    case WM_OP_HANGAR: OnHangar((HangarRequest*)lp); return 0;
    case WM_OP_TEMPLATES: templates::Load(); Invalidate(); return 0;
    case WM_OP_ACTIVATE: ShowWindowNow(); return 0;
    case WM_OP_TRAY:
        switch (LOWORD(lp)) {
            case WM_LBUTTONUP: case NIN_SELECT: case NIN_KEYSELECT: ShowWindowNow(); break;
            case WM_RBUTTONUP: case WM_CONTEXTMENU: TrayMenu(); break;
            case NIN_BALLOONUSERCLICK: ShowWindowNow(); break;
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet")) ApplyTheme();
        return 0;
    case WM_SYSCOLORCHANGE: case WM_THEMECHANGED: ApplyTheme(); return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        auto toRef = [](D2D1_COLOR_F c) { return RGB((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255)); };
        static HBRUSH br = nullptr;
        static COLORREF last = 0;
        COLORREF bg = toRef(Mix(Rgb(g_t.dark ? 0x101014 : 0xFFFFFF), g_t.surface, 0.5f));
        if (!br || bg != last) { if (br) DeleteObject(br); br = CreateSolidBrush(bg); last = bg; }
        SetTextColor(dc, toRef(g_t.text));
        SetBkColor(dc, bg);
        return (LRESULT)br;
    }
    case WM_CLOSE:
        // the window closes; servers keep running in their hosts either way
        if ((wp == 1 && lp == 0x0D17) || g_reallyQuit || !g_cfg.closeToTray || !g_trayAdded) {
            SaveWindowPlacement();
            DestroyWindow(hwnd);
        } else {
            SaveWindowPlacement();
            ShowWindow(hwnd, SW_HIDE);
            static bool told = false;
            if (!told) { told = true; Notify(L"Outpost is still here", L"Your servers keep running. Outpost is in the tray."); }
        }
        return 0;
    case WM_QUERYENDSESSION: SaveWindowPlacement(); return TRUE;
    case WM_DESTROY:
        if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        hangar::Stop();
        hostlink::Shutdown();
        DiscardDevice();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace ui

using namespace ui;

HWND UiCreate(HINSTANCE inst, int show, const UiStart& start) {
    g_start = start;
    if (!gfx::Init()) return nullptr;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"Outpost.larosa023.Main";
    RegisterClassExW(&wc);
    int w = g_cfg.windowW > 0 ? g_cfg.windowW : 0, h = g_cfg.windowH > 0 ? g_cfg.windowH : 0;
    UINT dpi = GetDpiForSystem();
    if (!w) { w = MulDiv(1240, dpi, 96); h = MulDiv(800, dpi, 96); }
    int x = g_cfg.windowX, y = g_cfg.windowY;
    // keep it on a screen that still exists
    RECT want{x, y, x + w, y + h};
    if (x == CW_USEDEFAULT || !MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) { x = CW_USEDEFAULT; y = CW_USEDEFAULT; }
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Outpost", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;
    Layout();
    hostlink::Init(hwnd);
    ReloadServers(g_cfg.lastServer);
    g_tab = std::max(0, std::min((int)TabCount - 1, g_cfg.lastTab));
    // reopen where you were (after an Outpost update, or a live update from Hangar)
    std::wstring restore = start.restoreState;
    std::wstring text;
    if (!restore.empty() && ReadTextFile(restore, text)) { RestoreFrom(text); DeleteFileW(restore.c_str()); }
    if (g_servers.empty() && !g_restoredFromState) g_page = Page::Welcome;
    else if (!g_restoredFromState) g_page = Page::Server;
    AddTray();
    hangar::Start(hwnd);
    SetTimer(hwnd, 1, 1000, nullptr);
    SampleSystem();
    RefreshNetwork();
    templates::FetchRemote(hwnd);
    // start in the tray at sign-in, starting the servers you marked
    if (start.boot)
        for (auto& v : g_servers) if (v.def.startOnBoot && !v.Running()) StartServer(v);
    if (start.tray) ShowWindow(hwnd, SW_HIDE);
    else ShowWindow(hwnd, g_cfg.maximized ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);
    // tell the copy we replaced (and Hangar) that we're up
    selfupdate::SignalHealthy(start.after);
    selfupdate::FinishAfterUpdate(start.after);
    if (start.after.active) { PruneSkippedVersions(OUTPOST_VERSION); Toast(L"Updated to Outpost " OUTPOST_VERSION L". Your servers kept running."); }
    std::wstring summary = g_restoreSummary;
    std::thread([summary] { Sleep(2000); hangar::SendHealth(summary); }).detach();
    g_selfCheckedTick = GetTickCount() - (DWORD)g_cfg.checkHours * 3600000u + 15000;   // first check 15 s after start
    ApplyStartup();
    return hwnd;
}
