// Outpost - app settings
#include "config.h"
#include "util.h"
#include "json.h"
#include <algorithm>

Config g_cfg;

static std::wstring Path() { return DataDir() + L"\\settings.json"; }

void ConfigLoad() {
    std::wstring text;
    if (!ReadTextFile(Path(), text)) return;
    json::Value v;
    if (!json::Parser(text).Parse(v)) return;
    Config& c = g_cfg;
    c.theme = v.GetStr(L"theme", c.theme);
    c.accent = v.GetStr(L"accent", c.accent);
    c.backdrop = v.GetStr(L"backdrop", c.backdrop);
    c.fontScale = std::max(0.85, std::min(1.4, v.GetNum(L"fontScale", c.fontScale)));
    c.density = v.GetStr(L"density", c.density);
    c.radius = std::max(0, std::min(16, (int)v.GetNum(L"radius", c.radius)));
    c.animation = v.GetStr(L"animation", c.animation);
    c.hangarThemes = v.GetBool(L"hangarThemes", c.hangarThemes);
    c.beta = v.GetBool(L"beta", c.beta);
    c.autoUpdate = v.GetBool(L"autoUpdate", c.autoUpdate);
    c.checkHours = std::max(1, (int)v.GetNum(L"checkHours", c.checkHours));
    c.skipVersion = v.GetStr(L"skipVersion");
    c.startWithWindows = v.GetBool(L"startWithWindows", c.startWithWindows);
    c.closeToTray = v.GetBool(L"closeToTray", c.closeToTray);
    c.notifyJoin = v.GetBool(L"notifyJoin", c.notifyJoin);
    c.notifyCrash = v.GetBool(L"notifyCrash", c.notifyCrash);
    c.notifyBackupFail = v.GetBool(L"notifyBackupFail", c.notifyBackupFail);
    c.hidePublicIp = v.GetBool(L"hidePublicIp", c.hidePublicIp);
    c.curseForgeKey = v.GetStr(L"curseForgeKey");
    c.duckDomain = v.GetStr(L"duckDomain");
    c.duckToken = v.GetStr(L"duckToken");
    c.windowX = (int)v.GetNum(L"windowX", c.windowX);
    c.windowY = (int)v.GetNum(L"windowY", c.windowY);
    c.windowW = (int)v.GetNum(L"windowW", 0);
    c.windowH = (int)v.GetNum(L"windowH", 0);
    c.maximized = v.GetBool(L"maximized", false);
    c.lastServer = v.GetStr(L"lastServer");
    c.lastTab = (int)v.GetNum(L"lastTab", 0);
}

void ConfigSave() {
    const Config& c = g_cfg;
    json::Value v = json::Value::MakeObject();
    v.Set(L"schema", json::Value::Num(1));
    v.Set(L"theme", json::Value::Str(c.theme));
    v.Set(L"accent", json::Value::Str(c.accent));
    v.Set(L"backdrop", json::Value::Str(c.backdrop));
    v.Set(L"fontScale", json::Value::Num(c.fontScale));
    v.Set(L"density", json::Value::Str(c.density));
    v.Set(L"radius", json::Value::Num(c.radius));
    v.Set(L"animation", json::Value::Str(c.animation));
    v.Set(L"hangarThemes", json::Value::Boolean(c.hangarThemes));
    v.Set(L"beta", json::Value::Boolean(c.beta));
    v.Set(L"autoUpdate", json::Value::Boolean(c.autoUpdate));
    v.Set(L"checkHours", json::Value::Num(c.checkHours));
    v.Set(L"skipVersion", json::Value::Str(c.skipVersion));
    v.Set(L"startWithWindows", json::Value::Boolean(c.startWithWindows));
    v.Set(L"closeToTray", json::Value::Boolean(c.closeToTray));
    v.Set(L"notifyJoin", json::Value::Boolean(c.notifyJoin));
    v.Set(L"notifyCrash", json::Value::Boolean(c.notifyCrash));
    v.Set(L"notifyBackupFail", json::Value::Boolean(c.notifyBackupFail));
    v.Set(L"hidePublicIp", json::Value::Boolean(c.hidePublicIp));
    v.Set(L"curseForgeKey", json::Value::Str(c.curseForgeKey));
    v.Set(L"duckDomain", json::Value::Str(c.duckDomain));
    v.Set(L"duckToken", json::Value::Str(c.duckToken));
    v.Set(L"windowX", json::Value::Num(c.windowX));
    v.Set(L"windowY", json::Value::Num(c.windowY));
    v.Set(L"windowW", json::Value::Num(c.windowW));
    v.Set(L"windowH", json::Value::Num(c.windowH));
    v.Set(L"maximized", json::Value::Boolean(c.maximized));
    v.Set(L"lastServer", json::Value::Str(c.lastServer));
    v.Set(L"lastTab", json::Value::Num(c.lastTab));
    std::wstring out;
    json::Write(v, out);
    EnsureDir(DataDir());
    WriteTextFileAtomic(Path(), out);
}

bool IsSkippedVersion(const std::wstring& ver) {
    std::wstring list = L"," + g_cfg.skipVersion + L",";
    return !ver.empty() && list.find(L"," + ver + L",") != std::wstring::npos;
}
void SkipVersion(const std::wstring& ver) {
    if (IsSkippedVersion(ver)) return;
    g_cfg.skipVersion += (g_cfg.skipVersion.empty() ? L"" : L",") + ver;
    ConfigSave();
}
void PruneSkippedVersions(const std::wstring& current) {
    std::wstring out, cur;
    std::wstring list = g_cfg.skipVersion + L",";
    for (wchar_t c : list) {
        if (c != L',') { cur += c; continue; }
        if (!cur.empty() && CompareVersions(cur, current) > 0) out += (out.empty() ? L"" : L",") + cur;
        cur.clear();
    }
    if (out != g_cfg.skipVersion) { g_cfg.skipVersion = out; ConfigSave(); }
}
