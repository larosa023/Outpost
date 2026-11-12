// Outpost - mods and plugins
#include "mods.h"
#include "net.h"
#include "util.h"
#include "fsutil.h"
#include "provision.h"
#include "config.h"
#include "json.h"
#include <bcrypt.h>
#include <algorithm>

namespace mods {

static std::wstring Enc(const std::wstring& s) {
    std::string u = WideToUtf8(s), o;
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { char b[4]; snprintf(b, 4, "%%%02X", c); o += b; }
    }
    return Utf8ToWide(o);
}

Target TargetFor(const ServerDef& s, const Template& t) {
    Target r;
    const json::Value* m = t.Obj(L"mods");
    if (!m) return r;
    if (t.modsType == L"modrinth") {
        const json::Value* vs = m->Get(L"variants");
        const json::Value* v = vs ? vs->Get(s.variant.empty() ? L"vanilla" : s.variant) : nullptr;
        if (!v) return r;
        r.loader = v->GetStr(L"loader"); r.folder = v->GetStr(L"folder"); r.kind = v->GetStr(L"kind", L"mod");
        r.mc = s.vars.count(L"mcVersion") ? s.vars.at(L"mcVersion") : L"";
        r.ok = true;
    } else if (t.modsType == L"folder") {
        r.folder = m->GetStr(L"folder"); r.kind = L"mod"; r.ok = !r.folder.empty();
    }
    return r;
}

std::wstring FolderFor(const ServerDef& s, const Template& t) {
    Target tg = TargetFor(s, t);
    return tg.ok ? s.FilesDir() + L"\\" + tg.folder : L"";
}

static bool GetJson(const std::wstring& url, json::Value& v, std::wstring* error, const std::wstring& headers = L"") {
    net::Response r;
    bool ok = headers.empty() ? net::Get(url, r) : net::Send(L"GET", url, "", headers, r);
    if (!ok) { if (error) *error = r.error; return false; }
    if (r.status != 200) { if (error) *error = L"The mod site answered HTTP " + std::to_wstring(r.status) + L"."; return false; }
    return json::Parser(Utf8ToWide(r.body)).Parse(v);
}

static std::wstring CfHeaders() { return L"x-api-key: " + g_cfg.curseForgeKey + L"\r\nAccept: application/json\r\n"; }
static int CfLoader(const std::wstring& l) { return l == L"forge" ? 1 : l == L"fabric" ? 4 : l == L"quilt" ? 5 : l == L"neoforge" ? 6 : 0; }

std::vector<ModHit> Search(const ServerDef& s, const Template& t, const std::wstring& query, const std::wstring& source, std::wstring* error) {
    std::vector<ModHit> out;
    Target tg = TargetFor(s, t);
    if (!tg.ok || t.modsType != L"modrinth") { if (error) *error = L"This server doesn't use a mod site."; return out; }
    if (source == L"curseforge") {
        if (g_cfg.curseForgeKey.empty()) { if (error) *error = L"Add your CurseForge API key in Outpost settings to search CurseForge."; return out; }
        std::wstring url = L"https://api.curseforge.com/v1/mods/search?gameId=432&classId=" + std::wstring(tg.kind == L"plugin" ? L"5" : L"6") +
                           L"&searchFilter=" + Enc(query) + L"&gameVersion=" + Enc(tg.mc) + L"&sortField=2&sortOrder=desc&pageSize=30";
        if (tg.kind != L"plugin" && CfLoader(tg.loader)) url += L"&modLoaderType=" + std::to_wstring(CfLoader(tg.loader));
        json::Value v;
        if (!GetJson(url, v, error, CfHeaders())) return out;
        if (const json::Value* d = v.Get(L"data"); d && d->type == json::Value::Array)
            for (auto& m : d->arr) {
                ModHit h; h.source = L"curseforge"; h.kind = tg.kind;
                wchar_t id[32]; swprintf(id, 32, L"%.0f", m.GetNum(L"id", 0)); h.projectId = id;
                h.slug = m.GetStr(L"slug"); h.title = m.GetStr(L"name"); h.description = m.GetStr(L"summary");
                h.downloads = m.GetNum(L"downloadCount", 0);
                if (const json::Value* a = m.Get(L"authors"); a && a->type == json::Value::Array && !a->arr.empty()) h.author = a->arr[0].GetStr(L"name");
                if (const json::Value* l = m.Get(L"logo")) h.icon = l->GetStr(L"thumbnailUrl");
                out.push_back(h);
            }
        return out;
    }
    std::wstring facets = L"[[\"project_type:" + std::wstring(tg.kind == L"plugin" ? L"plugin" : L"mod") + L"\"],[\"categories:" + tg.loader + L"\"]";
    if (!tg.mc.empty()) facets += L",[\"versions:" + tg.mc + L"\"]";
    if (tg.kind != L"plugin") facets += L",[\"server_side:required\",\"server_side:optional\"]";
    facets += L"]";
    std::wstring url = L"https://api.modrinth.com/v2/search?limit=30&index=" + std::wstring(query.empty() ? L"downloads" : L"relevance") +
                       L"&query=" + Enc(query) + L"&facets=" + Enc(facets);
    json::Value v;
    if (!GetJson(url, v, error)) return out;
    if (const json::Value* hits = v.Get(L"hits"); hits && hits->type == json::Value::Array)
        for (auto& m : hits->arr) {
            ModHit h; h.source = L"modrinth"; h.kind = tg.kind;
            h.projectId = m.GetStr(L"project_id"); h.slug = m.GetStr(L"slug"); h.title = m.GetStr(L"title");
            h.description = m.GetStr(L"description"); h.author = m.GetStr(L"author"); h.icon = m.GetStr(L"icon_url");
            h.downloads = m.GetNum(L"downloads", 0);
            out.push_back(h);
        }
    return out;
}

