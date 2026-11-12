// Outpost - servers and their config files
#include "server.h"
#include "util.h"
#include "net.h"
#include <bcrypt.h>
#include <algorithm>
#include <ctime>

int ServerDef::Port(const std::wstring& pid) const {
    std::wstring v = Get(pid);
    return v.empty() ? 0 : _wtoi(v.c_str());
}

namespace servers {

std::wstring BaseDir() { return DataDir() + L"\\servers"; }

std::wstring RandomToken(int n) {
    static const wchar_t* abc = L"abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::vector<uint8_t> buf(n);
    BCryptGenRandom(nullptr, buf.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::wstring s;
    for (int i = 0; i < n; ++i) s += abc[buf[i] % 56];
    return s;
}

std::wstring NewId(const std::wstring& name) {
    std::wstring slug;
    for (wchar_t c : Lower(name)) {
        if (iswalnum(c) && c < 128) slug += c;
        else if (!slug.empty() && slug.back() != L'-') slug += L'-';
        if (slug.size() >= 24) break;
    }
    while (!slug.empty() && slug.back() == L'-') slug.pop_back();
    if (slug.empty()) slug = L"server";
    std::wstring id = slug;
    for (int i = 2; DirExists(BaseDir() + L"\\" + id) || FileExists(BaseDir() + L"\\" + id + L".link"); ++i) id = slug + L"-" + std::to_wstring(i);
    return id;
}

static json::Value StrMap(const std::map<std::wstring, std::wstring>& m) {
    json::Value o = json::Value::MakeObject();
    for (auto& kv : m) o.Set(kv.first, json::Value::Str(kv.second));
    return o;
}
static void ReadMap(const json::Value* o, std::map<std::wstring, std::wstring>& m) {
    if (!o || o->type != json::Value::Object) return;
    for (auto& kv : o->obj) {
        if (kv.second.type == json::Value::String) m[kv.first] = kv.second.s;
        else if (kv.second.type == json::Value::Number) { wchar_t b[64]; swprintf(b, 64, L"%g", kv.second.n); m[kv.first] = b; }
        else if (kv.second.type == json::Value::Bool) m[kv.first] = kv.second.b ? L"true" : L"false";
    }
}

bool Load(const std::wstring& root, ServerDef& s) {
    std::wstring text;
    if (!ReadTextFile(root + L"\\server.json", text)) return false;
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) return false;
    s = ServerDef();
    s.root = root;
    s.id = v.GetStr(L"id", PathName(root));
    s.name = v.GetStr(L"name", s.id);
    s.game = v.GetStr(L"game");
    s.variant = v.GetStr(L"variant");
    s.version = v.GetStr(L"version", L"latest");
    s.installedVersion = v.GetStr(L"installedVersion");
    s.created = v.GetStr(L"created");
    ReadMap(v.Get(L"settings"), s.settings);
    ReadMap(v.Get(L"vars"), s.vars);
    s.ram = (int)v.GetNum(L"ram", 0);
    s.autoRestart = v.GetBool(L"autoRestart", true);
    s.restartHours = (int)v.GetNum(L"restartHours", 0);
    s.restartAt = v.GetStr(L"restartAt");
    s.sleepWhenEmpty = v.GetBool(L"sleepWhenEmpty", false);
    s.sleepMinutes = (int)v.GetNum(L"sleepMinutes", 15);
    s.startOnBoot = v.GetBool(L"startOnBoot", false);
    s.backupMinutes = (int)v.GetNum(L"backupMinutes", 30);
    s.keepBackups = (int)v.GetNum(L"keepBackups", 10);
    s.backupDir = v.GetStr(L"backupDir");
    s.upnp = v.GetBool(L"upnp", true);
    s.firewall = v.GetBool(L"firewall", true);
    s.lowerPriorityWhileGaming = v.GetBool(L"lowerPriorityWhileGaming", true);
    if (const json::Value* m = v.Get(L"mods"); m && m->type == json::Value::Array)
        for (auto& x : m->arr) {
            ModEntry e;
            e.source = x.GetStr(L"source"); e.projectId = x.GetStr(L"projectId"); e.versionId = x.GetStr(L"versionId");
            e.title = x.GetStr(L"title"); e.versionName = x.GetStr(L"versionName"); e.file = x.GetStr(L"file");
            e.hash = x.GetStr(L"hash"); e.enabled = x.GetBool(L"enabled", true);
            s.mods.push_back(e);
        }
    return !s.game.empty();
}

bool Save(const ServerDef& s) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"schema", json::Value::Num(1));
    v.Set(L"id", json::Value::Str(s.id));
    v.Set(L"name", json::Value::Str(s.name));
    v.Set(L"game", json::Value::Str(s.game));
    v.Set(L"variant", json::Value::Str(s.variant));
    v.Set(L"version", json::Value::Str(s.version));
    v.Set(L"installedVersion", json::Value::Str(s.installedVersion));
    v.Set(L"created", json::Value::Str(s.created));
    v.Set(L"settings", StrMap(s.settings));
    v.Set(L"vars", StrMap(s.vars));
    v.Set(L"ram", json::Value::Num(s.ram));
    v.Set(L"autoRestart", json::Value::Boolean(s.autoRestart));
    v.Set(L"restartHours", json::Value::Num(s.restartHours));
    v.Set(L"restartAt", json::Value::Str(s.restartAt));
    v.Set(L"sleepWhenEmpty", json::Value::Boolean(s.sleepWhenEmpty));
    v.Set(L"sleepMinutes", json::Value::Num(s.sleepMinutes));
    v.Set(L"startOnBoot", json::Value::Boolean(s.startOnBoot));
    v.Set(L"backupMinutes", json::Value::Num(s.backupMinutes));
    v.Set(L"keepBackups", json::Value::Num(s.keepBackups));
    v.Set(L"backupDir", json::Value::Str(s.backupDir));
    v.Set(L"upnp", json::Value::Boolean(s.upnp));
    v.Set(L"firewall", json::Value::Boolean(s.firewall));
    v.Set(L"lowerPriorityWhileGaming", json::Value::Boolean(s.lowerPriorityWhileGaming));
    json::Value mods = json::Value::MakeArray();
    for (auto& m : s.mods) {
        json::Value e = json::Value::MakeObject();
        e.Set(L"source", json::Value::Str(m.source)); e.Set(L"projectId", json::Value::Str(m.projectId));
        e.Set(L"versionId", json::Value::Str(m.versionId)); e.Set(L"title", json::Value::Str(m.title));
        e.Set(L"versionName", json::Value::Str(m.versionName)); e.Set(L"file", json::Value::Str(m.file));
        e.Set(L"hash", json::Value::Str(m.hash)); e.Set(L"enabled", json::Value::Boolean(m.enabled));
        mods.arr.push_back(e);
    }
    v.Set(L"mods", mods);
    std::wstring out;
    json::Write(v, out);
    EnsureDir(s.root);
    return WriteTextFileAtomic(s.root + L"\\server.json", out);
}

