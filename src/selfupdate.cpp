// Outpost - Outpost's own updater (see selfupdate.h)
#include "selfupdate.h"
#include "net.h"
#include "util.h"
#include "config.h"
#include "json.h"
#include <thread>
#include <atomic>
#include <shellapi.h>
#include <algorithm>

namespace selfupdate {

static std::atomic<bool> s_busy{false};

static void Post(HWND hwnd, SelfUpdateState* s) { if (!hwnd || !PostMessageW(hwnd, WM_OP_SELF, 0, (LPARAM)s)) delete s; }
static void PostState(HWND h, SelfUpdateState::Kind k, const std::wstring& ver = L"", const std::wstring& msg = L"", int pct = -1) {
    auto* s = new SelfUpdateState; s->kind = k; s->version = ver; s->message = msg; s->percent = pct; Post(h, s);
}

std::wstring ExpectedHashFromSums(const std::string& sums, const std::wstring& assetName) {
    std::wstring text = Utf8ToWide(sums);
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = Trim(text.substr(i, e - i));
        i = e + 1;
        size_t sp = line.find_first_of(L" \t");
        if (sp == std::wstring::npos) continue;
        std::wstring hash = Lower(line.substr(0, sp)), name = Trim(line.substr(sp));
        if (!name.empty() && name[0] == L'*') name.erase(0, 1);
        if (hash.size() == 64 && IEquals(name, assetName)) return hash;
    }
    return L"";
}

void Check(HWND notify, bool force) {
    if (s_busy.exchange(true)) return;
    bool beta = g_cfg.beta;
    std::thread([notify, beta, force] {
        PostState(notify, SelfUpdateState::Checking);
        net::Response r;
        std::wstring url = net::ApiBase() + L"/repos/" OUTPOST_REPO L"/releases?per_page=10";
        std::wstring err;
        json::Value root;
        if (!net::Get(url, r, L"", true)) err = r.error;
        else if (r.status != 200) err = r.status == 403 || r.status == 429 ? L"GitHub's hourly limit was reached; Outpost will try again later." : L"GitHub answered HTTP " + std::to_wstring(r.status) + L".";
        else if (!json::Parser(Utf8ToWide(r.body)).Parse(root) || root.type != json::Value::Array) err = L"GitHub sent an unexpected reply.";
        if (!err.empty()) { Log(L"update check: %ls", err.c_str()); PostState(notify, SelfUpdateState::Error, L"", err); s_busy = false; return; }

        const json::Value* best = nullptr;
        std::wstring bestVer;
        for (auto& rel : root.arr) {
            if (rel.GetBool(L"draft", false)) continue;
            std::wstring ver = NormalizeVersion(rel.GetStr(L"tag_name"));
            bool pre = rel.GetBool(L"prerelease", false) || ver.find(L'-') != std::wstring::npos;
            if (ver.empty() || (pre && !beta) || (!force && IsSkippedVersion(ver))) continue;
            if (!best || CompareVersions(ver, bestVer) > 0) { best = &rel; bestVer = ver; }
        }
        if (!best || CompareVersions(bestVer, OUTPOST_VERSION) <= 0) { PostState(notify, SelfUpdateState::UpToDate); s_busy = false; return; }
        std::wstring exeUrl, sumsUrl, digest;
        uint64_t size = 0;
        if (const json::Value* assets = best->Get(L"assets"))
            for (auto& a : assets->arr) {
                std::wstring n = a.GetStr(L"name");
                if (IEquals(n, L"Outpost.exe")) { exeUrl = a.GetStr(L"browser_download_url"); size = (uint64_t)a.GetNum(L"size", 0); std::wstring d = a.GetStr(L"digest"); if (IStartsWith(d, L"sha256:")) digest = Lower(d.substr(7)); }
                if (IEquals(n, L"SHA256SUMS.txt")) sumsUrl = a.GetStr(L"browser_download_url");
            }
        if (exeUrl.empty()) { PostState(notify, SelfUpdateState::UpToDate); s_busy = false; return; }
        std::wstring expected = digest;
        if (!sumsUrl.empty()) {
            net::Response sr;
            if (net::Get(sumsUrl, sr) && sr.status == 200) {
                std::wstring h = ExpectedHashFromSums(sr.body, L"Outpost.exe");
                if (!h.empty() && !expected.empty() && h != expected) { PostState(notify, SelfUpdateState::Error, bestVer, L"The checksums for Outpost " + bestVer + L" don't agree. Not installing it."); s_busy = false; return; }
                if (!h.empty()) expected = h;
            }
        }
        if (expected.empty()) { PostState(notify, SelfUpdateState::Error, bestVer, L"Outpost " + bestVer + L" has no checksum, so it can't be verified."); s_busy = false; return; }
        std::wstring path = DataDir() + L"\\updates\\" + bestVer + L"\\Outpost.exe";
        bool ok = FileExists(path) && Sha256File(path) == expected;
        if (!ok) {
            int last = -2;
            EnsureDir(PathParent(path));
            ok = net::Download(exeUrl, path, [&](uint64_t done, uint64_t total) {
                if (!total) total = size;
                int pct = total ? (int)(done * 100 / total) : -1;
                if (pct != last) { last = pct; PostState(notify, SelfUpdateState::Downloading, bestVer, L"", pct); }
                return true;
            }, &err);
            if (ok && Sha256File(path) != expected) { DeleteFileW(path.c_str()); ok = false; err = L"The download didn't match its checksum."; }
        }
        if (!ok) { PostState(notify, SelfUpdateState::Error, bestVer, err); s_busy = false; return; }
        Log(L"self-update: %ls downloaded and verified", bestVer.c_str());
        auto* s = new SelfUpdateState;
        s->kind = SelfUpdateState::Ready; s->version = bestVer; s->notes = best->GetStr(L"body"); s->staged = path;
        Post(notify, s);
        s_busy = false;
    }).detach();
}