static ModVersion ParseModrinthVersion(const json::Value& x) {
    ModVersion mv;
    mv.id = x.GetStr(L"id"); mv.name = x.GetStr(L"name"); mv.number = x.GetStr(L"version_number");
    if (const json::Value* gv = x.Get(L"game_versions")) for (auto& g : gv->arr) mv.gameVersions.push_back(g.s);
    if (const json::Value* lv = x.Get(L"loaders")) for (auto& g : lv->arr) mv.loaders.push_back(g.s);
    if (const json::Value* files = x.Get(L"files"); files && files->type == json::Value::Array && !files->arr.empty()) {
        const json::Value* f = &files->arr[0];
        for (auto& c : files->arr) if (c.GetBool(L"primary", false)) f = &c;
        mv.file = f->GetStr(L"filename"); mv.url = f->GetStr(L"url"); mv.size = (uint64_t)f->GetNum(L"size", 0);
        if (const json::Value* h = f->Get(L"hashes")) { mv.sha1 = h->GetStr(L"sha1"); mv.sha512 = h->GetStr(L"sha512"); }
    }
    return mv;
}

std::vector<ModVersion> Versions(const ServerDef& s, const Template& t, const ModHit& hit, std::wstring* error) {
    std::vector<ModVersion> out;
    Target tg = TargetFor(s, t);
    if (hit.source == L"curseforge") {
        std::wstring url = L"https://api.curseforge.com/v1/mods/" + hit.projectId + L"/files?gameVersion=" + Enc(tg.mc) + L"&pageSize=20";
        if (tg.kind != L"plugin" && CfLoader(tg.loader)) url += L"&modLoaderType=" + std::to_wstring(CfLoader(tg.loader));
        json::Value v;
        if (!GetJson(url, v, error, CfHeaders())) return out;
        if (const json::Value* d = v.Get(L"data"); d && d->type == json::Value::Array)
            for (auto& f : d->arr) {
                ModVersion mv;
                wchar_t id[32]; swprintf(id, 32, L"%.0f", f.GetNum(L"id", 0)); mv.id = id;
                mv.name = f.GetStr(L"displayName"); mv.number = mv.name; mv.file = f.GetStr(L"fileName");
                mv.url = f.GetStr(L"downloadUrl");      // null when the author doesn't allow third-party downloads
                mv.size = (uint64_t)f.GetNum(L"fileLength", 0);
                if (const json::Value* hs = f.Get(L"hashes")) for (auto& h : hs->arr) if ((int)h.GetNum(L"algo", 0) == 1) mv.sha1 = h.GetStr(L"value");
                if (const json::Value* gv = f.Get(L"gameVersions")) for (auto& g : gv->arr) mv.gameVersions.push_back(g.s);
                out.push_back(mv);
            }
        if (out.empty() && error && error->empty()) *error = L"No version of " + hit.title + L" works with Minecraft " + tg.mc + L".";
        return out;
    }
    std::wstring loaders = L"[\"" + tg.loader + L"\"";
    if (tg.loader == L"paper") loaders += L",\"spigot\",\"bukkit\"";
    if (tg.loader == L"neoforge") loaders += L",\"forge\"";
    loaders += L"]";
    std::wstring url = L"https://api.modrinth.com/v2/project/" + hit.projectId + L"/version?loaders=" + Enc(loaders);
    if (!tg.mc.empty()) url += L"&game_versions=" + Enc(L"[\"" + tg.mc + L"\"]");
    json::Value v;
    if (!GetJson(url, v, error)) return out;
    if (v.type == json::Value::Array) for (auto& x : v.arr) out.push_back(ParseModrinthVersion(x));
    if (out.empty() && error) *error = L"No version of " + hit.title + L" works with this server (" + tg.loader + L" " + tg.mc + L").";
    return out;
}

