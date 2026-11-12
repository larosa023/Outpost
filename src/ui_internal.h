// Outpost - the window's shared state and widgets (immediate-mode Direct2D, like the rest of the suite)
#pragma once
#include "common.h"
#include "gfx.h"
#include "themes.h"
#include "server.h"
#include "template.h"
#include "backup.h"
#include "netx.h"
#include "mods.h"
#include "config.h"
#include "selfupdate.h"
#include "provision.h"
#include <functional>
#include <deque>
#include <memory>
#include <atomic>

namespace ui {
using namespace gfx;

// ---------------------------------------------------------------- state
struct ConsoleLine { long long n = 0, t = 0; std::wstring kind, text; };
struct PlayerView { std::wstring name, id; long long since = 0; int ping = -1; double x = 0, y = 0; bool loc = false; int level = 0; };
struct Job { bool active = false; std::wstring kind, text; int pct = -1; std::shared_ptr<std::atomic<bool>> cancel; };
struct HistoryItem { long long t = 0; std::wstring ev, name; };

struct ServerView {
    ServerDef def;
    Template tmpl;
    std::wstring state = L"stopped", message, hostVersion;
    bool connected = false;
    std::vector<PlayerView> players;
    int maxPlayers = 0;
    double cpu = 0, perf = -1, perfMax = 0;
    uint64_t ram = 0, ramLimit = 0;
    std::wstring perfLabel;
    long long since = 0, readyAt = 0, lastBackup = 0, nextBackup = 0, nextRestart = 0, sleepingSince = 0;
    bool backingUp = false, gaming = false;
    std::wstring upnp, upnpError;
    std::deque<ConsoleLine> console;
    std::vector<BackupInfo> backups;
    bool backupsLoaded = false;
    Job job;
    netx::Report net;
    bool netLoaded = false, netBusy = false;
    std::vector<ModFile> modFiles;
    std::vector<ModHit> modHits;
    std::wstring modQuery, modSource = L"modrinth", modError;
    bool modBusy = false, modsLoaded = false, modSearched = false;
    std::vector<std::pair<int, ModVersion>> modUpdates;
    std::vector<std::wstring> lists[3];      // whitelist, ops, bans (from files)
    bool listsLoaded = false;
    std::vector<HistoryItem> history;
    bool historyLoaded = false;
    std::wstring updateAvailable;
    bool updateChecked = false, updateChecking = false;
    int firewall = -1;                        // -1 unknown, 0 missing, 1 present
    bool needsRestart = false;
    std::vector<std::wstring> cmdHistory;
    int historyPos = -1;
    std::wstring consoleFilter = L"all";
    float consoleBack = 0;                    // pixels scrolled up from the newest line (0 = follow)
    std::vector<std::wstring> versions;
    bool versionsLoading = false;
    bool Running() const { return state == L"running" || state == L"starting" || state == L"stopping" || state == L"restarting" || state == L"sleeping"; }
};

extern std::vector<ServerView> g_servers;
extern int g_sel;
enum class Page { Server, Wizard, AppSettings, Welcome };
extern Page g_page;
enum Tab { TabOverview, TabConsole, TabPlayers, TabMods, TabBackups, TabSettings, TabCount };
extern int g_tab;
extern HWND g_hwnd;
extern Theme g_t;
extern float g_s, g_d, g_W, g_H, g_mx, g_my;
extern bool g_mdown;
extern Canvas g_c;
extern SelfUpdateState g_self;
extern std::vector<ThemeDef> g_customThemes;

struct SysView { double cpu = 0; uint64_t ramUsed = 0, ramTotal = 0; std::wstring publicIp, localIp; int upnp = -1; std::wstring router; };
extern SysView g_sys;

// sizes: S() follows the screen's scale, D() also the density setting, F() the font size setting
inline float S(float v) { return v * g_s; }
inline float D(float v) { return v * g_s * g_d; }
inline float F(float px) { return px * g_s * (float)g_cfg.fontScale; }
inline D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
inline D2D1_RECT_F RW(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
inline bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// ---------------------------------------------------------------- plumbing
void Async(std::function<void()> work);              // runs on a worker thread
void Ui(std::function<void()> fn);                   // runs on the UI thread (from anywhere)
void Invalidate();
void StartAnim();                                    // fast repaints for a moment (animations)
float AnimMs(float normalMs);                        // scaled by the animation setting (0 = off)
ServerView* FindServer(const std::wstring& id);
ServerView* Sel();
void Toast(const std::wstring& text, bool bad = false);
void Confirm(const std::wstring& title, const std::wstring& text, const std::wstring& ok, std::function<void()> onOk, bool danger = false);
void SetClipboard(const std::wstring& text);
std::wstring PickFolder(const std::wstring& title);
std::wstring PickFile(const std::wstring& title, const wchar_t* filter);
void OpenPath(const std::wstring& path);
void OpenUrl(const std::wstring& url);
std::wstring Ago(long long unix);
std::wstring Duration(long long secs);
std::wstring ShortTime(long long unix);
int ActiveJobs();

// ---------------------------------------------------------------- widgets
void Hot(const D2D1_RECT_F& r, std::function<void()> click, bool enabled = true, bool hand = true);
bool Hover(const D2D1_RECT_F& r);
bool Pressed(const D2D1_RECT_F& r);
enum class Btn { Primary, Secondary, Ghost, Danger };
float ButtonW(const std::wstring& label, wchar_t glyph = 0);
void Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph, Btn style, std::function<void()> click, bool enabled = true);
void IconButton(const D2D1_RECT_F& r, wchar_t glyph, std::function<void()> click, bool enabled = true, const D2D1_COLOR_F* color = nullptr);
void Toggle(float x, float y, bool on, std::function<void(bool)> changed, bool enabled = true);   // 40 x 22 (scaled)
float Segmented(float x, float y, const std::vector<std::wstring>& opts, int sel, std::function<void(int)> pick);
// Single-line text box. commit runs on Enter or when focus leaves; change on every keystroke (optional).
struct FieldOpts {
    bool password = false, numeric = false, live = false;
    std::wstring placeholder;
    std::function<void(const std::wstring&)> change;                 // live edits
    std::function<bool(UINT vk, std::wstring& text)> key;            // Up/Down/Tab handling (console); return true if handled
};
void TextField(const D2D1_RECT_F& r, const std::wstring& key, const std::wstring& value, std::function<void(const std::wstring&)> commit, const FieldOpts& o = {});
bool Focused(const std::wstring& key);
void FocusField(const std::wstring& key, const std::wstring& value);
std::wstring& EditText();
void Dropdown(const D2D1_RECT_F& r, const std::vector<std::wstring>& labels, int sel, std::function<void(int)> pick, bool enabled = true);
void Slider(const D2D1_RECT_F& r, const std::wstring& key, double value, double lo, double hi, double step,
            std::function<void(double)> live, std::function<void(double)> commit);
void ProgressBar(const D2D1_RECT_F& r, int pct, D2D1_COLOR_F color);
void Card(const D2D1_RECT_F& r, bool hover = false);
float Text(const std::wstring& s, float x, float y, float w, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT wt = DWRITE_FONT_WEIGHT_NORMAL,
           DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING, float h = 0);         // one line; returns width used
float Para(const std::wstring& s, float x, float y, float w, float size, D2D1_COLOR_F c);   // wrapped; returns height
float TextW(const std::wstring& s, float size, DWRITE_FONT_WEIGHT wt = DWRITE_FONT_WEIGHT_NORMAL);
void Glyph(wchar_t g, float cx, float cy, float size, D2D1_COLOR_F c);
float Pill(float x, float y, const std::wstring& text, D2D1_COLOR_F color);                   // returns width
void GameTile(const Template& t, const D2D1_RECT_F& r, float textSize);
D2D1_COLOR_F StateColor(const std::wstring& state);
std::wstring StateLabel(const std::wstring& state);

// scrolling content: Begin returns the y where content starts (scroll applied); End records the height
struct ScrollArea { float pos = 0, target = 0, content = 0, view = 0; };
ScrollArea& PageScroll();
float BeginScroll(ScrollArea& sa, const D2D1_RECT_F& viewport);
void EndScroll(ScrollArea& sa, float contentBottom);
bool Visible(float top, float bottom);                         // inside the current scroll viewport
extern D2D1_RECT_F g_viewport;

// popups / overlays
void OpenMenu(const D2D1_RECT_F& anchor, const std::vector<std::wstring>& items, int sel, std::function<void(int)> pick);

// ---------------------------------------------------------------- actions (ui_server.cpp)
void ReloadServers(const std::wstring& select = L"");
void SelectServer(int i);
void SetTab(int t);
void StartServer(ServerView& v);
void StopServer(ServerView& v);
void RestartServer(ServerView& v);
void BackupNow(ServerView& v);
void LoadBackups(ServerView& v);
void RestoreBackup(ServerView& v, const BackupInfo& b);
void SendCommand(ServerView& v, const std::wstring& text);
void PlayerAction(ServerView& v, const std::wstring& action, const std::wstring& player, const std::wstring& text = L"");
void InstallJob(const std::wstring& id, const std::wstring& kind);     // install | update | repair
void SaveServer(ServerView& v, bool reloadHost = true);
void OnHostEvent(ServerView& v, const std::string& line);
std::wstring ConnectAddress(const ServerView& v, bool local, bool masked);

// ---------------------------------------------------------------- pages
void DrawSidebar();
void DrawServer(float x0);
void DrawWizard(float x0);
void DrawAppSettings(float x0);
void DrawWelcome(float x0);
void StartWizard();
void ConsoleWheel(float delta);                                 // true if the console consumed it
bool ConsoleHit(float x, float y);
void OpenRawEditor(ServerView& v, const std::wstring& path);
void CloseRawEditor(bool save);
bool RawEditorOpen();
void LayoutRawEditor();
void ApplyTheme();
void SidebarWheel(float delta);
void ImportServer();
void ApplyStartup();
void UpdateNow();
void CheckSelf(bool force);
} // namespace ui
