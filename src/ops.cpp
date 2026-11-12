// Outpost - create, install, import, update, delete
#include "ops.h"
#include "backup.h"
#include "host.h"
#include "fsutil.h"
#include "util.h"
#include "archive.h"
#include "json.h"
#include <set>

namespace ops {

void PickFreePorts(ServerDef& s, const Template& t) {
    std::set<int> used;
    for (auto& o : servers::LoadAll()) {
        if (o.id == s.id) continue;
        Template ot;
        if (!servers::LoadTemplate(o, ot)) continue;
        for (auto& p : ot.ports) { int v = o.Port(p.id); for (int i = 0; i < p.count; ++i) used.insert(v + i); }
    }
    for (auto& p : t.ports) {
        int v = s.Port(p.id);
        if (v <= 0) v = p.def;
        auto clash = [&](int base) { for (int i = 0; i < p.count; ++i) if (used.count(base + i)) return true; return false; };
        int step = std::max(1, p.count);
        while (clash(v)) v += step + (p.count > 1 ? 1 : 0);
        s.settings[p.id] = std::to_wstring(v);
        for (int i = 0; i < p.count; ++i) used.insert(v + i);
    }
}

bool Create(const std::wstring& game, const std::wstring& variant, const std::wstring& version, const std::wstring& name,
            const std::map<std::wstring, std::wstring>& settings, int ramMb, const std::wstring& location, ServerDef* out, std::wstring* error) {
    const Template* t = templates::Find(game);
    if (!t) { *error = L"Outpost has no template for \"" + game + L"\"."; return false; }
    ServerDef s;
    s.id = servers::NewId(name.empty() ? t->name : name);
    s.name = name.empty() ? t->name : name;
    s.game = t->id;
    s.variant = variant.empty() && !t->variants.empty() ? t->variants[0].id : variant;
    s.version = version.empty() ? L"latest" : version;
    s.created = fs::IsoNow();
    s.settings = settings;
    s.ram = ramMb;
    if (location.empty()) s.root = servers::BaseDir() + L"\\" + s.id;
    else {
        s.root = location + L"\\" + s.id;
        EnsureDir(servers::BaseDir());
        WriteTextFileAtomic(servers::BaseDir() + L"\\" + s.id + L".link", s.root);
    }
    servers::ApplyDefaults(s, *t);
    for (auto& kv : settings) s.settings[kv.first] = kv.second;   // what the wizard set wins over defaults
    bool portsGiven = false;
    for (auto& p : t->ports) portsGiven |= settings.count(p.id) > 0;
    if (!portsGiven) PickFreePorts(s, *t);
    // required settings (Minecraft's EULA, Valheim's password)
    for (auto& st : t->settings) {
        std::wstring v = s.Get(st.id);
        if (st.required == L"true" && v != st.valOn) { *error = st.label + L" is required."; return false; }
        if (IStartsWith(st.required, L"minlen:") && (int)v.size() < _wtoi(st.required.c_str() + 7)) {
            *error = st.label + L" needs at least " + st.required.substr(7) + L" characters."; return false;
        }
    }
    EnsureDir(s.FilesDir());
    if (!servers::SaveTemplate(s, *t) || !servers::Save(s)) { *error = L"Couldn't create the server folder " + s.root; return false; }
    Log(L"server %ls created (%ls)", s.id.c_str(), s.game.c_str());
    *out = s;
    return true;
}

bool Install(ServerDef& s, const provision::Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    Template t;
    if (!servers::LoadTemplate(s, t)) { *error = L"The server's game template is missing."; return false; }
    // a newer copy of the same template (from an Outpost update or the repo) is picked up on install/update
    if (const Template* fresh = templates::Find(s.game); fresh && fresh->version >= t.version) { t = *fresh; servers::SaveTemplate(s, t); }
    servers::ApplyDefaults(s, t);
    if (!provision::Install(s, t, progress, cancel, error)) { servers::Save(s); return false; }
    if (!WriteConfigFiles(s, t, error)) return false;
    return servers::Save(s);
}

bool UpdateFiles(ServerDef& s, const provision::Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    if (host::IsRunning(s.id)) { *error = L"Stop the server before updating it."; return false; }
    Template t;
    if (!servers::LoadTemplate(s, t)) { *error = L"The server's game template is missing."; return false; }
    if (progress) progress(-1, L"Backing up before the update");
    std::wstring path, err;
    if (!backup::Create(s, t, L"before update", nullptr, &path, &err) && err.find(L"nothing to back up") == std::wstring::npos) {
        *error = L"The backup before the update failed, so nothing was changed: " + err;
        return false;
    }
    return Install(s, progress, cancel, error);
}

bool Import(const std::wstring& folder, const std::wstring& name, const std::wstring& gameHint, const provision::Progress& progress,
            ServerDef* out, std::wstring* error) {
    std::wstring variant;
    std::wstring game = provision::DetectGame(folder, &variant);
    bool worldOnly = false;
    if (game.empty() && !gameHint.empty()) {
        // a bare world folder: Minecraft worlds have level.dat
        game = gameHint;
        worldOnly = true;
    }
    if (game.empty()) { *error = L"Outpost couldn't tell which game this folder is for. Choose the game, then import just the world."; return false; }
    ServerDef s;
    std::map<std::wstring, std::wstring> settings;
    if (game == L"minecraft-java" && FileExists(folder + L"\\level.dat")) { worldOnly = true; settings[L"levelName"] = PathName(folder); }
    if (game == L"minecraft-java") settings[L"eula"] = L"true";
    // keep the imported Minecraft version (so the world isn't upgraded by surprise)
    std::wstring version = L"latest";
    if (game == L"minecraft-java" && !worldOnly) {
        std::string vj;
        json::Value v;
        std::wstring hist;
        if (archive::ReadEntry(folder + L"\\server.jar", L"version.json", vj) && json::Parser(Utf8ToWide(vj)).Parse(v) && !v.GetStr(L"id").empty()) version = v.GetStr(L"id");
        else if (ReadTextFile(folder + L"\\version_history.json", hist)) {
            size_t p = hist.find(L"(MC: ");
            if (p != std::wstring::npos) version = hist.substr(p + 5, hist.find(L')', p) - p - 5);
        }
    }
    if (!Create(game, variant, version, name.empty() ? PathName(folder) : name, settings, 0, L"", &s, error)) return false;
    Template t;
    servers::LoadTemplate(s, t);
    if (progress) progress(-1, L"Copying files");
    uint64_t copied = 0, total = fs::TreeSize(folder);
    auto cb = [&](uint64_t n) { copied += n; if (progress && total) progress((int)(copied * 100 / total), L"Copying files (" + FormatBytes(total) + L")"); return true; };
    std::wstring dest = s.FilesDir();
    if (worldOnly) {
        // into the first save path
        const json::Value* saves = t.Obj(L"saves");
        std::wstring first = saves && !saves->arr.empty() ? Expand(saves->arr[0].s, s, t) : PathName(folder);
        dest = fs::Join(s.FilesDir(), first);
    }
    if (!fs::CopyTree(folder, dest, cb)) { *error = L"Copying the files failed."; servers::Save(s); return false; }
    if (!worldOnly) {
        // pick up the imported server's own settings
        ReadBackConfig(s, t);
    }
    servers::Save(s);
    *out = s;
    return true;
}

bool Delete(const ServerDef& s, bool backupsToo, std::wstring* error) {
    if (host::IsRunning(s.id)) { *error = L"Stop the server first."; return false; }
    if (backupsToo) fs::DeleteTree(backup::Dir(s));
    bool ok = fs::DeleteTree(s.root);
    DeleteFileW((servers::BaseDir() + L"\\" + s.id + L".link").c_str());
    if (!ok) { *error = L"Some files couldn't be deleted (in use?): " + s.root; return false; }
    Log(L"server %ls deleted", s.id.c_str());
    return true;
}

} // namespace ops
