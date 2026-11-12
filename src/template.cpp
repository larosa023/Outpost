// Outpost - game templates
#include "template.h"
#include "util.h"
#include "net.h"
#include <thread>
#include <algorithm>

static uint32_t HexColor(const std::wstring& s, uint32_t def) {
    if (s.size() != 7 || s[0] != L'#') return def;
    return (uint32_t)wcstoul(s.c_str() + 1, nullptr, 16);
}

const TSetting* Template::Setting(const std::wstring& sid) const {
    for (auto& s : settings) if (s.id == sid) return &s;
    return nullptr;
}

std::wstring Template::Str(const std::wstring& section, const std::wstring& key) const {
    const json::Value* o = raw.Get(section);
    return o ? o->GetStr(key) : L"";
}

bool Template::HasCommand(const std::wstring& action) const {
    const json::Value* c = raw.Get(L"commands");
    return c && !c->GetStr(action).empty();
}

static std::wstring ValStr(const json::Value* v) {
    if (!v) return L"";
    if (v->type == json::Value::String) return v->s;
    if (v->type == json::Value::Number) { wchar_t b[64]; swprintf(b, 64, L"%g", v->n); return b; }
    if (v->type == json::Value::Bool) return v->b ? L"true" : L"false";
    return L"";
}

bool ParseTemplate(const std::wstring& text, Template& t, std::wstring* error) {
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) { if (error) *error = L"not valid JSON"; return false; }
    t = Template();
    t.raw = v;
    t.id = Lower(v.GetStr(L"id"));
    t.name = v.GetStr(L"name", t.id);
    if (t.id.empty() || t.id.find_first_of(L"\\/:*?\"<>| ") != std::wstring::npos) { if (error) *error = L"missing or invalid \"id\""; return false; }
    t.shortName = v.GetStr(L"short", t.name.substr(0, 2));
    t.description = v.GetStr(L"description");
    t.notes = v.GetStr(L"notes");
    t.color = HexColor(v.GetStr(L"color"), 0x5B8C32);
    t.version = (int)v.GetNum(L"version", 1);
    if (const json::Value* s = v.Get(L"source")) t.sourceType = s->GetStr(L"type");
    if (t.sourceType.empty()) { if (error) *error = L"missing \"source\""; return false; }
    if (!v.Get(L"launch")) { if (error) *error = L"missing \"launch\""; return false; }
    if (const json::Value* a = v.Get(L"variants"); a && a->type == json::Value::Array)
        for (auto& x : a->arr) t.variants.push_back({x.GetStr(L"id"), x.GetStr(L"name", x.GetStr(L"id")), x.GetStr(L"description")});
    if (const json::Value* a = v.Get(L"ports"); a && a->type == json::Value::Array)
        for (auto& x : a->arr) {
            TPort p;
            p.id = x.GetStr(L"id", L"port"); p.name = x.GetStr(L"name", L"Game"); p.protocol = Lower(x.GetStr(L"protocol", L"tcp"));
            p.def = (int)x.GetNum(L"default", 0); p.count = std::max(1, (int)x.GetNum(L"count", 1)); p.local = x.GetBool(L"local", false);
            t.ports.push_back(p);
        }
    if (const json::Value* a = v.Get(L"settings"); a && a->type == json::Value::Array)
        for (auto& x : a->arr) {
            TSetting s;
            s.id = x.GetStr(L"id"); s.label = x.GetStr(L"label", s.id); s.type = x.GetStr(L"type", L"text");
            s.def = ValStr(x.Get(L"default")); s.help = x.GetStr(L"help"); s.group = x.GetStr(L"group", L"General");
            s.required = x.GetStr(L"required");
            s.wizard = x.GetBool(L"wizard", false); s.hidden = x.GetBool(L"hidden", false);
            s.min = x.GetNum(L"min", 0); s.max = x.GetNum(L"max", 0); s.step = x.GetNum(L"step", 1);
            if (const json::Value* vals = x.Get(L"values"); vals && vals->type == json::Value::Array && vals->arr.size() == 2) {
                s.valOff = ValStr(&vals->arr[0]); s.valOn = ValStr(&vals->arr[1]);
            }
            if (const json::Value* o = x.Get(L"options"); o && o->type == json::Value::Array)
                for (auto& op : o->arr) s.options.push_back({ValStr(op.Get(L"value")), op.GetStr(L"label", ValStr(op.Get(L"value")))});
            if (!s.id.empty()) t.settings.push_back(s);
        }
    if (const json::Value* r = v.Get(L"ram")) {
        t.hasRam = true; t.ramJava = r->GetBool(L"java", false);
        t.ramDefault = (int)r->GetNum(L"default", 4096); t.ramMin = (int)r->GetNum(L"min", 1024);
    }
    if (const json::Value* c = v.Get(L"console")) {
        t.consoleInput = c->GetBool(L"input", true);
        t.consoleVia = c->GetStr(L"via"); t.consoleNote = c->GetStr(L"note");
        if (const json::Value* a = c->Get(L"autocomplete"); a && a->type == json::Value::Array)
            for (auto& x : a->arr) t.autocomplete.push_back(x.s);
    }
    if (const json::Value* p = v.Get(L"performance")) { t.perfLabel = p->GetStr(L"label"); t.perfMax = p->GetNum(L"max", 0); }
    if (const json::Value* m = v.Get(L"mods")) t.modsType = m->GetStr(L"type", L"none");
    else t.modsType = L"none";
    if (const json::Value* s = v.Get(L"sleep")) t.sleepWake = s->GetStr(L"wake");
    return true;
}

