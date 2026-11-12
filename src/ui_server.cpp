// Outpost - a server's page: header, tabs (Overview, Console, Players, Mods, Backups, Settings) and actions
#include "ui_internal.h"
#include "util.h"
#include "hostlink.h"
#include "host.h"
#include "pipe.h"
#include "ops.h"
#include "fsutil.h"
#include "json.h"
#include <algorithm>
#include <cmath>
#include <shellapi.h>
#include <thread>

namespace ui {

static std::string J(const json::Value& v) { std::wstring o; json::Write(v, o); std::wstring one; for (wchar_t c : o) if (c != L'\n') one += c; return WideToUtf8(one); }

// ================================================================ actions
void ReloadServers(const std::wstring& select) {
    std::vector<ServerDef> defs = servers::LoadAll();
    std::vector<ServerView> next;
    for (auto& d : defs) {
        ServerView v;
        if (ServerView* old = FindServer(d.id)) v = std::move(*old);
        v.def = d;
        servers::LoadTemplate(v.def, v.tmpl);
        if (!v.connected) {
            if (host::IsRunning(d.id)) { if (!v.Running()) v.state = L"starting"; }
            else if (!v.job.active) {
                std::wstring st;
                json::Value sj;
                if (v.state != L"error" && ReadTextFile(d.root + L"\\state.json", st) && json::Parser(st).Parse(sj) && sj.GetStr(L"state") == L"error") { v.state = L"error"; v.message = sj.GetStr(L"message"); }
                else if (v.state != L"error") v.state = L"stopped";
            }
        }
        next.push_back(std::move(v));
    }
    std::wstring cur = !select.empty() ? select : (Sel() ? Sel()->def.id : L"");
    g_servers = std::move(next);
    g_sel = g_servers.empty() ? -1 : 0;
    for (size_t i = 0; i < g_servers.size(); ++i) if (g_servers[i].def.id == cur) g_sel = (int)i;
    for (auto& v : g_servers) hostlink::Watch(v.def.id);
    Invalidate();
}

void SelectServer(int i) {
    if (i < 0 || i >= (int)g_servers.size()) return;
    g_sel = i;
    g_page = Page::Server;
    g_cfg.lastServer = g_servers[i].def.id;
    StartAnim();
}
void SetTab(int t) { g_tab = std::max(0, std::min((int)TabCount - 1, t)); StartAnim(); }

static bool HostSend(ServerView& v, const std::string& msg) {
    if (hostlink::Send(v.def.id, msg)) return true;
    // not connected yet (just started): a one-off connection
    std::wstring id = v.def.id;
    Async([id, msg] { if (auto c = pipe::Connect(host::PipeName(id), 3000)) { c->Send(msg); std::thread([c] { Sleep(800); c->Close(); }).detach(); c->Run([](const std::string&) {}); } });
    return host::IsRunning(v.def.id);
}

static void EnsureFirewall(ServerView& v, std::function<void()> then) {
    if (!v.def.firewall) { then(); return; }
    std::wstring id = v.def.id;
    ServerDef def = v.def;
    Async([id, def, then] {
        bool has = netx::firewall::HasRules(def);
        Ui([id, has, then] {
            ServerView* sv = FindServer(id);
            if (!sv) return;
            sv->firewall = has ? 1 : 0;
            if (has) { then(); return; }
            Confirm(L"Let friends connect?", L"Windows Firewall needs a rule for " + sv->def.name + L"'s ports. Windows will ask for permission once. "
                    L"You can change this later in Settings → Network.", L"Allow", [id, then] {
                ServerView* s2 = FindServer(id);
                if (!s2) return;
                ServerDef d = s2->def;
                Async([id, d, then] {
                    std::wstring err;
                    bool ok = netx::firewall::Apply(d, true, &err);
                    Ui([id, ok, err, then] {
                        if (ServerView* s3 = FindServer(id)) s3->firewall = ok ? 1 : 0;
                        if (!ok) Toast(err, true);
                        then();
                    });
                });
            });
        });
    });
}

void StartServer(ServerView& v) {
    if (v.job.active) { Toast(L"Wait for " + v.job.text + L" to finish.", true); return; }
    if (v.Running()) return;
    if (v.def.installedVersion.empty()) { InstallJob(v.def.id, L"install+start"); return; }
    std::wstring id = v.def.id;
    auto go = [id] {
        ServerView* sv = FindServer(id);
        if (!sv || sv->Running()) return;
        sv->state = L"starting";
        sv->message.clear();
        sv->needsRestart = false;
        sv->console.push_back({0, fs::UnixNow(), L"outpost", L"Starting " + sv->def.name + L"…"});
        ServerDef d = sv->def;
        Async([id, d] {
            std::wstring err;
            bool ok = host::Launch(d, &err);
            Ui([id, ok, err] {
                ServerView* s2 = FindServer(id);
                if (!s2) return;
                if (!ok) { s2->state = L"error"; s2->message = err; Toast(err, true); }
                hostlink::Watch(id);
            });
        });
        Invalidate();
    };
    if (v.firewall == 1 || !v.def.firewall) go();
    else EnsureFirewall(v, go);
}
void StopServer(ServerView& v) {
    if (!v.Running()) return;
    if (v.state != L"sleeping") v.state = L"stopping";
    HostSend(v, "{\"type\":\"stop\"}");
    Invalidate();
}
void RestartServer(ServerView& v) {
    if (v.state != L"running") return;
    v.state = L"restarting";
    v.needsRestart = false;
    HostSend(v, "{\"type\":\"restart\"}");
    Invalidate();
}

void LoadBackups(ServerView& v) {
    std::wstring id = v.def.id;
    ServerDef d = v.def;
    v.backupsLoaded = true;
    Async([id, d] {
        auto list = backup::List(d);
        Ui([id, list] { if (ServerView* s = FindServer(id)) { s->backups = list; s->lastBackup = list.empty() ? s->lastBackup : std::max(s->lastBackup, list[0].time); } });
    });
}

void BackupNow(ServerView& v) {
    if (v.Running() && v.state != L"sleeping") {
        HostSend(v, "{\"type\":\"backup\",\"label\":\"manual\"}");
        v.backingUp = true;
        Toast(L"Backing up " + v.def.name + L"…");
        return;
    }
    if (v.job.active) return;
    std::wstring id = v.def.id;
    ServerDef d = v.def;
    Template t = v.tmpl;
    v.job = {true, L"backup", L"Backing up", -1, nullptr};
    Async([id, d, t] {
        std::wstring path, err;
        bool ok = backup::Create(d, t, L"manual", [id](uint64_t done, uint64_t total) {
            int pct = total ? (int)(done * 100 / total) : -1;
            Ui([id, pct] { if (ServerView* s = FindServer(id)) s->job.pct = pct; });
            return true;
        }, &path, &err);
        Ui([id, ok, err, path] {
            ServerView* s = FindServer(id);
            if (!s) return;
            s->job = Job();
            if (ok) { s->lastBackup = fs::UnixNow(); Toast(L"Backed up " + s->def.name + L" (" + FormatBytes(fs::TreeSize(path)) + L")"); }
            else Toast(err, true);
            LoadBackups(*s);
        });
    });
}

void RestoreBackup(ServerView& v, const BackupInfo& b) {
    std::wstring id = v.def.id;
    BackupInfo bi = b;
    bool running = v.Running();
    Confirm(L"Restore this backup?", L"The world from " + ShortTime(b.time) + L" replaces the current one. Outpost takes a safety backup first" +
            (running ? std::wstring(L", and stops the server for a moment.") : std::wstring(L".")), L"Restore", [id, bi, running] {
        ServerView* sv = FindServer(id);
        if (!sv) return;
        if (running) StopServer(*sv);
        sv->job = {true, L"restore", L"Restoring", -1, nullptr};
        ServerDef d = sv->def;
        Template t = sv->tmpl;
        Async([id, d, t, bi, running] {
            for (int i = 0; i < 3000 && host::IsRunning(d.id); ++i) Sleep(100);
            std::wstring err;
            bool ok = !host::IsRunning(d.id) && backup::Restore(d, t, bi.path, [id](uint64_t done, uint64_t total) {
                int pct = total ? (int)(done * 100 / total) : -1;
                Ui([id, pct] { if (ServerView* s = FindServer(id)) s->job.pct = pct; });
                return true;
            }, &err);
            if (!ok && err.empty()) err = L"The server didn't stop, so nothing was restored.";
            Ui([id, ok, err, running] {
                ServerView* s = FindServer(id);
                if (!s) return;
                s->job = Job();
                if (ok) { Toast(L"Restored. A safety backup of the previous world was saved."); if (running) StartServer(*s); }
                else Toast(err, true);
                LoadBackups(*s);
            });
        });
    });
}

void SendCommand(ServerView& v, const std::wstring& text) {
    if (Trim(text).empty()) return;
    json::Value m = json::Value::MakeObject();
    m.Set(L"type", json::Value::Str(L"command"));
    m.Set(L"text", json::Value::Str(text));
    if (!HostSend(v, J(m))) Toast(L"Start the server first.", true);
    v.cmdHistory.erase(std::remove(v.cmdHistory.begin(), v.cmdHistory.end(), text), v.cmdHistory.end());
    v.cmdHistory.push_back(text);
    if (v.cmdHistory.size() > 100) v.cmdHistory.erase(v.cmdHistory.begin());
    v.historyPos = -1;
    v.consoleBack = 0;
}

void PlayerAction(ServerView& v, const std::wstring& action, const std::wstring& player, const std::wstring& text) {
    json::Value m = json::Value::MakeObject();
    m.Set(L"type", json::Value::Str(L"action"));
    m.Set(L"action", json::Value::Str(action));
    m.Set(L"player", json::Value::Str(player));
    m.Set(L"text", json::Value::Str(text));
    if (!HostSend(v, J(m))) Toast(L"Start the server first.", true);
}

void InstallJob(const std::wstring& id, const std::wstring& kind) {
    ServerView* v = FindServer(id);
    if (!v || v->job.active) return;
    if (kind != L"install+start" && kind != L"install" && v->Running()) { Toast(L"Stop the server first.", true); return; }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    v->job = {true, kind, kind == L"update" ? L"Updating" : kind == L"repair" ? L"Repairing files" : L"Downloading", -1, cancel};
    if (kind == L"install+start" || kind == L"install") v->state = L"installing";
    ServerDef d = v->def;
    Async([id, d, kind, cancel]() mutable {
        DWORD last = 0;
        auto progress = [id, &last](int pct, const std::wstring& text) {
            DWORD now = GetTickCount();
            if (now - last < 120 && pct >= 0) return;
            last = now;
            Ui([id, pct, text] { if (ServerView* s = FindServer(id)) { s->job.pct = pct; s->job.text = text; } });
        };
        std::wstring err;
        bool ok = kind == L"update" ? ops::UpdateFiles(d, progress, cancel.get(), &err) : ops::Install(d, progress, cancel.get(), &err);
        Ui([id, d, ok, err, kind] {
            ServerView* s = FindServer(id);
            if (!s) return;
            s->job = Job();
            s->def = d;
            servers::LoadTemplate(s->def, s->tmpl);
            if (s->state == L"installing") s->state = ok ? L"stopped" : L"error";
            if (!ok) { s->message = err; Toast(err, true); return; }
            s->message.clear();
            s->updateAvailable.clear();
            s->updateChecked = false;
            Toast(s->def.name + (kind == L"update" ? L" updated to " : L" is ready: ") + s->def.installedVersion);
            if (kind == L"install+start") StartServer(*s);
        });
    });
}

void SaveServer(ServerView& v, bool reloadHost) {
    servers::Save(v.def);
    if (reloadHost && v.Running()) HostSend(v, "{\"type\":\"reload\"}");
}

void OnHostEvent(ServerView& v, const std::string& line) {
    json::Value e;
    if (!json::Parser(Utf8ToWide(line)).Parse(e)) return;
    std::wstring ev = e.GetStr(L"ev");
    if (ev == L"line") {
        ConsoleLine l{(long long)e.GetNum(L"n", 0), (long long)e.GetNum(L"t", 0), e.GetStr(L"k"), e.GetStr(L"s")};
        if (!v.console.empty() && l.n && v.console.back().n >= l.n) return;   // already have it (reconnect)
        v.console.push_back(l);
        while (v.console.size() > 5000) v.console.pop_front();
        if (v.consoleBack > 0) v.consoleBack += S(19) * (float)g_cfg.fontScale;   // stay where you scrolled to
    } else if (ev == L"status") {
        std::wstring st = e.GetStr(L"state");
        if (st == L"stopped" && !host::IsRunning(v.def.id)) st = L"stopped";
        v.state = st;
        v.message = e.GetStr(L"message");
        v.since = (long long)e.GetNum(L"since", 0);
        v.readyAt = (long long)e.GetNum(L"readyAt", 0);
        v.cpu = e.GetNum(L"cpu", 0);
        v.ram = (uint64_t)e.GetNum(L"ram", 0);
        v.ramLimit = (uint64_t)e.GetNum(L"ramLimit", 0);
        v.perf = e.GetNum(L"perf", -1);
        v.perfLabel = e.GetStr(L"perfLabel");
        v.perfMax = e.GetNum(L"perfMax", 0);
        v.maxPlayers = (int)e.GetNum(L"maxPlayers", 0);
        long long lb = (long long)e.GetNum(L"lastBackup", 0);
        if (lb > v.lastBackup) { v.lastBackup = lb; v.backupsLoaded = false; }
        v.nextBackup = (long long)e.GetNum(L"nextBackup", 0);
        v.nextRestart = (long long)e.GetNum(L"nextRestart", 0);
        v.sleepingSince = (long long)e.GetNum(L"sleepingSince", 0);
        v.backingUp = e.GetBool(L"backingUp", false);
        v.gaming = e.GetBool(L"gaming", false);
        v.upnp = e.GetStr(L"upnp");
        v.upnpError = e.GetStr(L"upnpError");
        v.hostVersion = e.GetStr(L"hostVersion");
        v.players.clear();
        if (const json::Value* pl = e.Get(L"players"); pl && pl->type == json::Value::Array)
            for (auto& p : pl->arr) {
                PlayerView pv;
                pv.name = p.GetStr(L"name"); pv.id = p.GetStr(L"id"); pv.since = (long long)p.GetNum(L"since", 0);
                pv.ping = (int)p.GetNum(L"ping", -1); pv.loc = p.Get(L"x") != nullptr; pv.x = p.GetNum(L"x", 0); pv.y = p.GetNum(L"y", 0);
                pv.level = (int)p.GetNum(L"level", 0);
                v.players.push_back(pv);
            }
        std::sort(v.players.begin(), v.players.end(), [](const PlayerView& a, const PlayerView& b) { return a.since < b.since; });
    } else if (ev == L"result") {
        if (!e.GetBool(L"ok", false)) Toast(e.GetStr(L"text"), true);
        v.listsLoaded = false;
    }
}

std::wstring ConnectAddress(const ServerView& v, bool local, bool masked) {
    std::wstring host;
    if (local) host = g_sys.localIp.empty() ? L"127.0.0.1" : g_sys.localIp;
    else if (!g_cfg.duckDomain.empty()) { host = g_cfg.duckDomain; if (host.find(L'.') == std::wstring::npos) host += L".duckdns.org"; masked = false; }
    else host = g_sys.publicIp.empty() ? L"(finding your address…)" : g_sys.publicIp;
    if (masked && !local && !g_sys.publicIp.empty()) host = L"\x2022\x2022\x2022.\x2022\x2022\x2022.\x2022\x2022.\x2022\x2022";
    int port = v.def.Port();
    // the game's default port can be left off for Minecraft Java
    bool dflt = (v.tmpl.id == L"minecraft-java" && port == 25565);
    return dflt || !port ? host : host + L":" + std::to_wstring(port);
}

// ================================================================ header
static const wchar_t* kTabs[] = {L"Overview", L"Console", L"Players", L"Mods", L"Backups", L"Settings"};

static float DrawHeader(ServerView& v, float x0) {
    float pad = D(28), top = D(22);
    float x = x0 + pad, right = g_W - pad;
    float tile = D(56);
    GameTile(v.tmpl, RW(x, top, tile, tile), F(20));
    float tx = x + tile + D(16);
    // buttons on the right
    float bh = D(36), by = top + (tile - bh) / 2;
    float bx = right;
    auto place = [&](const std::wstring& label, wchar_t g, Btn style, std::function<void()> fn, bool en = true) {
        float w = ButtonW(label, g);
        bx -= w;
        Button(RW(bx, by, w, bh), label, g, style, fn, en);
        bx -= D(8);
    };
    std::wstring id = v.def.id;
    if (v.job.active) {
        if (v.job.cancel) place(L"Cancel", icon::Cancel, Btn::Secondary, [id] { if (ServerView* s = FindServer(id)) if (s->job.cancel) *s->job.cancel = true; });
    } else if (v.Running()) {
        place(L"Stop", icon::Stop, Btn::Secondary, [id] { if (ServerView* s = FindServer(id)) StopServer(*s); }, v.state != L"stopping");
        if (v.state == L"sleeping") place(L"Wake up", icon::Play, Btn::Primary, [id] { if (ServerView* s = FindServer(id)) HostSend(*s, "{\"type\":\"wake\"}"); });
        else place(L"Restart", icon::Refresh, Btn::Secondary, [id] { if (ServerView* s = FindServer(id)) RestartServer(*s); }, v.state == L"running");
    } else {
        place(v.def.installedVersion.empty() ? L"Install and start" : L"Start", icon::Play, Btn::Primary, [id] { if (ServerView* s = FindServer(id)) StartServer(*s); });
    }
    // connect address
    bool masked = g_cfg.hidePublicIp;
    std::wstring addr = ConnectAddress(v, false, masked);
    float aw = std::min(D(300), std::max(D(180), TextW(addr, F(13), DWRITE_FONT_WEIGHT_SEMI_BOLD) + D(84)));
    float ax = bx - aw - D(8);
    if (ax > tx + D(260)) {
        D2D1_RECT_F ar = RW(ax, by, aw, bh);
        g_c.FillRound(ar, S(g_t.radius), g_t.surfaceHover);
        g_c.StrokeRound(ar, S(g_t.radius), g_t.divider);
        Text(addr, ar.left + D(12), ar.top, aw - D(84), F(13), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, bh);
        IconButton(RW(ar.right - D(68), ar.top + D(4), D(30), bh - D(8)), masked ? icon::Eye : icon::EyeOff, [] { g_cfg.hidePublicIp = !g_cfg.hidePublicIp; ConfigSave(); });
        std::wstring real = ConnectAddress(v, false, false);
        IconButton(RW(ar.right - D(36), ar.top + D(4), D(30), bh - D(8)), icon::Copy, [real] { SetClipboard(real); Toast(L"Address copied: send it to your friends."); });
        Text(L"Local: " + ConnectAddress(v, true, false), ar.left + D(2), ar.bottom + D(2), aw, F(11), g_t.faint);
    }
    // name, status, details
    float nameW = std::max(D(120), (ax > tx + D(260) ? ax : bx) - tx - D(16));
    float nw = Text(v.def.name, tx, top - D(2), nameW, F(21), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(34));
    std::wstring st = v.job.active ? L"installing" : v.state;
    if (tx + nw + D(12) < tx + nameW) Pill(tx + nw + D(12), top + D(5), StateLabel(st), StateColor(st));
    std::wstring details = v.tmpl.name;
    if (!v.def.variant.empty()) for (auto& var : v.tmpl.variants) if (var.id == v.def.variant) details += L" \x00B7 " + var.name;
    if (!v.def.installedVersion.empty()) details += L" \x00B7 " + v.def.installedVersion;
    if (v.state == L"running" && v.readyAt) details += L" \x00B7 up " + Duration(fs::UnixNow() - v.since);
    if (v.state == L"sleeping" && v.sleepingSince) details += L" \x00B7 asleep for " + Duration(fs::UnixNow() - v.sleepingSince) + L", wakes when someone joins";
    Text(details, tx, top + D(32), nameW, F(12.5f), g_t.subtext);
    float y = top + tile + D(14);
    if (v.job.active) {
        ProgressBar(RW(tx, y - D(6), nameW, S(5)), v.job.pct, g_t.accent);
        Text(v.job.text + (v.job.pct >= 0 ? L"  " + std::to_wstring(v.job.pct) + L"%" : L""), tx, y, nameW, F(12), g_t.subtext);
        y += D(22);
    }
    return y;
}

static float DrawTabs(float x0, float y) {
    float pad = D(28), x = x0 + pad - D(10);
    float h = D(38);
    for (int i = 0; i < TabCount; ++i) {
        float w = TextW(kTabs[i], F(13.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD) + D(22);
        D2D1_RECT_F r = RW(x, y, w, h);
        bool on = g_tab == i;
        if (Hover(r) && !on) g_c.FillRound(R(r.left, r.top + D(4), r.right, r.bottom - D(4)), S(g_t.radius), g_t.surfaceHover);
        Text(kTabs[i], x, y, w, F(13.5f), on ? g_t.text : g_t.subtext, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, h);
        if (on) g_c.FillRound(R(x + D(10), y + h - S(3), x + w - D(10), y + h), S(1.5f), g_t.accent);
        int t = i;
        Hot(r, [t] { SetTab(t); });
        x += w + D(2);
    }
    g_c.Line(x0 + pad, y + h, g_W - pad, y + h, g_t.divider);
    return y + h;
}

// ================================================================ shared bits
static float SectionTitle(const std::wstring& s, float x, float y, float w) {
    Text(s, x, y, w, F(15), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
    return y + D(34);
}

static float Banner(float x, float y, float w, D2D1_COLOR_F color, wchar_t glyph, const std::wstring& text, const std::wstring& action = L"", std::function<void()> fn = nullptr) {
    float aw = action.empty() ? 0 : ButtonW(action) + D(16);
    float th = MeasureHeight(text, Format(F(13), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), w - D(56) - aw);
    float h = std::max(D(48), th + D(24));
    D2D1_RECT_F r = RW(x, y, w, h);
    g_c.FillRound(r, S(g_t.radius + 2), Alpha(color, 0.12f));
    g_c.StrokeRound(r, S(g_t.radius + 2), Alpha(color, 0.35f));
    Glyph(glyph, x + D(22), y + h / 2, F(15), color);
    Para(text, x + D(42), y + (h - th) / 2, w - D(56) - aw, F(13), g_t.text);
    if (!action.empty()) Button(RW(r.right - aw, y + (h - D(32)) / 2, aw - D(10), D(32)), action, 0, Btn::Secondary, fn);
    return y + h + D(12);
}

static float Bar(float x, float y, float w, const std::wstring& label, const std::wstring& value, float frac, D2D1_COLOR_F color) {
    Text(label, x, y, w / 2, F(12.5f), g_t.subtext);
    Text(value, x + w / 2, y, w / 2, F(12.5f), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    float pct = frac < 0 ? -2.f : std::max(0.f, std::min(1.f, frac)) * 100.f;
    if (pct > -1) ProgressBar(RW(x, y + D(22), w, S(6)), (int)pct, color);
    else g_c.FillRound(RW(x, y + D(22), w, S(6)), S(3), g_t.surfaceStrong);
    return y + D(40);
}

// ================================================================ console
static D2D1_RECT_F g_consoleRect;
static std::wstring g_consoleServer;
bool ConsoleHit(float x, float y) { return g_page == Page::Server && In(g_consoleRect, x, y); }
void ConsoleWheel(float delta) {
    ServerView* v = FindServer(g_consoleServer);
    if (!v) return;
    v->consoleBack = std::max(0.f, v->consoleBack + delta);
    Invalidate();
}

static D2D1_COLOR_F LineColor(const std::wstring& k) {
    if (k == L"error") return g_t.bad;
    if (k == L"warn") return g_t.warn;
    if (k == L"chat") return g_t.info;
    if (k == L"join" || k == L"leave") return g_t.good;
    if (k == L"cmd") return g_t.accent;
    if (k == L"outpost") return g_t.subtext;
    return Mix(g_t.text, g_t.subtext, 0.25f);
}

static bool PassFilter(const ServerView& v, const ConsoleLine& l) {
    if (v.consoleFilter == L"chat") return l.kind == L"chat" || l.kind == L"join" || l.kind == L"leave";
    if (v.consoleFilter == L"problems") return l.kind == L"error" || l.kind == L"warn";
    return true;
}

static void DrawConsoleBox(ServerView& v, const D2D1_RECT_F& r, bool full) {
    g_c.FillRound(r, S(g_t.radius + 2), g_t.dark ? Rgb(0x000000, 0.28f) : Rgb(0xFFFFFF, 0.7f));
    g_c.StrokeRound(r, S(g_t.radius + 2), g_t.divider);
    g_consoleRect = r;
    g_consoleServer = v.def.id;
    float lh = S(19) * (float)g_cfg.fontScale;
    float fs = F(12.5f);
    IDWriteTextFormat* mono = MonoFormat(fs);
    D2D1_RECT_F inner = R(r.left + D(12), r.top + D(8), r.right - D(12), r.bottom - D(8));
    std::vector<const ConsoleLine*> lines;
    for (auto& l : v.console) if (PassFilter(v, l)) lines.push_back(&l);
    float total = lines.size() * lh, viewH = inner.bottom - inner.top;
    float maxBack = std::max(0.f, total - viewH);
    if (v.consoleBack > maxBack) v.consoleBack = maxBack;
    float bottom = inner.bottom + v.consoleBack;
    g_c.rt->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_ALIASED);
    float tsW = full ? MeasureWidth(L"00:00:00 ", mono) : 0;
    int first = std::max(0, (int)lines.size() - (int)((viewH + v.consoleBack) / lh) - 2);
    for (int i = first; i < (int)lines.size(); ++i) {
        float y = bottom - (lines.size() - i) * lh;
        if (y + lh < inner.top) continue;
        if (y > inner.bottom) break;
        const ConsoleLine& l = *lines[i];
        if (full && l.t) {
            time_t tt = (time_t)l.t; struct tm lt; localtime_s(&lt, &tt);
            wchar_t ts[16]; swprintf(ts, 16, L"%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
            g_c.Text(ts, mono, R(inner.left, y, inner.left + tsW, y + lh), g_t.faint);
        }
        g_c.Text(l.text, mono, R(inner.left + tsW, y, inner.right, y + lh), LineColor(l.kind));
    }
    if (lines.empty()) g_c.Text(v.Running() ? L"Waiting for the server…" : L"The console shows here when the server runs.", mono, R(inner.left, inner.top, inner.right, inner.top + lh), g_t.faint);
    g_c.rt->PopAxisAlignedClip();
    if (v.consoleBack > 1) {
        std::wstring id = v.def.id;
        float w = ButtonW(L"Jump to newest", icon::Down);
        Button(RW(r.right - w - D(16), r.bottom - D(44), w, D(32)), L"Jump to newest", icon::Down, Btn::Secondary, [id] { if (ServerView* s = FindServer(id)) s->consoleBack = 0; });
    }
    if (total > viewH) {
        float trackH = viewH;
        float h = std::max(S(24), trackH * viewH / total);
        float pos = (maxBack - v.consoleBack) / std::max(1.f, maxBack);
        float y = inner.top + (trackH - h) * pos;
        g_c.FillRound(R(r.right - S(6), y, r.right - S(3), y + h), S(1.5f), Alpha(g_t.subtext, 0.35f));
    }
}

static std::vector<std::wstring> Suggestions(const ServerView& v, const std::wstring& text) {
    std::vector<std::wstring> out;
    if (text.empty()) return out;
    size_t sp = text.rfind(L' ');
    if (sp == std::wstring::npos) {
        for (auto& a : v.tmpl.autocomplete) if (IStartsWith(a, text) && !IEquals(a, text)) out.push_back(a);
        for (auto it = v.cmdHistory.rbegin(); it != v.cmdHistory.rend() && out.size() < 8; ++it)
            if (IStartsWith(*it, text) && std::find(out.begin(), out.end(), *it) == out.end() && !IEquals(*it, text)) out.push_back(*it);
    } else {
        // complete player names in arguments
        std::wstring part = text.substr(sp + 1), head = text.substr(0, sp + 1);
        for (auto& p : v.players) if (IStartsWith(p.name, part) && !IEquals(p.name, part)) out.push_back(head + p.name);
    }
    if (out.size() > 6) out.resize(6);
    return out;
}

static void DrawCommandInput(ServerView& v, const D2D1_RECT_F& r) {
    std::wstring id = v.def.id, key = L"cmd:" + id;
    bool can = v.Running() && (v.tmpl.consoleInput || v.tmpl.HasCommand(L"say"));
    FieldOpts o;
    o.placeholder = !v.Running() ? L"Start the server to send commands" : v.tmpl.consoleInput ? L"Type a command (Tab completes, ↑ ↓ for history)"
                                 : v.tmpl.consoleNote.empty() ? L"Type a message to announce" : v.tmpl.consoleNote;
    o.key = [id](UINT vk, std::wstring& text) {
        ServerView* s = FindServer(id);
        if (!s) return false;
        if (vk == VK_RETURN) { std::wstring t = text; text.clear(); SendCommand(*s, t); return true; }
        if (vk == VK_TAB) { auto sug = Suggestions(*s, text); if (!sug.empty()) text = sug[0]; return true; }
        if (vk == VK_UP && !s->cmdHistory.empty()) {
            s->historyPos = s->historyPos < 0 ? (int)s->cmdHistory.size() - 1 : std::max(0, s->historyPos - 1);
            text = s->cmdHistory[s->historyPos]; return true;
        }
        if (vk == VK_DOWN && s->historyPos >= 0) {
            s->historyPos++;
            if (s->historyPos >= (int)s->cmdHistory.size()) { s->historyPos = -1; text.clear(); } else text = s->cmdHistory[s->historyPos];
            return true;
        }
        return false;
    };
    float bw = D(40);
    D2D1_RECT_F fr = R(r.left, r.top, r.right - bw - D(8), r.bottom);
    if (can) TextField(fr, key, L"", [](const std::wstring&) {}, o);
    else { g_c.FillRound(fr, S(g_t.radius), g_t.surfaceHover); Text(o.placeholder, fr.left + D(10), fr.top, fr.right - fr.left - D(20), F(13), g_t.faint, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, fr.bottom - fr.top); }
    Button(R(r.right - bw, r.top, r.right, r.bottom), L"", icon::Send, Btn::Primary, [id, key] {
        ServerView* s = FindServer(id);
        if (!s) return;
        if (Focused(key)) { std::wstring t = EditText(); EditText().clear(); SendCommand(*s, t); }
    }, can);
    // suggestions above the box
    if (Focused(key)) {
        auto sug = Suggestions(v, EditText());
        float rowH = D(28);
        float h = sug.size() * rowH + (sug.empty() ? 0 : D(8));
        if (!sug.empty()) {
            D2D1_RECT_F box = R(fr.left, fr.top - h - D(6), std::min(fr.right, fr.left + D(380)), fr.top - D(6));
            g_c.FillRound(box, S(g_t.radius + 2), g_t.dark ? Rgb(0x26262C) : Rgb(0xFFFFFF));
            g_c.StrokeRound(box, S(g_t.radius + 2), g_t.divider);
            for (size_t i = 0; i < sug.size(); ++i) {
                D2D1_RECT_F rr = R(box.left + D(4), box.top + D(4) + i * rowH, box.right - D(4), box.top + D(4) + (i + 1) * rowH);
                if (Hover(rr) || i == 0) g_c.FillRound(rr, S(g_t.radius - 1), i == 0 ? Alpha(g_t.accent, 0.16f) : g_t.surfaceHover);
                g_c.Text(sug[i], MonoFormat(F(12.5f)), R(rr.left + D(8), rr.top, rr.right, rr.bottom), g_t.text);
                if (i == 0) Text(L"Tab", rr.right - D(40), rr.top, D(34), F(11), g_t.faint, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, rowH);
                std::wstring s = sug[i];
                Hot(rr, [s] { EditText() = s; });
            }
        }
    }
}

// ================================================================ Overview
static void DrawOverview(ServerView& v, float x, float y0, float w, float bottom) {
    ScrollArea& sa = PageScroll();
    float y = BeginScroll(sa, R(x - D(4), y0, x + w + D(12), bottom));
    y += D(18);
    std::wstring id = v.def.id;
    if (v.state == L"error" && !v.message.empty()) y = Banner(x, y, w, g_t.bad, icon::Warning, v.message, L"Show console", [] { SetTab(TabConsole); });
    if (v.def.installedVersion.empty() && !v.job.active) y = Banner(x, y, w, g_t.info, icon::Download, L"The game's server files aren't downloaded yet.", L"Download", [id] { InstallJob(id, L"install"); });
    if (v.needsRestart && v.state == L"running") y = Banner(x, y, w, g_t.warn, icon::Refresh, L"Restart the server to use your new settings.", L"Restart", [id] { if (ServerView* s = FindServer(id)) RestartServer(*s); });
    if (v.state == L"running" && v.upnp == L"failed") y = Banner(x, y, w, g_t.warn, icon::Network, L"Your router didn't accept automatic port forwarding. " + v.upnpError, L"Network help", [] { SetTab(TabSettings); });
    if (v.gaming && v.state == L"running") y = Banner(x, y, w, g_t.info, icon::Game, L"You're playing a game: the server runs at lower priority and automatic backups wait until you're done.");
    if (!v.tmpl.notes.empty() && v.def.installedVersion.size() && v.console.size() < 3) y = Banner(x, y, w, g_t.info, icon::Info, v.tmpl.notes);

    // three cards
    float gap = D(14), cw = (w - gap * 2) / 3, ch = D(158);
    D2D1_RECT_F c1 = RW(x, y, cw, ch), c2 = RW(x + cw + gap, y, cw, ch), c3 = RW(x + (cw + gap) * 2, y, cw, ch);
    Card(c1); Card(c2); Card(c3);
    float p = D(18);
    // players
    Text(L"Players online", c1.left + p, c1.top + p, cw - p * 2, F(12.5f), g_t.subtext);
    std::wstring big = v.Running() && v.state != L"sleeping" ? std::to_wstring(v.players.size()) : L"\x2013";
    float bw = Text(big, c1.left + p, c1.top + p + D(22), cw / 2, F(34), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(46));
    if (v.maxPlayers > 0) Text(L"/ " + std::to_wstring(v.maxPlayers), c1.left + p + bw + D(6), c1.top + p + D(36), cw / 2, F(14), g_t.faint);
    float px = c1.left + p, py = c1.top + p + D(78);
    for (size_t i = 0; i < v.players.size() && i < 6; ++i) {
        float pw = std::min(cw - p * 2, TextW(v.players[i].name, F(12)) + D(20));
        if (px + pw > c1.right - p) { px = c1.left + p; py += D(28); if (py > c1.bottom - D(24)) break; }
        g_c.FillRound(RW(px, py, pw, D(24)), S(12), g_t.surfaceStrong);
        Text(v.players[i].name, px + D(10), py, pw - D(14), F(12), g_t.text, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(24));
        px += pw + D(6);
    }
    if (v.players.empty()) Text(v.state == L"running" ? L"Nobody's on right now" : v.state == L"sleeping" ? L"Asleep: joining wakes it up" : L"Start the server to play", c1.left + p, py, cw - p * 2, F(12.5f), g_t.faint);
    // resources
    Text(L"Resources", c2.left + p, c2.top + p, cw - p * 2, F(12.5f), g_t.subtext);
    float ry = c2.top + p + D(26), rw = cw - p * 2;
    bool live = v.state == L"running" || v.state == L"starting";
    wchar_t cpu[32]; swprintf(cpu, 32, L"%.0f%%", v.cpu);
    ry = Bar(c2.left + p, ry, rw, L"CPU", live ? cpu : L"\x2013", live ? (float)v.cpu / 100.f : -1, v.cpu > 85 ? g_t.warn : g_t.accent);
    double lim = v.ramLimit ? (double)v.ramLimit : (double)g_sys.ramTotal;
    ry = Bar(c2.left + p, ry, rw, L"Memory", live ? FormatBytes(v.ram) + (v.ramLimit ? L" of " + FormatBytes(v.ramLimit) : L"") : L"\x2013",
             live && lim > 0 ? (float)(v.ram / lim) : -1, v.ramLimit && v.ram > v.ramLimit * 0.92 ? g_t.warn : g_t.accent);
    if (!v.tmpl.perfLabel.empty()) {
        wchar_t pf[32]; swprintf(pf, 32, v.perf >= 100 ? L"%.0f" : L"%.1f", v.perf);
        float frac = v.perf >= 0 && v.perfMax > 0 ? (float)(v.perf / v.perfMax) : -1;
        Bar(c2.left + p, ry, rw, v.tmpl.perfLabel, live && v.perf >= 0 ? pf : L"\x2013", live ? frac : -1, frac >= 0 && frac < 0.75f ? g_t.warn : g_t.good);
    }
    // last backup
    Text(L"Last backup", c3.left + p, c3.top + p, cw - p * 2, F(12.5f), g_t.subtext);
    if (!v.backupsLoaded) LoadBackups(v);
    long long lb = v.lastBackup;
    if (!v.backups.empty()) lb = std::max(lb, v.backups[0].time);
    Text(lb ? Ago(lb) : L"None yet", c3.left + p, c3.top + p + D(22), cw - p * 2, F(20), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(32));
    std::wstring sub = !v.backups.empty() ? FormatBytes(v.backups[0].size) + L" \x00B7 " + std::to_wstring(v.backups.size()) + L" kept" : L"";
    if (v.state == L"running" && v.nextBackup) sub += (sub.empty() ? L"" : L" \x00B7 ") + std::wstring(L"next in ") + Duration(std::max(0LL, v.nextBackup - fs::UnixNow()));
    else if (v.def.backupMinutes <= 0) sub += (sub.empty() ? L"" : L" \x00B7 ") + std::wstring(L"automatic backups off");
    Text(sub, c3.left + p, c3.top + p + D(56), cw - p * 2, F(12), g_t.faint);
    float b1 = ButtonW(L"Back up now", icon::Save), b2 = ButtonW(L"Restore", icon::Restore);
    Button(RW(c3.left + p, c3.bottom - p - D(34), b1, D(34)), v.backingUp ? L"Backing up…" : L"Back up now", icon::Save, Btn::Secondary, [id] { if (ServerView* s = FindServer(id)) BackupNow(*s); }, !v.backingUp && !v.job.active);
    Button(RW(c3.left + p + b1 + D(8), c3.bottom - p - D(34), b2, D(34)), L"Restore", icon::Restore, Btn::Ghost, [] { SetTab(TabBackups); }, !v.job.active);
    y += ch + gap;
    // console + player list
    float lw = (w - gap) * 0.62f, rw2 = w - gap - lw, h2 = std::max(D(300), bottom - y - D(20) + sa.pos);
    h2 = std::min(h2, D(520));
    D2D1_RECT_F cc = RW(x, y, lw, h2), pc = RW(x + lw + gap, y, rw2, h2);
    Card(cc); Card(pc);
    Text(L"Live console", cc.left + p, cc.top + D(12), lw / 2, F(13), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float ow = ButtonW(L"Open", icon::Console);
    Button(RW(cc.right - p - ow, cc.top + D(8), ow, D(30)), L"Open", icon::Console, Btn::Ghost, [] { SetTab(TabConsole); });
    DrawConsoleBox(v, R(cc.left + D(10), cc.top + D(46), cc.right - D(10), cc.bottom - D(58)), false);
    DrawCommandInput(v, R(cc.left + D(10), cc.bottom - D(48), cc.right - D(10), cc.bottom - D(10)));
    Text(L"Players", pc.left + p, pc.top + D(12), rw2, F(13), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float ly = pc.top + D(48);
    for (auto& pl : v.players) {
        if (ly > pc.bottom - D(40)) break;
        D2D1_RECT_F row = R(pc.left + D(8), ly, pc.right - D(8), ly + D(40));
        if (Hover(row)) g_c.FillRound(row, S(g_t.radius), g_t.surfaceHover);
        g_c.Circle(row.left + D(20), ly + D(20), D(13), Rgb(v.tmpl.color, 0.85f));
        Text(pl.name.substr(0, 1), row.left + D(7), ly, D(26), F(12), Rgb(0xFFFFFF), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, D(40));
        Text(pl.name, row.left + D(42), ly + D(2), row.right - row.left - D(120), F(13), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(20));
        std::wstring meta = L"online " + Duration(fs::UnixNow() - pl.since) + (pl.ping >= 0 ? L" \x00B7 " + std::to_wstring(pl.ping) + L" ms" : L"");
        Text(meta, row.left + D(42), ly + D(20), row.right - row.left - D(120), F(11.5f), g_t.faint);
        if (v.tmpl.HasCommand(L"kick")) {
            std::wstring name = pl.name;
            float kw = ButtonW(L"Kick");
            Button(RW(row.right - kw - D(6), ly + D(5), kw, D(30)), L"Kick", 0, Btn::Ghost, [id, name] {
                Confirm(L"Kick " + name + L"?", name + L" can join again right away.", L"Kick", [id, name] { if (ServerView* s = FindServer(id)) PlayerAction(*s, L"kick", name); });
            });
        }
        ly += D(44);
    }
    if (v.players.empty()) Para(v.state == L"running" ? L"When friends join, they show up here." : L"Players show up here while the server runs.", pc.left + p, ly, rw2 - p * 2, F(12.5f), g_t.faint);
    EndScroll(sa, y + h2 + D(24));
}

// ================================================================ Console tab
static void DrawConsoleTab(ServerView& v, float x, float y, float w, float bottom) {
    y += D(16);
    std::wstring id = v.def.id;
    static const wchar_t* filters[] = {L"all", L"chat", L"problems"};
    int sel = v.consoleFilter == L"chat" ? 1 : v.consoleFilter == L"problems" ? 2 : 0;
    Segmented(x, y, {L"Everything", L"Chat and joins", L"Warnings and errors"}, sel, [id](int i) { if (ServerView* s = FindServer(id)) { s->consoleFilter = filters[i]; s->consoleBack = 0; } });
    float bx = x + w;
    auto btn = [&](const std::wstring& l, wchar_t g, std::function<void()> fn) { float bw = ButtonW(l, g); bx -= bw; Button(RW(bx, y, bw, D(32)), l, g, Btn::Ghost, fn); bx -= D(6); };
    btn(L"Log folder", icon::Folder, [id] { if (ServerView* s = FindServer(id)) OpenPath(s->def.root + L"\\logs"); });
    btn(L"Copy", icon::Copy, [id] {
        ServerView* s = FindServer(id);
        if (!s) return;
        std::wstring all;
        for (auto& l : s->console) if (PassFilter(*s, l)) all += l.text + L"\r\n";
        SetClipboard(all);
        Toast(L"Console copied.");
    });
    btn(L"Clear", icon::Delete, [id] { if (ServerView* s = FindServer(id)) { s->console.clear(); s->consoleBack = 0; } });
    y += D(44);
    DrawConsoleBox(v, R(x, y, x + w, bottom - D(66)), true);
    DrawCommandInput(v, R(x, bottom - D(54), x + w, bottom - D(14)));
}

// ================================================================ Players tab
static const wchar_t* kListNames[] = {L"whitelist", L"ops", L"bans"};
static void LoadLists(ServerView& v) {
    v.listsLoaded = true;
    std::wstring id = v.def.id;
    ServerDef d = v.def;
    Template t = v.tmpl;
    Async([id, d, t] {
        std::vector<std::wstring> l[3];
        for (int i = 0; i < 3; ++i) l[i] = ReadPlayerList(d, t, kListNames[i]);
        std::vector<HistoryItem> hist;
        std::wstring text;
        if (ReadTextFile(d.root + L"\\history.jsonl", text)) {
            size_t i = 0;
            while (i < text.size()) {
                size_t e = text.find(L'\n', i);
                if (e == std::wstring::npos) e = text.size();
                json::Value v;
                if (json::Parser(text.substr(i, e - i)).Parse(v)) hist.push_back({(long long)v.GetNum(L"t", 0), v.GetStr(L"ev"), v.GetStr(L"name")});
                i = e + 1;
            }
        }
        std::reverse(hist.begin(), hist.end());
        if (hist.size() > 200) hist.resize(200);
        Ui([id, l0 = l[0], l1 = l[1], l2 = l[2], hist] {
            ServerView* s = FindServer(id);
            if (!s) return;
            s->lists[0] = l0; s->lists[1] = l1; s->lists[2] = l2;
            s->history = hist;
            s->historyLoaded = true;
        });
    });
}

static void ListEdit(ServerView& v, int list, const std::wstring& name, bool add) {
    std::wstring id = v.def.id;
    static const wchar_t* addCmd[] = {L"whitelistAdd", L"op", L"ban"};
    static const wchar_t* remCmd[] = {L"whitelistRemove", L"deop", L"unban"};
    std::wstring action = add ? addCmd[list] : remCmd[list];
    if (v.state == L"running" && v.tmpl.HasCommand(action)) {
        PlayerAction(v, action, name);
        Async([id] { Sleep(2500); Ui([id] { if (ServerView* s = FindServer(id)) s->listsLoaded = false; }); });
        return;
    }
    // stopped (or no command for it): edit the file
    std::vector<std::wstring> names = v.lists[list];
    if (add) { if (std::find_if(names.begin(), names.end(), [&](const std::wstring& n) { return IEquals(n, name); }) == names.end()) names.push_back(name); }
    else names.erase(std::remove_if(names.begin(), names.end(), [&](const std::wstring& n) { return IEquals(n, name); }), names.end());
    v.lists[list] = names;
    ServerDef d = v.def;
    Template t = v.tmpl;
    Async([id, d, t, list, names] {
        bool ok = WritePlayerList(d, t, kListNames[list], names);
        Ui([id, ok] { if (!ok) Toast(L"Couldn't save the list.", true); if (ServerView* s = FindServer(id)) s->listsLoaded = false; });
    });
    if (v.Running()) Toast(L"Saved. The server picks it up after a restart.");
}

static void DrawPlayers(ServerView& v, float x, float y0, float w, float bottom) {
    ScrollArea& sa = PageScroll();
    float y = BeginScroll(sa, R(x - D(4), y0, x + w + D(12), bottom)) + D(18);
    std::wstring id = v.def.id;
    if (!v.listsLoaded) LoadLists(v);
    if (v.state == L"running" && v.tmpl.Obj(L"query") && v.tmpl.Obj(L"query")->GetStr(L"type") == L"stdin") {
        float rb = ButtonW(L"Refresh", icon::Refresh);
        Button(RW(x + w - rb, y, rb, D(32)), L"Refresh", icon::Refresh, Btn::Ghost, [id] { if (ServerView* s = FindServer(id)) HostSend(*s, "{\"type\":\"players\"}"); });
    }
    y = SectionTitle(L"Online now" + std::wstring(v.Running() ? L" (" + std::to_wstring(v.players.size()) + L")" : L""), x, y, w);
    if (v.players.empty()) { Text(v.Running() ? L"Nobody's online." : L"The server is stopped.", x, y, w, F(13), g_t.faint); y += D(36); }
    for (auto& pl : v.players) {
        D2D1_RECT_F row = RW(x, y, w, D(54));
        Card(row, Hover(row));
        g_c.Circle(x + D(28), y + D(27), D(16), Rgb(v.tmpl.color, 0.85f));
        Text(pl.name.substr(0, 1), x + D(12), y, D(32), F(13), Rgb(0xFFFFFF), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, D(54));
        Text(pl.name, x + D(56), y + D(8), w / 2, F(14), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(20));
        std::wstring meta = L"Online for " + Duration(fs::UnixNow() - pl.since);
        if (pl.ping >= 0) meta += L" \x00B7 " + std::to_wstring(pl.ping) + L" ms ping";
        if (pl.level) meta += L" \x00B7 level " + std::to_wstring(pl.level);
        if (pl.loc) { wchar_t b[64]; swprintf(b, 64, L" \x00B7 at %.0f, %.0f", pl.x, pl.y); meta += b; }
        Text(meta, x + D(56), y + D(28), w / 2, F(12), g_t.faint);
        float bx = x + w - D(12);
        std::wstring name = pl.name;
        auto act = [&](const std::wstring& label, const std::wstring& action, Btn style, bool confirm) {
            if (!v.tmpl.HasCommand(action)) return;
            float bw = ButtonW(label);
            bx -= bw;
            Button(RW(bx, y + D(11), bw, D(32)), label, 0, style, [id, name, action, label, confirm] {
                auto run = [id, name, action] { if (ServerView* s = FindServer(id)) PlayerAction(*s, action, name); };
                if (confirm) Confirm(label + L" " + name + L"?", action == L"ban" ? name + L" won't be able to join until you unban them." : name + L" can join again right away.", label, run, action == L"ban");
                else run();
            });
            bx -= D(6);
        };
        act(L"Ban", L"ban", Btn::Ghost, true);
        act(L"Kick", L"kick", Btn::Ghost, true);
        act(L"Make admin", L"op", Btn::Ghost, false);
        y += D(62);
    }
    // message everyone
    if (v.tmpl.HasCommand(L"say") && v.state == L"running") {
        y += D(6);
        std::wstring key = L"say:" + id;
        FieldOpts o; o.placeholder = L"Message everyone on the server";
        o.key = [id](UINT vk, std::wstring& t) { if (vk == VK_RETURN && !t.empty()) { if (ServerView* s = FindServer(id)) PlayerAction(*s, L"say", L"", t); t.clear(); Toast(L"Message sent."); return true; } return false; };
        TextField(RW(x, y, w - D(100), D(38)), key, L"", [](const std::wstring&) {}, o);
        Button(RW(x + w - D(92), y, D(92), D(38)), L"Send", icon::Send, Btn::Primary, [id, key] {
            if (Focused(key) && !EditText().empty()) { if (ServerView* s = FindServer(id)) PlayerAction(*s, L"say", L"", EditText()); EditText().clear(); Toast(L"Message sent."); }
        });
        y += D(54);
    }
    // whitelist / admins / bans
    const json::Value* pdefs = v.tmpl.Obj(L"players");
    static const wchar_t* titles[] = {L"Whitelist", L"Admins", L"Banned"};
    static const wchar_t* blurbs[] = {L"Only these players can join when the whitelist is on.", L"Admins can use commands in the game.", L"These players can't join."};
    int shown = 0;
    float gap = D(14);
    std::vector<int> which;
    for (int i = 0; i < 3; ++i) if ((pdefs && pdefs->Get(kListNames[i])) || (i == 2 && v.tmpl.HasCommand(L"ban")) || (i == 1 && v.tmpl.HasCommand(L"op"))) which.push_back(i);
    if (!which.empty()) {
        y += D(8);
        y = SectionTitle(L"Lists", x, y, w);
        float cw = (w - gap * (which.size() - 1)) / which.size();
        float maxH = 0;
        for (int i : which) {
            float cx = x + shown++ * (cw + gap);
            const json::Value* ld = pdefs ? pdefs->Get(kListNames[i]) : nullptr;
            std::wstring title = ld && !ld->GetStr(L"label").empty() ? ld->GetStr(L"label") : titles[i];
            float cy = y, p = D(16);
            float h = D(150) + std::max<size_t>(1, v.lists[i].size()) * D(34);
            Card(RW(cx, cy, cw, h));
            Text(title, cx + p, cy + p, cw - p * 2, F(13.5f), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            float bh = Para(blurbs[i], cx + p, cy + p + D(24), cw - p * 2, F(12), g_t.faint);
            float ly = cy + p + D(30) + bh;
            if (i == 0 && v.tmpl.Setting(L"whitelist")) {
                bool on = v.def.Get(L"whitelist") == v.tmpl.Setting(L"whitelist")->valOn;
                Text(L"Whitelist on", cx + p, ly, cw / 2, F(12.5f), g_t.subtext, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, S(22));
                Toggle(cx + cw - p - S(40), ly, on, [id](bool nv) {
                    ServerView* s = FindServer(id);
                    if (!s) return;
                    s->def.settings[L"whitelist"] = nv ? s->tmpl.Setting(L"whitelist")->valOn : s->tmpl.Setting(L"whitelist")->valOff;
                    SaveServer(*s);
                    if (s->state == L"running") { SendCommand(*s, nv ? L"whitelist on" : L"whitelist off"); }
                });
                ly += D(32);
            }
            std::wstring key = L"list:" + id + L":" + std::to_wstring(i);
            FieldOpts o; o.placeholder = ld && ld->GetStr(L"format") == L"lines" ? L"Steam ID or name" : L"Player name";
            int li = i;
            o.key = [id, li](UINT vk, std::wstring& t) { if (vk == VK_RETURN && !Trim(t).empty()) { if (ServerView* s = FindServer(id)) ListEdit(*s, li, Trim(t), true); t.clear(); return true; } return false; };
            TextField(RW(cx + p, ly, cw - p * 2 - D(48), D(34)), key, L"", [](const std::wstring&) {}, o);
            Button(RW(cx + cw - p - D(40), ly, D(40), D(34)), L"", icon::Add, Btn::Secondary, [id, li, key] {
                if (Focused(key) && !Trim(EditText()).empty()) { if (ServerView* s = FindServer(id)) ListEdit(*s, li, Trim(EditText()), true); EditText().clear(); }
            });
            ly += D(44);
            for (auto& n : v.lists[i]) {
                D2D1_RECT_F row = RW(cx + p - D(6), ly, cw - p * 2 + D(12), D(32));
                if (Hover(row)) g_c.FillRound(row, S(g_t.radius), g_t.surfaceHover);
                Text(n, row.left + D(8), ly, cw - p * 2 - D(40), F(13), g_t.text, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(32));
                std::wstring nm = n;
                IconButton(RW(row.right - D(32), ly + D(2), D(28), D(28)), icon::Cancel, [id, li, nm] { if (ServerView* s = FindServer(id)) ListEdit(*s, li, nm, false); });
                ly += D(34);
            }
            if (v.lists[i].empty()) { Text(L"Empty", cx + p, ly, cw, F(12.5f), g_t.faint); ly += D(30); }
            maxH = std::max(maxH, ly - cy + D(10));
        }
        y += maxH + D(20);
    }
    // history
    y = SectionTitle(L"Recent joins and leaves", x, y, w);
    if (v.history.empty()) { Text(L"No one has joined yet.", x, y, w, F(13), g_t.faint); y += D(30); }
    int n = 0;
    for (auto& h : v.history) {
        if (++n > 60) break;
        D2D1_COLOR_F c = h.ev == L"join" ? g_t.good : g_t.faint;
        g_c.Circle(x + D(8), y + D(13), D(4), c);
        Text(h.name, x + D(22), y, w * 0.4f, F(13), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(26));
        Text(h.ev == L"join" ? L"joined" : L"left", x + w * 0.42f, y, w * 0.2f, F(12.5f), g_t.subtext, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(26));
        Text(ShortTime(h.t), x + w * 0.6f, y, w * 0.4f, F(12.5f), g_t.faint, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, D(26));
        y += D(28);
    }
    EndScroll(sa, y + D(20));
}

// ================================================================ Mods tab
static void LoadMods(ServerView& v) {
    v.modsLoaded = true;
    std::wstring id = v.def.id;
    ServerDef d = v.def;
    Template t = v.tmpl;
    Async([id, d, t] {
        auto list = mods::List(d, t);
        Ui([id, list] { if (ServerView* s = FindServer(id)) s->modFiles = list; });
    });
}
static void SearchMods(ServerView& v) {
    v.modBusy = true; v.modError.clear(); v.modSearched = true;
    std::wstring id = v.def.id, q = v.modQuery, src = v.modSource;
    ServerDef d = v.def;
    Template t = v.tmpl;
    Async([id, d, t, q, src] {
        std::wstring err;
        auto hits = mods::Search(d, t, q, src, &err);
        Ui([id, hits, err] { if (ServerView* s = FindServer(id)) { s->modHits = hits; s->modError = err; s->modBusy = false; } });
    });
}

static void DrawMods(ServerView& v, float x, float y0, float w, float bottom) {
    ScrollArea& sa = PageScroll();
    float y = BeginScroll(sa, R(x - D(4), y0, x + w + D(12), bottom)) + D(18);
    std::wstring id = v.def.id;
    mods::Target tg = mods::TargetFor(v.def, v.tmpl);
    const json::Value* mj = v.tmpl.Obj(L"mods");
    if (v.tmpl.modsType == L"none" || (v.tmpl.modsType == L"modrinth" && !tg.ok)) {
        std::wstring msg = v.tmpl.modsType == L"modrinth"
            ? L"Vanilla Minecraft doesn't load mods or plugins. Create a server with Paper (plugins) or Fabric, Forge or NeoForge (mods), and Outpost installs them from Modrinth for you."
            : v.tmpl.name + L" doesn't support server mods through Outpost.";
        y = Banner(x, y, w, g_t.info, icon::Puzzle, msg);
        EndScroll(sa, y);
        return;
    }
    if (v.tmpl.modsType == L"settings") {
        y = Banner(x, y, w, g_t.info, icon::Puzzle, v.tmpl.name + L" loads mods from Steam Workshop or CurseForge by ID. Add them in Settings: they download when the server starts.", L"Open settings", [] { SetTab(TabSettings); });
        EndScroll(sa, y);
        return;
    }
    if (!v.modsLoaded) LoadMods(v);
    std::wstring folder = mods::FolderFor(v.def, v.tmpl);
    if (mj && !mj->GetStr(L"note").empty()) y = Banner(x, y, w, g_t.info, icon::Info, mj->GetStr(L"note"));
    if (v.Running()) y = Banner(x, y, w, g_t.warn, icon::Info, L"Changes to mods take effect after a restart.");
    // installed
    bool isPlugin = tg.kind == L"plugin";
    std::wstring noun = isPlugin ? L"plugins" : L"mods";
    float tw = Text(L"Installed " + noun + L" (" + std::to_wstring(v.modFiles.size()) + L")", x, y, w, F(15), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
    (void)tw;
    float bx = x + w;
    auto btn = [&](const std::wstring& l, wchar_t g, std::function<void()> fn, bool en = true) { float bw = ButtonW(l, g); bx -= bw; Button(RW(bx, y - D(2), bw, D(32)), l, g, Btn::Ghost, fn, en); bx -= D(6); };
    btn(L"Open folder", icon::Folder, [folder] { OpenPath(folder); });
    btn(L"Add file", icon::Add, [id] {
        std::wstring f = PickFile(L"Add a mod file", L"*.jar;*.dll;*.pak;*.zip");
        if (f.empty()) return;
        ServerView* s = FindServer(id);
        std::wstring err;
        if (s && mods::AddFile(s->def, s->tmpl, f, &err)) { Toast(L"Added " + PathName(f)); s->modsLoaded = false; }
        else Toast(err, true);
    });
    if (v.tmpl.modsType == L"modrinth") {
        btn(v.modUpdates.empty() ? L"Check for updates" : L"Update all (" + std::to_wstring(v.modUpdates.size()) + L")", icon::Update, [id] {
            ServerView* s = FindServer(id);
            if (!s) return;
            ServerDef d = s->def;
            Template t = s->tmpl;
            auto updates = s->modUpdates;
            s->modBusy = true;
            Async([id, d, t, updates]() mutable {
                std::wstring err;
                if (updates.empty()) {
                    auto found = mods::CheckUpdates(d, t, &err);
                    Ui([id, found, err] { if (ServerView* s2 = FindServer(id)) { s2->modBusy = false; s2->modUpdates = found; Toast(found.empty() ? (err.empty() ? L"Everything is up to date." : err) : std::to_wstring(found.size()) + L" updates available.", !err.empty()); } });
                    return;
                }
                int done = 0;
                for (auto& u : updates) if (mods::Update(d, t, u.first, u.second, &err)) ++done;
                Ui([id, d, done, err] { if (ServerView* s2 = FindServer(id)) { s2->def = d; s2->modBusy = false; s2->modUpdates.clear(); s2->modsLoaded = false; Toast(std::to_wstring(done) + L" updated." + (err.empty() ? L"" : L" " + err), !err.empty()); } });
            });
        }, !v.modBusy);
    }
    y += D(40);
    if (v.modFiles.empty()) { Text(L"No " + noun + L" yet." + std::wstring(v.tmpl.modsType == L"modrinth" ? L" Search below to add some." : L" Use Add file, or put files in the folder."), x, y, w, F(13), g_t.faint); y += D(34); }
    for (auto& m : v.modFiles) {
        D2D1_RECT_F row = RW(x, y, w, D(50));
        Card(row, Hover(row));
        Toggle(x + D(14), y + D(14), m.enabled, [id, file = m.file](bool on) {
            ServerView* s = FindServer(id);
            std::wstring err;
            if (s && !mods::SetEnabled(s->def, s->tmpl, file, on, &err)) Toast(err, true);
            if (s) s->modsLoaded = false;
        });
        Text(m.title, x + D(68), y + D(6), w * 0.55f, F(13.5f), m.enabled ? g_t.text : g_t.faint, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(20));
        std::wstring meta = (m.version.empty() ? m.file : m.version + L" \x00B7 " + m.file) + L" \x00B7 " + FormatBytes(m.size) + (m.enabled ? L"" : L" \x00B7 disabled");
        Text(meta, x + D(68), y + D(26), w * 0.6f, F(11.5f), g_t.faint);
        float rx = x + w - D(12);
        rx -= D(32);
        std::wstring file = m.file;
        IconButton(RW(rx, y + D(10), D(30), D(30)), icon::Delete, [id, file] {
            Confirm(L"Remove " + file + L"?", L"The file is deleted from the server's folder.", L"Remove", [id, file] {
                ServerView* s = FindServer(id);
                std::wstring err;
                if (s && !mods::Remove(s->def, s->tmpl, file, &err)) Toast(err, true);
                if (s) s->modsLoaded = false;
            }, true);
        });
        for (auto& u : v.modUpdates) if (u.first == m.index) {
            float uw = ButtonW(L"Update to " + u.second.number);
            rx -= uw + D(8);
            int idx = u.first;
            ModVersion mv = u.second;
            Button(RW(rx, y + D(9), uw, D(32)), L"Update to " + u.second.number, 0, Btn::Primary, [id, idx, mv] {
                ServerView* s = FindServer(id);
                if (!s) return;
                ServerDef d = s->def; Template t = s->tmpl;
                Async([id, d, t, idx, mv]() mutable {
                    std::wstring err;
                    bool ok = mods::Update(d, t, idx, mv, &err);
                    Ui([id, d, ok, err, idx] { if (ServerView* s2 = FindServer(id)) { if (ok) s2->def = d; s2->modUpdates.erase(std::remove_if(s2->modUpdates.begin(), s2->modUpdates.end(), [&](auto& p) { return p.first == idx; }), s2->modUpdates.end()); s2->modsLoaded = false; Toast(ok ? L"Updated." : err, !ok); } });
                });
            });
        }
        y += D(58);
    }
    // browse
    if (v.tmpl.modsType == L"modrinth") {
        y += D(12);
        y = SectionTitle(L"Find " + noun + L" for " + (tg.loader.empty() ? L"" : std::wstring(1, (wchar_t)towupper(tg.loader[0])) + tg.loader.substr(1)) + L" " + tg.mc, x, y, w);
        std::wstring key = L"modq:" + id;
        FieldOpts o; o.placeholder = L"Search " + std::wstring(v.modSource == L"curseforge" ? L"CurseForge" : L"Modrinth") + L" (leave empty for the most popular)";
        o.key = [id](UINT vk, std::wstring& t) { if (vk == VK_RETURN) { if (ServerView* s = FindServer(id)) { s->modQuery = t; SearchMods(*s); } return true; } return false; };
        float segW = Segmented(x + w - D(250), y, {L"Modrinth", L"CurseForge"}, v.modSource == L"curseforge" ? 1 : 0, [id](int i) {
            if (ServerView* s = FindServer(id)) { s->modSource = i ? L"curseforge" : L"modrinth"; SearchMods(*s); }
        });
        (void)segW;
        TextField(RW(x, y, w - D(370), D(36)), key, v.modQuery, [id](const std::wstring& t) { if (ServerView* s = FindServer(id)) s->modQuery = t; }, o);
        Button(RW(x + w - D(362), y, D(100), D(36)), L"Search", icon::Search, Btn::Primary, [id, key] {
            ServerView* s = FindServer(id);
            if (!s) return;
            if (Focused(key)) s->modQuery = EditText();
            SearchMods(*s);
        }, !v.modBusy);
        y += D(50);
        if (!v.modSearched && !v.modBusy) SearchMods(v);
        if (v.modBusy) { ProgressBar(RW(x, y, w, S(4)), -1, g_t.accent); y += D(20); }
        if (!v.modError.empty()) y = Banner(x, y, w, g_t.warn, icon::Warning, v.modError);
        for (auto& h : v.modHits) {
            float th = D(68);
            D2D1_RECT_F row = RW(x, y, w, th);
            if (!Visible(row.top, row.bottom)) { y += th + D(8); continue; }
            Card(row, Hover(row));
            bool installed = false;
            for (auto& m : v.def.mods) if (m.projectId == h.projectId) installed = true;
            Text(h.title, x + D(16), y + D(10), w * 0.6f, F(14), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(22));
            wchar_t dl[32];
            if (h.downloads >= 1e6) swprintf(dl, 32, L"%.1fM", h.downloads / 1e6); else if (h.downloads >= 1e3) swprintf(dl, 32, L"%.0fK", h.downloads / 1e3); else swprintf(dl, 32, L"%.0f", h.downloads);
            Text(L"by " + h.author + L" \x00B7 " + dl + L" downloads", x + D(16) + std::min(w * 0.6f, TextW(h.title, F(14), DWRITE_FONT_WEIGHT_SEMI_BOLD)) + D(10), y + D(12), w * 0.35f, F(11.5f), g_t.faint);
            Text(h.description, x + D(16), y + D(34), w - D(160), F(12.5f), g_t.subtext);
            float iw = ButtonW(installed ? L"Installed" : L"Install", installed ? icon::Check : icon::Download);
            ModHit hit = h;
            Button(RW(x + w - iw - D(16), y + (th - D(34)) / 2, iw, D(34)), installed ? L"Installed" : L"Install", installed ? icon::Check : icon::Download,
                   installed ? Btn::Ghost : Btn::Secondary, [id, hit] {
                ServerView* s = FindServer(id);
                if (!s) return;
                ServerDef d = s->def; Template t = s->tmpl;
                s->modBusy = true;
                Toast(L"Installing " + hit.title + L"…");
                Async([id, d, t, hit]() mutable {
                    std::wstring err;
                    auto vs = mods::Versions(d, t, hit, &err);
                    bool ok = !vs.empty() && mods::Install(d, t, hit, vs[0], &err);
                    std::wstring ver = vs.empty() ? L"" : vs[0].number;
                    Ui([id, d, ok, err, hit, ver] {
                        ServerView* s2 = FindServer(id);
                        if (!s2) return;
                        s2->modBusy = false;
                        if (ok) { s2->def = d; s2->modsLoaded = false; Toast(L"Installed " + hit.title + L" " + ver + (s2->Running() ? L". Restart to load it." : L"")); }
                        else Toast(err, true);
                    });
                });
            }, !installed && !v.modBusy);
            y += th + D(8);
        }
    }
    EndScroll(sa, y + D(20));
}

// ================================================================ Backups tab
static void DrawBackups(ServerView& v, float x, float y0, float w, float bottom) {
    ScrollArea& sa = PageScroll();
    float y = BeginScroll(sa, R(x - D(4), y0, x + w + D(12), bottom)) + D(18);
    std::wstring id = v.def.id;
    if (!v.backupsLoaded) LoadBackups(v);
    float p = D(18);
    D2D1_RECT_F card = RW(x, y, w, D(210));
    Card(card);
    float cy = y + p;
    Text(L"Automatic backups", x + p, cy, w / 2, F(14), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float bw = ButtonW(L"Back up now", icon::Save);
    Button(RW(card.right - p - bw, cy - D(4), bw, D(34)), v.backingUp || (v.job.active && v.job.kind == L"backup") ? L"Backing up…" : L"Back up now", icon::Save, Btn::Primary,
           [id] { if (ServerView* s = FindServer(id)) BackupNow(*s); }, !v.backingUp && !v.job.active);
    cy += D(40);
    static const int mins[] = {0, 15, 30, 60, 120, 360};
    int sel = 2;
    for (int i = 0; i < 6; ++i) if (mins[i] == v.def.backupMinutes) sel = i;
    Text(L"While running, every", x + p, cy, D(170), F(13), g_t.subtext, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(32));
    Segmented(x + p + D(170), cy, {L"Off", L"15 min", L"30 min", L"1 hour", L"2 hours", L"6 hours"}, sel, [id](int i) {
        if (ServerView* s = FindServer(id)) { s->def.backupMinutes = mins[i]; SaveServer(*s); }
    });
    cy += D(44);
    static const int keeps[] = {3, 5, 10, 20, 50, 100};
    int ks = 2;
    for (int i = 0; i < 6; ++i) if (keeps[i] == v.def.keepBackups) ks = i;
    Text(L"Keep the newest", x + p, cy, D(170), F(13), g_t.subtext, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(34));
    Dropdown(RW(x + p + D(170), cy, D(140), D(34)), {L"3 backups", L"5 backups", L"10 backups", L"20 backups", L"50 backups", L"100 backups"}, ks, [id](int i) {
        if (ServerView* s = FindServer(id)) { s->def.keepBackups = keeps[i]; SaveServer(*s); Async([d = s->def] { backup::Prune(d); }); s->backupsLoaded = false; }
    });
    cy += D(46);
    Text(L"Save to", x + p, cy, D(170), F(13), g_t.subtext, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(34));
    std::wstring dir = backup::Dir(v.def);
    float cw = ButtonW(L"Change…"), rw = v.def.backupDir.empty() ? 0 : ButtonW(L"Default");
    Text(dir, x + p + D(170), cy, w - p * 2 - D(170) - cw - rw - D(30), F(12.5f), g_t.text, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(34));
    Button(RW(card.right - p - cw - rw - (rw ? D(8) : 0), cy, cw, D(34)), L"Change…", 0, Btn::Secondary, [id] {
        std::wstring f = PickFolder(L"Where should backups go? (an external drive or your OneDrive folder works well)");
        if (f.empty()) return;
        if (ServerView* s = FindServer(id)) { s->def.backupDir = f; SaveServer(*s); s->backupsLoaded = false; }
    });
    if (rw) Button(RW(card.right - p - rw, cy, rw, D(34)), L"Default", 0, Btn::Ghost, [id] { if (ServerView* s = FindServer(id)) { s->def.backupDir.clear(); SaveServer(*s); s->backupsLoaded = false; } });
    y += D(210) + D(20);
    if (v.def.lowerPriorityWhileGaming) { Text(L"Automatic backups wait while you're playing a full-screen game on this PC, and skip when nobody has played since the last one.", x, y - D(10), w, F(12), g_t.faint); y += D(18); }
    // list
    float tw = Text(L"Backups (" + std::to_wstring(v.backups.size()) + L")", x, y, w, F(15), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(28));
    (void)tw;
    float ow = ButtonW(L"Open folder", icon::Folder), fw = ButtonW(L"Restore from file…", icon::Upload);
    Button(RW(x + w - ow, y - D(2), ow, D(32)), L"Open folder", icon::Folder, Btn::Ghost, [dir] { OpenPath(dir); });
    Button(RW(x + w - ow - fw - D(6), y - D(2), fw, D(32)), L"Restore from file…", icon::Upload, Btn::Ghost, [id] {
        std::wstring f = PickFile(L"Choose an Outpost backup", L"*.zip");
        if (f.empty()) return;
        BackupInfo b; b.path = f; b.time = fs::UnixNow();
        if (ServerView* s = FindServer(id)) RestoreBackup(*s, b);
    }, !v.job.active);
    y += D(40);
    if (v.backups.empty()) { Text(L"No backups yet.", x, y, w, F(13), g_t.faint); y += D(30); }
    uint64_t total = 0;
    for (auto& b : v.backups) total += b.size;
    for (auto& b : v.backups) {
        D2D1_RECT_F row = RW(x, y, w, D(52));
        if (!Visible(row.top, row.bottom)) { y += D(58); continue; }
        Card(row, Hover(row));
        Glyph(b.label.find(L"before") != std::wstring::npos ? icon::Shield : b.label == L"automatic" ? icon::Clock : icon::Save, x + D(24), y + D(26), F(14), g_t.subtext);
        Text(ShortTime(b.time), x + D(48), y + D(7), w * 0.4f, F(13.5f), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(20));
        std::wstring label = b.label;
        if (!label.empty()) label[0] = towupper(label[0]);
        Text(label + L" \x00B7 " + FormatBytes(b.size) + L" \x00B7 " + Ago(b.time), x + D(48), y + D(27), w * 0.5f, F(12), g_t.faint);
        float bx = x + w - D(12);
        BackupInfo bi = b;
        bx -= D(32);
        IconButton(RW(bx, y + D(11), D(30), D(30)), icon::Delete, [id, bi] {
            Confirm(L"Delete this backup?", L"The backup from " + ShortTime(bi.time) + L" is deleted for good.", L"Delete", [id, bi] {
                DeleteFileW(bi.path.c_str());
                if (ServerView* s = FindServer(id)) LoadBackups(*s);
            }, true);
        });
        bx -= D(34);
        IconButton(RW(bx, y + D(11), D(30), D(30)), icon::Folder, [bi] { std::wstring a = L"/select,\"" + bi.path + L"\""; ShellExecuteW(nullptr, L"open", L"explorer.exe", a.c_str(), nullptr, SW_SHOWNORMAL); });
        float rw2 = ButtonW(L"Restore", icon::Restore);
        bx -= rw2 + D(8);
        Button(RW(bx, y + D(9), rw2, D(34)), L"Restore", icon::Restore, Btn::Secondary, [id, bi] { if (ServerView* s = FindServer(id)) RestoreBackup(*s, bi); }, !v.job.active);
        y += D(58);
    }
    if (!v.backups.empty()) { Text(L"Total " + FormatBytes(total), x, y, w, F(12), g_t.faint); y += D(24); }
    EndScroll(sa, y + D(20));
}

// ================================================================ Settings tab
static float Row(float x, float y, float w, const std::wstring& label, const std::wstring& help, float controlW, float* cx, float rowMinH = 0) {
    float lw = w - controlW - D(24);
    Text(label, x, y, lw, F(13.5f), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(22));
    float h = D(22);
    if (!help.empty()) h += Para(help, x, y + D(22), lw, F(12), g_t.faint) + D(2);
    *cx = x + w - controlW;
    return y + std::max(std::max(h, D(36)), rowMinH) + D(14);
}

static bool SettingNeedsRestart(const ServerView& v) { return v.state == L"running" || v.state == L"starting"; }

static float SettingControl(ServerView& v, const TSetting& st, float x, float y, float w) {
    std::wstring id = v.def.id, sid = st.id;
    std::wstring val = v.def.Get(st.id);
    float cx;
    float cw = st.type == L"bool" ? S(40) : st.type == L"number" ? D(140) : D(260);
    float ny = Row(x, y, w, st.label, st.help, cw, &cx);
    auto set = [id, sid](const std::wstring& nv) {
        ServerView* s = FindServer(id);
        if (!s || s->def.Get(sid) == nv) return;
        s->def.settings[sid] = nv;
        SaveServer(*s, false);
        if (SettingNeedsRestart(*s)) s->needsRestart = true;
    };
    if (st.type == L"bool") Toggle(cx, y, val == st.valOn, [set, on = st.valOn, off = st.valOff](bool b) { set(b ? on : off); });
    else if (st.type == L"choice") {
        std::vector<std::wstring> labels;
        int sel = 0;
        for (size_t i = 0; i < st.options.size(); ++i) { labels.push_back(st.options[i].label); if (st.options[i].value == val) sel = (int)i; }
        std::vector<TOption> opts = st.options;
        Dropdown(RW(cx, y - D(2), cw, D(36)), labels, sel, [set, opts](int i) { set(opts[i].value); });
    } else {
        FieldOpts o;
        o.password = st.type == L"password";
        o.numeric = st.type == L"number";
        o.placeholder = st.type == L"seed" ? L"Random" : L"";
        double lo = st.min, hi = st.max;
        TextField(RW(cx, y - D(2), cw, D(36)), L"set:" + id + L":" + sid, val, [set, lo, hi, label = st.label, num = o.numeric](const std::wstring& nv) {
            if (num && hi > lo) {
                double d = _wtof(nv.c_str());
                if (d < lo || d > hi) { Toast(label + L" must be between " + std::to_wstring((long long)lo) + L" and " + std::to_wstring((long long)hi) + L".", true); return; }
            }
            set(Trim(nv));
        }, o);
    }
    return ny;
}

static void Diagnose(ServerView& v) {
    v.netBusy = true;
    std::wstring id = v.def.id;
    ServerDef d = v.def; Template t = v.tmpl;
    bool running = v.state == L"running";
    Async([id, d, t, running] {
        netx::Report r = netx::Diagnose(d, t, running);
        Ui([id, r] { if (ServerView* s = FindServer(id)) { s->net = r; s->netLoaded = true; s->netBusy = false; s->firewall = r.ports.empty() ? s->firewall : r.ports[0].firewall; } });
    });
}

static std::wstring UpperStr(const std::wstring& s) { std::wstring o = s; for (auto& c : o) c = towupper(c); return o; }

static float DrawNetwork(ServerView& v, float x, float y, float w) {
    std::wstring id = v.def.id;
    float cx;
    y = Row(x, y, w, L"Automatic port forwarding (UPnP)", v.upnp == L"ok" ? L"Your router is forwarding the ports while the server runs." :
            v.upnp == L"failed" ? L"Your router refused: " + v.upnpError : v.upnp == L"unavailable" ? L"No UPnP router found. Forward the ports by hand (below)." :
            L"Asks your router to forward the ports while the server runs.", S(40), &cx);
    Toggle(cx, y - D(14) - std::max(D(36), D(22)) , v.def.upnp, [id](bool on) { if (ServerView* s = FindServer(id)) { s->def.upnp = on; SaveServer(*s); } });
    // firewall
    std::wstring fwText = v.firewall == 1 ? L"Rules are in place for this server's ports." : v.firewall == 0 ? L"No rules yet: people outside this PC can't connect." : L"Windows Firewall rules for this server's ports.";
    float fy = y;
    y = Row(x, y, w, L"Windows Firewall", fwText, D(200), &cx);
    float aw = ButtonW(v.firewall == 1 ? L"Remove rules" : L"Add rules");
    ServerDef d = v.def;
    Button(RW(x + w - aw, fy - D(2), aw, D(34)), v.firewall == 1 ? L"Remove rules" : L"Add rules", icon::Shield, Btn::Secondary, [id, d] {
        ServerView* s = FindServer(id);
        bool add = !s || s->firewall != 1;
        Async([id, d, add] {
            std::wstring err;
            bool ok = netx::firewall::Apply(d, add, &err);
            Ui([id, ok, add, err] { if (ServerView* s2 = FindServer(id)) { if (ok) s2->firewall = add ? 1 : 0; s2->def.firewall = add; servers::Save(s2->def); } Toast(ok ? (add ? L"Firewall rules added." : L"Firewall rules removed.") : err, !ok); });
        });
    });
    if (v.firewall < 0 && !v.netBusy && !v.netLoaded) Async([id, d] { bool has = netx::firewall::HasRules(d); Ui([id, has] { if (ServerView* s = FindServer(id)) s->firewall = has ? 1 : 0; }); }), v.firewall = -2;
    // check
    float ky = y;
    y = Row(x, y, w, L"Can friends reach it?", L"Checks your router, your internet provider and the ports from outside.", D(200), &cx);
    float kw = ButtonW(v.netBusy ? L"Checking…" : L"Check connection", icon::Network);
    Button(RW(x + w - kw, ky - D(2), kw, D(34)), v.netBusy ? L"Checking…" : L"Check connection", icon::Network, Btn::Primary, [id] { if (ServerView* s = FindServer(id)) Diagnose(*s); }, !v.netBusy);
    if (v.netLoaded) {
        const netx::Report& r = v.net;
        D2D1_COLOR_F c = r.nat == L"Open (UPnP)" ? g_t.good : r.nat == L"Double NAT" || r.nat == L"Offline" ? g_t.bad : g_t.warn;
        y = Banner(x, y, w, c, icon::Network, r.nat + L". " + r.advice);
        std::wstring pub = g_cfg.hidePublicIp ? L"hidden" : r.publicIp;
        Text(L"This PC " + r.localIp + L" \x00B7 router " + r.gateway + (r.routerName.empty() ? L"" : L" (" + r.routerName + L")") + L" \x00B7 public " + pub, x, y, w, F(12), g_t.subtext);
        y += D(28);
        for (auto& p : r.ports) {
            std::wstring status = p.reachable == 1 ? L"Reachable from the internet" : p.reachable == 0 ? L"Not reachable from outside" : p.protocol == L"UDP" ? L"UDP can't be tested from outside: ask a friend to try" : L"Not tested (start the server first)";
            D2D1_COLOR_F pc = p.reachable == 1 ? g_t.good : p.reachable == 0 ? g_t.bad : g_t.faint;
            g_c.Circle(x + D(8), y + D(13), D(4), pc);
            Text(std::to_wstring(p.port) + L" " + p.protocol, x + D(22), y, D(110), F(13), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D(26));
            Text(status + (p.firewall == 0 ? L" \x00B7 no firewall rule" : L""), x + D(130), y, w - D(130), F(12.5f), g_t.subtext, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(26));
            y += D(28);
        }
        if (!r.upnp || r.nat == L"Manual forwarding") {
            y += D(8);
            std::wstring ports;
            for (auto& p : netx::ForwardedPorts(v.def, v.tmpl)) {
                if (!ports.empty()) ports += L", ";
                ports += (p.count > 1 ? std::to_wstring(p.port) + L"\x2013" + std::to_wstring(p.port + p.count - 1) : std::to_wstring(p.port)) + L" " + (p.protocol == L"both" ? L"TCP and UDP" : UpperStr(p.protocol));
            }
            std::wstring steps = L"Forward the ports on your router:\n1. Open http://" + r.gateway + L" in your browser and sign in (the password is often on a sticker on the router).\n"
                                 L"2. Find Port Forwarding (sometimes under Advanced, NAT or Virtual Servers).\n3. Add: " + ports + L" to " + r.localIp + L".\n"
                                 L"4. Save, then press Check connection again.\nTip: give this PC a reserved IP (DHCP reservation) so the rule keeps working.";
            float h = Para(steps, x + D(16), y + D(12), w - D(32), F(12.5f), g_t.text);
            D2D1_RECT_F sr = RW(x, y, w, h + D(24));
            g_c.StrokeRound(sr, S(g_t.radius + 2), g_t.divider);
            std::wstring gw = r.gateway;
            float ow = ButtonW(L"Open router page", icon::Globe);
            Button(RW(sr.right - ow - D(12), sr.top + D(10), ow, D(32)), L"Open router page", icon::Globe, Btn::Secondary, [gw] { OpenUrl(L"http://" + gw); });
            y += h + D(36);
        }
    }
    if (v.tmpl.id == L"minecraft-bedrock") {
        float by = y;
        y = Row(x, y, w, L"Allow joining from this PC", L"Windows blocks Minecraft (Bedrock) from joining a server on the same PC until you allow it once.", D(120), &cx);
        float bw = ButtonW(L"Allow");
        Button(RW(x + w - bw, by - D(2), bw, D(34)), L"Allow", 0, Btn::Secondary, [] {
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.lpVerb = L"runas"; sei.lpFile = L"CheckNetIsolation.exe";
            sei.lpParameters = L"LoopbackExempt -a -n=\"Microsoft.MinecraftUWP_8wekyb3d8bbwe\""; sei.nShow = SW_HIDE;
            Toast(ShellExecuteExW(&sei) ? L"Done: you can join from this PC." : L"Windows didn't allow it.", false);
        });
    }
    return y;
}


// raw config editor (a native multi-line edit box over the page)
static HWND g_raw = nullptr;
static HFONT g_rawFont = nullptr;
static std::wstring g_rawPath, g_rawServer;
static D2D1_RECT_F g_rawArea;
bool RawEditorOpen() { return g_raw != nullptr; }
void LayoutRawEditor() {
    if (!g_raw) return;
    SetWindowPos(g_raw, nullptr, (int)g_rawArea.left, (int)g_rawArea.top, (int)(g_rawArea.right - g_rawArea.left), (int)(g_rawArea.bottom - g_rawArea.top), SWP_NOZORDER);
}
void OpenRawEditor(ServerView& v, const std::wstring& path) {
    CloseRawEditor(false);
    std::wstring text;
    ReadTextFile(path, text);
    // the edit box needs CRLF line endings
    std::wstring crlf;
    for (size_t i = 0; i < text.size(); ++i) { if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) crlf += L'\r'; crlf += text[i]; }
    g_rawPath = path; g_rawServer = v.def.id;
    g_raw = CreateWindowExW(0, L"EDIT", crlf.c_str(), WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                            0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)500, GetModuleHandleW(nullptr), nullptr);
    if (g_rawFont) DeleteObject(g_rawFont);
    g_rawFont = CreateFontW(-(int)F(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Cascadia Mono");
    SendMessageW(g_raw, WM_SETFONT, (WPARAM)g_rawFont, TRUE);
    SendMessageW(g_raw, EM_SETLIMITTEXT, 8 * 1024 * 1024, 0);
    SetFocus(g_raw);
    Invalidate();
}
void CloseRawEditor(bool save) {
    if (!g_raw) return;
    if (save) {
        int n = GetWindowTextLengthW(g_raw);
        std::wstring text(n, 0);
        GetWindowTextW(g_raw, &text[0], n + 1);
        if (WriteFileAtomic(g_rawPath, WideToUtf8(text))) {
            if (ServerView* s = FindServer(g_rawServer)) {
                ReadBackConfig(s->def, s->tmpl);
                servers::Save(s->def);
                if (s->Running()) s->needsRestart = true;
            }
            Toast(L"Saved " + PathName(g_rawPath) + L".");
        } else Toast(L"Couldn't save " + g_rawPath, true);
    }
    DestroyWindow(g_raw);
    g_raw = nullptr;
    SetFocus(g_hwnd);
    Invalidate();
}

static void DrawRawEditor(float x, float y, float w, float bottom) {
    Text(PathName(g_rawPath), x, y + D(14), w / 2, F(15), g_t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Text(g_rawPath, x, y + D(40), w * 0.7f, F(11.5f), g_t.faint);
    float sw = ButtonW(L"Save", icon::Save), cw = ButtonW(L"Cancel");
    Button(RW(x + w - sw, y + D(16), sw, D(34)), L"Save", icon::Save, Btn::Primary, [] { CloseRawEditor(true); });
    Button(RW(x + w - sw - cw - D(8), y + D(16), cw, D(34)), L"Cancel", 0, Btn::Secondary, [] { CloseRawEditor(false); });
    D2D1_RECT_F area = R(x, y + D(66), x + w, bottom - D(16));
    g_c.FillRound(R(area.left - 2, area.top - 2, area.right + 2, area.bottom + 2), S(4), g_t.divider);
    if (std::fabs(area.top - g_rawArea.top) > 0.5f || std::fabs(area.right - g_rawArea.right) > 0.5f || std::fabs(area.bottom - g_rawArea.bottom) > 0.5f) { g_rawArea = area; LayoutRawEditor(); }
}

static void LoadVersions(ServerView& v) {
    if (v.versionsLoading) return;
    v.versionsLoading = true;
    std::wstring id = v.def.id, variant = v.def.variant;
    Template t = v.tmpl;
    Async([id, t, variant] {
        auto vs = provision::ListVersions(t, variant);
        Ui([id, vs] { if (ServerView* s = FindServer(id)) { s->versions = vs.list; if (s->versions.size() > 120) s->versions.resize(120); } });
    });
}

static void DrawSettings(ServerView& v, float x, float y0, float w, float bottom) {
    if (RawEditorOpen() && g_rawServer == v.def.id) { DrawRawEditor(x, y0, w, bottom); return; }
    ScrollArea& sa = PageScroll();
    float y = BeginScroll(sa, R(x - D(4), y0, x + w + D(12), bottom)) + D(18);
    std::wstring id = v.def.id;
    float cx;
    if (v.needsRestart && v.state == L"running") y = Banner(x, y, w, g_t.warn, icon::Refresh, L"Restart the server to use your new settings.", L"Restart", [id] { if (ServerView* s = FindServer(id)) RestartServer(*s); });
    // ---- server
    y = SectionTitle(L"Server", x, y, w);
    float ny = y;
    y = Row(x, y, w, L"Name", L"Shown in Outpost.", D(260), &cx);
    TextField(RW(cx, ny - D(2), D(260), D(36)), L"name:" + id, v.def.name, [id](const std::wstring& nv) {
        if (ServerView* s = FindServer(id)) { if (!Trim(nv).empty()) { s->def.name = Trim(nv); SaveServer(*s); } }
    });
    if (v.tmpl.hasRam) {
        double totalMb = g_sys.ramTotal ? g_sys.ramTotal / 1048576.0 : 16384;
        double hi = std::max((double)v.tmpl.ramMin, std::min(65536.0, std::floor((totalMb - 2048) / 512) * 512));
        int ram = v.def.ram > 0 ? v.def.ram : v.tmpl.ramDefault;
        float ry = y;
        wchar_t lbl[64]; swprintf(lbl, 64, L"Memory: %.1f GB", ram / 1024.0);
        y = Row(x, y, w, lbl, L"How much RAM the server may use. " + std::wstring(ram > hi * 0.8 ? L"Leave some for Windows and your games." : L"More helps with many players and mods."), D(300), &cx);
        Slider(RW(cx, ry + D(4), D(300), D(24)), L"ram:" + id, ram, v.tmpl.ramMin, hi, 512,
               [id](double nv) { if (ServerView* s = FindServer(id)) s->def.ram = (int)nv; },
               [id](double nv) { if (ServerView* s = FindServer(id)) { s->def.ram = (int)nv; SaveServer(*s, false); if (SettingNeedsRestart(*s)) s->needsRestart = true; } });
    }
    for (auto& p : v.tmpl.ports) {
        if (p.local) continue;
        float py = y;
        std::wstring help = (p.protocol == L"both" ? L"TCP and UDP" : p.protocol == L"tcp" ? L"TCP" : L"UDP") + std::wstring(p.count > 1 ? L", plus the next " + std::to_wstring(p.count - 1) : L"");
        y = Row(x, y, w, p.name + L" port", help, D(140), &cx);
        FieldOpts o; o.numeric = true;
        std::wstring pid = p.id;
        TextField(RW(cx, py - D(2), D(140), D(36)), L"port:" + id + p.id, v.def.Get(p.id), [id, pid](const std::wstring& nv) {
            int n = _wtoi(nv.c_str());
            if (n < 1024 || n > 65535) { Toast(L"Ports go from 1024 to 65535.", true); return; }
            if (ServerView* s = FindServer(id)) { s->def.settings[pid] = std::to_wstring(n); SaveServer(*s, false); s->firewall = -1; s->netLoaded = false; if (SettingNeedsRestart(*s)) s->needsRestart = true; }
        }, o);
    }
    // ---- template settings by group
    std::vector<std::wstring> groups;
    for (auto& st : v.tmpl.settings) if (!st.hidden && std::find(groups.begin(), groups.end(), st.group) == groups.end()) groups.push_back(st.group);
    for (auto& g : groups) {
        y += D(10);
        y = SectionTitle(g == L"General" ? L"Game" : g, x, y, w);
        for (auto& st : v.tmpl.settings) if (!st.hidden && st.group == g && Visible(y - D(100), y + D(100))) y = SettingControl(v, st, x, y, w);
            else if (!st.hidden && st.group == g) y += D(64);
    }
    // ---- automation
    y += D(10);
    y = SectionTitle(L"Automation", x, y, w);
    float ty = y;
    y = Row(x, y, w, L"Restart after a crash", L"Up to 3 times in 15 minutes.", S(40), &cx);
    Toggle(cx, ty, v.def.autoRestart, [id](bool on) { if (ServerView* s = FindServer(id)) { s->def.autoRestart = on; SaveServer(*s); } });
    ty = y;
    y = Row(x, y, w, L"Scheduled restart", L"Players get a one-minute warning.", D(220), &cx);
    static const wchar_t* sched[] = {L"Off", L"Every 6 hours", L"Every 12 hours", L"Every 24 hours", L"Daily at 4:00 AM", L"Daily at 6:00 AM"};
    int ssel = v.def.restartAt == L"04:00" ? 4 : v.def.restartAt == L"06:00" ? 5 : v.def.restartHours == 6 ? 1 : v.def.restartHours == 12 ? 2 : v.def.restartHours == 24 ? 3 : 0;
    Dropdown(RW(cx, ty - D(2), D(220), D(36)), {sched[0], sched[1], sched[2], sched[3], sched[4], sched[5]}, ssel, [id](int i) {
        ServerView* s = FindServer(id);
        if (!s) return;
        s->def.restartHours = i == 1 ? 6 : i == 2 ? 12 : i == 3 ? 24 : 0;
        s->def.restartAt = i == 4 ? L"04:00" : i == 5 ? L"06:00" : L"";
        SaveServer(*s);
    });
    if (!v.tmpl.sleepWake.empty()) {
        ty = y;
        y = Row(x, y, w, L"Sleep when empty", L"Stops the game when nobody's on to free your PC, and wakes it when someone connects (the first try may time out while it starts).", D(220), &cx);
        Toggle(cx, ty, v.def.sleepWhenEmpty, [id](bool on) { if (ServerView* s = FindServer(id)) { s->def.sleepWhenEmpty = on; SaveServer(*s); } });
        static const int sm[] = {5, 10, 15, 30, 60};
        int msel = 2;
        for (int i = 0; i < 5; ++i) if (sm[i] == v.def.sleepMinutes) msel = i;
        if (v.def.sleepWhenEmpty) Dropdown(RW(cx + S(52), ty - D(2), D(220) - S(52), D(36)), {L"after 5 min", L"after 10 min", L"after 15 min", L"after 30 min", L"after 1 hour"}, msel,
                                          [id](int i) { if (ServerView* s = FindServer(id)) { s->def.sleepMinutes = sm[i]; SaveServer(*s); } });
    }
    ty = y;
    y = Row(x, y, w, L"Start when Windows starts", L"Outpost starts in the tray at sign-in and starts this server.", S(40), &cx);
    Toggle(cx, ty, v.def.startOnBoot, [id](bool on) {
        ServerView* s = FindServer(id);
        if (!s) return;
        s->def.startOnBoot = on;
        SaveServer(*s);
        if (on && !g_cfg.startWithWindows) { g_cfg.startWithWindows = true; ConfigSave(); ApplyStartup(); }
    });
    ty = y;
    y = Row(x, y, w, L"Be gentle while I play", L"While a full-screen game runs on this PC, the server gets lower priority and automatic backups wait.", S(40), &cx);
    Toggle(cx, ty, v.def.lowerPriorityWhileGaming, [id](bool on) { if (ServerView* s = FindServer(id)) { s->def.lowerPriorityWhileGaming = on; SaveServer(*s); } });
    // ---- network
    y += D(10);
    y = SectionTitle(L"Network", x, y, w);
    y = DrawNetwork(v, x, y, w);
    // ---- game files
    y += D(10);
    y = SectionTitle(L"Game files", x, y, w);
    ty = y;
    std::wstring verHelp = v.def.installedVersion.empty() ? L"Not downloaded yet." : L"Installed: " + v.def.installedVersion + L".";
    if (v.updateChecked) verHelp += v.updateAvailable.empty() ? L" Up to date." : L" Update available: " + v.updateAvailable + L".";
    bool versionPick = v.tmpl.sourceType == L"minecraft" || v.tmpl.sourceType == L"terraria";
    y = Row(x, y, w, L"Version", verHelp, versionPick ? D(220) : D(10), &cx);
    if (versionPick) {
        if (v.versions.empty()) LoadVersions(v);
        std::vector<std::wstring> labels = {L"Latest (follows updates)"};
        int sel = 0;
        for (size_t i = 0; i < v.versions.size(); ++i) { labels.push_back(v.versions[i]); if (v.versions[i] == v.def.version) sel = (int)i + 1; }
        Dropdown(RW(cx, ty - D(2), D(220), D(36)), labels, sel, [id](int i) {
            ServerView* s = FindServer(id);
            if (!s) return;
            std::wstring nv = i == 0 ? L"latest" : s->versions[i - 1];
            if (nv == s->def.version) return;
            s->def.version = nv;
            SaveServer(*s, false);
            Confirm(L"Switch to " + (i == 0 ? std::wstring(L"the latest version") : nv) + L"?", L"Outpost backs up the world, then downloads the server files. Going back to an older version can break a world, so the backup matters.", L"Switch now",
                    [id] { if (ServerView* s2 = FindServer(id)) { if (s2->Running()) Toast(L"Stop the server first, then press Update now.", true); else InstallJob(id, L"update"); } });
        }, !v.job.active);
    }
    float gx = x;
    float gy = y;
    auto gbtn = [&](const std::wstring& l, wchar_t g, Btn st, std::function<void()> fn, bool en = true) { float bw = ButtonW(l, g); Button(RW(gx, gy, bw, D(34)), l, g, st, fn, en); gx += bw + D(8); };
    gbtn(v.updateChecking ? L"Checking…" : L"Check for updates", icon::Refresh, Btn::Secondary, [id] {
        ServerView* s = FindServer(id);
        if (!s) return;
        s->updateChecking = true;
        ServerDef d = s->def; Template t = s->tmpl;
        Async([id, d, t] {
            std::wstring err;
            std::wstring up = provision::CheckUpdate(d, t, &err);
            Ui([id, up, err] { if (ServerView* s2 = FindServer(id)) { s2->updateChecking = false; s2->updateChecked = true; s2->updateAvailable = up; Toast(up.empty() ? (err.empty() ? L"Up to date." : err) : L"Update available: " + up, !err.empty() && up.empty()); } });
        });
    }, !v.updateChecking && !v.job.active);
    gbtn(L"Update now", icon::Update, v.updateAvailable.empty() ? Btn::Secondary : Btn::Primary, [id] {
        ServerView* s = FindServer(id);
        if (!s) return;
        if (s->Running()) { Confirm(L"Stop and update?", L"Outpost stops the server, backs up the world, updates the files and starts it again.", L"Update", [id] {
            ServerView* s2 = FindServer(id);
            if (!s2) return;
            StopServer(*s2);
            Async([id] { for (int i = 0; i < 3000 && host::IsRunning(id); ++i) Sleep(100); Ui([id] { InstallJob(id, L"update"); }); });
        }); return; }
        InstallJob(id, L"update");
    }, !v.job.active);
    gbtn(L"Repair files", icon::Repair, Btn::Ghost, [id] { InstallJob(id, L"repair"); }, !v.job.active && !v.Running());
    gbtn(L"Open server folder", icon::Folder, Btn::Ghost, [root = v.def.FilesDir()] { OpenPath(root); });
    y = gy + D(50);
    Text(L"Template: " + v.tmpl.name + L" v" + std::to_wstring(v.tmpl.version) + L" (" + (v.tmpl.origin.empty() ? L"saved with this server" : v.tmpl.origin) + L")", x, y, w, F(12), g_t.faint);
    y += D(30);
    // ---- config files
    auto files = EditableConfigFiles(v.def, v.tmpl);
    if (!files.empty()) {
        y += D(6);
        y = SectionTitle(L"Config files", x, y, w);
        Text(L"For advanced changes. The settings above are written into these files each time the server starts.", x, y - D(8), w, F(12), g_t.faint);
        y += D(20);
        for (auto& f : files) {
            D2D1_RECT_F row = RW(x, y, w, D(44));
            Card(row, Hover(row));
            bool exists = FileExists(f);
            Glyph(icon::Edit, x + D(22), y + D(22), F(13), g_t.subtext);
            std::wstring rel = PathStartsWith(f, v.def.FilesDir()) ? f.substr(v.def.FilesDir().size() + 1) : f;
            Text(rel, x + D(44), y, w - D(180), F(13), exists ? g_t.text : g_t.faint, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D(44));
            float ew = ButtonW(L"Edit");
            Button(RW(x + w - ew - D(10), y + D(6), ew, D(32)), L"Edit", 0, Btn::Secondary, [id, f] {
                ServerView* s = FindServer(id);
                if (!s) return;
                if (!FileExists(f)) WriteConfigFiles(s->def, s->tmpl, nullptr);
                OpenRawEditor(*s, f);
            });
            y += D(50);
        }
    }
    // ---- danger
    y += D(16);
    y = SectionTitle(L"Delete", x, y, w);
    ty = y;
    y = Row(x, y, w, L"Delete this server", L"Removes the server and its world. Backups are kept unless you choose to delete them too.", D(160), &cx);
    float dw = ButtonW(L"Delete server", icon::Delete);
    Button(RW(x + w - dw, ty - D(2), dw, D(34)), L"Delete server", icon::Delete, Btn::Danger, [id] {
        ServerView* s = FindServer(id);
        if (!s) return;
        if (s->Running()) { Toast(L"Stop the server first.", true); return; }
        Confirm(L"Delete " + s->def.name + L"?", L"The server and its world are deleted. Its backups stay in " + backup::Dir(s->def) + L".", L"Delete", [id] {
            ServerView* s2 = FindServer(id);
            if (!s2) return;
            ServerDef d = s2->def;
            Template t = s2->tmpl;
            hostlink::Unwatch(id);
            std::wstring err;
            if (d.upnp) Async([d, t] { netx::upnp::UnmapServer(d, t); });
            if (s2->firewall == 1) Async([d] { std::wstring e; netx::firewall::Apply(d, false, &e); });
            if (!ops::Delete(d, false, &err)) Toast(err, true);
            else Toast(L"Deleted " + d.name + L".");
            ReloadServers();
            if (g_servers.empty()) g_page = Page::Welcome;
        }, true);
    });
    EndScroll(sa, y + D(30));
}

// ================================================================ page
void DrawServer(float x0) {
    ServerView* v = Sel();
    if (!v) return;
    float y = DrawHeader(*v, x0);
    y = DrawTabs(x0, y);
    float pad = D(28);
    float x = x0 + pad, w = g_W - x0 - pad * 2;
    if (g_tab != TabSettings && RawEditorOpen()) CloseRawEditor(false);
    g_consoleRect = {0, 0, 0, 0};
    switch (g_tab) {
        case TabOverview: DrawOverview(*v, x, y, w, g_H); break;
        case TabConsole: DrawConsoleTab(*v, x, y, w, g_H); break;
        case TabPlayers: DrawPlayers(*v, x, y, w, g_H); break;
        case TabMods: DrawMods(*v, x, y, w, g_H); break;
        case TabBackups: DrawBackups(*v, x, y, w, g_H); break;
        case TabSettings: DrawSettings(*v, x, y, w, g_H); break;
    }
}

} // namespace ui
