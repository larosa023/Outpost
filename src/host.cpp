// Outpost - the host process (see host.h)
#include "host.h"
#include "pipe.h"
#include "proc.h"
#include "remote.h"
#include "backup.h"
#include "netx.h"
#include "util.h"
#include "fsutil.h"
#include "json.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <regex>
#include <thread>
#include <mutex>
#include <deque>
#include <set>
#include <condition_variable>
#include <algorithm>
#include <ctime>

namespace host {

std::wstring PipeName(const std::wstring& id) { return L"\\\\.\\pipe\\outpost." + id + L"." + pipe::SessionSuffix(); }
static std::wstring MutexName(const std::wstring& id) { return L"Local\\Outpost.Host." + id; }

bool IsRunning(const std::wstring& id) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, MutexName(id).c_str());
    if (m) { CloseHandle(m); return true; }
    return false;
}

static std::wstring RuntimeDir() { return DataDir() + L"\\runtime\\host"; }

// The host runs from its own copy of Outpost.exe, so the real exe can be replaced by an update or the installer
static std::wstring RuntimeExe(std::wstring* error) {
    static std::wstring cached;
    if (!cached.empty() && FileExists(cached)) return cached;
    std::wstring hash = Sha256File(ExePath());
    std::wstring name = std::wstring(L"Outpost-") + OUTPOST_VERSION + L"-" + (hash.size() >= 8 ? hash.substr(0, 8) : L"dev") + L".exe";
    std::wstring dest = RuntimeDir() + L"\\" + name;
    if (!FileExists(dest)) {
        EnsureDir(RuntimeDir());
        if (!CopyFileW(ExePath().c_str(), (dest + L".part").c_str(), FALSE) || !MoveFileExW((dest + L".part").c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (error) *error = L"Couldn't prepare the server host in " + RuntimeDir();
            return L"";
        }
        // a portable copy keeps its data next to the real exe: tell the host where that is
        if (IsPortable()) WriteTextFileAtomic(RuntimeDir() + L"\\portable-data.txt", DataDir());
    }
    cached = dest;
    return dest;
}

void CleanupRuntimeCopies() {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((RuntimeDir() + L"\\Outpost-*.exe").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    std::wstring keep;
    RuntimeExe(nullptr);
    do {
        std::wstring p = RuntimeDir() + L"\\" + fd.cFileName;
        if (IEquals(p, RuntimeExe(nullptr))) continue;
        DeleteFileW(p.c_str());   // fails while a host still runs from it, which is fine
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

bool Launch(const ServerDef& s, std::wstring* error) {
    if (IsRunning(s.id)) return true;
    std::wstring exe = RuntimeExe(error);
    if (exe.empty()) return false;
    std::wstring cmd = proc::Quote(exe) + L" --host " + proc::Quote(s.root);
    cmd += L" --data " + proc::Quote(DataDir());   // the host runs from runtime\, so tell it where the data is
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    // no CREATE_NEW_PROCESS_GROUP: that would switch Ctrl+C off for the host and every game it starts (Valheim stops on Ctrl+C)
    DWORD flags = DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
    BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, flags, nullptr, s.root.c_str(), &si, &pi);
    if (!ok) ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, flags & ~CREATE_BREAKAWAY_FROM_JOB, nullptr, s.root.c_str(), &si, &pi);
    if (!ok) { if (error) *error = L"Couldn't start the server host (error " + std::to_wstring(GetLastError()) + L")."; return false; }
    CloseHandle(pi.hThread);
    // wait until it's listening, so the window can connect straight away
    for (int i = 0; i < 60 && !IsRunning(s.id); ++i) {
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
    }
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code != STILL_ACTIVE && !IsRunning(s.id)) { if (error) *error = L"The server host stopped right away. Details are in outpost.log."; return false; }
    return true;
}

// ================================================================ the host
static std::string J(const json::Value& v) { std::wstring o; json::WriteCompact(v, o); return WideToUtf8(o); }

static bool Rx(const std::wstring& pat0, std::wregex& out) {
    if (pat0.empty()) return false;
    std::wstring pat = pat0;
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    if (pat.compare(0, 4, L"(?i)") == 0) { pat = pat.substr(4); flags |= std::regex::icase; }
    try { out = std::wregex(pat, flags); return true; }
    catch (...) { Log(L"bad pattern in template: %ls", pat0.c_str()); return false; }
}

static std::wstring StripCodes(const std::wstring& in) {
    std::wstring o;
    o.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == 0x1B) {   // ANSI escape
            if (i + 1 < in.size() && in[i + 1] == L'[') { i += 2; while (i < in.size() && !(in[i] >= 0x40 && in[i] <= 0x7E)) ++i; }
            continue;
        }
        if (c == 0xA7 && i + 1 < in.size()) { ++i; continue; }   // Minecraft §x colors
        if (c == L'\r' || c == 0 || (c < 0x20 && c != L'\t')) continue;
        o += c;
    }
    return o;
}

struct Player { std::wstring name, id, userId; long long since = 0; int ping = -1; double x = 0, y = 0; bool loc = false; int level = 0; };
struct Line { long long n = 0, t = 0; std::wstring k, s; };

enum class St { Starting, Running, Stopping, Sleeping, Restarting, Stopped, Error };
static const wchar_t* StName(St s) {
    switch (s) {
        case St::Starting: return L"starting"; case St::Running: return L"running"; case St::Stopping: return L"stopping";
        case St::Sleeping: return L"sleeping"; case St::Restarting: return L"restarting"; case St::Stopped: return L"stopped"; default: return L"error";
    }
}

class Host {
public:
    explicit Host(const std::wstring& root) : root_(root) {}
    int Run();
    void RequestStop(const std::wstring& why) { std::lock_guard<std::mutex> g(m_); stopWanted_ = true; stopWhy_ = why; }
    bool ChildRunning() { return child_.Running(); }
private:
    // setup
    bool LoadDef();
    void CompilePatterns();
    bool StartServer();
    bool ResolveLaunch(std::wstring* exe, std::wstring* args, std::wstring* error);
    void BeginStop(bool forSleep, bool forRestart);
    void FinishStopped();
    // console
    void ReaderLoop(HANDLE out);
    void TailLog(std::wstring path);
    void OnLine(const std::wstring& raw, const std::wstring& kindOverride = L"");
    void Say(const std::wstring& text, const std::wstring& kind = L"outpost") { OnLine(text, kind); }
    // talking to the server
    bool Exec(const std::wstring& cmd, const std::wstring& via, std::string* reply);
    bool PalApi(const std::wstring& what, const std::wstring& userId, const std::wstring& text, std::string* reply = nullptr);
    std::wstring CommandVia() const;
    void DoAction(const json::Value& req, std::wstring* result, bool* ok);
    // periodic
    void Tick();
    void PollLoop();
    void QueryPlayers(bool force);
    void MergePlayers(const std::vector<Player>& list, bool fromQuery);
    void RunBackup(const std::wstring& label, bool scheduled);
    void ScheduleRestart();
    void StartWakeListener();
    void StopWakeListener();
    void WakeLoop();
    void MapPorts();
    // clients
    void OnClient(std::shared_ptr<pipe::Conn> c);
    void Broadcast(const std::string& line);
    void BroadcastStatus();
    std::string StatusJson();
    void Notify(const std::wstring& kind, const std::wstring& text);
    void History(const std::wstring& ev, const std::wstring& name);
    void SetState(St s, const std::wstring& msg = L"");

