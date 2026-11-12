// Outpost - sidebar, welcome, the new server wizard, import, and app settings
#include "ui_internal.h"
#include "util.h"
#include "ops.h"
#include "host.h"
#include "fsutil.h"
#include "pipe.h"
#include <wincodec.h>
#include <commdlg.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>

namespace ui {

// ---------------------------------------------------------------- the app icon (from the exe's resources)
static ID2D1Bitmap* g_logo = nullptr;
static ID2D1RenderTarget* g_logoRt = nullptr;
static ID2D1Bitmap* Logo() {
    if (g_logo && g_logoRt == g_c.rt) return g_logo;
    SafeRelease(g_logo);
    g_logoRt = g_c.rt;
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(102), RT_RCDATA);
    if (!r) return nullptr;
    HGLOBAL h = LoadResource(nullptr, r);
    DWORD size = SizeofResource(nullptr, r);
    const void* data = LockResource(h);
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory((BYTE*)data, size)) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &frame)) &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        g_c.rt->CreateBitmapFromWicBitmap(conv, nullptr, &g_logo);
    SafeRelease(conv); SafeRelease(frame); SafeRelease(dec); SafeRelease(stream); SafeRelease(wic);
    return g_logo;
}

// ---------------------------------------------------------------- sidebar
static float SidebarW() { return S(264); }
static ScrollArea sideScroll;
void SidebarWheel(float delta) {
    float maxPos = std::max(0.f, sideScroll.content - sideScroll.view);
    sideScroll.target = sideScroll.pos = std::max(0.f, std::min(maxPos, sideScroll.pos - delta));
    Invalidate();
}