namespace templates {

static std::vector<Template> s_all;

std::wstring UserDir() { return DataDir() + L"\\templates"; }
static std::wstring RemoteDir() { return CacheDir() + L"\\templates"; }

// Built-in templates are RCDATA resources 300.. (see res/outpost.rc, generated by build.sh)
static void LoadBuiltin(std::vector<Template>& out) {
    for (int id = 300; id < 400; ++id) {
        std::wstring text;
        if (!LoadResourceText(id, text)) break;
        Template t; std::wstring err;
        if (ParseTemplate(text, t, &err)) { t.origin = L"built-in"; out.push_back(t); }
        else Log(L"built-in template %d: %ls", id, err.c_str());
    }
}

static void LoadDir(const std::wstring& dir, const wchar_t* origin, std::vector<Template>& out, bool userWins) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (IEquals(fd.cFileName, L"index.json")) continue;
        std::wstring path = dir + L"\\" + fd.cFileName, text, err;
        Template t;
        if (!ReadTextFile(path, text) || !ParseTemplate(text, t, &err)) { Log(L"template %ls: %ls", path.c_str(), err.c_str()); continue; }
        t.origin = origin; t.file = path;
        auto it = std::find_if(out.begin(), out.end(), [&](const Template& o) { return o.id == t.id; });
        if (it == out.end()) out.push_back(t);
        else if (userWins || t.version > it->version) *it = t;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

void Load() {
    std::vector<Template> list;
    LoadBuiltin(list);
    LoadDir(RemoteDir(), L"downloaded", list, false);
    LoadDir(UserDir(), L"your templates folder", list, true);
    // keep a stable, friendly order: the suite's own order first (Minecraft first), then by name
    static const wchar_t* order[] = {L"minecraft-java", L"minecraft-bedrock", L"valheim", L"palworld", L"terraria", L"satisfactory",
                                     L"project-zomboid", L"enshrouded", L"ark-ascended", L"rust", L"cs2", L"7-days-to-die"};
    auto rank = [&](const Template& t) { for (int i = 0; i < 12; ++i) if (t.id == order[i]) return i; return 100; };
    std::stable_sort(list.begin(), list.end(), [&](const Template& a, const Template& b) {
        int ra = rank(a), rb = rank(b);
        return ra != rb ? ra < rb : _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    s_all = list;
    Log(L"%zu game templates loaded", s_all.size());
}

const std::vector<Template>& All() { return s_all; }
const Template* Find(const std::wstring& id) {
    for (auto& t : s_all) if (t.id == id) return &t;
    return nullptr;
}

void FetchRemote(HWND notify) {
    std::vector<std::pair<std::wstring, int>> have;
    for (auto& t : s_all) have.push_back({t.id, t.version});
    std::thread([notify, have] {
        net::Response r;
        if (!net::Get(std::wstring(OUTPOST_TEMPLATES_URL) + L"index.json", r) || r.status != 200) return;
        json::Value idx;
        if (!json::Parser(Utf8ToWide(r.body)).Parse(idx)) return;
        const json::Value* list = idx.Get(L"templates");
        if (!list || list->type != json::Value::Array) return;
        int got = 0;
        for (auto& e : list->arr) {
            std::wstring id = Lower(e.GetStr(L"id"));
            int ver = (int)e.GetNum(L"version", 0);
            if (id.empty() || id.find_first_of(L"\\/:.") != std::wstring::npos) continue;
            int mine = 0;
            for (auto& h : have) if (h.first == id) mine = h.second;
            if (ver <= mine) continue;
            net::Response t;
            if (!net::Get(std::wstring(OUTPOST_TEMPLATES_URL) + id + L".json", t) || t.status != 200) continue;
            Template parsed; std::wstring err;
            if (!ParseTemplate(Utf8ToWide(t.body), parsed, &err) || parsed.id != id) { Log(L"remote template %ls: %ls", id.c_str(), err.c_str()); continue; }
            EnsureDir(RemoteDir());
            if (WriteFileAtomic(RemoteDir() + L"\\" + id + L".json", t.body)) ++got;
        }
        if (got) { Log(L"%d new or updated templates downloaded", got); if (notify) PostMessageW(notify, WM_OP_TEMPLATES, 0, 0); }
    }).detach();
}

} // namespace templates