std::vector<ServerDef> LoadAll() {
    std::vector<ServerDef> out;
    std::wstring base = BaseDir();
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring root;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) root = base + L"\\" + n;
        else if (n.size() > 5 && IEquals(n.substr(n.size() - 5), L".link")) {
            std::wstring t;
            if (ReadTextFile(base + L"\\" + n, t)) root = Trim(t);   // a server kept somewhere else (another drive)
        }
        ServerDef s;
        if (!root.empty() && Load(root, s)) out.push_back(s);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const ServerDef& a, const ServerDef& b) { return a.created < b.created; });
    return out;
}

bool LoadTemplate(const ServerDef& s, Template& t) {
    std::wstring text, err;
    if (ReadTextFile(s.root + L"\\template.json", text) && ParseTemplate(text, t, &err)) return true;
    if (const Template* g = templates::Find(s.game)) { t = *g; return true; }
    return false;
}

bool SaveTemplate(const ServerDef& s, const Template& t) {
    std::wstring out;
    json::Write(t.raw, out);
    return WriteTextFileAtomic(s.root + L"\\template.json", out);
}

void ApplyDefaults(ServerDef& s, const Template& t) {
    for (auto& p : t.ports)
        if (s.settings.find(p.id) == s.settings.end()) s.settings[p.id] = std::to_wstring(p.def);
    for (auto& st : t.settings) {
        if (s.settings.find(st.id) != s.settings.end()) continue;
        std::wstring v = st.def;
        if (v == L"{random}") v = RandomToken(12);
        else if (v == L"{randomNumber}") {
            uint32_t r = 0;
            BCryptGenRandom(nullptr, (PUCHAR)&r, sizeof(r), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            v = std::to_wstring(r % 2147483647u);
        }
        s.settings[st.id] = v;
    }
    if (t.hasRam && s.ram <= 0) s.ram = t.ramDefault;
}

} // namespace servers

