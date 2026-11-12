// Outpost - world backups
#include "backup.h"
#include "fsutil.h"
#include "util.h"
#include "json.h"
#include <algorithm>

namespace backup {

std::wstring Dir(const ServerDef& s) {
    return s.backupDir.empty() ? DataDir() + L"\\backups\\" + s.id : ExpandEnv(s.backupDir) + L"\\" + s.id;
}

static std::vector<std::wstring> SavePaths(const ServerDef& s, const Template& t) {
    std::vector<std::wstring> out;
    if (const json::Value* a = t.Obj(L"saves"); a && a->type == json::Value::Array)
        for (auto& p : a->arr) { std::wstring e = Expand(p.s, s, t); if (!e.empty()) out.push_back(e); }
    return out;
}

static long long FileTimeUnix(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

std::vector<BackupInfo> List(const ServerDef& s) {
    std::vector<BackupInfo> out;
    std::wstring dir = Dir(s);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*.zip").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        BackupInfo b;
        b.path = dir + L"\\" + fd.cFileName;
        b.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        b.time = FileTimeUnix(fd.ftLastWriteTime);
        // "<id>-20261112-153000-<label>.zip"
        std::wstring n = fd.cFileName;
        n = n.substr(0, n.size() - 4);
        size_t p = n.find(L'-', s.id.size() + 1 + 8 + 1);
        b.label = p == std::wstring::npos ? L"backup" : n.substr(p + 1);
        for (auto& c : b.label) if (c == L'_') c = L' ';
        out.push_back(b);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.time > b.time; });
    return out;
}

bool Create(const ServerDef& s, const Template& t, const std::wstring& label, const archive::Progress& progress,
            std::wstring* outPath, std::wstring* error) {
    std::vector<archive::Entry> entries;
    uint64_t bytes = 0;
    json::Value man = json::Value::MakeObject(), ext = json::Value::MakeArray();
    man.Set(L"schema", json::Value::Num(1));
    man.Set(L"server", json::Value::Str(s.id));
    man.Set(L"name", json::Value::Str(s.name));
    man.Set(L"game", json::Value::Str(s.game));
    man.Set(L"version", json::Value::Str(s.installedVersion));
    man.Set(L"created", json::Value::Str(fs::IsoNow()));
    man.Set(L"label", json::Value::Str(label));
    int ei = 0;
    for (auto& p : SavePaths(s, t)) {
        std::wstring full = fs::Join(s.FilesDir(), p);
        if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (fs::IsAbsolute(ExpandEnv(p))) {
            std::wstring key = L"_external/" + std::to_wstring(ei++);
            json::Value e = json::Value::MakeObject();
            e.Set(L"zip", json::Value::Str(key));
            e.Set(L"path", json::Value::Str(p));     // unexpanded, so a restore on another PC lands in the right place
            ext.arr.push_back(e);
            archive::Collect(full, full, key, entries, &bytes);
        } else {
            std::wstring rel = p;
            for (auto& c : rel) if (c == L'\\') c = L'/';
            archive::Collect(s.FilesDir(), full, L"files/" + rel, entries, &bytes);
        }
    }
    man.Set(L"external", ext);
    if (entries.empty()) { *error = L"There's nothing to back up yet (the world hasn't been created)."; return false; }
    uint64_t free = fs::FreeSpace(Dir(s));
    if (free && free < bytes + 64ull * 1024 * 1024) { *error = L"Not enough free space in " + Dir(s) + L" for a backup (" + FormatBytes(bytes) + L")."; return false; }
    std::wstring l = label;
    for (auto& c : l) if (!iswalnum(c)) c = L'_';
    std::wstring path = Dir(s) + L"\\" + s.id + L"-" + fs::TimeStamp() + L"-" + l + L".zip";
    std::wstring mj;
    json::Write(man, mj);
    if (!archive::Create(path, entries, WideToUtf8(mj), progress, error)) return false;
    if (outPath) *outPath = path;
    Log(L"backup %ls: %ls (%ls)", s.id.c_str(), PathName(path).c_str(), FormatBytes(fs::TreeSize(path)).c_str());
    Prune(s);
    return true;
}

void Prune(const ServerDef& s) {
    if (s.keepBackups <= 0) return;
    auto list = List(s);
    long long now = fs::UnixNow();
    int kept = 0;
    for (auto& b : list) {
        bool safety = b.label.find(L"before") != std::wstring::npos && now - b.time < 24 * 3600;
        if (safety) continue;
        if (++kept > s.keepBackups) { DeleteFileW(b.path.c_str()); Log(L"backup %ls: removed old %ls", s.id.c_str(), PathName(b.path).c_str()); }
    }
}

bool Restore(const ServerDef& s, const Template& t, const std::wstring& zip, const archive::Progress& progress, std::wstring* error) {
    std::string mtext;
    json::Value man;
    if (!archive::ReadEntry(zip, L"outpost-backup.json", mtext) || !json::Parser(Utf8ToWide(mtext)).Parse(man)) {
        *error = L"That file isn't an Outpost backup.";
        return false;
    }
    if (!IEquals(man.GetStr(L"game"), s.game)) { *error = L"That backup is from a different game (" + man.GetStr(L"game") + L")."; return false; }
    // 1. safety backup of what's there now (skipped if there's nothing yet)
    std::wstring safety, err;
    if (!Create(s, t, L"before restore", progress, &safety, &err) && err.find(L"nothing to back up") == std::wstring::npos) {
        *error = L"The safety backup failed, so nothing was changed: " + err;
        return false;
    }
    // 2. move the current saves aside
    std::vector<std::pair<std::wstring, std::wstring>> moved;
    for (auto& p : SavePaths(s, t)) {
        std::wstring full = fs::Join(s.FilesDir(), p);
        if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::wstring aside = full + L".outpost-old";
        fs::DeleteTree(aside);
        if (!MoveFileExW(full.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            for (auto& m : moved) MoveFileExW(m.second.c_str(), m.first.c_str(), MOVEFILE_REPLACE_EXISTING);
            *error = L"Couldn't replace " + p + L". Is the server (or another program) still using it?";
            return false;
        }
        moved.push_back({full, aside});
    }
    // 3. extract
    bool ok = archive::Extract(zip, s.FilesDir(), L"files", progress, error);
    if (ok)
        if (const json::Value* ext = man.Get(L"external"); ext && ext->type == json::Value::Array)
            for (auto& e : ext->arr) {
                std::wstring dest = ExpandEnv(e.GetStr(L"path"));
                if (!fs::IsAbsolute(dest)) continue;
                std::vector<std::wstring> names = archive::List(zip);
                bool isFile = false;
                for (auto& n : names) if (n == e.GetStr(L"zip")) isFile = true;
                ok = isFile ? archive::Extract(zip, PathParent(dest), L"", nullptr, error)   // single file: rare
                            : archive::Extract(zip, dest, e.GetStr(L"zip"), progress, error);
                if (!ok) break;
            }
    if (!ok) {
        // put everything back the way it was
        for (auto& m : moved) { fs::DeleteTree(m.first); MoveFileExW(m.second.c_str(), m.first.c_str(), MOVEFILE_REPLACE_EXISTING); }
        return false;
    }
    for (auto& m : moved) fs::DeleteTree(m.second);
    Log(L"restore %ls: %ls (safety backup %ls)", s.id.c_str(), PathName(zip).c_str(), PathName(safety).c_str());
    return true;
}

} // namespace backup
