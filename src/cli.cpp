// Outpost - command line
//   list | templates | status <id> | create --game <id> [--variant v] [--version v] [--name n] [--ram MB] [--set k=v]…
//   install <id> | start <id> | stop <id> | restart <id> | wait <id> <state> [seconds] | send <id> <command…>
//   action <id> <action> [player] [text] | backup <id> | backups <id> | restore <id> <zip|latest> | update <id>
//   delete <id> | net <id> | mods <id> <search…> | selftest | e2e <game> [variant] [seconds]
#include "cli.h"
#include "server.h"
#include "template.h"
#include "ops.h"
#include "host.h"
#include "pipe.h"
#include "backup.h"
#include "netx.h"
#include "mods.h"
#include "util.h"
#include "fsutil.h"
#include "archive.h"
#include "json.h"
#include <cstdio>
#include <thread>
#include <atomic>
#include <regex>

namespace cli {

static HANDLE s_out = nullptr;
static void Out(const std::wstring& s) {
    std::string u = WideToUtf8(s + L"\r\n");
    DWORD w;
    if (s_out && s_out != INVALID_HANDLE_VALUE) WriteFile(s_out, u.data(), (DWORD)u.size(), &w, nullptr);
    Log(L"cli: %ls", s.c_str());
}

static bool Find(const std::wstring& id, ServerDef& s) {
    for (auto& x : servers::LoadAll()) if (x.id == id || IEquals(x.name, id)) { s = x; return true; }
    Out(L"No server called " + id);
    return false;
}

static provision::Progress Printer() {
    auto last = std::make_shared<std::wstring>();
    auto lastPct = std::make_shared<int>(-5);
    return [last, lastPct](int pct, const std::wstring& text) {
        if (text == *last && (pct < 0 || pct - *lastPct < 10)) return;
        if (text == *last && pct >= 0) *lastPct = pct;
        if (text != *last) *lastPct = pct;
        *last = text;
        Out(L"  " + text + (pct >= 0 ? L" " + std::to_wstring(pct) + L"%" : L""));
    };
}

// Follows a host's events until `until` says stop (true = success) or the timeout passes
static bool Follow(const std::wstring& id, int seconds, const std::function<int(const json::Value&)>& until, bool echoConsole = true) {
    DWORD deadline = GetTickCount() + seconds * 1000;
    while (GetTickCount() < deadline) {
        auto c = pipe::Connect(host::PipeName(id), 2000);
        if (!c) {
            if (!host::IsRunning(id)) {
                json::Value gone = json::Value::MakeObject();
                gone.Set(L"ev", json::Value::Str(L"status")); gone.Set(L"state", json::Value::Str(L"stopped"));
                std::wstring st;
                ServerDef s;
                for (auto& x : servers::LoadAll()) if (x.id == id) s = x;
                if (ReadTextFile(s.root + L"\\state.json", st)) { json::Value v; if (json::Parser(st).Parse(v)) { gone = v; if (gone.GetStr(L"state") != L"error") gone.Set(L"state", json::Value::Str(L"stopped")); } }
                int r = until(gone);
                if (r) return r > 0;
            }
            Sleep(500);
            continue;
        }
        int result = 0;
        std::thread timer([c, deadline] { while (c->Open() && GetTickCount() < deadline) Sleep(200); c->Close(); });
        c->Send("{\"type\":\"subscribe\"}");
        c->Run([&](const std::string& line) {
            json::Value v;
            if (!json::Parser(Utf8ToWide(line)).Parse(v)) return;
            if (echoConsole && v.GetStr(L"ev") == L"line") Out(L"  | " + v.GetStr(L"s"));
            if (v.GetStr(L"ev") == L"notify") Out(L"  * " + v.GetStr(L"text"));
            int r = until(v);
            if (r) { result = r; c->Close(); }
        });
        timer.join();
        if (result) return result > 0;
    }
    return false;
}

static bool SendTo(const std::wstring& id, const std::string& msg, std::string* reply = nullptr, int waitMs = 0) {
    auto c = pipe::Connect(host::PipeName(id), 3000);
    if (!c) return false;
    c->Send(msg);
    std::thread t([c, waitMs] { Sleep(std::max(300, waitMs)); c->Close(); });
    c->Run([&](const std::string& l) { if (reply && l.find("\"ev\":\"result\"") != std::string::npos) { *reply = l; c->Close(); } });
    t.join();
    return true;
}

static int SelfTest() {
    int fails = 0;
    auto check = [&](bool ok, const wchar_t* what) { Out(std::wstring(ok ? L"PASS " : L"FAIL ") + what); if (!ok) ++fails; };
    std::wstring p = L"#comment\nmotd=hi\nmax-players=20\n";
    cfg::SetProperties(p, L"max-players", L"10"); cfg::SetProperties(p, L"new-key", L"x");
    check(cfg::GetProperties(p, L"max-players") == L"10" && cfg::GetProperties(p, L"new-key") == L"x" && p.find(L"#comment") == 0, L"properties");
    std::wstring ini = L"[A]\nx=1\n\n[B]\ny=2\n";
    cfg::SetIni(ini, L"B", L"z", L"3"); cfg::SetIni(ini, L"A", L"x", L"9"); cfg::SetIni(ini, L"C", L"w", L"4");
    check(cfg::GetIni(ini, L"A", L"x") == L"9" && cfg::GetIni(ini, L"B", L"z") == L"3" && cfg::GetIni(ini, L"C", L"w") == L"4" && cfg::GetIni(ini, L"B", L"x").empty(), L"ini");
    std::wstring ue = L"[/Script/Pal.PalGameWorldSettings]\r\nOptionSettings=(Difficulty=None,ServerName=\"A, B\",ExpRate=1.000000)\r\n";
    cfg::SetUeOption(ue, L"/Script/Pal.PalGameWorldSettings", L"OptionSettings", L"ExpRate", L"2.0");
    cfg::SetUeOption(ue, L"/Script/Pal.PalGameWorldSettings", L"OptionSettings", L"RESTAPIEnabled", L"True");
    check(cfg::GetUeOption(ue, L"OptionSettings", L"ExpRate") == L"2.0" && cfg::GetUeOption(ue, L"OptionSettings", L"ServerName") == L"\"A, B\"" &&
          cfg::GetUeOption(ue, L"OptionSettings", L"RESTAPIEnabled") == L"True", L"unreal option list");
    std::wstring xml = L"<?xml version=\"1.0\"?>\n<ServerSettings>\n\t<property name=\"ServerPort\" value=\"26900\"/>\n<!-- <property name=\"SaveGameFolder\" value=\"x\"/> -->\n</ServerSettings>\n";
    cfg::SetXmlProperty(xml, L"ServerPort", L"27000"); cfg::SetXmlProperty(xml, L"SaveGameFolder", L"C:\\a&b");
    check(cfg::GetXmlProperty(xml, L"ServerPort") == L"27000" && cfg::GetXmlProperty(xml, L"SaveGameFolder") == L"C:\\a&b" && xml.find(L"&amp;") != std::wstring::npos, L"xml properties");
    ServerDef s; s.root = L"C:\\srv"; s.name = L"Test"; s.settings[L"port"] = L"25565"; s.settings[L"gslt"] = L""; s.settings[L"mods"] = L"1,2";
    Template t; t.ramDefault = 2048;
    check(Expand(L"-p {port} {gslt|+tok } {mods|-mods=} {dir} {ram}", s, t) == L"-p 25565  -mods=1,2 C:\\srv\\files 2048", L"placeholders");
    int n = 0;
    for (auto& tt : templates::All()) {
        ++n;
        std::wregex r;
        if (const json::Value* l = tt.Obj(L"log"))
            for (auto& kv : l->obj) {
                std::wstring pat = kv.second.s;
                auto f = std::regex::ECMAScript;
                if (pat.compare(0, 4, L"(?i)") == 0) { pat = pat.substr(4); f |= std::regex::icase; }
                try { r = std::wregex(pat, f); } catch (...) { check(false, (tt.id + L" pattern " + kv.first).c_str()); }
            }
    }
    check(n >= 12, L"12 built-in templates load");
    // log patterns against real lines
    auto match = [&](const wchar_t* game, const wchar_t* key, const std::wstring& line, const std::wstring& expect) {
        const Template* tt = templates::Find(game);
        if (!tt) return false;
        std::wstring pat = tt->Obj(L"log")->GetStr(key);
        auto f = std::regex::ECMAScript;
        if (pat.compare(0, 4, L"(?i)") == 0) { pat = pat.substr(4); f |= std::regex::icase; }
        std::wsmatch m;
        std::wregex r(pat, f);
        if (!std::regex_search(line, m, r)) return false;
        return expect.empty() || (m.size() > 1 && m[1].str() == expect);
    };
    check(match(L"minecraft-java", L"ready", L"[12:00:01] [Server thread/INFO]: Done (3.512s)! For help, type \"help\"", L""), L"minecraft ready");
    check(match(L"minecraft-java", L"join", L"[12:00:01] [Server thread/INFO]: Steve joined the game", L"Steve"), L"minecraft join");
    check(match(L"minecraft-java", L"leave", L"[12:00:01] [Server thread/INFO]: Steve left the game", L"Steve"), L"minecraft leave");
    check(match(L"valheim", L"join", L"11/12/2026 10:00:00: Got character ZDOID from Ragnar : -1234:1", L"Ragnar"), L"valheim join");
    check(match(L"terraria", L"join", L": Bob has joined.", L"Bob"), L"terraria join");
    // zip round trip
    std::wstring dir = DataDir() + L"\\selftest";
    fs::DeleteTree(dir);
    EnsureDir(dir + L"\\a\\b");
    WriteFileAtomic(dir + L"\\a\\b\\f.txt", std::string(100000, 'x'));
    std::vector<archive::Entry> e;
    uint64_t bytes = 0;
    archive::Collect(dir, dir + L"\\a", L"files/a", e, &bytes);
    std::wstring err;
    bool zipped = archive::Create(dir + L"\\t.zip", e, "{}", nullptr, &err);
    bool unzipped = zipped && archive::Extract(dir + L"\\t.zip", dir + L"\\out", L"files", nullptr, &err);
    std::string back;
    ReadFileBytes(dir + L"\\out\\a\\b\\f.txt", back);
    check(zipped && unzipped && back.size() == 100000, L"zip round trip");
    fs::DeleteTree(dir);
    Out(fails ? std::to_wstring(fails) + L" FAILED" : L"ALL PASSED");
    return fails ? 1 : 0;
}

// Full end-to-end run for one game: create → install → start → ready → command → backup → stop → restore → start → stop
static int E2E(const std::wstring& game, const std::wstring& variant, int seconds) {
    int fails = 0;
    auto step = [&](bool ok, const std::wstring& what) { Out(std::wstring(ok ? L"PASS " : L"FAIL ") + what); if (!ok) ++fails; return ok; };
    std::map<std::wstring, std::wstring> set;
    if (game == L"minecraft-java") set[L"eula"] = L"true";
    if (game == L"valheim") set[L"password"] = L"outpost-test";
    ServerDef s;
    std::wstring err;
    std::wstring name = L"e2e " + game + (variant.empty() ? L"" : L" " + variant);
    if (!step(ops::Create(game, variant, L"latest", name, set, game == L"minecraft-java" ? 2048 : 0, L"", &s, &err), L"create " + name + L" " + err)) return 1;
    Template t;
    servers::LoadTemplate(s, t);
    DWORD t0 = GetTickCount();
    std::atomic<bool> cancel{false};
    if (!step(ops::Install(s, Printer(), &cancel, &err), L"install (" + s.installedVersion + L", " + std::to_wstring((GetTickCount() - t0) / 1000) + L"s) " + err)) return 1;
    if (!step(host::Launch(s, &err), L"host started " + err)) return 1;
    t0 = GetTickCount();
    std::wstring finalState;
    bool ready = Follow(s.id, seconds, [&](const json::Value& v) {
        if (v.GetStr(L"ev") != L"status") return 0;
        finalState = v.GetStr(L"state");
        if (finalState == L"running") return 1;
        if (finalState == L"error" || finalState == L"stopped") return -1;
        return 0;
    });
    if (!step(ready, L"server ready in " + std::to_wstring((GetTickCount() - t0) / 1000) + L"s (state " + finalState + L")")) {
        SendTo(s.id, "{\"type\":\"stop\"}");
        Follow(s.id, 120, [](const json::Value& v) { return v.GetStr(L"state") == L"stopped" || v.GetStr(L"state") == L"error" ? 1 : 0; }, false);
        return 1;
    }
    Sleep(5000);
    // status details
    json::Value status;
    Follow(s.id, 20, [&](const json::Value& v) { if (v.GetStr(L"ev") == L"status") { status = v; return 1; } return 0; }, false);
    step(status.GetNum(L"ram", 0) > 0, L"resource monitoring (RAM " + FormatBytes((uint64_t)status.GetNum(L"ram", 0)) + L", CPU " + std::to_wstring((int)status.GetNum(L"cpu", 0)) + L"%)");
    if (!t.perfLabel.empty()) {
        double perf = -1;
        Follow(s.id, 40, [&](const json::Value& v) { if (v.GetStr(L"ev") == L"status" && v.GetNum(L"perf", -1) >= 0) { perf = v.GetNum(L"perf", -1); return 1; } return 0; }, false);
        step(perf >= 0, t.perfLabel + L" reported (" + std::to_wstring((int)perf) + L")");
    }
    // a console command and a friendly action
    if (t.consoleInput) {
        std::wstring cmd = game == L"minecraft-java" ? L"say Hello from Outpost" : game == L"terraria" ? L"say Hello from Outpost" : L"";
        if (!cmd.empty()) {
            SendTo(s.id, "{\"type\":\"command\",\"text\":\"" + WideToUtf8(cmd) + "\"}");
            bool echoed = Follow(s.id, 20, [&](const json::Value& v) { return v.GetStr(L"ev") == L"line" && v.GetStr(L"s").find(L"Hello from Outpost") != std::wstring::npos && v.GetStr(L"k") != L"cmd" ? 1 : 0; }, false);
            step(echoed, L"console command answered");
        }
    }
    if (t.HasCommand(L"say")) {
        std::string reply;
        SendTo(s.id, "{\"type\":\"action\",\"action\":\"say\",\"text\":\"Outpost action test\"}", &reply, 8000);
        step(reply.find("\"ok\":true") != std::string::npos, L"player action (say) " + Utf8ToWide(reply));
    }
    // live backup
    SendTo(s.id, "{\"type\":\"backup\",\"label\":\"e2e\"}");
    // the console backlog is replayed on connect, so a quick backup isn't missed
    bool backed = Follow(s.id, 300, [&](const json::Value& v) {
        if (v.GetStr(L"ev") != L"line") return 0;
        if (v.GetStr(L"s").find(L"Backup saved:") != std::wstring::npos) return 1;
        if (v.GetStr(L"s").find(L"Backup failed") != std::wstring::npos) return -1;
        return 0;
    }, false);
    auto list = backup::List(s);
    step(backed && !list.empty(), L"backup while running (" + (list.empty() ? std::wstring(L"none") : PathName(list[0].path) + L", " + FormatBytes(list[0].size)) + L")");
    // graceful stop
    t0 = GetTickCount();
    SendTo(s.id, "{\"type\":\"stop\"}");
    bool stopped = Follow(s.id, 240, [](const json::Value& v) { std::wstring st = v.GetStr(L"state"); return st == L"stopped" ? 1 : st == L"error" ? -1 : 0; }, true);
    for (int i = 0; i < 100 && host::IsRunning(s.id); ++i) Sleep(100);
    step(stopped && !host::IsRunning(s.id), L"graceful stop in " + std::to_wstring((GetTickCount() - t0) / 1000) + L"s");
    // the world was saved: the save paths exist
    bool saved = false;
    if (const json::Value* sv = t.Obj(L"saves"))
        for (auto& p : sv->arr) if (GetFileAttributesW(fs::Join(s.FilesDir(), Expand(p.s, s, t)).c_str()) != INVALID_FILE_ATTRIBUTES) saved = true;
    step(saved, L"world files on disk");
    // restore (takes a safety backup first)
    if (!list.empty()) {
        size_t before = backup::List(s).size();
        bool restored = backup::Restore(s, t, list[0].path, nullptr, &err);
        step(restored && backup::List(s).size() == before + 1, L"restore with safety backup " + err);
        // and it starts again from the restored world
        if (host::Launch(s, &err)) {
            bool again = Follow(s.id, seconds, [](const json::Value& v) { std::wstring st = v.GetStr(L"state"); return st == L"running" ? 1 : (st == L"error" || st == L"stopped") ? -1 : 0; }, false);
            step(again, L"starts again after restore");
            SendTo(s.id, "{\"type\":\"stop\"}");
            Follow(s.id, 240, [](const json::Value& v) { std::wstring st = v.GetStr(L"state"); return st == L"stopped" || st == L"error" ? 1 : 0; }, false);
        }
    }
    for (int i = 0; i < 100 && host::IsRunning(s.id); ++i) Sleep(100);
    Out(fails ? std::to_wstring(fails) + L" FAILED for " + game : L"ALL PASSED for " + game);
    return fails ? 1 : 0;
}

int Main(int argc, wchar_t** argv) {
    s_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!s_out || s_out == INVALID_HANDLE_VALUE || GetFileType(s_out) == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) s_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    // argv: [exe, (--data dir), --cli, command, ...]: drop everything up to --cli and any --data pair
    std::vector<wchar_t*> args;
    args.push_back(argv[0]);
    bool seenCli = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--data") && i + 1 < argc) { ++i; continue; }
        if (!seenCli) { if (!wcscmp(argv[i], L"--cli")) { seenCli = true; args.push_back(argv[i]); } continue; }
        args.push_back(argv[i]);
    }
    argc = (int)args.size();
    argv = args.data();
    if (argc < 3) { Out(L"Usage: Outpost.exe --cli list|templates|create|install|start|stop|wait|send|backup|restore|status|delete|selftest|e2e"); return 2; }
    std::wstring cmd = argv[2];
    std::vector<std::wstring> a;
    for (int i = 3; i < argc; ++i) a.push_back(argv[i]);
    auto opt = [&](const wchar_t* k, const std::wstring& def = L"") {
        for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == k) return a[i + 1];
        return def;
    };
    ServerDef s;
    std::wstring err;
    if (cmd == L"selftest") return SelfTest();
    if (cmd == L"templates") {
        for (auto& t : templates::All()) Out(t.id + L"  " + t.name + L"  (v" + std::to_wstring(t.version) + L", " + t.origin + L")");
        return 0;
    }
    if (cmd == L"list") {
        for (auto& x : servers::LoadAll()) Out(x.id + L"  " + x.name + L"  " + x.game + L"  " + (host::IsRunning(x.id) ? L"running" : L"stopped") + L"  " + x.root);
        return 0;
    }
    if (cmd == L"stop-all") {
        for (auto& x : servers::LoadAll()) if (host::IsRunning(x.id)) { Out(L"Stopping " + x.name); SendTo(x.id, "{\"type\":\"stop\"}"); }
        for (int i = 0; i < 1800; ++i) { bool any = false; for (auto& x : servers::LoadAll()) any |= host::IsRunning(x.id); if (!any) break; Sleep(100); }
        return 0;
    }
    if (cmd == L"e2e" && !a.empty()) return E2E(a[0], a.size() > 1 && a[1][0] != L'-' ? a[1] : L"", _wtoi(opt(L"--timeout", L"900").c_str()));
    if (cmd == L"create") {
        std::map<std::wstring, std::wstring> set;
        for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == L"--set") { size_t eq = a[i + 1].find(L'='); if (eq != std::wstring::npos) set[a[i + 1].substr(0, eq)] = a[i + 1].substr(eq + 1); }
        if (!ops::Create(opt(L"--game"), opt(L"--variant"), opt(L"--version", L"latest"), opt(L"--name"), set, _wtoi(opt(L"--ram", L"0").c_str()), opt(L"--location"), &s, &err)) { Out(err); return 1; }
        Out(L"Created " + s.id + L" in " + s.root);
        std::atomic<bool> cancel{false};
        if (opt(L"--no-install").empty() && !ops::Install(s, Printer(), &cancel, &err)) { Out(L"Install failed: " + err); return 1; }
        Out(L"Ready: " + s.installedVersion);
        return 0;
    }
    if (a.empty()) { Out(L"Which server?"); return 2; }
    if (!Find(a[0], s)) return 1;
    if (cmd == L"install" || cmd == L"update") {
        std::atomic<bool> cancel{false};
        bool ok = cmd == L"install" ? ops::Install(s, Printer(), &cancel, &err) : ops::UpdateFiles(s, Printer(), &cancel, &err);
        Out(ok ? L"Done: " + s.installedVersion : L"Failed: " + err);
        return ok ? 0 : 1;
    }
    if (cmd == L"start") { bool ok = host::Launch(s, &err); Out(ok ? L"Starting " + s.name : err); return ok ? 0 : 1; }
    if (cmd == L"stop" || cmd == L"restart") {
        if (!host::IsRunning(s.id)) { Out(L"Not running."); return 0; }
        SendTo(s.id, "{\"type\":\"" + WideToUtf8(cmd) + "\"}");
        if (cmd == L"stop" && opt(L"--wait") != L"no") Follow(s.id, 300, [](const json::Value& v) { return v.GetStr(L"state") == L"stopped" || v.GetStr(L"state") == L"error" ? 1 : 0; });
        return 0;
    }
    if (cmd == L"wait" && a.size() >= 2) {
        std::wstring want = a[1];
        int secs = a.size() > 2 ? _wtoi(a[2].c_str()) : 600;
        bool ok = Follow(s.id, secs, [&](const json::Value& v) {
            std::wstring st = v.GetStr(L"state");
            if (v.GetStr(L"ev") != L"status") return 0;
            if (st == want) return 1;
            if (st == L"error" && want != L"error") return -1;
            return 0;
        });
        Out(ok ? L"State: " + want : L"Timed out or failed waiting for " + want);
        return ok ? 0 : 1;
    }
    if (cmd == L"send" && a.size() >= 2) {
        std::wstring text;
        for (size_t i = 1; i < a.size(); ++i) text += (i > 1 ? L" " : L"") + a[i];
        json::Value v = json::Value::MakeObject();
        v.Set(L"type", json::Value::Str(L"command")); v.Set(L"text", json::Value::Str(text));
        std::wstring o; json::WriteCompact(v, o);
        return SendTo(s.id, WideToUtf8(o)) ? 0 : 1;
    }
    if (cmd == L"action" && a.size() >= 2) {
        json::Value v = json::Value::MakeObject();
        v.Set(L"type", json::Value::Str(L"action")); v.Set(L"action", json::Value::Str(a[1]));
        v.Set(L"player", json::Value::Str(a.size() > 2 ? a[2] : L"")); v.Set(L"text", json::Value::Str(a.size() > 3 ? a[3] : L""));
        std::wstring o; json::WriteCompact(v, o);
        std::string reply;
        SendTo(s.id, WideToUtf8(o), &reply, 8000);
        Out(Utf8ToWide(reply));
        return reply.find("\"ok\":true") != std::string::npos ? 0 : 1;
    }
    if (cmd == L"status") {
        std::wstring st;
        if (host::IsRunning(s.id)) Follow(s.id, 10, [&](const json::Value& v) { if (v.GetStr(L"ev") == L"status") { std::wstring o; json::Write(v, o); Out(o); return 1; } return 0; }, false);
        else Out(L"stopped");
        return 0;
    }
    if (cmd == L"backup") {
        Template t;
        servers::LoadTemplate(s, t);
        if (host::IsRunning(s.id)) {
            auto newest = [&] { auto l = backup::List(s); return l.empty() ? std::wstring() : l[0].path; };
            std::wstring before = newest();
            SendTo(s.id, "{\"type\":\"backup\",\"label\":\"manual\"}");
            bool ok = false;
            for (int i = 0; i < 6000 && !ok; ++i) { Sleep(100); ok = newest() != before; }
            Out(ok ? L"Backed up." : L"The backup didn't finish.");
            return ok ? 0 : 1;
        }
        std::wstring path;
        bool ok = backup::Create(s, t, L"manual", nullptr, &path, &err);
        Out(ok ? L"Backed up to " + path : L"Backup failed: " + err);
        return ok ? 0 : 1;
    }
    if (cmd == L"backups") { for (auto& b : backup::List(s)) Out(PathName(b.path) + L"  " + FormatBytes(b.size) + L"  " + b.label); return 0; }
    if (cmd == L"restore" && a.size() >= 2) {
        Template t;
        servers::LoadTemplate(s, t);
        std::wstring zip = a[1];
        if (zip == L"latest") { auto l = backup::List(s); if (l.empty()) { Out(L"No backups."); return 1; } zip = l[0].path; }
        if (host::IsRunning(s.id)) { Out(L"Stop the server first."); return 1; }
        bool ok = backup::Restore(s, t, zip, nullptr, &err);
        Out(ok ? L"Restored " + PathName(zip) : L"Restore failed: " + err);
        return ok ? 0 : 1;
    }
    if (cmd == L"delete") { bool ok = ops::Delete(s, opt(L"--backups") == L"yes", &err); Out(ok ? L"Deleted." : err); return ok ? 0 : 1; }
    if (cmd == L"net") {
        Template t;
        servers::LoadTemplate(s, t);
        netx::Report r = netx::Diagnose(s, t, host::IsRunning(s.id));
        Out(L"Local " + r.localIp + L", gateway " + r.gateway + L", public " + r.publicIp + L", router WAN " + r.routerIp + L" (" + r.routerName + L")");
        Out(L"NAT: " + r.nat + L". " + r.advice);
        for (auto& p : r.ports) Out(L"  " + std::to_wstring(p.port) + L"/" + p.protocol + L" firewall=" + std::to_wstring(p.firewall) + L" reachable=" + std::to_wstring(p.reachable));
        return 0;
    }
    if (cmd == L"mods") {
        Template t;
        servers::LoadTemplate(s, t);
        std::wstring q;
        for (size_t i = 1; i < a.size(); ++i) q += (i > 1 ? L" " : L"") + a[i];
        auto hits = mods::Search(s, t, q, L"modrinth", &err);
        for (auto& h : hits) Out(h.projectId + L"  " + h.title + L" - " + h.description);
        if (!err.empty()) Out(err);
        if (opt(L"--install").size() && !hits.empty()) {
            auto vs = mods::Versions(s, t, hits[0], &err);
            if (vs.empty()) { Out(err); return 1; }
            bool ok = mods::Install(s, t, hits[0], vs[0], &err);
            Out(ok ? L"Installed " + hits[0].title + L" " + vs[0].number : err);
            return ok ? 0 : 1;
        }
        return hits.empty() ? 1 : 0;
    }
    Out(L"Unknown command: " + cmd);
    return 2;
}

} // namespace cli