void DrawSidebar() {
    float w = SidebarW();
    g_c.Fill(R(0, 0, w, g_H), g_t.sidebar);
    g_c.Line(w - 0.5f, 0, w - 0.5f, g_H, g_t.divider);
    float pad = D(16), y = D(20);
    if (ID2D1Bitmap* logo = Logo()) g_c.rt->DrawBitmap(logo, RW(pad, y, D(30), D(30)));
    Text(L"Outpost", pad + D(40), y, w - pad * 2, F(17), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(30));
    IconButton(RW(w - pad - D(32), y - D(1), D(32), D(32)), icon::Settings, [] { g_page = Page::AppSettings; StartAnim(); });
    y += D(48);
    Text(L"SERVERS", pad, y, w, F(11), g_t.faint, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    y += D(24);
    float bottomPanel = g_H - D(150) - (g_self.kind == SelfUpdateState::Ready ? D(52) : 0);
    float listBottom = bottomPanel - D(56);
    float rowH = D(58);
    float sy = BeginScroll(sideScroll, R(0, y, w, listBottom));
    for (size_t i = 0; i < g_servers.size(); ++i) {
        ServerView& v = g_servers[i];
        D2D1_RECT_F r = R(pad - D(6), sy, w - pad + D(6), sy + rowH - D(4));
        bool sel = (int)i == g_sel && g_page == Page::Server;
        if (sel) g_c.FillRound(r, S(g_t.radius + 2), g_t.surfaceStrong);
        else if (Hover(r)) g_c.FillRound(r, S(g_t.radius + 2), g_t.surfaceHover);
        GameTile(v.tmpl, RW(r.left + D(8), sy + D(8), D(38), D(38)), F(13));
        Text(v.def.name, r.left + D(56), sy + D(7), r.right - r.left - D(64), F(13.5f), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(22));
        std::wstring st = v.job.active ? L"installing" : v.state;
        g_c.Circle(r.left + D(60), sy + D(37), D(3.5f), StateColor(st));
        std::wstring line = v.job.active ? v.job.text + (v.job.pct >= 0 ? L" " + std::to_wstring(v.job.pct) + L"%" : L"") : StateLabel(st);
        if (v.state == L"running") line += L" \x00B7 " + std::to_wstring(v.players.size()) + (v.players.size() == 1 ? L" player" : L" players");
        Text(line, r.left + D(70), sy + D(27), r.right - r.left - D(78), F(12), g_t.subtext, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(20));
        int idx = (int)i;
        Hot(r, [idx] { SelectServer(idx); });
        sy += rowH;
    }
    EndScroll(sideScroll, sy);
    Button(RW(pad, listBottom + D(10), w - pad * 2, D(38)), L"New server", icon::Add, g_servers.empty() ? Btn::Primary : Btn::Secondary, [] { StartWizard(); });
    // this PC
    float py = bottomPanel;
    g_c.Line(pad, py - D(6), w - pad, py - D(6), g_t.divider);
    if (g_self.kind == SelfUpdateState::Ready) {
        Button(RW(pad, py, w - pad * 2, D(40)), L"Update to " + g_self.version, icon::Update, Btn::Primary, [] { UpdateNow(); });
        py += D(52);
    }
    Text(L"THIS PC", pad, py + D(4), w, F(11), g_t.faint, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    py += D(28);
    float bw = w - pad * 2;
    wchar_t cpu[16]; swprintf(cpu, 16, L"%.0f%%", g_sys.cpu);
    Text(L"CPU", pad, py, bw / 2, F(12), g_t.subtext);
    Text(cpu, pad + bw / 2, py, bw / 2, F(12), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    ProgressBar(RW(pad, py + D(20), bw, S(4)), (int)g_sys.cpu, g_sys.cpu > 85 ? g_t.warn : g_t.accent);
    py += D(32);
    Text(L"Memory", pad, py, bw / 2, F(12), g_t.subtext);
    Text(FormatBytes(g_sys.ramUsed) + L" / " + FormatBytes(g_sys.ramTotal), pad + bw / 3, py, bw * 2 / 3, F(12), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    ProgressBar(RW(pad, py + D(20), bw, S(4)), g_sys.ramTotal ? (int)(g_sys.ramUsed * 100 / g_sys.ramTotal) : 0, g_t.accent);
    py += D(32);
    std::wstring ports = g_sys.upnp < 0 ? L"Checking the router…" : g_sys.upnp ? L"Ports: automatic (UPnP)" : L"Ports: forward by hand";
    g_c.Circle(pad + D(4), py + D(10), D(3.5f), g_sys.upnp < 0 ? g_t.faint : g_sys.upnp ? g_t.good : g_t.warn);
    Text(ports, pad + D(14), py, bw - D(14), F(12), g_t.subtext, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(20));
}

// ---------------------------------------------------------------- welcome
void DrawWelcome(float x0) {
    float pad = D(48), x = x0 + pad, w = g_W - x0 - pad * 2;
    ScrollArea& sa = PageScroll();
    float y = BeginScroll(sa, R(x0, 0, g_W, g_H)) + D(56);
    Text(L"Host a game server in one click", x, y, w, F(28), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(44));
    y += D(50);
    y += Para(L"Pick a game, press Start, and send your friends the address. Outpost downloads the server, sets up ports and the firewall, "
              L"backs up your world, and keeps the server running even when you close this window.", x, y, std::min(w, D(640)), F(14), g_t.subtext) + D(24);
    float bw = ButtonW(L"New server", icon::Add), iw = ButtonW(L"Import a server or world", icon::Upload);
    Button(RW(x, y, bw, D(42)), L"New server", icon::Add, Btn::Primary, [] { StartWizard(); });
    Button(RW(x + bw + D(10), y, iw, D(42)), L"Import a server or world", icon::Upload, Btn::Secondary, [] { ImportServer(); });
    y += D(74);
    Text(L"SUPPORTED GAMES", x, y, w, F(11), g_t.faint, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    y += D(28);
    float tw = D(196), th = D(76), gap = D(12);
    int cols = std::max(1, (int)((w + gap) / (tw + gap)));
    int i = 0;
    for (auto& t : templates::All()) {
        float cx = x + (i % cols) * (tw + gap), cy = y + (i / cols) * (th + gap);
        D2D1_RECT_F r = RW(cx, cy, tw, th);
        Card(r, Hover(r));
        GameTile(t, RW(cx + D(14), cy + D(16), D(44), D(44)), F(15));
        Text(t.name, cx + D(68), cy + D(14), tw - D(76), F(13), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(22));
        Text(t.sourceType == L"steamcmd" ? L"via SteamCMD" : t.variants.empty() ? L"Official server" : std::to_wstring(t.variants.size()) + L" server types", cx + D(68), cy + D(38), tw - D(76), F(11.5f), g_t.faint);
        std::wstring id = t.id;
        Hot(r, [id] { StartWizard(); extern void WizardPick(const std::wstring&); WizardPick(id); });
        ++i;
    }
    y += ((i + cols - 1) / cols) * (th + gap) + D(30);
    Para(L"Want another game? Drop a template file in " + templates::UserDir() + L" (see docs/TEMPLATES.md on GitHub).", x, y, w, F(12), g_t.faint);
    EndScroll(sa, y + D(40));
}

// ---------------------------------------------------------------- wizard
struct Wizard {
    int step = 0;
    std::wstring game, variant, version = L"latest", name, location;
    std::map<std::wstring, std::wstring> settings;
    int ram = 0;
    std::vector<std::wstring> versions;
    bool versionsLoading = false;
    std::wstring versionsFor;
    bool creating = false;
};
static Wizard g_wiz;
static const wchar_t* kSteps[] = {L"Game", L"Version", L"Name", L"World", L"Memory", L"Create"};

void StartWizard() {
    g_wiz = Wizard();
    g_page = Page::Wizard;
    StartAnim();
}
static const Template* WizTemplate() { return templates::Find(g_wiz.game); }

void WizardPick(const std::wstring& id) {
    const Template* t = templates::Find(id);
    if (!t) return;
    g_wiz.game = id;
    g_wiz.variant = t->variants.empty() ? L"" : (t->id == L"minecraft-java" ? L"paper" : t->variants[0].id);
    g_wiz.version = L"latest";
    g_wiz.versions.clear(); g_wiz.versionsFor.clear();
    g_wiz.settings.clear();
    // defaults for the wizard's fields
    ServerDef tmp;
    servers::ApplyDefaults(tmp, *t);
    for (auto& st : t->settings) g_wiz.settings[st.id] = tmp.settings[st.id];
    g_wiz.ram = t->hasRam ? t->ramDefault : 0;
    int n = 1;
    for (auto& s : g_servers) if (s.def.game == id) ++n;
    g_wiz.name = t->name + (n > 1 ? L" " + std::to_wstring(n) : L"");
    if (t->id == L"minecraft-java") g_wiz.name = L"Minecraft" + std::wstring(n > 1 ? L" " + std::to_wstring(n) : L"");
    g_wiz.step = 1;
    StartAnim();
}

static void WizLoadVersions() {
    const Template* t = WizTemplate();
    if (!t || (t->sourceType != L"minecraft" && t->sourceType != L"terraria")) return;
    std::wstring key = g_wiz.game + L"/" + g_wiz.variant;
    if (g_wiz.versionsFor == key) return;
    g_wiz.versionsFor = key;
    g_wiz.versions.clear();
    g_wiz.versionsLoading = true;
    Template tc = *t;
    std::wstring variant = g_wiz.variant;
    Async([tc, variant, key] {
        auto v = provision::ListVersions(tc, variant);
        Ui([v, key] { if (g_wiz.versionsFor == key) { g_wiz.versions = v.list; if (g_wiz.versions.size() > 150) g_wiz.versions.resize(150); g_wiz.versionsLoading = false; } });
    });
}

static std::wstring WizValidate(int step) {
    const Template* t = WizTemplate();
    if (!t) return L"Choose a game.";
    if (step == 2 && Trim(g_wiz.name).empty()) return L"Give the server a name.";
    if (step == 3)
        for (auto& st : t->settings) {
            std::wstring v = g_wiz.settings[st.id];
            if (st.required == L"true" && v != st.valOn) return st.label + L" is required.";
            if (IStartsWith(st.required, L"minlen:") && (int)v.size() < _wtoi(st.required.c_str() + 7)) return st.label + L" needs at least " + st.required.substr(7) + L" characters.";
        }
    return L"";
}

static void WizCreate() {
    const Template* t = WizTemplate();
    if (!t || g_wiz.creating) return;
    for (int s = 2; s <= 3; ++s) { std::wstring e = WizValidate(s); if (!e.empty()) { Toast(e, true); g_wiz.step = s; return; } }
    std::wstring err;
    ServerDef s;
    std::map<std::wstring, std::wstring> set = g_wiz.settings;
    // the template's own defaults ({random} passwords and the like) stay generated by the server, not the wizard's copy
    for (auto it = set.begin(); it != set.end();) { const TSetting* st = t->Setting(it->first); if (st && !st->wizard) it = set.erase(it); else ++it; }
    if (!ops::Create(g_wiz.game, g_wiz.variant, g_wiz.version, Trim(g_wiz.name), set, g_wiz.ram, g_wiz.location, &s, &err)) { Toast(err, true); return; }
    ReloadServers(s.id);
    g_page = Page::Server;
    g_tab = TabOverview;
    InstallJob(s.id, L"install+start");
    Toast(L"Creating " + s.name + L". The download runs in the background.");
}

static void WizSettingRow(const TSetting& st, float x, float& y, float w) {
    std::wstring sid = st.id;
    std::wstring val = g_wiz.settings[sid];
    float lw = w - D(300);
    Text(st.label, x, y, lw, F(13.5f), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(22));
    float h = D(22);
    if (!st.help.empty()) h += Para(st.help, x, y + D(22), lw, F(12), g_t.faint);
    float cx = x + w - D(280);
    if (st.type == L"bool") {
        Toggle(x + w - S(40), y, val == st.valOn, [sid, on = st.valOn, off = st.valOff](bool b) { g_wiz.settings[sid] = b ? on : off; });
    } else if (st.type == L"choice") {
        std::vector<std::wstring> labels;
        int sel = 0;
        for (size_t i = 0; i < st.options.size(); ++i) { labels.push_back(st.options[i].label); if (st.options[i].value == val) sel = (int)i; }
        auto opts = st.options;
        Dropdown(RW(cx, y - D(2), D(280), D(36)), labels, sel, [sid, opts](int i) { g_wiz.settings[sid] = opts[i].value; });
    } else {
        FieldOpts o;
        o.password = false;   // you're typing it now: show it
        o.numeric = st.type == L"number";
        o.placeholder = st.type == L"seed" ? L"Random" : st.type == L"password" ? L"(none)" : L"";
        o.live = true;
        o.change = [sid](const std::wstring& t) { g_wiz.settings[sid] = t; };
        TextField(RW(cx, y - D(2), D(280), D(36)), L"wiz:" + sid, val, [sid](const std::wstring& t) { g_wiz.settings[sid] = t; }, o);
    }
    y += std::max(h, D(36)) + D(16);
}

void DrawWizard(float x0) {
    float pad = D(40), w = std::min(g_W - x0 - pad * 2, D(900));
    float x = x0 + std::max(pad, (g_W - x0 - w) / 2);
    float y = D(28);
    Text(L"New server", x, y, w, F(24), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(36));
    float cw = ButtonW(L"Cancel");
    Button(RW(x + w - cw, y, cw, D(34)), L"Cancel", 0, Btn::Ghost, [] { g_page = g_servers.empty() ? Page::Welcome : Page::Server; StartAnim(); });
    y += D(50);
    // steps
    const Template* t = WizTemplate();
    float sx = x;
    for (int i = 0; i < 6; ++i) {
        bool done = i < g_wiz.step, on = i == g_wiz.step;
        bool skip = (i == 1 && t && t->variants.empty() && t->sourceType != L"minecraft" && t->sourceType != L"terraria") || (i == 4 && t && !t->hasRam);
        D2D1_COLOR_F c = on ? g_t.accent : done ? g_t.good : g_t.faint;
        g_c.Circle(sx + D(12), y + D(12), D(12), Alpha(c, on ? 1.f : 0.18f));
        if (done) Glyph(icon::Check, sx + D(12), y + D(12), F(10), g_t.good);
        else Text(std::to_wstring(i + 1), sx, y, D(24), F(11.5f), on ? g_t.onAccent : c, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, D(24));
        float lw = TextW(kSteps[i], F(12.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        Text(kSteps[i], sx + D(30), y, lw + 4, F(12.5f), skip ? g_t.faint : on ? g_t.text : g_t.subtext, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(24));
        D2D1_RECT_F hr = RW(sx, y, lw + D(36), D(24));
        int step = i;
        if (t && i < g_wiz.step) Hot(hr, [step] { g_wiz.step = step; StartAnim(); });
        sx += lw + D(36);
        if (i < 5) { g_c.Line(sx, y + D(12), sx + D(18), y + D(12), g_t.divider, S(1.5f)); sx += D(26); }
    }
    y += D(46);
    float bottom = g_H - D(76);
    ScrollArea& sa = PageScroll();
    float cy = BeginScroll(sa, R(x - D(4), y, x + w + D(12), bottom));
    int step = g_wiz.step;
    if (step == 0) {
        Text(L"Which game?", x, cy, w, F(17), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
        float iw = ButtonW(L"Import existing", icon::Upload);
        Button(RW(x + w - iw, cy - D(2), iw, D(34)), L"Import existing", icon::Upload, Btn::Secondary, [] { ImportServer(); });
        cy += D(46);
        float tw = (w - D(24)) / 3, th = D(84), gap = D(12);
        int i = 0;
        for (auto& tt : templates::All()) {
            float tx = x + (i % 3) * (tw + gap), ty = cy + (i / 3) * (th + gap);
            D2D1_RECT_F r = RW(tx, ty, tw, th);
            bool sel = g_wiz.game == tt.id;
            Card(r, Hover(r));
            if (sel) g_c.StrokeRound(r, S(g_t.radius + 2), g_t.accent, S(2));
            GameTile(tt, RW(tx + D(16), ty + D(18), D(48), D(48)), F(16));
            Text(tt.name, tx + D(78), ty + D(16), tw - D(86), F(14), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(22));
            Para(tt.description, tx + D(78), ty + D(40), tw - D(90), F(11.5f), g_t.faint);
            std::wstring id = tt.id;
            Hot(r, [id] { WizardPick(id); });
            ++i;
        }
        cy += ((i + 2) / 3) * (th + gap);
    } else if (step == 1 && t) {
        if (!t->variants.empty()) {
            Text(L"Server type", x, cy, w, F(17), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
            cy += D(40);
            float vw = (w - D(12) * (t->variants.size() - 1)) / t->variants.size(), vh = D(92);
            for (size_t i = 0; i < t->variants.size(); ++i) {
                auto& var = t->variants[i];
                D2D1_RECT_F r = RW(x + i * (vw + D(12)), cy, vw, vh);
                bool sel = g_wiz.variant == var.id;
                Card(r, Hover(r));
                if (sel) g_c.StrokeRound(r, S(g_t.radius + 2), g_t.accent, S(2));
                Text(var.name, r.left + D(16), r.top + D(14), vw - D(32), F(15), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(24));
                Para(var.description, r.left + D(16), r.top + D(42), vw - D(32), F(12), g_t.subtext);
                std::wstring vid = var.id;
                Hot(r, [vid] { g_wiz.variant = vid; g_wiz.version = L"latest"; });
            }
            cy += vh + D(16);
            if (g_wiz.variant == L"vanilla") { Para(L"Vanilla can't load mods or plugins. Paper runs vanilla worlds faster and supports plugins.", x, cy, w, F(12.5f), g_t.faint); cy += D(28); }
        }
        if (t->sourceType == L"minecraft" || t->sourceType == L"terraria") {
            WizLoadVersions();
            Text(L"Game version", x, cy + D(8), w, F(17), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
            cy += D(48);
            std::vector<std::wstring> labels = {g_wiz.versionsLoading ? L"Latest (checking…)" : L"Latest" + std::wstring(g_wiz.versions.empty() ? L"" : L" (" + g_wiz.versions[0] + L")")};
            int sel = 0;
            for (size_t i = 0; i < g_wiz.versions.size(); ++i) { labels.push_back(g_wiz.versions[i]); if (g_wiz.versions[i] == g_wiz.version) sel = (int)i + 1; }
            Dropdown(RW(x, cy, D(280), D(38)), labels, sel, [](int i) { g_wiz.version = i == 0 ? L"latest" : g_wiz.versions[i - 1]; });
            Para(L"Match the version your friends play. \"Latest\" also lets Outpost update the server when a new version comes out.", x + D(300), cy, w - D(300), F(12.5f), g_t.faint);
            cy += D(56);
        } else if (t->variants.empty()) {
            Para(t->name + L" servers come from Steam (through SteamCMD, which Outpost downloads for you). You always get the newest version, and Outpost tells you when an update is out.", x, cy, w, F(13.5f), g_t.subtext);
            cy += D(40);
        }
    } else if (step == 2 && t) {
        Text(L"Name your server", x, cy, w, F(17), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
        cy += D(40);
        FieldOpts o; o.live = true; o.change = [](const std::wstring& s) { g_wiz.name = s; };
        TextField(RW(x, cy, std::min(w, D(460)), D(40)), L"wiz:name", g_wiz.name, [](const std::wstring& s) { g_wiz.name = s; }, o);
        cy += D(56);
        Text(L"Where to keep it", x, cy, w, F(13.5f), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(22));
        cy += D(28);
        std::wstring loc = g_wiz.location.empty() ? servers::BaseDir() : g_wiz.location;
        Text(loc, x, cy, w - D(200), F(12.5f), g_t.subtext, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(34));
        float bw = ButtonW(L"Change…");
        Button(RW(x + w - D(200) + D(10), cy, bw, D(34)), L"Change…", 0, Btn::Secondary, [] { std::wstring f = PickFolder(L"Where should this server's files go? (a big, fast drive is best)"); if (!f.empty()) g_wiz.location = f; });
        cy += D(44);
        uint64_t free = fs::FreeSpace(loc);
        Para(L"Each server gets its own folder. " + (free ? FormatBytes(free) + L" free on this drive." : L"") + (t->id == L"cs2" ? L" Counter-Strike 2 needs about 35 GB." : t->id == L"ark-ascended" ? L" ARK needs about 15 GB." : L""), x, cy, w, F(12.5f), g_t.faint);
        cy += D(30);
    } else if (step == 3 && t) {
        Text(L"World settings", x, cy, w, F(17), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
        cy += D(44);
        bool any = false;
        for (auto& st : t->settings) if (st.wizard) { WizSettingRow(st, x, cy, w); any = true; }
        if (!any) { Para(L"Nothing to set up here: you can change everything later in Settings.", x, cy, w, F(13), g_t.subtext); cy += D(30); }
        if (!t->notes.empty()) { cy += D(8); cy += Para(t->notes, x, cy, w, F(12.5f), g_t.faint) + D(10); }
    } else if (step == 4 && t) {
        Text(L"Memory", x, cy, w, F(17), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
        cy += D(44);
        if (!t->hasRam) { Para(t->name + L" uses what it needs; there's nothing to set.", x, cy, w, F(13), g_t.subtext); cy += D(30); }
        else {
            double totalMb = g_sys.ramTotal ? g_sys.ramTotal / 1048576.0 : 16384;
            double hi = std::max((double)t->ramMin, std::min(65536.0, std::floor((totalMb - 2048) / 512) * 512));
            wchar_t big[64]; swprintf(big, 64, L"%.1f GB", g_wiz.ram / 1024.0);
            Text(big, x, cy, w, F(30), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(44));
            cy += D(52);
            Slider(RW(x, cy, std::min(w, D(520)), D(28)), L"wiz:ram", g_wiz.ram, t->ramMin, hi, 512, [](double v) { g_wiz.ram = (int)v; }, [](double v) { g_wiz.ram = (int)v; });
            cy += D(40);
            wchar_t note[256];
            swprintf(note, 256, L"This PC has %.0f GB. %ls", totalMb / 1024.0,
                     g_wiz.ram < 3072 ? L"2–3 GB is fine for a few friends on vanilla." : g_wiz.ram <= 6144 ? L"4–6 GB suits most servers, including light mods." : L"Modpacks and big groups like 8 GB or more.");
            Para(note, x, cy, w, F(12.5f), g_t.faint);
            cy += D(30);
        }
    } else if (step == 5 && t) {
        Text(L"Ready to create", x, cy, w, F(17), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
        cy += D(44);
        D2D1_RECT_F card = RW(x, cy, w, D(120));
        Card(card);
        GameTile(*t, RW(x + D(20), cy + D(24), D(72), D(72)), F(22));
        Text(g_wiz.name, x + D(110), cy + D(22), w - D(130), F(18), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
        std::wstring desc = t->name;
        for (auto& var : t->variants) if (var.id == g_wiz.variant) desc += L" \x00B7 " + var.name;
        desc += L" \x00B7 " + (g_wiz.version == L"latest" ? std::wstring(L"latest version") : g_wiz.version);
        if (t->hasRam) { wchar_t r[32]; swprintf(r, 32, L" \x00B7 %.1f GB", g_wiz.ram / 1024.0); desc += r; }
        Text(desc, x + D(110), cy + D(54), w - D(130), F(13), g_t.subtext);
        cy += D(140);
        cy += Para(L"Outpost downloads the server files" + std::wstring(t->sourceType == L"steamcmd" ? L" through SteamCMD" : t->ramJava ? L" and Java" : L"") +
                   L", writes the settings, and starts it. Windows may ask once to allow it through the firewall so friends can connect.", x, cy, w, F(13), g_t.subtext) + D(12);
    }
    EndScroll(sa, cy + D(20));
    // footer
    float fy = g_H - D(64);
    g_c.Line(x, fy - D(12), x + w, fy - D(12), g_t.divider);
    auto next = [](int from) {
        const Template* tt = WizTemplate();
        int s = from + 1;
        if (s == 1 && tt && tt->variants.empty() && tt->sourceType != L"minecraft" && tt->sourceType != L"terraria") s = 2;
        if (s == 4 && tt && !tt->hasRam) s = 5;
        return s;
    };
    auto prev = [](int from) {
        const Template* tt = WizTemplate();
        int s = from - 1;
        if (s == 4 && tt && !tt->hasRam) s = 3;
        if (s == 1 && tt && tt->variants.empty() && tt->sourceType != L"minecraft" && tt->sourceType != L"terraria") s = 0;
        return std::max(0, s);
    };
    if (step > 0) Button(RW(x, fy, ButtonW(L"Back"), D(40)), L"Back", 0, Btn::Secondary, [prev] { g_wiz.step = prev(g_wiz.step); StartAnim(); });
    if (step == 5) {
        float bw = ButtonW(L"Create and start", icon::Play);
        Button(RW(x + w - bw, fy, bw, D(40)), L"Create and start", icon::Play, Btn::Primary, [] { WizCreate(); });
    } else if (step > 0) {
        float bw = ButtonW(L"Next");
        Button(RW(x + w - bw, fy, bw, D(40)), L"Next", 0, Btn::Primary, [next] {
            std::wstring e = WizValidate(g_wiz.step);
            if (!e.empty()) { Toast(e, true); return; }
            g_wiz.step = next(g_wiz.step);
            StartAnim();
        });
    }
}

// ---------------------------------------------------------------- import
void ImportServer() {
    std::wstring folder = PickFolder(L"Choose a server folder, or a world folder");
    if (folder.empty()) return;
    std::wstring variant;
    std::wstring game = provision::DetectGame(folder, &variant);
    auto go = [folder](const std::wstring& hint) {
        Toast(L"Importing " + PathName(folder) + L"…");
        Async([folder, hint] {
            ServerDef s;
            std::wstring err;
            bool ok = ops::Import(folder, L"", hint, nullptr, &s, &err);
            Ui([ok, s, err] {
                if (!ok) { Toast(err, true); return; }
                ReloadServers(s.id);
                g_page = Page::Server;
                g_tab = TabOverview;
                // make sure the server files (and Java) are there; existing files are kept
                InstallJob(s.id, L"install");
            });
        });
    };
    if (!game.empty()) { go(L""); return; }
    // a bare world: ask which game it's for
    std::vector<std::wstring> labels;
    std::vector<std::wstring> ids;
    for (auto& t : templates::All()) { labels.push_back(t.name); ids.push_back(t.id); }
    if (FileExists(folder + L"\\level.dat")) { go(L"minecraft-java"); return; }
    OpenMenu(RW(g_W / 2 - S(150), g_H / 3, S(300), S(1)), labels, -1, [ids, go](int i) { go(ids[i]); });
    Toast(L"Which game is this world from?");
}

// ---------------------------------------------------------------- app settings
static float ARow(float x, float y, float w, const std::wstring& label, const std::wstring& help, float* cx, float controlW) {
    float lw = w - controlW - D(24);
    Text(label, x, y, lw, F(13.5f), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(22));
    float h = D(22);
    if (!help.empty()) h += Para(help, x, y + D(22), lw, F(12), g_t.faint);
    *cx = x + w - controlW;
    return y + std::max(h, D(36)) + D(14);
}

static void ThemeCard(const ThemeDef& d0, const D2D1_RECT_F& r, bool sel) {
    ThemeDef d = d0;
    static bool light = false;
    static DWORD lightAt = 0;
    if (!lightAt || GetTickCount() - lightAt > 5000) { light = SystemUsesLightTheme(); lightAt = GetTickCount(); }
    if (d.followWindows) d = BuiltinThemes()[light ? 1 : 0];
    Theme th = MakeTheme(d, g_cfg.accent);
    float rad = S(g_t.radius + 2);
    g_c.FillRound(r, rad, Rgb(d.bg));
    g_c.FillRound(R(r.left, r.top, r.left + (r.right - r.left) * 0.3f, r.bottom - D(28)), rad, Rgb(d.sidebar));
    g_c.FillRound(R(r.left + (r.right - r.left) * 0.36f, r.top + D(10), r.right - D(10), r.top + D(30)), S(4), d.surfaceOpacity < 1 ? Rgb(d.surface, 0.12f) : Rgb(d.surface));
    g_c.FillRound(R(r.left + (r.right - r.left) * 0.36f, r.top + D(36), r.left + (r.right - r.left) * 0.66f, r.top + D(46)), S(3), th.accent);
    g_c.Fill(R(r.left, r.bottom - D(28), r.right, r.bottom - D(27)), Rgb(d.text, 0.08f));
    Text(d0.name, r.left + D(10), r.bottom - D(28), r.right - r.left - D(20), F(12), Rgb(d.text), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
    g_c.StrokeRound(r, rad, sel ? g_t.accent : g_t.divider, sel ? S(2.5f) : 1.f);
}

static std::wstring Hex(uint32_t c) { wchar_t b[16]; swprintf(b, 16, L"#%06X", c & 0xFFFFFF); return b; }

void DrawAppSettings(float x0) {
    float pad = D(40), w = std::min(g_W - x0 - pad * 2, D(860));
    float x = x0 + std::max(pad, (g_W - x0 - w) / 2);
    float y0 = D(28);
    Text(L"Outpost settings", x, y0, w, F(24), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(36));
    float bw = ButtonW(L"Done");
    Button(RW(x + w - bw, y0, bw, D(34)), L"Done", 0, Btn::Secondary, [] { g_page = g_servers.empty() ? Page::Welcome : Page::Server; StartAnim(); });
    ScrollArea& sa = PageScroll();
    float y = BeginScroll(sa, R(x - D(4), y0 + D(52), x + w + D(12), g_H)) + D(8);
    float cx;
    auto title = [&](const std::wstring& s) { y += D(12); Text(s, x, y, w, F(16), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28)); y += D(40); };
    auto save = [] { ConfigSave(); ApplyTheme(); };

    // ---- appearance
    title(L"Appearance");
    static std::vector<ThemeDef> all;
    static size_t customCount = (size_t)-1;
    if (customCount != g_customThemes.size() || all.empty()) {
        all = BuiltinThemes();
        for (auto& c : g_customThemes) all.push_back(c);
        customCount = g_customThemes.size();
    }
    float tw = D(150), th = D(96), gap = D(12);
    int cols = std::max(1, (int)((w + gap) / (tw + gap)));
    for (size_t i = 0; i < all.size(); ++i) {
        D2D1_RECT_F r = RW(x + (i % cols) * (tw + gap), y + (i / cols) * (th + gap), tw, th);
        if (Visible(r.top, r.bottom)) ThemeCard(all[i], r, all[i].id == g_cfg.theme);
        std::wstring id = all[i].id;
        Hot(r, [id, save] { g_cfg.theme = id; save(); });
    }
    y += ((all.size() + cols - 1) / cols) * (th + gap) + D(8);
    float tb1 = ButtonW(L"Open themes folder", icon::Folder), tb2 = ButtonW(L"Reload", icon::Refresh);
    Button(RW(x, y, tb1, D(32)), L"Open themes folder", icon::Folder, Btn::Ghost, [] { EnsureDir(ThemesDir()); OpenPath(ThemesDir()); });
    Button(RW(x + tb1 + D(8), y, tb2, D(32)), L"Reload", icon::Refresh, Btn::Ghost, [save] { save(); Toast(std::to_wstring(g_customThemes.size()) + L" custom themes."); });
    Text(L"Theme files use the suite's shared format, so themes saved in Hangar show up here too.", x + tb1 + tb2 + D(24), y, w - tb1 - tb2 - D(24), F(12), g_t.faint, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(32));
    y += D(50);
    // accent
    float ay = y;
    y = ARow(x, y, w, L"Accent color", L"", &cx, D(470));
    {
        float sx = cx;
        auto sw = [&](uint32_t c, const std::wstring& setting, const std::wstring& label) {
            D2D1_RECT_F r = RW(sx, ay - D(2), D(28), D(28));
            bool sel = g_cfg.accent == setting;
            g_c.Circle(sx + D(14), ay + D(12), D(12), Rgb(c));
            if (sel) { g_c.StrokeRound(R(r.left - S(3), r.top - S(3), r.right + S(3), r.bottom + S(3)), D(17), g_t.text, S(2)); }
            (void)label;
            Hot(r, [setting, save] { g_cfg.accent = setting; save(); });
            sx += D(34);
        };
        ThemeDef cur = FindTheme(g_cfg.theme, g_customThemes);
        sw(ResolveAccent(cur, L"theme"), L"theme", L"Theme");
        static uint32_t sysAccent = 0;
        static DWORD sysAt = 0;
        if (!sysAt || GetTickCount() - sysAt > 5000) { sysAccent = SystemAccent(); sysAt = GetTickCount(); }
        sw(sysAccent, L"windows", L"Windows");
        for (int i = 0; i < kAccentSwatchCount && i < 8; ++i) sw(kAccentSwatches[i], Hex(kAccentSwatches[i]), L"");
        D2D1_RECT_F cr = RW(sx, ay - D(2), D(28), D(28));
        g_c.StrokeRound(cr, D(14), g_t.subtext, S(1.5f));
        Glyph(icon::Add, sx + D(14), ay + D(12), F(11), g_t.subtext);
        Hot(cr, [save] {
            static COLORREF custom[16];
            CHOOSECOLORW cc{sizeof(cc)};
            cc.hwndOwner = g_hwnd; cc.lpCustColors = custom; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            uint32_t a = (uint32_t)(g_t.accent.r * 255) << 16 | (uint32_t)(g_t.accent.g * 255) << 8 | (uint32_t)(g_t.accent.b * 255);
            cc.rgbResult = RGB((a >> 16) & 255, (a >> 8) & 255, a & 255);
            if (ChooseColorW(&cc)) { g_cfg.accent = Hex((GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult)); save(); }
        });
    }
    float ry = y;
    y = ARow(x, y, w, L"Background", BackdropSupported() ? L"Mica and acrylic let your wallpaper's color show through (Windows 11)." : L"Mica and acrylic need Windows 11.", &cx, D(330));
    static const wchar_t* bds[] = {L"theme", L"solid", L"mica", L"acrylic"};
    int bsel = 0;
    for (int i = 0; i < 4; ++i) if (g_cfg.backdrop == bds[i]) bsel = i;
    Segmented(cx, ry - D(2), {L"Theme", L"Solid", L"Mica", L"Acrylic"}, bsel, [save](int i) { g_cfg.backdrop = bds[i]; save(); });
    ry = y;
    y = ARow(x, y, w, L"Text size", L"", &cx, D(330));
    static const double fsz[] = {0.9, 1.0, 1.15, 1.3};
    int fsel = 1;
    for (int i = 0; i < 4; ++i) if (std::fabs(g_cfg.fontScale - fsz[i]) < 0.01) fsel = i;
    Segmented(cx, ry - D(2), {L"Small", L"Default", L"Large", L"Larger"}, fsel, [save](int i) { g_cfg.fontScale = fsz[i]; save(); });
    ry = y;
    y = ARow(x, y, w, L"Density", L"", &cx, D(330));
    static const wchar_t* dens[] = {L"compact", L"comfortable", L"spacious"};
    int dsel = 1;
    for (int i = 0; i < 3; ++i) if (g_cfg.density == dens[i]) dsel = i;
    Segmented(cx, ry - D(2), {L"Compact", L"Comfortable", L"Spacious"}, dsel, [save](int i) { g_cfg.density = dens[i]; save(); });
    ry = y;
    wchar_t rl[48]; swprintf(rl, 48, L"Corner radius: %d", g_cfg.radius);
    y = ARow(x, y, w, rl, L"", &cx, D(330));
    Slider(RW(cx, ry, D(330), D(24)), L"app:radius", g_cfg.radius, 0, 16, 1, [](double v) { g_cfg.radius = (int)v; g_t.radius = (float)v; }, [save](double v) { g_cfg.radius = (int)v; save(); });
    ry = y;
    y = ARow(x, y, w, L"Animations", L"", &cx, D(330));
    static const wchar_t* an[] = {L"off", L"fast", L"normal", L"slow"};
    int asel = 2;
    for (int i = 0; i < 4; ++i) if (g_cfg.animation == an[i]) asel = i;
    Segmented(cx, ry - D(2), {L"Off", L"Fast", L"Normal", L"Slow"}, asel, [](int i) { g_cfg.animation = an[i]; ConfigSave(); });
    ry = y;
    y = ARow(x, y, w, L"Live themes from Hangar", L"When you change the suite's theme in Hangar, Outpost follows right away.", &cx, S(40));
    Toggle(cx, ry, g_cfg.hangarThemes, [](bool on) { g_cfg.hangarThemes = on; ConfigSave(); });

    // ---- updates
    title(L"Updates");
    std::wstring st;
    switch (g_self.kind) {
        case SelfUpdateState::Checking: st = L"Checking GitHub…"; break;
        case SelfUpdateState::UpToDate: st = L"You have the newest version."; break;
        case SelfUpdateState::Downloading: st = L"Downloading Outpost " + g_self.version + (g_self.percent >= 0 ? L" (" + std::to_wstring(g_self.percent) + L"%)" : L""); break;
        case SelfUpdateState::Ready: st = L"Outpost " + g_self.version + L" is ready. Updating never stops your servers."; break;
        case SelfUpdateState::Error: st = g_self.message; break;
        default: st = L"Checks github.com/larosa023/Outpost."; break;
    }
    ry = y;
    y = ARow(x, y, w, L"Outpost " OUTPOST_VERSION, st, &cx, D(200));
    if (g_self.kind == SelfUpdateState::Ready) { float uw = ButtonW(L"Update now", icon::Update); Button(RW(x + w - uw, ry - D(2), uw, D(34)), L"Update now", icon::Update, Btn::Primary, [] { UpdateNow(); }); }
    else { float uw = ButtonW(L"Check now", icon::Refresh); Button(RW(x + w - uw, ry - D(2), uw, D(34)), L"Check now", icon::Refresh, Btn::Secondary, [] { CheckSelf(true); }, g_self.kind != SelfUpdateState::Checking && g_self.kind != SelfUpdateState::Downloading); }
    if (g_self.kind == SelfUpdateState::Ready && !g_self.notes.empty()) {
        std::wstring notes = g_self.notes;
        for (size_t p; (p = notes.find(L"**")) != std::wstring::npos;) notes.erase(p, 2);
        for (size_t p; (p = notes.find(L"## ")) != std::wstring::npos;) notes.erase(p, 3);
        float h = Para(notes, x + D(16), y + D(12), w - D(32), F(12.5f), g_t.subtext);
        g_c.StrokeRound(RW(x, y, w, h + D(24)), S(g_t.radius + 2), g_t.divider);
        y += h + D(40);
    }
    ry = y;
    y = ARow(x, y, w, L"Channel", L"Beta gets new features first.", &cx, D(200));
    Segmented(cx, ry - D(2), {L"Stable", L"Beta"}, g_cfg.beta ? 1 : 0, [](int i) { g_cfg.beta = i == 1; ConfigSave(); CheckSelf(true); });
    ry = y;
    y = ARow(x, y, w, L"Install updates automatically", L"Quietly, when you're not using Outpost. Your servers keep running.", &cx, S(40));
    Toggle(cx, ry, g_cfg.autoUpdate, [](bool on) { g_cfg.autoUpdate = on; ConfigSave(); });
    ry = y;
    y = ARow(x, y, w, L"Check every", L"", &cx, D(200));
    static const int hrs[] = {1, 3, 6, 12, 24};
    int hsel = 1;
    for (int i = 0; i < 5; ++i) if (hrs[i] == g_cfg.checkHours) hsel = i;
    Dropdown(RW(cx, ry - D(2), D(200), D(36)), {L"hour", L"3 hours", L"6 hours", L"12 hours", L"day"}, hsel, [](int i) { g_cfg.checkHours = hrs[i]; ConfigSave(); });
    static std::vector<selfupdate::Previous> prevs;
    static DWORD prevsAt = 0;
    if (!prevsAt || GetTickCount() - prevsAt > 5000) { prevs = selfupdate::PreviousVersions(); prevsAt = GetTickCount(); }
    for (auto& p : prevs) {
        ry = y;
        y = ARow(x, y, w, L"Go back to " + p.version, L"If something broke after an update.", &cx, D(160));
        float rbw = ButtonW(L"Roll back");
        selfupdate::Previous pv = p;
        Button(RW(x + w - rbw, ry - D(2), rbw, D(34)), L"Roll back", 0, Btn::Secondary, [pv] {
            Confirm(L"Go back to Outpost " + pv.version + L"?", L"Outpost restarts as version " + pv.version + L". Your servers keep running.", L"Roll back", [pv] {
                g_self.kind = SelfUpdateState::Ready; g_self.version = pv.version; g_self.staged = pv.exe; g_self.notes.clear();
                UpdateNow();
            });
        });
    }

    // ---- system
    title(L"System");
    ry = y;
    y = ARow(x, y, w, L"Start with Windows", L"Outpost starts in the tray when you sign in, and starts the servers you marked \"Start when Windows starts\".", &cx, S(40));
    Toggle(cx, ry, g_cfg.startWithWindows, [](bool on) { g_cfg.startWithWindows = on; ConfigSave(); ApplyStartup(); });
    ry = y;
    y = ARow(x, y, w, L"Keep running in the tray when closed", L"Servers keep running either way; the tray shows their status and notifications.", &cx, S(40));
    Toggle(cx, ry, g_cfg.closeToTray, [](bool on) { g_cfg.closeToTray = on; ConfigSave(); });
    ry = y;
    y = ARow(x, y, w, L"Notify me when a player joins", L"Only while the Outpost window isn't in front.", &cx, S(40));
    Toggle(cx, ry, g_cfg.notifyJoin, [](bool b) { g_cfg.notifyJoin = b; ConfigSave(); });
    ry = y;
    y = ARow(x, y, w, L"Notify me when a server crashes", L"", &cx, S(40));
    Toggle(cx, ry, g_cfg.notifyCrash, [](bool b) { g_cfg.notifyCrash = b; ConfigSave(); });
    ry = y;
    y = ARow(x, y, w, L"Notify me when a backup fails", L"", &cx, S(40));
    Toggle(cx, ry, g_cfg.notifyBackupFail, [](bool b) { g_cfg.notifyBackupFail = b; ConfigSave(); });
    ry = y;
    y = ARow(x, y, w, L"Hide my public IP", L"Masks your internet address on screen (handy when streaming). Copy still copies the real address.", &cx, S(40));
    Toggle(cx, ry, g_cfg.hidePublicIp, [](bool on) { g_cfg.hidePublicIp = on; ConfigSave(); });
    ry = y;
    y = ARow(x, y, w, L"Friendly address (DuckDNS)", L"Free: sign in at duckdns.org, create a name, and paste it and your token here. Friends then use name.duckdns.org, even when your IP changes.", &cx, D(330));
    TextField(RW(cx, ry - D(2), D(160), D(36)), L"app:duck", g_cfg.duckDomain, [](const std::wstring& v) { g_cfg.duckDomain = Trim(v); ConfigSave(); }, FieldOpts{false, false, false, L"yourname"});
    TextField(RW(cx + D(170), ry - D(2), D(160), D(36)), L"app:ducktoken", g_cfg.duckToken, [](const std::wstring& v) {
        g_cfg.duckToken = Trim(v); ConfigSave();
        if (!g_cfg.duckDomain.empty() && !g_cfg.duckToken.empty()) Async([] { std::wstring e; bool ok = netx::UpdateDuckDns(g_cfg.duckDomain, g_cfg.duckToken, &e); Ui([ok, e] { Toast(ok ? L"DuckDNS is pointing at this PC." : e, !ok); }); });
    }, FieldOpts{true, false, false, L"token"});
    ry = y;
    y = ARow(x, y, w, L"CurseForge API key", L"Optional, to search CurseForge too. Get a free key at console.curseforge.com.", &cx, D(330));
    TextField(RW(cx, ry - D(2), D(330), D(36)), L"app:cf", g_cfg.curseForgeKey, [](const std::wstring& v) { g_cfg.curseForgeKey = Trim(v); ConfigSave(); }, FieldOpts{true, false, false, L"Not set"});

    // ---- about
    title(L"About");
    float ab = 0;
    auto link = [&](const std::wstring& label, wchar_t g, std::function<void()> f) { float lw = ButtonW(label, g); Button(RW(x + ab, y, lw, D(34)), label, g, Btn::Ghost, f); ab += lw + D(8); };
    link(L"Data folder", icon::Folder, [] { OpenPath(DataDir()); });
    link(L"Log file", icon::Console, [] { OpenPath(LogPath()); });
    link(L"Game templates", icon::Puzzle, [] { EnsureDir(templates::UserDir()); OpenPath(templates::UserDir()); });
    link(L"Reload templates", icon::Refresh, [] { templates::Load(); templates::FetchRemote(g_hwnd); Toast(std::to_wstring(templates::All().size()) + L" game templates."); });
    link(L"GitHub", icon::Globe, [] { OpenUrl(L"https://github.com/larosa023/Outpost"); });
    y += D(48);
    static bool hangar = false;
    static DWORD hangarAt = 0;
    if (!hangarAt || GetTickCount() - hangarAt > 5000) {
        hangar = WaitNamedPipeW((L"\\\\.\\pipe\\larosa023.hangar." + pipe::SessionSuffix()).c_str(), 0) || GetLastError() == ERROR_SEM_TIMEOUT;
        hangarAt = GetTickCount();
    }
    Para(std::wstring(L"Outpost " OUTPOST_VERSION) + (IsPortable() ? L" (portable)" : L"") + L" \x00B7 " + std::to_wstring(templates::All().size()) + L" games \x00B7 " +
         (hangar ? L"connected to Hangar" : L"Hangar isn't running (Outpost updates itself)") + L"\nData: " + DataDir(), x, y, w, F(12), g_t.faint);
    y += D(50);
    Para(L"Keyboard: Ctrl+N new server \x00B7 Ctrl+1–6 tabs \x00B7 \x2191 \x2193 switch servers \x00B7 Ctrl+, settings \x00B7 F5 refresh. "
         L"Outpost follows Windows contrast themes and text size.", x, y, w, F(12), g_t.faint);
    y += D(50);
    EndScroll(sa, y);
}

} // namespace ui