    std::wstring root_;
    ServerDef s_;
    Template t_;
    std::mutex m_;
    St state_ = St::Starting;
    std::wstring message_;
    proc::Child child_;
    std::thread reader_, tailer_;
    std::atomic<bool> readerDone_{false};
    std::atomic<bool> tailStop_{false};
    proc::Meter meter_;
    proc::Usage usage_;
    double perf_ = -1;
    std::wstring perfLabel_;
    int perfCmd_ = -2;                       // index into query.perf that works (-1 none, -2 unknown)
    std::map<std::wstring, Player> players_;
    std::wstring pendingId_;
    std::map<std::wstring, std::wstring> idToName_;
    std::deque<Line> ring_;
    long long seq_ = 0;
    std::vector<std::shared_ptr<pipe::Conn>> clients_;
    pipe::Server server_;
    Rcon rcon_;
    Telnet telnet_;
    // patterns
    std::wregex reReady_, reJoin_, reLeave_, reJoinId_, reLeaveId_, reChat_, reWarn_, reErr_, reWait_;
    bool hasReady_ = false, hasJoin_ = false, hasLeave_ = false, hasJoinId_ = false, hasLeaveId_ = false, hasChat_ = false, hasWarn_ = false, hasErr_ = false;
    // flags
    bool stopWanted_ = false, restartWanted_ = false, sleepWanted_ = false, wakeWanted_ = false, stopping_ = false;
    bool stopForSleep_ = false, stopForRestart_ = false, ctrlCSent_ = false;
    std::wstring stopWhy_;
    ULONGLONG stopDeadline_ = 0, stopHalf_ = 0;
    long long startedAt_ = 0, readyAt_ = 0, lastPlayerAt_ = 0, lastBackup_ = 0, nextBackup_ = 0, nextRestart_ = 0, sleepingSince_ = 0;
    bool restartWarned_ = false, playedSinceBackup_ = false, gaming_ = false;
    std::atomic<bool> backingUp_{false};
    std::deque<long long> crashes_;
    // backup wait-for
    std::mutex waitM_;
    std::condition_variable waitCv_;
    bool waiting_ = false, waitHit_ = false;
    // stdin list capture
    bool capturing_ = false;
    std::vector<std::wstring> captured_;
    // wake listener
    std::thread wake_;
    std::atomic<bool> wakeStop_{false};
    uintptr_t wakeSock_ = ~(uintptr_t)0;
    // UPnP
    std::wstring upnp_ = L"off", upnpError_;
    long long upnpAt_ = 0;
    // log file
    HANDLE logFile_ = INVALID_HANDLE_VALUE;
    uint64_t logSize_ = 0;
    std::atomic<bool> quit_{false};
    long long lastStatusFile_ = 0;
};

bool Host::LoadDef() {
    if (!servers::Load(root_, s_)) return false;
    if (!servers::LoadTemplate(s_, t_)) return false;
    CompilePatterns();
    return true;
}

void Host::CompilePatterns() {
    const json::Value* l = t_.Obj(L"log");
    auto get = [&](const wchar_t* k) { return l ? l->GetStr(k) : L""; };
    hasReady_ = Rx(get(L"ready"), reReady_);
    hasJoin_ = Rx(get(L"join"), reJoin_);
    hasLeave_ = Rx(get(L"leave"), reLeave_);
    hasJoinId_ = Rx(get(L"joinId"), reJoinId_);
    hasLeaveId_ = Rx(get(L"leaveId"), reLeaveId_);
    hasChat_ = Rx(get(L"chat"), reChat_);
    hasWarn_ = Rx(get(L"warn"), reWarn_);
    hasErr_ = Rx(get(L"error"), reErr_);
    if (const json::Value* p = t_.Obj(L"performance")) perfLabel_ = p->GetStr(L"label");
}

// ---------------------------------------------------------------- clients
void Host::Broadcast(const std::string& line) {
    std::vector<std::shared_ptr<pipe::Conn>> list;
    { std::lock_guard<std::mutex> g(m_); list = clients_; }
    for (auto& c : list) c->Send(line);
}

std::string Host::StatusJson() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"ev", json::Value::Str(L"status"));
    v.Set(L"state", json::Value::Str(StName(state_)));
    v.Set(L"message", json::Value::Str(message_));
    v.Set(L"since", json::Value::Num((double)startedAt_));
    v.Set(L"readyAt", json::Value::Num((double)readyAt_));
    v.Set(L"pid", json::Value::Num(child_.pid));
    v.Set(L"hostPid", json::Value::Num(GetCurrentProcessId()));
    v.Set(L"hostVersion", json::Value::Str(OUTPOST_VERSION));
    v.Set(L"version", json::Value::Str(s_.installedVersion));
    json::Value pl = json::Value::MakeArray();
    for (auto& kv : players_) {
        json::Value p = json::Value::MakeObject();
        p.Set(L"name", json::Value::Str(kv.second.name));
        p.Set(L"since", json::Value::Num((double)kv.second.since));
        if (kv.second.ping >= 0) p.Set(L"ping", json::Value::Num(kv.second.ping));
        if (kv.second.loc) { p.Set(L"x", json::Value::Num(kv.second.x)); p.Set(L"y", json::Value::Num(kv.second.y)); }
        if (kv.second.level) p.Set(L"level", json::Value::Num(kv.second.level));
        if (!kv.second.id.empty()) p.Set(L"id", json::Value::Str(kv.second.id));
        pl.arr.push_back(p);
    }
    v.Set(L"players", pl);
    v.Set(L"maxPlayers", json::Value::Num(_wtoi(s_.Get(L"maxPlayers").c_str())));
    v.Set(L"cpu", json::Value::Num(usage_.cpu));
    v.Set(L"ram", json::Value::Num((double)usage_.ram));
    v.Set(L"ramLimit", json::Value::Num(t_.hasRam ? (double)s_.ram * 1024 * 1024 : 0));
    v.Set(L"perf", json::Value::Num(perf_));
    v.Set(L"perfLabel", json::Value::Str(perfLabel_));
    v.Set(L"perfMax", json::Value::Num(t_.perfMax));
    v.Set(L"lastBackup", json::Value::Num((double)lastBackup_));
    v.Set(L"nextBackup", json::Value::Num((double)nextBackup_));
    v.Set(L"nextRestart", json::Value::Num((double)nextRestart_));
    v.Set(L"sleepingSince", json::Value::Num((double)sleepingSince_));
    v.Set(L"backingUp", json::Value::Boolean(backingUp_));
    v.Set(L"gaming", json::Value::Boolean(gaming_));
    v.Set(L"upnp", json::Value::Str(upnp_));
    v.Set(L"upnpError", json::Value::Str(upnpError_));
    v.Set(L"crashes", json::Value::Num((double)crashes_.size()));
    return J(v);
}

void Host::BroadcastStatus() {
    std::string st;
    { std::lock_guard<std::mutex> g(m_); st = StatusJson(); }
    Broadcast(st);
    long long now = fs::UnixNow();
    if (now != lastStatusFile_) {
        lastStatusFile_ = now;
        WriteFileAtomic(root_ + L"\\state.json", st);
    }
}

void Host::SetState(St st, const std::wstring& msg) {
    { std::lock_guard<std::mutex> g(m_); state_ = st; message_ = msg; }
    Log(L"host %ls: %ls%ls%ls", s_.id.c_str(), StName(st), msg.empty() ? L"" : L" - ", msg.c_str());
    BroadcastStatus();
}

void Host::Notify(const std::wstring& kind, const std::wstring& text) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"ev", json::Value::Str(L"notify"));
    v.Set(L"kind", json::Value::Str(kind));
    v.Set(L"text", json::Value::Str(text));
    v.Set(L"t", json::Value::Num((double)fs::UnixNow()));
    std::string line = J(v);
    Broadcast(line);
    // the window may be closed: keep the last few so the tray can show them when it reconnects
    HANDLE h = CreateFileW((root_ + L"\\notify.jsonl").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; std::string l = line + "\n"; WriteFile(h, l.data(), (DWORD)l.size(), &w, nullptr); CloseHandle(h); }
}

void Host::History(const std::wstring& ev, const std::wstring& name) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"t", json::Value::Num((double)fs::UnixNow()));
    v.Set(L"ev", json::Value::Str(ev));
    v.Set(L"name", json::Value::Str(name));
    std::string l = J(v) + "\n";
    HANDLE h = CreateFileW((root_ + L"\\history.jsonl").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, l.data(), (DWORD)l.size(), &w, nullptr); CloseHandle(h); }
}