bool Apply(HWND hwnd, const std::wstring& newExe, const std::wstring& version, const std::wstring& restoreArgs, std::wstring* error) {
    std::wstring exe = ExePath(), old = exe + L".old";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = L"Outpost can't update itself in " + ExeDir() + L" (no permission). Move Outpost.exe to a folder you own, or use the installer.";
        return false;
    }
    if (!CopyFileW(newExe.c_str(), exe.c_str(), FALSE)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        *error = L"Couldn't put the new Outpost.exe in place.";
        return false;
    }
    wchar_t evName[96];
    swprintf(evName, 96, L"Local\\Outpost.larosa023.Healthy.%lu", GetCurrentProcessId());
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, evName);
    ShowWindow(hwnd, SW_HIDE);
    ReleaseInstanceMutex();
    wchar_t args[256];
    swprintf(args, 256, L"--after-update %ls --healthy-event %ls --old-pid %lu", OUTPOST_VERSION, evName, GetCurrentProcessId());
    std::wstring cmd = L"\"" + exe + L"\" " + args + L" " + restoreArgs;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    bool started = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi) != 0;
    bool healthy = false;
    if (started) {
        CloseHandle(pi.hThread);
        HANDLE waits[2] = {ev, pi.hProcess};
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 20000);
        healthy = w == WAIT_OBJECT_0;
        if (!healthy) { Log(L"self-update: %ls didn't start (%lu); rolling back", version.c_str(), w); TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); }
        CloseHandle(pi.hProcess);
    }
    if (ev) CloseHandle(ev);
    if (healthy) { Log(L"self-update: handed over to Outpost %ls", version.c_str()); return true; }
    DeleteFileW(exe.c_str());
    MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
    ReacquireInstanceMutex();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    *error = L"Outpost " + version + L" didn't start, so you're still on " + OUTPOST_VERSION + L". Outpost will skip that version.";
    return false;
}

AfterUpdate ParseArgs(const wchar_t* cmdLine) {
    AfterUpdate a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"x", &argc);
    if (!argv) return a;
    for (int i = 0; i + 1 < argc; ++i) {
        std::wstring k = argv[i];
        if (k == L"--after-update") { a.active = true; a.fromVersion = argv[i + 1]; }
        else if (k == L"--healthy-event") a.healthyEvent = argv[i + 1];
        else if (k == L"--old-pid") a.oldPid = (DWORD)_wtoi(argv[i + 1]);
    }
    LocalFree(argv);
    return a;
}

void SignalHealthy(const AfterUpdate& a) {
    if (!a.active || a.healthyEvent.empty()) return;
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, a.healthyEvent.c_str())) { SetEvent(ev); CloseHandle(ev); }
}

static std::wstring VersionsDir() { return DataDir() + L"\\versions"; }

void FinishAfterUpdate(const AfterUpdate& a) {
    if (!a.active) return;
    AfterUpdate copy = a;
    std::thread([copy] {
        if (copy.oldPid) if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, copy.oldPid)) { WaitForSingleObject(h, 15000); CloseHandle(h); }
        std::wstring old = ExePath() + L".old";
        if (!FileExists(old)) return;
        std::wstring ver = NormalizeVersion(copy.fromVersion);
        if (!ver.empty() && ver.find_first_of(L"\\/:") == std::wstring::npos) {
            std::wstring dir = VersionsDir() + L"\\" + ver;
            EnsureDir(dir);
            CopyFileW(old.c_str(), (dir + L"\\Outpost.exe").c_str(), FALSE);
        }
        for (int i = 0; i < 10 && FileExists(old); ++i) { if (DeleteFileW(old.c_str())) break; Sleep(1000); }
        if (FileExists(old)) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        auto prev = PreviousVersions();
        for (size_t i = 2; i < prev.size(); ++i) { DeleteFileW(prev[i].exe.c_str()); RemoveDirectoryW(PathParent(prev[i].exe).c_str()); }
    }).detach();
}

std::vector<Previous> PreviousVersions() {
    std::vector<Previous> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((VersionsDir() + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring exe = VersionsDir() + L"\\" + n + L"\\Outpost.exe";
        if (FileExists(exe) && CompareVersions(n, OUTPOST_VERSION) != 0) out.push_back({n, exe});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const Previous& a, const Previous& b) { return CompareVersions(a.version, b.version) > 0; });
    return out;
}

} // namespace selfupdate