std::wstring Expand(const std::wstring& text, const ServerDef& s, const Template& t) {
    (void)t;
    std::wstring out;
    out.reserve(text.size() + 32);
    for (size_t i = 0; i < text.size();) {
        if (text[i] != L'{') { out += text[i++]; continue; }
        size_t e = text.find(L'}', i);
        if (e == std::wstring::npos) { out += text.substr(i); break; }
        std::wstring key = text.substr(i + 1, e - i - 1), prefix;
        bool conditional = false;
        size_t bar = key.find(L'|');
        if (bar != std::wstring::npos) { prefix = key.substr(bar + 1); key = key.substr(0, bar); conditional = true; }
        bool valid = !key.empty() && key.find_first_of(L" \"{") == std::wstring::npos;
        if (!valid) { out += text[i++]; continue; }
        std::wstring val;
        auto sv = s.settings.find(key);
        auto vv = s.vars.find(key);
        if (key == L"dir") val = s.FilesDir();
        else if (key == L"root") val = s.root;
        else if (key == L"name") val = s.name;
        else if (key == L"ram") val = std::to_wstring(s.ram > 0 ? s.ram : t.ramDefault);
        else if (key == L"ramMin") { int r = s.ram > 0 ? s.ram : t.ramDefault; val = std::to_wstring(std::min(r, std::max(512, r / 4))); }
        else if (sv != s.settings.end()) val = sv->second;
        else if (vv != s.vars.end()) val = vv->second;
        if (conditional) out += val.empty() ? L"" : prefix + val;
        else out += val;
        i = e + 1;
    }
    return out;
}

// ---------------------------------------------------------------- config formats
namespace cfg {

static std::vector<std::wstring> SplitLines(const std::wstring& text, bool* crlf) {
    std::vector<std::wstring> lines;
    *crlf = text.find(L"\r\n") != std::wstring::npos;
    size_t i = 0;
    while (i <= text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) { if (i < text.size()) lines.push_back(text.substr(i)); break; }
        std::wstring l = text.substr(i, e - i);
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        lines.push_back(l);
        i = e + 1;
    }
    return lines;
}
static std::wstring JoinLines(const std::vector<std::wstring>& lines, bool crlf) {
    std::wstring out;
    for (auto& l : lines) { out += l; out += crlf ? L"\r\n" : L"\n"; }
    return out;
}
static bool KeyMatch(const std::wstring& line, const std::wstring& key, std::wstring* value) {
    std::wstring l = Trim(line);
    if (l.empty() || l[0] == L'#' || l[0] == L';' || l[0] == L'!') return false;
    size_t eq = l.find_first_of(L"=:");
    if (eq == std::wstring::npos) return false;
    if (Trim(l.substr(0, eq)) != key) return false;
    if (value) *value = Trim(l.substr(eq + 1));
    return true;
}