void Host::OnClient(std::shared_ptr<pipe::Conn> c) {
    c->Run([this, c](const std::string& line) {
        json::Value req;
        if (!json::Parser(Utf8ToWide(line)).Parse(req)) return;
        std::wstring type = req.GetStr(L"type");
        if (type == L"subscribe") {
            std::vector<std::string> backlog;
            {
                std::lock_guard<std::mutex> g(m_);
                long long since = (long long)req.GetNum(L"since", 0);
                for (auto& l : ring_) {
                    if (l.n <= since) continue;
                    json::Value v = json::Value::MakeObject();
                    v.Set(L"ev", json::Value::Str(L"line")); v.Set(L"n", json::Value::Num((double)l.n)); v.Set(L"t", json::Value::Num((double)l.t));
                    v.Set(L"k", json::Value::Str(l.k)); v.Set(L"s", json::Value::Str(l.s));
                    backlog.push_back(J(v));
                }
                backlog.push_back(StatusJson());
                clients_.push_back(c);
            }
            for (auto& b : backlog) c->Send(b);
        } else if (type == L"command") {
            std::wstring text = Trim(req.GetStr(L"text"));
            if (text.empty()) return;
            OnLine(L"> " + text, L"cmd");
            std::wstring via = CommandVia();
            if (!t_.consoleInput && t_.Obj(L"commands") && t_.Obj(L"commands")->GetStr(L"say").compare(0, 4, L"api:") == 0) {
                // Palworld: no console, but a typed message becomes an announcement
                bool ok = PalApi(L"announce", L"", text);
                OnLine(ok ? L"Announced to everyone." : L"Couldn't reach the server's admin API.", L"outpost");
                return;
            }
            std::string reply;
            if (!Exec(text, via, &reply)) OnLine(L"The command couldn't be sent" + std::wstring(via.empty() ? L"." : L" (" + via + L" isn't answering yet)."), L"error");
            else if (!via.empty()) {
                std::wstring r = StripCodes(Utf8ToWide(reply));
                size_t i = 0;
                while (i < r.size()) {
                    size_t e = r.find(L'\n', i);
                    if (e == std::wstring::npos) e = r.size();
                    std::wstring l = Trim(r.substr(i, e - i));
                    if (!l.empty()) OnLine(l, L"info");
                    i = e + 1;
                }
            }
        } else if (type == L"action") {
            std::wstring result;
            bool ok = false;
            DoAction(req, &result, &ok);
            json::Value v = json::Value::MakeObject();
            v.Set(L"ev", json::Value::Str(L"result"));
            v.Set(L"ok", json::Value::Boolean(ok));
            v.Set(L"text", json::Value::Str(result));
            c->Send(J(v));
        } else if (type == L"stop") {
            RequestStop(L"stopped from Outpost");
        } else if (type == L"restart") {
            std::lock_guard<std::mutex> g(m_); restartWanted_ = true;
        } else if (type == L"backup") {
            std::wstring label = req.GetStr(L"label", L"manual");
            std::thread([this, label] { RunBackup(label, false); }).detach();
        } else if (type == L"reload") {
            ServerDef fresh;
            if (servers::Load(root_, fresh)) {
                std::lock_guard<std::mutex> g(m_);
                s_.autoRestart = fresh.autoRestart; s_.restartHours = fresh.restartHours; s_.restartAt = fresh.restartAt;
                s_.sleepWhenEmpty = fresh.sleepWhenEmpty; s_.sleepMinutes = fresh.sleepMinutes; s_.backupMinutes = fresh.backupMinutes;
                s_.keepBackups = fresh.keepBackups; s_.backupDir = fresh.backupDir; s_.lowerPriorityWhileGaming = fresh.lowerPriorityWhileGaming;
                s_.upnp = fresh.upnp; s_.name = fresh.name; s_.mods = fresh.mods;
                nextBackup_ = s_.backupMinutes > 0 ? fs::UnixNow() + s_.backupMinutes * 60 : 0;
            }
            ScheduleRestart();
            BroadcastStatus();
        } else if (type == L"sleep") {
            std::lock_guard<std::mutex> g(m_); sleepWanted_ = true;
        } else if (type == L"wake") {
            std::lock_guard<std::mutex> g(m_); wakeWanted_ = true;
        } else if (type == L"players") {
            std::thread([this] { QueryPlayers(true); }).detach();
        } else if (type == L"status") {
            c->Send(StatusJson());
        }
    });
    std::lock_guard<std::mutex> g(m_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), c), clients_.end());
}