static bool Download(const ModVersion& v, const std::wstring& dest, std::wstring* error) {
    if (v.url.empty()) { *error = L"The author of this mod doesn't allow downloads from other apps. Download it from the website and use Add file."; return false; }
    std::wstring tmp = dest + L".part";
    if (!net::Download(v.url, tmp, nullptr, error)) return false;
    bool ok = true;
    if (!v.sha512.empty()) ok = provision::HashFile(tmp, BCRYPT_SHA512_ALGORITHM) == Lower(v.sha512);
    else if (!v.sha1.empty()) ok = provision::HashFile(tmp, BCRYPT_SHA1_ALGORITHM) == Lower(v.sha1);
    if (!ok) { DeleteFileW(tmp.c_str()); *error = L"The download didn't match its checksum."; return false; }
    if (!MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) { *error = L"Couldn't save " + PathName(dest); return false; }
    return true;
}

static bool SafeName(const std::wstring& n) { return !n.empty() && n.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos && n != L"." && n != L".."; }

bool Install(ServerDef& s, const Template& t, const ModHit& hit, const ModVersion& v, std::wstring* error) {
    std::wstring folder = FolderFor(s, t);
    if (folder.empty() || !SafeName(v.file)) { *error = L"Can't install that here."; return false; }
    EnsureDir(folder);
    for (auto& m : s.mods) if (m.projectId == hit.projectId && m.source == hit.source) { *error = hit.title + L" is already installed."; return false; }
    if (!Download(v, folder + L"\\" + v.file, error)) return false;
    ModEntry e;
    e.source = hit.source; e.projectId = hit.projectId; e.versionId = v.id; e.title = hit.title; e.versionName = v.number;
    e.file = v.file; e.hash = !v.sha1.empty() ? v.sha1 : provision::HashFile(folder + L"\\" + v.file, BCRYPT_SHA1_ALGORITHM);
    s.mods.push_back(e);
    servers::Save(s);
    Log(L"mod %ls installed on %ls (%ls)", hit.title.c_str(), s.id.c_str(), v.number.c_str());
    return true;
}

std::vector<std::pair<int, ModVersion>> CheckUpdates(const ServerDef& s, const Template& t, std::wstring* error) {
    std::vector<std::pair<int, ModVersion>> out;
    Target tg = TargetFor(s, t);
    if (!tg.ok || t.modsType != L"modrinth") return out;
    // POST /v2/version_files/update: newest compatible version for every hash at once
    std::wstring hashes;
    for (auto& m : s.mods) if (m.source == L"modrinth" && !m.hash.empty()) hashes += (hashes.empty() ? L"\"" : L",\"") + m.hash + L"\"";
    if (hashes.empty()) return out;
    std::wstring loaders = L"\"" + tg.loader + L"\"";
    if (tg.loader == L"paper") loaders += L",\"spigot\",\"bukkit\"";
    std::wstring body = L"{\"hashes\":[" + hashes + L"],\"algorithm\":\"sha1\",\"loaders\":[" + loaders + L"],\"game_versions\":[\"" + tg.mc + L"\"]}";
    net::Response r;
    if (!net::Send(L"POST", L"https://api.modrinth.com/v2/version_files/update", WideToUtf8(body), L"Content-Type: application/json\r\n", r) || r.status != 200) {
        if (error) *error = r.error.empty() ? L"Modrinth answered HTTP " + std::to_wstring(r.status) : r.error;
        return out;
    }
    json::Value v;
    if (!json::Parser(Utf8ToWide(r.body)).Parse(v) || v.type != json::Value::Object) return out;
    for (int i = 0; i < (int)s.mods.size(); ++i) {
        const json::Value* nv = v.Get(s.mods[i].hash);
        if (!nv) continue;
        ModVersion mv = ParseModrinthVersion(*nv);
        if (!mv.id.empty() && mv.id != s.mods[i].versionId) out.push_back({i, mv});
    }
    return out;
}