bool SetProperties(std::wstring& text, const std::wstring& key, const std::wstring& value) {
    bool crlf;
    auto lines = SplitLines(text, &crlf);
    bool found = false;
    for (auto& l : lines) if (KeyMatch(l, key, nullptr)) { l = key + L"=" + value; found = true; }
    if (!found) lines.push_back(key + L"=" + value);
    text = JoinLines(lines, crlf);
    return true;
}
std::wstring GetProperties(const std::wstring& text, const std::wstring& key, bool* found) {
    bool crlf;
    for (auto& l : SplitLines(text, &crlf)) { std::wstring v; if (KeyMatch(l, key, &v)) { if (found) *found = true; return v; } }
    if (found) *found = false;
    return L"";
}

static bool IsSection(const std::wstring& l, std::wstring* name) {
    std::wstring t = Trim(l);
    if (t.size() < 2 || t[0] != L'[' || t.back() != L']') return false;
    if (name) *name = t.substr(1, t.size() - 2);
    return true;
}

bool SetIni(std::wstring& text, const std::wstring& section, const std::wstring& key, const std::wstring& value) {
    if (section.empty()) return SetProperties(text, key, value);
    bool crlf;
    auto lines = SplitLines(text, &crlf);
    int secStart = -1, secEnd = (int)lines.size();
    for (int i = 0; i < (int)lines.size(); ++i) {
        std::wstring n;
        if (IsSection(lines[i], &n)) {
            if (secStart >= 0) { secEnd = i; break; }
            if (IEquals(n, section)) secStart = i;
        }
    }
    if (secStart < 0) {
        if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back(L"");
        lines.push_back(L"[" + section + L"]");
        lines.push_back(key + L"=" + value);
    } else {
        bool found = false;
        for (int i = secStart + 1; i < secEnd; ++i) if (KeyMatch(lines[i], key, nullptr)) { lines[i] = key + L"=" + value; found = true; }
        if (!found) {
            int at = secEnd;
            while (at > secStart + 1 && Trim(lines[at - 1]).empty()) --at;
            lines.insert(lines.begin() + at, key + L"=" + value);
        }
    }
    text = JoinLines(lines, crlf);
    return true;
}
std::wstring GetIni(const std::wstring& text, const std::wstring& section, const std::wstring& key, bool* found) {
    if (section.empty()) return GetProperties(text, key, found);
    bool crlf, in = false;
    for (auto& l : SplitLines(text, &crlf)) {
        std::wstring n;
        if (IsSection(l, &n)) { in = IEquals(n, section); continue; }
        std::wstring v;
        if (in && KeyMatch(l, key, &v)) { if (found) *found = true; return v; }
    }
    if (found) *found = false;
    return L"";
}

// Unreal "OptionSettings=(A=1,B="text, with comma",C=True)"
static std::vector<std::pair<std::wstring, std::wstring>> ParseUeList(const std::wstring& inner) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    std::wstring cur;
    bool q = false;
    int depth = 0;
    auto flush = [&] {
        std::wstring item = Trim(cur);
        cur.clear();
        if (item.empty()) return;
        size_t eq = item.find(L'=');
        if (eq == std::wstring::npos) out.push_back({item, L""});
        else out.push_back({Trim(item.substr(0, eq)), item.substr(eq + 1)});
    };
    for (wchar_t c : inner) {
        if (c == L'"') q = !q;
        if (!q && c == L'(') ++depth;
        if (!q && c == L')') --depth;
        if (c == L',' && !q && depth == 0) { flush(); continue; }
        cur += c;
    }
    flush();
    return out;
}