// ---------------------------------------------------------------- console
void Host::OnLine(const std::wstring& raw, const std::wstring& kindOverride) {
    std::wstring text = StripCodes(raw);
    if (Trim(text).empty()) return;
    if (text.size() > 4000) text = text.substr(0, 4000) + L"…";
    std::wstring kind = kindOverride.empty() ? L"info" : kindOverride;
    bool statusChanged = false;
    std::vector<std::pair<std::wstring, std::wstring>> notes;
    if (kindOverride.empty()) {
        std::wsmatch m;
        if (hasErr_ && std::regex_search(text, reErr_)) kind = L"error";
        else if (hasWarn_ && std::regex_search(text, reWarn_)) kind = L"warn";
        if (hasChat_ && std::regex_search(text, m, reChat_)) kind = L"chat";
        std::lock_guard<std::mutex> g(m_);
        if (capturing_) captured_.push_back(text);
        if (hasReady_ && state_ == St::Starting && std::regex_search(text, reReady_)) {
            state_ = St::Running; message_.clear(); readyAt_ = lastPlayerAt_ = fs::UnixNow(); statusChanged = true;
            notes.push_back({L"ready", s_.name + L" is ready."});
        }
        if (hasJoinId_ && std::regex_search(text, m, reJoinId_) && m.size() > 1) pendingId_ = m[1].str();
        if (hasJoin_ && std::regex_search(text, m, reJoin_) && m.size() > 1) {
            std::wstring name = Trim(m[1].str());
            if (!name.empty() && !players_.count(name)) {
                Player p; p.name = name; p.since = fs::UnixNow(); p.id = pendingId_;
                if (!pendingId_.empty()) idToName_[pendingId_] = name;
                pendingId_.clear();
                players_[name] = p;
                lastPlayerAt_ = fs::UnixNow(); playedSinceBackup_ = true;
                statusChanged = true;
                notes.push_back({L"join", name + L" joined " + s_.name});
            }
            kind = L"join";
        }
        std::wstring left;
        if (hasLeave_ && std::regex_search(text, m, reLeave_) && m.size() > 1) left = Trim(m[1].str());
        if (hasLeaveId_ && std::regex_search(text, m, reLeaveId_) && m.size() > 1) {
            auto it = idToName_.find(m[1].str());
            if (it != idToName_.end()) { left = it->second; idToName_.erase(it); }
        }
        if (!left.empty()) {
            if (players_.erase(left)) { statusChanged = true; notes.push_back({L"leave", left + L" left " + s_.name}); lastPlayerAt_ = fs::UnixNow(); }
            kind = L"leave";
        }
        if (waiting_ && std::regex_search(text, reWait_)) { waitHit_ = true; waitCv_.notify_all(); }
    }
    Line l;
    {
        std::lock_guard<std::mutex> g(m_);
        l.n = ++seq_; l.t = fs::UnixNow(); l.k = kind; l.s = text;
        ring_.push_back(l);
        if (ring_.size() > 2000) ring_.pop_front();
    }
    for (auto& n : notes) {
        if (n.first == L"join") History(L"join", n.second.substr(0, n.second.find(L" joined ")));
        if (n.first == L"leave") History(L"leave", n.second.substr(0, n.second.find(L" left ")));
        Notify(n.first, n.second);
    }
    json::Value v = json::Value::MakeObject();
    v.Set(L"ev", json::Value::Str(L"line")); v.Set(L"n", json::Value::Num((double)l.n)); v.Set(L"t", json::Value::Num((double)l.t));
    v.Set(L"k", json::Value::Str(kind)); v.Set(L"s", json::Value::Str(text));
    Broadcast(J(v));
    // console.log, rolled over at 10 MB
    if (logFile_ == INVALID_HANDLE_VALUE || logSize_ > 10ull * 1024 * 1024) {
        if (logFile_ != INVALID_HANDLE_VALUE) {
            CloseHandle(logFile_);
            MoveFileExW((root_ + L"\\logs\\console.log").c_str(), (root_ + L"\\logs\\console.1.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        EnsureDir(root_ + L"\\logs");
        logFile_ = CreateFileW((root_ + L"\\logs\\console.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
        LARGE_INTEGER sz{};
        if (logFile_ != INVALID_HANDLE_VALUE) GetFileSizeEx(logFile_, &sz);
        logSize_ = (uint64_t)sz.QuadPart;
    }
    if (logFile_ != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t ts[32]; swprintf(ts, 32, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
        std::string u = WideToUtf8(ts + text + L"\r\n");
        DWORD w; WriteFile(logFile_, u.data(), (DWORD)u.size(), &w, nullptr);
        logSize_ += u.size();
    }
    if (statusChanged) BroadcastStatus();
}

// Some servers (Palworld) print a few lines as UTF-16: drop the zero bytes so they read as text
static std::wstring Decode(std::string line) {
    size_t zeros = 0;
    for (char c : line) zeros += c == 0;
    if (zeros && zeros * 3 >= line.size()) {
        std::string clean;
        for (char c : line) if (c) clean += c;
        line = clean;
    }
    return Utf8ToWide(line);
}

void Host::ReaderLoop(HANDLE out) {
    std::string pending;
    char buf[8192];
    DWORD n;
    while (ReadFile(out, buf, sizeof(buf), &n, nullptr) && n) {
        pending.append(buf, n);
        size_t p;
        while ((p = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, p);
            pending.erase(0, p + 1);
            OnLine(Decode(line));
        }
        // a prompt without a newline (e.g. "> "): show long partial lines anyway
        if (pending.size() > 8192) { OnLine(Decode(pending)); pending.clear(); }
    }
    if (!pending.empty()) OnLine(Decode(pending));
    readerDone_ = true;
}

void Host::TailLog(std::wstring path) {
    // Some servers (ARK, Satisfactory, 7 Days to Die) write their log to a file instead of the console
    uint64_t pos = 0;
    bool first = true;
    std::string pending;
    while (!tailStop_) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            GetFileSizeEx(h, &sz);
            if (first) { pos = (uint64_t)sz.QuadPart > 0 && startedAt_ && false ? (uint64_t)sz.QuadPart : 0; first = false; }
            if ((uint64_t)sz.QuadPart < pos) pos = 0;   // the game started a new log
            if ((uint64_t)sz.QuadPart > pos) {
                LARGE_INTEGER li; li.QuadPart = (LONGLONG)pos;
                SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
                std::vector<char> buf(std::min<uint64_t>((uint64_t)sz.QuadPart - pos, 1 << 20));
                DWORD n = 0;
                if (ReadFile(h, buf.data(), (DWORD)buf.size(), &n, nullptr)) {
                    pos += n;
                    pending.append(buf.data(), n);
                    size_t p;
                    while ((p = pending.find('\n')) != std::string::npos) {
                        std::string line = pending.substr(0, p);
                        pending.erase(0, p + 1);
                        OnLine(Utf8ToWide(line));
                    }
                }
            }
            CloseHandle(h);
        }
        for (int i = 0; i < 5 && !tailStop_; ++i) Sleep(100);
    }
}

// ---------------------------------------------------------------- commands
std::wstring Host::CommandVia() const {
    if (const json::Value* c = t_.Obj(L"console")) { std::wstring v = c->GetStr(L"via"); if (!v.empty()) return v; }
    return L"";
}

bool Host::Exec(const std::wstring& cmd, const std::wstring& via, std::string* reply) {
    if (cmd.compare(0, 4, L"api:") == 0) return PalApi(cmd.substr(4), L"", L"", reply);
    if (via == L"rcon") return rcon_.Exec(WideToUtf8(cmd), reply);
    if (via == L"telnet") return telnet_.Exec(WideToUtf8(cmd), reply);
    return child_.Write(WideToUtf8(cmd));
}

bool Host::PalApi(const std::wstring& what, const std::wstring& userId, const std::wstring& text, std::string* reply) {
    const json::Value* q = t_.Obj(L"query");
    if (!q) return false;
    int port = _wtoi(Expand(q->GetStr(L"port"), s_, t_).c_str());
    std::string auth = "Authorization: Basic " + http::Base64("admin:" + WideToUtf8(Expand(q->GetStr(L"password"), s_, t_)));
    std::string base = "http://127.0.0.1:" + std::to_string(port) + "/v1/api/";
    json::Value body = json::Value::MakeObject();
    std::string method = "POST";
    if (what == L"announce") body.Set(L"message", json::Value::Str(text));
    else if (what == L"kick" || what == L"ban") { body.Set(L"userid", json::Value::Str(userId)); body.Set(L"message", json::Value::Str(text)); }
    else if (what == L"unban") body.Set(L"userid", json::Value::Str(userId));
    else if (what == L"shutdown") { body.Set(L"waittime", json::Value::Num(1)); body.Set(L"message", json::Value::Str(L"The server is shutting down.")); }
    else if (what == L"players" || what == L"metrics" || what == L"info" || what == L"settings") method = "GET";
    std::string b = method == "POST" ? J(body) : "";
    http::Result r = http::Request(method, base + WideToUtf8(what), b, {auth, "Content-Type: application/json", "Accept: application/json"}, 5000);
    if (reply) *reply = r.body;
    return r.status == 200;
}

void Host::DoAction(const json::Value& req, std::wstring* result, bool* ok) {
    std::wstring action = req.GetStr(L"action"), player = req.GetStr(L"player"), text = req.GetStr(L"text");
    const json::Value* cmds = t_.Obj(L"commands");
    std::wstring tpl = cmds ? cmds->GetStr(action) : L"";
    if (tpl.empty()) { *result = t_.name + L" doesn't support that."; *ok = false; return; }
    std::wstring via = cmds->GetStr(L"via");
    if (tpl.compare(0, 4, L"api:") == 0) {
        std::wstring userId;
        { std::lock_guard<std::mutex> g(m_); auto it = players_.find(player); if (it != players_.end()) userId = it->second.userId; }
        if (userId.empty()) userId = player;   // unban by id typed in
        *ok = PalApi(tpl.substr(4), userId, text);
    } else {
        std::wstring cmd = tpl;
        auto rep = [&](const wchar_t* k, const std::wstring& v) { for (size_t p; (p = cmd.find(k)) != std::wstring::npos;) cmd.replace(p, wcslen(k), v); };
        rep(L"{player}", player);
        rep(L"{text}", text);
        std::string reply;
        OnLine(L"> " + cmd, L"cmd");
        *ok = Exec(cmd, via, &reply);
        if (!reply.empty()) OnLine(StripCodes(Utf8ToWide(reply)), L"info");
    }
    *result = *ok ? L"Done." : L"The server didn't accept that (is it still starting?).";
    if (*ok && (action == L"kick" || action == L"ban")) std::thread([this] { Sleep(1500); QueryPlayers(true); }).detach();
}

// ---------------------------------------------------------------- starting and stopping
bool Host::ResolveLaunch(std::wstring* exe, std::wstring* args, std::wstring* error) {
    const json::Value* l = t_.Obj(L"launch");
    std::wstring dir = s_.FilesDir();
    std::wstring tplArgs = Expand(l->GetStr(L"args"), s_, t_);
    std::wstring batch = l->GetStr(L"batch");
    if (!batch.empty()) {
        // use the game's own start script's java line, with our memory setting
        std::wstring text;
        if (!ReadTextFile(dir + L"\\" + batch, text)) { *error = batch + L" is missing. Try Settings → Repair files."; return false; }
        std::wstring line;
        size_t i = 0;
        while (i < text.size()) {
            size_t e = text.find(L'\n', i);
            if (e == std::wstring::npos) e = text.size();
            std::wstring ln = Trim(text.substr(i, e - i));
            i = e + 1;
            if (IContains(ln, L"java") && IContains(ln, L".exe") && !IStartsWith(ln, L"rem") && !IStartsWith(ln, L"::")) { line = ln; break; }
        }
        if (line.empty()) { *error = L"Couldn't find how to start the server in " + batch + L"."; return false; }
        if (line[0] == L'"') { size_t q = line.find(L'"', 1); *exe = line.substr(1, q - 1); line = line.substr(q + 1); }
        else { size_t sp = line.find(L' '); *exe = line.substr(0, sp); line = sp == std::wstring::npos ? L"" : line.substr(sp); }
        if (exe->size() > 1 && (*exe)[0] == L'.' ) *exe = dir + L"\\" + exe->substr(exe->find_first_not_of(L".\\/"));
        else if (!fs::IsAbsolute(*exe)) *exe = dir + L"\\" + *exe;
        for (auto pct : {L"%*", L"%1", L"%2", L"%3", L"%4", L"%5"}) for (size_t p; (p = line.find(pct)) != std::wstring::npos;) line.erase(p, 2);
        if (t_.ramJava) {
            int ram = s_.ram > 0 ? s_.ram : t_.ramDefault;
            line = std::regex_replace(line, std::wregex(L"-Xmx\\d+[mMgG]"), L"-Xmx" + std::to_wstring(ram) + L"m");
            line = std::regex_replace(line, std::wregex(L"-Xms\\d+[mMgG]"), L"-Xms" + std::to_wstring(std::min(ram, 2048)) + L"m");
        }
        *args = Trim(line) + L" " + tplArgs;
    } else {
        *exe = Expand(l->GetStr(L"exe"), s_, t_);
        if (!fs::IsAbsolute(*exe)) *exe = dir + L"\\" + *exe;
        *args = tplArgs;
    }
    if (!FileExists(*exe)) { *error = PathName(*exe) + L" is missing. The server files may not be installed (Settings → Repair files)."; return false; }
    // collapse the double spaces left by empty settings
    std::wstring a;
    for (wchar_t c : *args) if (!(c == L' ' && !a.empty() && a.back() == L' ')) a += c;
    *args = Trim(a);
    return true;
}

bool Host::StartServer() {
    {
        std::lock_guard<std::mutex> g(m_);
        state_ = St::Starting; message_.clear();
        players_.clear(); idToName_.clear(); pendingId_.clear();
        perf_ = -1; perfCmd_ = -2; ctrlCSent_ = false; stopping_ = false; stopForSleep_ = stopForRestart_ = false;
        startedAt_ = fs::UnixNow(); readyAt_ = 0; sleepingSince_ = 0; restartWarned_ = false;
    }
    BroadcastStatus();
    std::wstring err;
    // settings → the game's config files, every start (so a restore or update can't leave them stale)
    servers::Load(root_, s_);
    if (!WriteConfigFiles(s_, t_, &err)) { SetState(St::Error, err); return false; }
    std::wstring exe, args;
    if (!ResolveLaunch(&exe, &args, &err)) { SetState(St::Error, err); return false; }
    std::vector<std::pair<std::wstring, std::wstring>> env;
    if (const json::Value* e = t_.Obj(L"launch")->Get(L"env"); e && e->type == json::Value::Object)
        for (auto& kv : e->obj) env.push_back({kv.first, Expand(kv.second.s, s_, t_)});
    uint64_t limit = (t_.hasRam && !t_.ramJava && s_.ram > 0) ? (uint64_t)s_.ram * 1024 * 1024 : 0;
    Say(L"Starting " + s_.name + L" (" + t_.name + (s_.installedVersion.empty() ? L"" : L" " + s_.installedVersion) + L")…");
    Log(L"host %ls: %ls %ls", s_.id.c_str(), exe.c_str(), args.c_str());
    if (!child_.Start(exe, args, s_.FilesDir(), env, limit, &err)) { SetState(St::Error, err); return false; }
    if (reader_.joinable()) reader_.join();
    readerDone_ = false;
    HANDLE out = child_.out;
    reader_ = std::thread([this, out] { ReaderLoop(out); });
    std::wstring logFile = t_.raw.GetStr(L"logFile");
    if (!logFile.empty() && !tailer_.joinable()) {
        std::wstring p = fs::Join(s_.FilesDir(), Expand(logFile, s_, t_));
        DeleteFileW(p.c_str());   // start fresh, so old lines aren't read as new
        tailStop_ = false;
        tailer_ = std::thread([this, p] { TailLog(p); });
    }
    // RCON / telnet for player lists and commands
    if (const json::Value* q = t_.Obj(L"query")) {
        int port = _wtoi(Expand(q->GetStr(L"port"), s_, t_).c_str());
        std::string pw = WideToUtf8(Expand(q->GetStr(L"password"), s_, t_));
        if (q->GetStr(L"type") == L"rcon") rcon_.Configure(port, pw);
        if (q->GetStr(L"type") == L"telnet") telnet_.Configure(port, pw);
    }
    if (gaming_) child_.SetLowPriority(true);
    {
        std::lock_guard<std::mutex> g(m_);
        if (s_.backupMinutes > 0 && !nextBackup_) nextBackup_ = fs::UnixNow() + s_.backupMinutes * 60;
    }
    ScheduleRestart();
    if (!hasReady_) SetState(St::Running);
    BroadcastStatus();
    return true;
}

void Host::BeginStop(bool forSleep, bool forRestart) {
    {
        std::lock_guard<std::mutex> g(m_);
        if (stopping_) return;
        stopping_ = true; stopForSleep_ = forSleep; stopForRestart_ = forRestart; ctrlCSent_ = false;
        state_ = St::Stopping;
    }
    const json::Value* st = t_.Obj(L"stop");
    int timeout = st ? (int)st->GetNum(L"timeout", 60) : 60;
    stopDeadline_ = GetTickCount64() + timeout * 1000ull;
    stopHalf_ = GetTickCount64() + timeout * 500ull;
    Say(forSleep ? L"Nobody's online: saving and going to sleep…" : forRestart ? L"Restarting: saving the world first…" : L"Stopping: saving the world first…");
    BroadcastStatus();
    bool sent = false;
    if (!child_.Running()) return;
    if (st) {
        std::wstring cmd = st->GetStr(L"command");
        if (!cmd.empty()) sent = child_.Write(WideToUtf8(cmd));
        if (!sent) if (const json::Value* r = st->Get(L"rcon"); r && r->type == json::Value::Array) {
            for (auto& c : r->arr) { std::string reply; sent = rcon_.Exec(WideToUtf8(c.s), &reply, 15000) || sent; }
        }
        if (!sent) if (const json::Value* r = st->Get(L"telnet"); r && r->type == json::Value::Array) {
            for (auto& c : r->arr) { std::string reply; sent = telnet_.Exec(WideToUtf8(c.s), &reply, 10000) || sent; }
        }
        if (!sent && st->GetStr(L"api") == L"shutdown") {
            PalApi(L"save", L"", L"");
            sent = PalApi(L"shutdown", L"", L"");
        }
        if (!sent && st->GetBool(L"ctrlC", false)) { child_.SendCtrlC(); ctrlCSent_ = true; sent = true; }
    }
    if (!sent) { Say(L"This game has no graceful stop; closing it.", L"warn"); child_.Kill(); }
}

void Host::FinishStopped() {
    // a helper the game started may still hold its console open: give the reader a moment, then end the whole job
    for (int i = 0; i < 30 && !readerDone_; ++i) Sleep(100);
    if (!readerDone_) { Log(L"host %ls: closing leftover processes", s_.id.c_str()); child_.Kill(); }
    if (reader_.joinable()) reader_.join();
    tailStop_ = true;
    if (tailer_.joinable()) tailer_.join();
    child_.Close();
    rcon_.Disconnect();
    telnet_.Disconnect();
    std::lock_guard<std::mutex> g(m_);
    players_.clear();
    usage_ = proc::Usage();
    perf_ = -1;
}

// ---------------------------------------------------------------- players and performance
void Host::MergePlayers(const std::vector<Player>& list, bool fromQuery) {
    bool changed = false;
    std::vector<std::wstring> joined, left;
    {
        std::lock_guard<std::mutex> g(m_);
        std::set<std::wstring> seen;
        for (auto& p : list) {
            seen.insert(p.name);
            auto it = players_.find(p.name);
            if (it == players_.end()) {
                Player np = p; np.since = fs::UnixNow();
                players_[p.name] = np; changed = true; joined.push_back(p.name);
                lastPlayerAt_ = fs::UnixNow(); playedSinceBackup_ = true;
            } else {
                Player& e = it->second;
                if (e.ping != p.ping || e.x != p.x || e.y != p.y || e.level != p.level || e.userId != p.userId) changed = true;
                e.ping = p.ping; e.x = p.x; e.y = p.y; e.loc = p.loc; e.level = p.level;
                if (!p.userId.empty()) e.userId = p.userId;
            }
        }
        for (auto it = players_.begin(); it != players_.end();) {
            if (!seen.count(it->first)) { left.push_back(it->first); it = players_.erase(it); changed = true; }
            else ++it;
        }
        if (!players_.empty()) lastPlayerAt_ = fs::UnixNow();
    }
    // only record history from the query when the log doesn't report joins
    if (fromQuery && !hasJoin_) {
        for (auto& n : joined) { History(L"join", n); Notify(L"join", n + L" joined " + s_.name); }
        for (auto& n : left) { History(L"leave", n); Notify(L"leave", n + L" left " + s_.name); }
    }
    if (changed) BroadcastStatus();
}

static std::vector<std::wstring> SplitNames(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find(L',', i);
        if (e == std::wstring::npos) e = s.size();
        std::wstring n = Trim(s.substr(i, e - i));
        if (!n.empty()) out.push_back(n);
        i = e + 1;
    }
    return out;
}

void Host::QueryPlayers(bool force) {
    const json::Value* q = t_.Obj(L"query");
    if (!q) return;
    {
        std::lock_guard<std::mutex> g(m_);
        if (state_ != St::Running && state_ != St::Starting) return;
    }
    std::wstring type = q->GetStr(L"type");
    std::wregex re;
    bool hasRe = Rx(q->GetStr(L"listPattern"), re);
    bool multi = q->GetBool(L"listMulti", false);
    auto parse = [&](const std::wstring& reply) {
        std::vector<Player> list;
        std::vector<std::wstring> lines;
        size_t i = 0;
        while (i < reply.size()) {
            size_t e = reply.find(L'\n', i);
            if (e == std::wstring::npos) e = reply.size();
            lines.push_back(Trim(reply.substr(i, e - i)));
            i = e + 1;
        }
        for (size_t k = 0; k < lines.size(); ++k) {
            std::wsmatch m;
            if (!hasRe || !std::regex_search(lines[k], m, re)) continue;
            if (q->GetBool(L"listNextLine", false)) {
                if (k + 1 < lines.size()) for (auto& n : SplitNames(lines[k + 1])) { Player p; p.name = n; list.push_back(p); }
                break;
            }
            if (m.size() < 2) continue;
            if (multi) { Player p; p.name = Trim(m[1].str()); if (!p.name.empty()) list.push_back(p); }
            else { for (auto& n : SplitNames(m[1].str())) { Player p; p.name = n; list.push_back(p); } break; }
        }
        return list;
    };
    if (type == L"rcon" || type == L"telnet") {
        std::string reply;
        std::wstring cmd = q->GetStr(L"list");
        bool ok = type == L"rcon" ? rcon_.Exec(WideToUtf8(cmd), &reply) : telnet_.Exec(WideToUtf8(cmd), &reply);
        if (ok) {
            if (q->GetBool(L"ready", false)) {
                std::lock_guard<std::mutex> g(m_);
                if (state_ == St::Starting) { state_ = St::Running; readyAt_ = lastPlayerAt_ = fs::UnixNow(); }
            }
            MergePlayers(parse(StripCodes(Utf8ToWide(reply))), true);
        }
        // performance: try each command once, then keep the one that works
        if (const json::Value* perf = q->Get(L"perf"); ok && perf && perf->type == json::Value::Array) {
            for (int i = 0; i < (int)perf->arr.size(); ++i) {
                if (perfCmd_ >= 0 && perfCmd_ != i) continue;
                if (perfCmd_ == -1) break;
                const json::Value& pc = perf->arr[i];
                std::string r;
                bool got = type == L"rcon" ? rcon_.Exec(WideToUtf8(pc.GetStr(L"cmd")), &r) : telnet_.Exec(WideToUtf8(pc.GetStr(L"cmd")), &r);
                std::wregex pr;
                std::wsmatch m;
                std::wstring text = StripCodes(Utf8ToWide(r));
                if (got && Rx(pc.GetStr(L"pattern"), pr) && std::regex_search(text, m, pr) && m.size() > 1) {
                    double v = _wtof(m[1].str().c_str());
                    if (pc.GetStr(L"kind") == L"mspt") v = v > 0 ? std::min(20.0, 1000.0 / v) : 20.0;
                    std::lock_guard<std::mutex> g(m_);
                    perf_ = v; perfCmd_ = i;
                    break;
                }
                if (i == (int)perf->arr.size() - 1 && perfCmd_ < 0) perfCmd_ = -1;
            }
        }
    } else if (type == L"palworld") {
        std::string reply;
        if (PalApi(L"players", L"", L"", &reply)) {
            if (q->GetBool(L"ready", false)) {
                std::lock_guard<std::mutex> g(m_);
                if (state_ == St::Starting) { state_ = St::Running; readyAt_ = lastPlayerAt_ = fs::UnixNow(); }
            }
            json::Value v;
            std::vector<Player> list;
            if (json::Parser(Utf8ToWide(reply)).Parse(v))
                if (const json::Value* arr = v.Get(L"players"); arr && arr->type == json::Value::Array)
                    for (auto& e : arr->arr) {
                        Player p;
                        p.name = e.GetStr(L"name"); p.userId = e.GetStr(L"userId"); p.id = e.GetStr(L"playerId");
                        p.ping = (int)e.GetNum(L"ping", -1);
                        p.x = e.GetNum(L"location_x", 0); p.y = e.GetNum(L"location_y", 0); p.loc = e.Get(L"location_x") != nullptr;
                        p.level = (int)e.GetNum(L"level", 0);
                        if (!p.name.empty()) list.push_back(p);
                    }
            MergePlayers(list, true);
            std::string metrics;
            if (PalApi(L"metrics", L"", L"", &metrics)) {
                json::Value mv;
                if (json::Parser(Utf8ToWide(metrics)).Parse(mv)) { std::lock_guard<std::mutex> g(m_); perf_ = mv.GetNum(L"serverfps", -1); }
            }
        }
    } else if (type == L"stdin" && force) {
        // the list command prints to the console; only on request (Players tab) and once after start
        { std::lock_guard<std::mutex> g(m_); capturing_ = true; captured_.clear(); }
        child_.Write(WideToUtf8(q->GetStr(L"list")));
        Sleep(1500);
        std::wstring all;
        { std::lock_guard<std::mutex> g(m_); capturing_ = false; for (auto& l : captured_) all += l + L"\n"; }
        MergePlayers(parse(all), true);
    }
    BroadcastStatus();
}

void Host::PollLoop() {
    int n = 0;
    bool listedOnce = false;
    while (!quit_) {
        Sleep(1000);
        ++n;
        St st;
        { std::lock_guard<std::mutex> g(m_); st = state_; }
        if (st != St::Running && st != St::Starting) { listedOnce = false; continue; }
        if (n % 2 == 0) {
            proc::Usage u = meter_.Sample(child_.job);
            { std::lock_guard<std::mutex> g(m_); usage_ = u; }
        }
        bool doQuery = (n % 10 == 0) || (st == St::Starting && n % 3 == 0);
        const json::Value* q = t_.Obj(L"query");
        if (q && q->GetStr(L"type") == L"stdin") {
            if (st == St::Running && !listedOnce) { listedOnce = true; QueryPlayers(true); }
            doQuery = false;
        }
        if (doQuery) QueryPlayers(false);
        if (n % 2 == 0) BroadcastStatus();
    }
}

// ---------------------------------------------------------------- backups
void Host::RunBackup(const std::wstring& label, bool scheduled) {
    if (backingUp_.exchange(true)) return;
    BroadcastStatus();
    bool running = child_.Running();
    const json::Value* b = t_.Obj(L"backup");
    std::wstring via = t_.Obj(L"commands") ? t_.Obj(L"commands")->GetStr(L"via") : L"";
    if (running && b) {
        if (const json::Value* before = b->Get(L"before"); before && before->type == json::Value::Array) {
            std::wstring wait = b->GetStr(L"waitFor");
            if (Rx(wait, reWait_)) { std::lock_guard<std::mutex> g(waitM_); waiting_ = true; waitHit_ = false; }
            for (auto& c : before->arr) { std::string r; Exec(c.s, via, &r); }
            if (waiting_) {
                int timeout = (int)b->GetNum(L"timeout", 60);
                std::wstring poll = b->GetStr(L"poll");
                std::unique_lock<std::mutex> lk(waitM_);
                auto until = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
                while (!waitHit_ && std::chrono::steady_clock::now() < until) {
                    if (!poll.empty()) { lk.unlock(); std::string r; Exec(poll, via, &r); lk.lock(); }
                    waitCv_.wait_for(lk, std::chrono::seconds(2));
                }
                if (!waitHit_) Log(L"backup %ls: the save confirmation didn't appear in time; backing up anyway", s_.id.c_str());
                waiting_ = false;
            } else {
                Sleep(3000);   // give the save a moment
            }
        }
    }
    Say(L"Backing up the world…");
    std::wstring path, err;
    int lastPct = -1;
    bool ok = backup::Create(s_, t_, label, [&](uint64_t done, uint64_t total) {
        int pct = total ? (int)(done * 100 / total) : 0;
        if (pct / 25 != lastPct / 25) { lastPct = pct; }
        return !quit_;
    }, &path, &err);
    if (running && b)
        if (const json::Value* after = b->Get(L"after"); after && after->type == json::Value::Array)
            for (auto& c : after->arr) { std::string r; Exec(c.s, via, &r); }
    {
        std::lock_guard<std::mutex> g(m_);
        if (ok) { lastBackup_ = fs::UnixNow(); playedSinceBackup_ = false; }
        if (s_.backupMinutes > 0) nextBackup_ = fs::UnixNow() + s_.backupMinutes * 60;
    }
    backingUp_ = false;
    if (ok) { Say(L"Backup saved: " + PathName(path) + L" (" + FormatBytes(fs::TreeSize(path)) + L")"); Notify(L"backup", L"Backed up " + s_.name + L"."); }
    else { Say(L"Backup failed: " + err, L"error"); Notify(L"backup-failed", L"Backup of " + s_.name + L" failed: " + err); }
    (void)scheduled;
    BroadcastStatus();
}

void Host::ScheduleRestart() {
    long long next = 0, now = fs::UnixNow();
    if (s_.restartHours > 0) next = (startedAt_ ? startedAt_ : now) + s_.restartHours * 3600LL;
    int hh, mm;
    if (!s_.restartAt.empty() && swscanf(s_.restartAt.c_str(), L"%d:%d", &hh, &mm) == 2) {
        time_t t = (time_t)now;
        struct tm lt; localtime_s(&lt, &t);
        lt.tm_hour = hh; lt.tm_min = mm; lt.tm_sec = 0;
        long long at = (long long)mktime(&lt);
        if (at <= now + 60) at += 24 * 3600;
        if (!next || at < next) next = at;
    }
    std::lock_guard<std::mutex> g(m_);
    nextRestart_ = next;
    restartWarned_ = false;
}

// ---------------------------------------------------------------- sleep and wake
static int ReadVarInt(const std::string& b, size_t& i) {
    int v = 0, shift = 0;
    while (i < b.size()) {
        uint8_t c = (uint8_t)b[i++];
        v |= (c & 0x7F) << shift;
        if (!(c & 0x80)) return v;
        shift += 7;
        if (shift > 28) break;
    }
    return -1;
}
static std::string VarInt(int v) {
    std::string o;
    uint32_t u = (uint32_t)v;
    do { uint8_t c = u & 0x7F; u >>= 7; if (u) c |= 0x80; o.push_back((char)c); } while (u);
    return o;
}
static std::string McString(const std::string& s) { return VarInt((int)s.size()) + s; }
static std::string McPacket(int id, const std::string& payload) { std::string body = VarInt(id) + payload; return VarInt((int)body.size()) + body; }

void Host::WakeLoop() {
    sock::Init();
    std::wstring mode = t_.sleepWake;
    int port = s_.Port();
    bool udp = mode == L"udp";
    SOCKET ls = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, udp ? IPPROTO_UDP : IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = INADDR_ANY;
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof(yes));
    bool bound = false;
    for (int i = 0; i < 20 && !wakeStop_; ++i) { if (bind(ls, (sockaddr*)&a, sizeof(a)) == 0) { bound = true; break; } Sleep(500); }
    if (!bound) { Log(L"host %ls: can't listen on port %d for wake-ups", s_.id.c_str(), port); closesocket(ls); return; }
    if (!udp) listen(ls, 8);
    wakeSock_ = (uintptr_t)ls;
    while (!wakeStop_) {
        fd_set r; FD_ZERO(&r); FD_SET(ls, &r);
        timeval tv{0, 300000};
        if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
        if (udp) {
            char buf[1500];
            sockaddr_in from{}; int fl = sizeof(from);
            if (recvfrom(ls, buf, sizeof(buf), 0, (sockaddr*)&from, &fl) > 0) {
                Say(L"Someone is connecting: waking up.");
                std::lock_guard<std::mutex> g(m_); wakeWanted_ = true;
                break;
            }
            continue;
        }
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        bool wake = true;
        if (mode == L"minecraft") {
            // answer the server-list ping ourselves; wake only when someone actually joins
            std::string in;
            char buf[2048];
            DWORD start = GetTickCount();
            int next = -1, protocol = 0;
            while (GetTickCount() - start < 3000) {
                fd_set cr; FD_ZERO(&cr); FD_SET(c, &cr); timeval ct{0, 200000};
                if (select(0, &cr, nullptr, nullptr, &ct) <= 0) continue;
                int n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                in.append(buf, n);
                size_t i = 0;
                int len = ReadVarInt(in, i);
                if (len < 0 || in.size() < i + len) continue;
                size_t j = i;
                int id = ReadVarInt(in, j);
                if (id == 0) { protocol = ReadVarInt(in, j); int sl = ReadVarInt(in, j); j += sl + 2; next = ReadVarInt(in, j); }
                break;
            }
            if (next == 1) {
                wake = false;
                std::string status = "{\"version\":{\"name\":\"Sleeping\",\"protocol\":" + std::to_string(protocol) + "},"
                                     "\"players\":{\"max\":" + std::to_string(std::max(1, _wtoi(s_.Get(L"maxPlayers").c_str()))) + ",\"online\":0},"
                                     "\"description\":{\"text\":\"\\u00a76Sleeping \\u00a77- join to wake it up\"}}";
                std::string out = McPacket(0, McString(status));
                send(c, out.data(), (int)out.size(), 0);
                // ping/pong
                fd_set cr; FD_ZERO(&cr); FD_SET(c, &cr); timeval ct{2, 0};
                if (select(0, &cr, nullptr, nullptr, &ct) > 0) {
                    int n = recv(c, buf, sizeof(buf), 0);
                    std::string p(buf, n > 0 ? n : 0);
                    size_t k = 0;
                    while (k < p.size()) {
                        size_t s0 = k;
                        int l = ReadVarInt(p, k);
                        if (l <= 0 || p.size() < k + l) break;
                        size_t q = k;
                        int pid = ReadVarInt(p, q);
                        if (pid == 1) { std::string pong = p.substr(s0, k + l - s0); send(c, pong.data(), (int)pong.size(), 0); }
                        k += l;
                    }
                }
            } else if (next == 2) {
                std::string msg = "{\"text\":\"Waking the server up. Try again in a minute!\"}";
                std::string out = McPacket(0, McString(msg));
                send(c, out.data(), (int)out.size(), 0);
            } else wake = next != -1;
        }
        shutdown(c, SD_BOTH);
        closesocket(c);
        if (wake) {
            Say(L"Someone is joining: waking up.");
            std::lock_guard<std::mutex> g(m_); wakeWanted_ = true;
            break;
        }
    }
    closesocket(ls);
    wakeSock_ = ~(uintptr_t)0;
}

void Host::StartWakeListener() {
    if (t_.sleepWake.empty()) return;
    wakeStop_ = false;
    if (wake_.joinable()) wake_.join();
    wake_ = std::thread([this] { WakeLoop(); });
}
void Host::StopWakeListener() {
    wakeStop_ = true;
    if (wake_.joinable()) wake_.join();
}

void Host::MapPorts() {
    if (!s_.upnp) { std::lock_guard<std::mutex> g(m_); upnp_ = L"off"; return; }
    std::wstring err;
    bool ok = netx::upnp::MapServer(s_, t_, &err);
    {
        std::lock_guard<std::mutex> g(m_);
        upnp_ = ok ? L"ok" : (netx::upnp::Available() ? L"failed" : L"unavailable");
        upnpError_ = err;
        upnpAt_ = fs::UnixNow();
    }
    BroadcastStatus();
}

// ---------------------------------------------------------------- main loop
void Host::Tick() {
    long long now = fs::UnixNow();
    St st;
    bool stopWanted, restartWanted, sleepWanted, wakeWanted;
    {
        std::lock_guard<std::mutex> g(m_);
        st = state_; stopWanted = stopWanted_; restartWanted = restartWanted_; sleepWanted = sleepWanted_; wakeWanted = wakeWanted_;
        restartWanted_ = sleepWanted_ = false;
    }
    // while sleeping: wait for a wake-up
    if (st == St::Sleeping) {
        if (stopWanted) { StopWakeListener(); quit_ = true; return; }
        if (wakeWanted) {
            { std::lock_guard<std::mutex> g(m_); wakeWanted_ = false; }
            StopWakeListener();
            if (!StartServer()) quit_ = true;
        }
        return;
    }
    { std::lock_guard<std::mutex> g(m_); wakeWanted_ = false; }

    // the server process ended
    if (!child_.Running() && child_.process) {
        DWORD code = child_.ExitCode();
        bool expected;
        { std::lock_guard<std::mutex> g(m_); expected = stopping_; }
        FinishStopped();
        if (expected) {
            bool sleep = stopForSleep_, restart = stopForRestart_;
            { std::lock_guard<std::mutex> g(m_); stopping_ = false; }
            if (stopWanted) { Say(L"Stopped."); SetState(St::Stopped); quit_ = true; return; }
            if (restart) { Say(L"Starting again…"); if (!StartServer()) quit_ = true; return; }
            if (sleep) {
                { std::lock_guard<std::mutex> g(m_); sleepingSince_ = now; }
                SetState(St::Sleeping, t_.sleepWake.empty() ? L"" : L"Wakes up when someone joins");
                StartWakeListener();
                return;
            }
            SetState(St::Stopped); quit_ = true; return;
        }
        // a crash
        crashes_.push_back(now);
        while (!crashes_.empty() && now - crashes_.front() > 15 * 60) crashes_.pop_front();
        wchar_t codeText[32]; swprintf(codeText, 32, L"0x%lX", code);
        std::wstring msg = L"The server stopped unexpectedly (exit code " + std::wstring(code < 256 ? std::to_wstring(code) : codeText) + L").";
        Say(msg, L"error");
        // keep the last lines for later
        {
            std::wstring dump;
            std::lock_guard<std::mutex> g(m_);
            size_t from = ring_.size() > 200 ? ring_.size() - 200 : 0;
            for (size_t i = from; i < ring_.size(); ++i) dump += ring_[i].s + L"\r\n";
            EnsureDir(root_ + L"\\logs");
            WriteFileAtomic(root_ + L"\\logs\\crash-" + fs::TimeStamp() + L".log", WideToUtf8(dump));
        }
        if (stopWanted) { SetState(St::Stopped); quit_ = true; return; }
        if (s_.autoRestart && crashes_.size() < 3) {
            Notify(L"crash", s_.name + L" crashed. Restarting it.");
            SetState(St::Restarting, msg + L" Restarting in 5 seconds.");
            for (int i = 0; i < 50 && !quit_; ++i) Sleep(100);
            if (!StartServer()) quit_ = true;
        } else {
            Notify(L"crash", s_.name + L" crashed" + (crashes_.size() >= 3 ? L" 3 times in 15 minutes, so Outpost stopped restarting it." : L"."));
            SetState(St::Error, msg + (crashes_.size() >= 3 ? L" It crashed 3 times in 15 minutes, so it wasn't restarted. Check the console." : L""));
            quit_ = true;
        }
        return;
    }

    // stopping: escalate if it takes too long
    if (st == St::Stopping) {
        ULONGLONG t = GetTickCount64();
        const json::Value* stp = t_.Obj(L"stop");
        if (!ctrlCSent_ && t > stopHalf_ && stp && stp->GetBool(L"ctrlC", false)) { child_.SendCtrlC(); ctrlCSent_ = true; }
        if (t > stopDeadline_) { Say(L"The server didn't stop in time; closing it.", L"warn"); child_.Kill(); }
        return;
    }
    if (stopWanted) { BeginStop(false, false); return; }
    if (restartWanted) { BeginStop(false, true); return; }
    if (sleepWanted && st == St::Running) { BeginStop(true, false); return; }
    if (st != St::Running) return;

    // scheduled restart, with a one-minute warning
    long long nr; { std::lock_guard<std::mutex> g(m_); nr = nextRestart_; }
    if (nr && now >= nr - 60 && !restartWarned_) {
        restartWarned_ = true;
        if (t_.HasCommand(L"say")) { json::Value req = json::Value::MakeObject(); req.Set(L"action", json::Value::Str(L"say")); req.Set(L"text", json::Value::Str(L"The server restarts in 1 minute.")); std::wstring r; bool ok; DoAction(req, &r, &ok); }
    }
    if (nr && now >= nr) { Say(L"Scheduled restart."); BeginStop(false, true); return; }

    // automatic backups (skipped while you're playing a game on this PC, and when nobody played since the last one)
    long long nb; { std::lock_guard<std::mutex> g(m_); nb = nextBackup_; }
    if (s_.backupMinutes > 0 && nb && now >= nb && !backingUp_) {
        bool skip = gaming_ && s_.lowerPriorityWhileGaming;
        bool idle = !playedSinceBackup_ && lastBackup_ != 0;
        if (skip || idle) { std::lock_guard<std::mutex> g(m_); nextBackup_ = now + (skip ? 5 * 60 : s_.backupMinutes * 60); }
        else std::thread([this] { RunBackup(L"automatic", true); }).detach();
    }

    // sleep when empty
    if (s_.sleepWhenEmpty && !t_.sleepWake.empty() && !backingUp_) {
        bool empty; long long last;
        { std::lock_guard<std::mutex> g(m_); empty = players_.empty(); last = lastPlayerAt_; }
        if (empty && last && now - last >= std::max(1, s_.sleepMinutes) * 60LL) { BeginStop(true, false); return; }
    }

    // UPnP leases: refresh every 30 minutes
    if (s_.upnp && now - upnpAt_ > 30 * 60) { upnpAt_ = now; std::thread([this] { MapPorts(); }).detach(); }
}

int Host::Run() {
    if (!LoadDef()) { Log(L"host: can't load the server in %ls", root_.c_str()); return 2; }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, MutexName(s_.id).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) { Log(L"host %ls: already running", s_.id.c_str()); return 0; }
    Log(L"host %ls: starting (Outpost %ls, pid %lu)", s_.id.c_str(), OUTPOST_VERSION, GetCurrentProcessId());
    DeleteFileW((root_ + L"\\notify.jsonl").c_str());
    if (!server_.Start(PipeName(s_.id), [this](std::shared_ptr<pipe::Conn> c) { OnClient(c); })) { CloseHandle(mutex); return 3; }
    std::thread([this] { MapPorts(); }).detach();
    upnpAt_ = fs::UnixNow();
    std::thread poll([this] { PollLoop(); });
    bool started = StartServer();
    int gameCheck = 0;
    while (started && !quit_) {
        Tick();
        if (quit_) break;
        if (++gameCheck % 20 == 0 && s_.lowerPriorityWhileGaming) {
            bool g = proc::GameInForeground();
            if (g != gaming_) {
                gaming_ = g;
                if (child_.Running()) child_.SetLowPriority(g);
                Log(L"host %ls: %ls", s_.id.c_str(), g ? L"a game is in front: lower priority, backups paused" : L"back to normal priority");
                BroadcastStatus();
            }
        }
        Sleep(250);
    }
    quit_ = true;
    if (child_.Running()) { BeginStop(false, false); for (int i = 0; i < 1200 && child_.Running(); ++i) Sleep(100); child_.Kill(); }
    FinishStopped();
    StopWakeListener();
    if (s_.upnp) netx::upnp::UnmapServer(s_, t_);
    BroadcastStatus();
    Sleep(300);   // let the last status reach the window
    server_.Stop();
    if (poll.joinable()) poll.join();
    if (logFile_ != INVALID_HANDLE_VALUE) CloseHandle(logFile_);
    Log(L"host %ls: exiting", s_.id.c_str());
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return state_ == St::Error ? 1 : 0;
}

// The host needs a (hidden) window so Windows tells it about sign-out and shutdown: it stops the server properly first.
static Host* g_host = nullptr;
static LRESULT CALLBACK HostWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_QUERYENDSESSION) {
        ShutdownBlockReasonCreate(h, L"Saving your game server's world");
        if (g_host) g_host->RequestStop(L"Windows is shutting down");
        return TRUE;
    }
    if (msg == WM_ENDSESSION && wp) {
        for (int i = 0; i < 300 && g_host && g_host->ChildRunning(); ++i) Sleep(100);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int Main(const std::wstring& root) {
    SetProcessShutdownParameters(0x3FF, 0);   // be told early at shutdown
    SetConsoleCtrlHandler(nullptr, FALSE);     // games we start inherit this: make sure Ctrl+C reaches them
    Host h(root);
    g_host = &h;
    WNDCLASSW wc{};
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Outpost.larosa023.Host";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Outpost host", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    int code = 0;
    std::atomic<bool> done{false};
    DWORD mainTid = GetCurrentThreadId();
    std::thread worker([&] { code = h.Run(); done = true; if (!hwnd || !PostMessageW(hwnd, WM_CLOSE, 0, 0)) PostThreadMessageW(mainTid, WM_QUIT, 0, 0); });
    MSG msg;
    while (!done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_CLOSE && msg.hwnd == hwnd && done) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (done) break;
    }
    worker.join();
    if (hwnd) DestroyWindow(hwnd);
    g_host = nullptr;
    return code;
}

} // namespace host