bool Update(ServerDef& s, const Template& t, int index, const ModVersion& v, std::wstring* error) {
    if (index < 0 || index >= (int)s.mods.size() || !SafeName(v.file)) return false;
    ModEntry& m = s.mods[index];
    std::wstring folder = FolderFor(s, t);
    std::wstring dir = m.enabled ? folder : folder + L"\\.disabled";
    if (!Download(v, dir + L"\\" + v.file, error)) return false;
    if (!IEquals(m.file, v.file)) DeleteFileW((dir + L"\\" + m.file).c_str());
    m.file = v.file; m.versionId = v.id; m.versionName = v.number;
    m.hash = !v.sha1.empty() ? v.sha1 : provision::HashFile(dir + L"\\" + v.file, BCRYPT_SHA1_ALGORITHM);
    servers::Save(s);
    return true;
}

std::vector<ModFile> List(const ServerDef& s, const Template& t) {
    std::vector<ModFile> out;
    std::wstring folder = FolderFor(s, t);
    if (folder.empty()) return out;
    auto scan = [&](const std::wstring& dir, bool enabled) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || n == L".disabled" || n[0] == L'.') continue;
            // Paper and Fabric keep config folders next to the jars: only files are mods there
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && t.modsType == L"modrinth") continue;
            ModFile mf;
            mf.file = n; mf.title = n; mf.enabled = enabled; mf.source = L"file";
            mf.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            for (int i = 0; i < (int)s.mods.size(); ++i)
                if (IEquals(s.mods[i].file, n)) { mf.title = s.mods[i].title; mf.version = s.mods[i].versionName; mf.source = s.mods[i].source; mf.tracked = true; mf.index = i; }
            out.push_back(mf);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    };
    scan(folder, true);
    scan(folder + L"\\.disabled", false);
    std::sort(out.begin(), out.end(), [](const ModFile& a, const ModFile& b) { return _wcsicmp(a.title.c_str(), b.title.c_str()) < 0; });
    return out;
}

bool SetEnabled(ServerDef& s, const Template& t, const std::wstring& file, bool enabled, std::wstring* error) {
    std::wstring folder = FolderFor(s, t);
    if (folder.empty() || !SafeName(file)) return false;
    std::wstring on = folder + L"\\" + file, off = folder + L"\\.disabled\\" + file;
    EnsureDir(folder + L"\\.disabled");
    BOOL ok = enabled ? MoveFileExW(off.c_str(), on.c_str(), MOVEFILE_REPLACE_EXISTING) : MoveFileExW(on.c_str(), off.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) { *error = L"Couldn't move " + file + L". Stop the server first if it's using it."; return false; }
    for (auto& m : s.mods) if (IEquals(m.file, file)) m.enabled = enabled;
    servers::Save(s);
    return true;
}

bool Remove(ServerDef& s, const Template& t, const std::wstring& file, std::wstring* error) {
    std::wstring folder = FolderFor(s, t);
    if (folder.empty() || !SafeName(file)) return false;
    bool ok = fs::DeleteTree(folder + L"\\" + file) && fs::DeleteTree(folder + L"\\.disabled\\" + file);
    if (!ok) { *error = L"Couldn't delete " + file + L". Stop the server first."; return false; }
    s.mods.erase(std::remove_if(s.mods.begin(), s.mods.end(), [&](const ModEntry& m) { return IEquals(m.file, file); }), s.mods.end());
    servers::Save(s);
    return true;
}

bool AddFile(ServerDef& s, const Template& t, const std::wstring& path, std::wstring* error) {
    std::wstring folder = FolderFor(s, t);
    if (folder.empty()) { *error = L"This game has no mods folder."; return false; }
    EnsureDir(folder);
    if (!fs::CopyTree(path, folder + L"\\" + PathName(path))) { *error = L"Couldn't copy " + PathName(path); return false; }
    return true;
}

} // namespace mods