bool SetUeOption(std::wstring& text, const std::wstring& section, const std::wstring& key, const std::wstring& opt, const std::wstring& value) {
    bool f = false;
    std::wstring cur = GetIni(text, section, key, &f);
    std::wstring inner = cur;
    if (inner.size() >= 2 && inner.front() == L'(' && inner.back() == L')') inner = inner.substr(1, inner.size() - 2);
    auto items = ParseUeList(inner);
    bool found = false;
    for (auto& it : items) if (it.first == opt) { it.second = value; found = true; }
    if (!found) items.push_back({opt, value});
    std::wstring rebuilt = L"(";
    for (size_t i = 0; i < items.size(); ++i) { if (i) rebuilt += L","; rebuilt += items[i].first + L"=" + items[i].second; }
    rebuilt += L")";
    return SetIni(text, section, key, rebuilt);
}
std::wstring GetUeOption(const std::wstring& text, const std::wstring& key, const std::wstring& opt, bool* found) {
    bool crlf;
    for (auto& l : SplitLines(text, &crlf)) {
        std::wstring v;
        if (!KeyMatch(l, key, &v)) continue;
        if (v.size() >= 2 && v.front() == L'(' && v.back() == L')') v = v.substr(1, v.size() - 2);
        for (auto& it : ParseUeList(v)) if (it.first == opt) { if (found) *found = true; return it.second; }
    }
    if (found) *found = false;
    return L"";
}

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'&') o += L"&amp;"; else if (c == L'<') o += L"&lt;"; else if (c == L'>') o += L"&gt;"; else if (c == L'"') o += L"&quot;"; else o += c;
    }
    return o;
}
static std::wstring XmlUnescape(const std::wstring& s) {
    std::wstring o = s;
    struct { const wchar_t* a; const wchar_t* b; } m[] = {{L"&quot;", L"\""}, {L"&lt;", L"<"}, {L"&gt;", L">"}, {L"&amp;", L"&"}};
    for (auto& r : m) for (size_t p; (p = o.find(r.a)) != std::wstring::npos;) o.replace(p, wcslen(r.a), r.b);
    return o;
}
// finds <property name="X" value="Y"/> outside comments; returns [start,end) of the tag and value span
static bool FindXmlProp(const std::wstring& text, const std::wstring& name, size_t* tagStart, size_t* valStart, size_t* valEnd) {
    size_t pos = 0;
    while ((pos = text.find(L"<property", pos)) != std::wstring::npos) {
        size_t c1 = text.rfind(L"<!--", pos), c2 = text.rfind(L"-->", pos);
        bool inComment = c1 != std::wstring::npos && (c2 == std::wstring::npos || c2 < c1);
        size_t end = text.find(L'>', pos);
        if (end == std::wstring::npos) return false;
        std::wstring tag = text.substr(pos, end - pos);
        size_t n = tag.find(L"name=\"");
        if (!inComment && n != std::wstring::npos) {
            size_t ne = tag.find(L'"', n + 6);
            if (ne != std::wstring::npos && tag.substr(n + 6, ne - n - 6) == name) {
                size_t v = tag.find(L"value=\"");
                if (v == std::wstring::npos) return false;
                size_t ve = tag.find(L'"', v + 7);
                *tagStart = pos; *valStart = pos + v + 7; *valEnd = pos + ve;
                return true;
            }
        }
        pos = end;
    }
    return false;
}
bool SetXmlProperty(std::wstring& text, const std::wstring& name, const std::wstring& value) {
    size_t t, a, b;
    if (FindXmlProp(text, name, &t, &a, &b)) { text.replace(a, b - a, XmlEscape(value)); return true; }
    std::wstring line = L"\t<property name=\"" + name + L"\" value=\"" + XmlEscape(value) + L"\" />";
    size_t close = text.rfind(L"</ServerSettings>");
    if (close == std::wstring::npos) {
        if (text.empty()) text = L"<?xml version=\"1.0\"?>\r\n<ServerSettings>\r\n" + line + L"\r\n</ServerSettings>\r\n";
        else text += line + L"\r\n";
    } else {
        text.insert(close, line + L"\r\n");
    }
    return true;
}
std::wstring GetXmlProperty(const std::wstring& text, const std::wstring& name, bool* found) {
    size_t t, a, b;
    if (FindXmlProp(text, name, &t, &a, &b)) { if (found) *found = true; return XmlUnescape(text.substr(a, b - a)); }
    if (found) *found = false;
    return L"";
}

// JSON: dotted paths ("userGroups.1.password"); "#value" writes a number
static json::Value* JsonPath(json::Value& root, const std::wstring& path, bool create) {
    json::Value* cur = &root;
    size_t i = 0;
    while (true) {
        size_t d = path.find(L'.', i);
        std::wstring part = path.substr(i, d == std::wstring::npos ? std::wstring::npos : d - i);
        json::Value* next = nullptr;
        if (cur->type == json::Value::Array) {
            size_t idx = (size_t)_wtoi(part.c_str());
            if (idx < cur->arr.size()) next = &cur->arr[idx];
        } else {
            if (cur->type != json::Value::Object) { if (!create) return nullptr; *cur = json::Value::MakeObject(); }
            for (auto& kv : cur->obj) if (kv.first == part) next = &kv.second;
            if (!next && create && (d != std::wstring::npos ? false : true)) { cur->obj.emplace_back(part, json::Value()); next = &cur->obj.back().second; }
            else if (!next && create) { cur->obj.emplace_back(part, json::Value::MakeObject()); next = &cur->obj.back().second; }
        }
        if (!next) return nullptr;
        if (d == std::wstring::npos) return next;
        cur = next;
        i = d + 1;
    }
}

} // namespace cfg

static std::wstring ConfigPath(const ServerDef& s, const Template& t, const json::Value& cf) {
    std::wstring p = Expand(cf.GetStr(L"path"), s, t);
    if (p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'))) return p;
    return s.FilesDir() + L"\\" + p;
}

static bool WriteText(const std::wstring& path, const std::wstring& text) {
    EnsureDir(PathParent(path));
    return WriteFileAtomic(path, WideToUtf8(text));
}

bool WriteConfigFiles(const ServerDef& s, const Template& t, std::wstring* error) {
    if (const json::Value* files = t.Obj(L"files"); files && files->type == json::Value::Array)
        for (auto& f : files->arr) {
            std::wstring path = s.FilesDir() + L"\\" + Expand(f.GetStr(L"path"), s, t);
            if (f.GetBool(L"ifMissing", false) && FileExists(path)) continue;
            if (!WriteText(path, Expand(f.GetStr(L"content"), s, t))) { if (error) *error = L"Couldn't write " + path; return false; }
        }
    const json::Value* cfs = t.Obj(L"configFiles");
    if (!cfs || cfs->type != json::Value::Array) return true;
    for (auto& cf : cfs->arr) {
        std::wstring path = ConfigPath(s, t, cf), fmt = cf.GetStr(L"format", L"properties"), section = cf.GetStr(L"section");
        std::wstring text;
        if (!ReadTextFile(path, text)) {
            std::wstring from = cf.GetStr(L"copyFrom");
            if (!from.empty()) ReadTextFile(s.FilesDir() + L"\\" + from, text);
        }
        const json::Value* vals = cf.Get(L"values");
        if (!vals || vals->type != json::Value::Object) continue;
        if (fmt == L"json") {
            json::Value root;
            if (text.empty() || !json::Parser(text).Parse(root)) root = json::Value::MakeObject();
            for (auto& kv : vals->obj) {
                std::wstring v = Expand(kv.second.s, s, t);
                bool num = !kv.second.s.empty() && kv.second.s[0] == L'#';
                if (num) v = v.substr(1);
                // array entries are only changed when they exist (the game writes the defaults on first run)
                bool viaArray = false;
                for (wchar_t c : kv.first) if (iswdigit(c)) viaArray = true;
                json::Value* node = cfg::JsonPath(root, kv.first, !viaArray);
                if (!node) continue;
                if (num) *node = json::Value::Num(wcstod(v.c_str(), nullptr));
                else if (v == L"true" || v == L"false") *node = json::Value::Boolean(v == L"true");
                else *node = json::Value::Str(v);
            }
            text.clear();
            json::Write(root, text);
        } else {
            for (auto& kv : vals->obj) {
                std::wstring v = Expand(kv.second.s, s, t);
                if (fmt == L"ini") cfg::SetIni(text, section, kv.first, v);
                else if (fmt == L"ue-options") cfg::SetUeOption(text, section, cf.GetStr(L"key", L"OptionSettings"), kv.first, v);
                else if (fmt == L"xml-properties") cfg::SetXmlProperty(text, kv.first, v);
                else cfg::SetProperties(text, kv.first, v);
            }
        }
        if (!WriteText(path, text)) { if (error) *error = L"Couldn't write " + path; return false; }
    }
    return true;
}

void ReadBackConfig(ServerDef& s, const Template& t) {
    const json::Value* cfs = t.Obj(L"configFiles");
    if (!cfs || cfs->type != json::Value::Array) return;
    for (auto& cf : cfs->arr) {
        std::wstring path = ConfigPath(s, t, cf), fmt = cf.GetStr(L"format", L"properties"), text;
        if (!ReadTextFile(path, text)) continue;
        const json::Value* vals = cf.Get(L"values");
        if (!vals || vals->type != json::Value::Object) continue;
        json::Value root;
        if (fmt == L"json") json::Parser(text).Parse(root);
        for (auto& kv : vals->obj) {
            std::wstring tpl = kv.second.s;
            bool quoted = tpl.size() > 4 && tpl.front() == L'"' && tpl.back() == L'"';
            if (quoted) tpl = tpl.substr(1, tpl.size() - 2);
            if (!tpl.empty() && tpl[0] == L'#') tpl = tpl.substr(1);
            if (tpl.size() < 3 || tpl.front() != L'{' || tpl.back() != L'}' || tpl.find(L'{', 1) != std::wstring::npos) continue;
            std::wstring id = tpl.substr(1, tpl.size() - 2);
            if (!t.Setting(id) && !s.settings.count(id)) continue;
            bool found = false;
            std::wstring v;
            if (fmt == L"ini") v = cfg::GetIni(text, cf.GetStr(L"section"), kv.first, &found);
            else if (fmt == L"ue-options") v = cfg::GetUeOption(text, cf.GetStr(L"key", L"OptionSettings"), kv.first, &found);
            else if (fmt == L"xml-properties") v = cfg::GetXmlProperty(text, kv.first, &found);
            else if (fmt == L"json") {
                if (json::Value* n = cfg::JsonPath(root, kv.first, false)) {
                    found = true;
                    if (n->type == json::Value::String) v = n->s;
                    else if (n->type == json::Value::Number) { wchar_t b[64]; swprintf(b, 64, L"%g", n->n); v = b; }
                    else if (n->type == json::Value::Bool) v = n->b ? L"true" : L"false";
                    else found = false;
                }
            } else v = cfg::GetProperties(text, kv.first, &found);
            if (!found) continue;
            if (quoted && v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
            s.settings[id] = v;
        }
    }
}

std::vector<std::wstring> EditableConfigFiles(const ServerDef& s, const Template& t) {
    std::vector<std::wstring> out;
    if (const json::Value* cfs = t.Obj(L"configFiles"); cfs && cfs->type == json::Value::Array)
        for (auto& cf : cfs->arr) out.push_back(ConfigPath(s, t, cf));
    static const wchar_t* extra[] = {L"bukkit.yml", L"spigot.yml", L"config\\paper-global.yml", L"config\\paper-world-defaults.yml",
                                     L"permissions.json", L"Pal\\Saved\\Config\\WindowsServer\\Engine.ini", L"Pal\\Saved\\Config\\WindowsServer\\GameUserSettings.ini",
                                     L"ShooterGame\\Saved\\Config\\WindowsServer\\GameUserSettings.ini", L"ShooterGame\\Saved\\Config\\WindowsServer\\Game.ini",
                                     L"game\\csgo\\cfg\\server.cfg", L"user_jvm_args.txt"};
    for (auto e : extra) {
        std::wstring p = s.FilesDir() + L"\\" + e;
        if (FileExists(p) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    return out;
}

// ---------------------------------------------------------------- player lists
static const json::Value* ListDef(const Template& t, const std::wstring& list) {
    const json::Value* p = t.Obj(L"players");
    return p ? p->Get(list) : nullptr;
}

std::vector<std::wstring> ReadPlayerList(const ServerDef& s, const Template& t, const std::wstring& list) {
    std::vector<std::wstring> out;
    const json::Value* d = ListDef(t, list);
    if (!d) return out;
    std::wstring text;
    if (!ReadTextFile(s.FilesDir() + L"\\" + d->GetStr(L"file"), text)) return out;
    if (d->GetStr(L"format") == L"mc-json") {
        json::Value v;
        if (json::Parser(text).Parse(v) && v.type == json::Value::Array)
            for (auto& e : v.arr) { std::wstring n = e.GetStr(L"name"); if (!n.empty()) out.push_back(n); }
    } else {
        size_t i = 0;
        while (i < text.size()) {
            size_t e = text.find(L'\n', i);
            if (e == std::wstring::npos) e = text.size();
            std::wstring l = Trim(text.substr(i, e - i));
            i = e + 1;
            if (!l.empty() && l[0] != L'#' && l.compare(0, 2, L"//") != 0) out.push_back(l);
        }
    }
    return out;
}

// Mojang profile lookup for offline whitelist edits (uuid with dashes; "" when unknown or offline)
static std::wstring MojangUuid(const std::wstring& name) {
    net::Response r;
    if (!net::Get(L"https://api.mojang.com/users/profiles/minecraft/" + name, r) || r.status != 200) return L"";
    json::Value v;
    if (!json::Parser(Utf8ToWide(r.body)).Parse(v)) return L"";
    std::wstring id = v.GetStr(L"id");
    if (id.size() != 32) return L"";
    return id.substr(0, 8) + L"-" + id.substr(8, 4) + L"-" + id.substr(12, 4) + L"-" + id.substr(16, 4) + L"-" + id.substr(20);
}

bool WritePlayerList(const ServerDef& s, const Template& t, const std::wstring& list, const std::vector<std::wstring>& names) {
    const json::Value* d = ListDef(t, list);
    if (!d) return false;
    std::wstring path = s.FilesDir() + L"\\" + d->GetStr(L"file");
    if (d->GetStr(L"format") != L"mc-json") {
        std::wstring text;
        for (auto& n : names) text += n + L"\r\n";
        return WriteFileAtomic(path, WideToUtf8(text));
    }
    json::Value old, out = json::Value::MakeArray();
    std::wstring text;
    if (ReadTextFile(path, text)) json::Parser(text).Parse(old);
    for (auto& n : names) {
        const json::Value* existing = nullptr;
        if (old.type == json::Value::Array) for (auto& e : old.arr) if (IEquals(e.GetStr(L"name"), n)) existing = &e;
        if (existing) { out.arr.push_back(*existing); continue; }
        json::Value e = json::Value::MakeObject();
        std::wstring uuid = MojangUuid(n);
        if (!uuid.empty()) e.Set(L"uuid", json::Value::Str(uuid));
        e.Set(L"name", json::Value::Str(n));
        if (list == L"ops") { e.Set(L"level", json::Value::Num(4)); e.Set(L"bypassesPlayerLimit", json::Value::Boolean(false)); }
        if (list == L"bans") {
            wchar_t b[64]; time_t now = time(nullptr); struct tm tmv; localtime_s(&tmv, &now);
            wcsftime(b, 64, L"%Y-%m-%d %H:%M:%S +0000", &tmv);
            e.Set(L"created", json::Value::Str(b)); e.Set(L"source", json::Value::Str(L"Outpost"));
            e.Set(L"expires", json::Value::Str(L"forever")); e.Set(L"reason", json::Value::Str(L"Banned by an operator."));
        }
        out.arr.push_back(e);
    }
    std::wstring o;
    json::Write(out, o);
    return WriteFileAtomic(path, WideToUtf8(o));
}
