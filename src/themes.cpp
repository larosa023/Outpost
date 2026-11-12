// Outpost - themes
#include "themes.h"
#include "util.h"
#include "json.h"
#include <shlobj.h>
#include <algorithm>

using namespace gfx;

const uint32_t kAccentSwatches[] = {0x4F8CFF, 0x9B6CF2, 0xE86A92, 0xFF5D5D, 0xFF8A4C, 0xF2B84B, 0x3FB98E, 0x2EC4C4, 0xA0A0AA};
const int kAccentSwatchCount = sizeof(kAccentSwatches) / sizeof(kAccentSwatches[0]);

static ThemeDef Def(const wchar_t* id, const wchar_t* name, const wchar_t* desc, bool dark, const wchar_t* backdrop, float transparency,
                    uint32_t bg, uint32_t sidebar, uint32_t surface, uint32_t hover, uint32_t strong,
                    uint32_t text, uint32_t subtext, uint32_t faint, uint32_t accent) {
    ThemeDef t;
    t.id = id; t.name = name; t.description = desc; t.dark = dark; t.backdrop = backdrop; t.transparency = transparency;
    t.bg = bg; t.sidebar = sidebar; t.surface = surface; t.surfaceHover = hover; t.surfaceStrong = strong;
    t.text = text; t.subtext = subtext; t.faint = faint; t.accent = accent;
    return t;
}

const std::vector<ThemeDef>& BuiltinThemes() {
    static std::vector<ThemeDef> list;
    if (!list.empty()) return list;
    //                id           name          description                          dark   backdrop    see-through
    //                bg        sidebar   surface   hover     strong    text      subtext   faint     accent
    list.push_back(Def(L"dark", L"Dark", L"The default", true, L"mica", 0.20f,
                       0x16161A, 0x111114, 0x222228, 0x2A2A31, 0x34343C, 0xF2F2F5, 0xA8A8B3, 0x6E6E7A, 0));
    list.push_back(Def(L"light", L"Light", L"Bright and clean", false, L"mica", 0.20f,
                       0xF5F5F7, 0xEBEBEF, 0xFFFFFF, 0xF7F7FA, 0xE6E6EC, 0x16161A, 0x5C5C68, 0x8E8E9A, 0));
    ThemeDef sys = list[0];
    sys.id = L"system"; sys.name = L"Windows"; sys.description = L"Light or dark, like Windows"; sys.followWindows = true;
    list.push_back(sys);
    list.push_back(Def(L"midnight", L"Midnight", L"Pure black for OLED screens", true, L"none", 0.0f,
                       0x000000, 0x000000, 0x0E0E10, 0x16161A, 0x202025, 0xF5F5F7, 0xA0A0AA, 0x64646E, 0x7AA2FF));
    ThemeDef glass = Def(L"glass", L"Glass", L"See-through, over your wallpaper", true, L"acrylic", 0.55f,
                         0x141418, 0x0E0E12, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xF7F7FA, 0xC2C2CC, 0x8A8A96, 0);
    glass.surfaceOpacity = 0.07f;
    list.push_back(glass);
    list.push_back(Def(L"frost", L"Frost", L"Cool arctic blues", true, L"mica", 0.12f,
                       0x2E3440, 0x272C36, 0x3B4252, 0x434C5E, 0x4C566A, 0xECEFF4, 0xC0C8D6, 0x7B8698, 0x88C0D0));
    list.push_back(Def(L"twilight", L"Twilight", L"Deep violet", true, L"mica", 0.12f,
                       0x1E1B2E, 0x19162A, 0x2A2640, 0x332E4D, 0x3E3859, 0xEEEAF7, 0xB4ADC9, 0x7D7596, 0xB794F6));
    list.push_back(Def(L"forest", L"Forest", L"Evergreen and calm", true, L"mica", 0.12f,
                       0x141B17, 0x101612, 0x1D2721, 0x24302A, 0x2E3B33, 0xE8F0EA, 0xA7B8AD, 0x6C7F72, 0x5FD08B));
    list.push_back(Def(L"sunset", L"Sunset", L"Warm and cozy", true, L"mica", 0.12f,
                       0x1C1614, 0x17110F, 0x2A201C, 0x332722, 0x3E302A, 0xF6EEE9, 0xC4B3A9, 0x8B776C, 0xFF8A4C));
    list.push_back(Def(L"ocean", L"Ocean", L"Deep teal", true, L"mica", 0.12f,
                       0x0F1A20, 0x0B151A, 0x16252D, 0x1C2E37, 0x243843, 0xE6F1F5, 0xA3BAC4, 0x66808C, 0x3CC8C8));
    list.push_back(Def(L"rose", L"Rose", L"Soft pink on plum", true, L"mica", 0.12f,
                       0x1D1519, 0x181115, 0x2A1F25, 0x33262D, 0x3E2E37, 0xF7ECF1, 0xC7AFBA, 0x8D7380, 0xFF7AA8));
    list.push_back(Def(L"graphite", L"Graphite", L"Monochrome, no color", true, L"none", 0.0f,
                       0x1A1A1A, 0x141414, 0x262626, 0x2E2E2E, 0x3A3A3A, 0xF0F0F0, 0xABABAB, 0x707070, 0xE6E6E6));
    list.push_back(Def(L"paper", L"Paper", L"Warm light, easy on the eyes", false, L"none", 0.0f,
                       0xF4EFE6, 0xEAE3D6, 0xFBF8F2, 0xF6F1E7, 0xE5DCCB, 0x2B241B, 0x6B5F50, 0x9A8E7E, 0xB5651D));
    list.push_back(Def(L"mist", L"Mist", L"Cool light gray-blue", false, L"mica", 0.15f,
                       0xEEF2F6, 0xE3E9EF, 0xFAFCFE, 0xF3F7FA, 0xDCE4EC, 0x17202A, 0x56636F, 0x8A97A3, 0x2F7DD1));
    return list;
}

std::wstring ThemesDir() { return DataDir() + L"\\themes"; }

// ---------------------------------------------------------------- JSON
static bool ParseHex(const std::wstring& s0, uint32_t& out) {
    std::wstring s = Trim(s0);
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() == 3) s = std::wstring{s[0], s[0], s[1], s[1], s[2], s[2]};
    if (s.size() != 6 || s.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) return false;
    out = (uint32_t)wcstoul(s.c_str(), nullptr, 16);
    return true;
}

static std::wstring Hex(uint32_t c) {
    wchar_t b[16];
    swprintf(b, 16, L"#%06X", c & 0xFFFFFF);
    return b;
}

bool ParseThemeJson(const std::wstring& text, ThemeDef& t, std::wstring* error) {
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) { if (error) *error = L"not valid JSON"; return false; }
    t = ThemeDef{};
    t.name = Trim(v.GetStr(L"name"));
    if (t.name.empty()) { if (error) *error = L"missing \"name\""; return false; }
    t.id = L"custom:" + Lower(v.GetStr(L"id", t.name));
    t.description = v.GetStr(L"description", L"Your theme");
    t.dark = Lower(v.GetStr(L"mode", L"dark")) != L"light";
    t.backdrop = Lower(v.GetStr(L"backdrop", L"none"));
    if (t.backdrop != L"mica" && t.backdrop != L"acrylic") t.backdrop = L"none";
    t.transparency = std::clamp((float)v.GetNum(L"transparency", 0.15), 0.0f, 0.9f);
    t.surfaceOpacity = std::clamp((float)v.GetNum(L"surfaceOpacity", 1.0), 0.02f, 1.0f);
    // start from Dark or Light so a theme only has to list the colors it changes
    const ThemeDef& base = BuiltinThemes()[t.dark ? 0 : 1];
    t.bg = base.bg; t.sidebar = base.sidebar; t.surface = base.surface; t.surfaceHover = base.surfaceHover;
    t.surfaceStrong = base.surfaceStrong; t.text = base.text; t.subtext = base.subtext; t.faint = base.faint;
    if (const json::Value* c = v.Get(L"colors"); c && c->type == json::Value::Object) {
        struct { const wchar_t* key; uint32_t* field; } map[] = {
            {L"background", &t.bg}, {L"sidebar", &t.sidebar}, {L"surface", &t.surface}, {L"surfaceHover", &t.surfaceHover},
            {L"surfaceStrong", &t.surfaceStrong}, {L"text", &t.text}, {L"subtext", &t.subtext}, {L"faint", &t.faint},
        };
        for (auto& m : map) {
            std::wstring s = c->GetStr(m.key);
            if (!s.empty() && !ParseHex(s, *m.field)) { if (error) *error = std::wstring(L"bad color for ") + m.key; return false; }
        }
        std::wstring a = Lower(c->GetStr(L"accent", L"windows"));
        if (a != L"windows" && !ParseHex(a, t.accent)) { if (error) *error = L"bad accent color"; return false; }
        if (t.accent == 0 && a != L"windows") t.accent = 0x010101;   // pure black is a real choice
    }
    t.custom = true;
    return true;
}

std::wstring ThemeToJson(const ThemeDef& t, const std::wstring& accentSetting) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"schema", json::Value::Num(1));
    v.Set(L"name", json::Value::Str(t.custom ? t.name : t.name + L" (copy)"));
    v.Set(L"description", json::Value::Str(t.description));
    v.Set(L"mode", json::Value::Str(t.dark ? L"dark" : L"light"));
    v.Set(L"backdrop", json::Value::Str(t.backdrop));
    v.Set(L"transparency", json::Value::Num(t.transparency));
    if (t.surfaceOpacity < 1.0f) v.Set(L"surfaceOpacity", json::Value::Num(t.surfaceOpacity));
    json::Value c = json::Value::MakeObject();
    c.Set(L"background", json::Value::Str(Hex(t.bg)));
    c.Set(L"sidebar", json::Value::Str(Hex(t.sidebar)));
    c.Set(L"surface", json::Value::Str(Hex(t.surface)));
    c.Set(L"surfaceHover", json::Value::Str(Hex(t.surfaceHover)));
    c.Set(L"surfaceStrong", json::Value::Str(Hex(t.surfaceStrong)));
    c.Set(L"text", json::Value::Str(Hex(t.text)));
    c.Set(L"subtext", json::Value::Str(Hex(t.subtext)));
    c.Set(L"faint", json::Value::Str(Hex(t.faint)));
    uint32_t accent = ResolveAccent(t, accentSetting);
    c.Set(L"accent", json::Value::Str(accentSetting == L"windows" || (accentSetting == L"theme" && t.accent == 0) ? L"windows" : Hex(accent)));
    v.Set(L"colors", c);
    std::wstring out;
    json::Write(v, out);
    return out;
}

std::vector<ThemeDef> LoadCustomThemes() {
    std::vector<ThemeDef> out;
    // Outpost's own theme files first, then Hangar's, so a theme saved in Hangar shows up here too
    std::vector<std::wstring> dirs = {ThemesDir(), KnownFolder(FOLDERID_LocalAppData) + L"\\Hangar\\themes"};
    for (auto& dir : dirs) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring path = dir + L"\\" + fd.cFileName, text, err;
            ThemeDef t;
            if (ReadTextFile(path, text) && ParseThemeJson(text, t, &err)) {
                t.file = path;
                bool dup = false;
                for (auto& o : out) dup |= o.id == t.id;
                if (!dup) out.push_back(t);
            } else {
                Log(L"theme %ls: %ls", fd.cFileName, err.c_str());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(out.begin(), out.end(), [](const ThemeDef& a, const ThemeDef& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

ThemeDef FindTheme(const std::wstring& id, const std::vector<ThemeDef>& custom) {
    for (auto& t : BuiltinThemes()) if (t.id == id) return t;
    for (auto& t : custom) if (t.id == id) return t;
    return BuiltinThemes()[0];
}

// ---------------------------------------------------------------- building
uint32_t ResolveAccent(const ThemeDef& def, const std::wstring& accentSetting) {
    uint32_t c;
    if (accentSetting.size() == 7 && accentSetting[0] == L'#' && ParseHex(accentSetting, c)) return c;
    if (accentSetting == L"theme" && def.accent) return def.accent & 0xFFFFFF;
    return SystemAccent();
}

Theme MakeTheme(const ThemeDef& def0, const std::wstring& accentSetting) {
    ThemeDef def = def0;
    if (def.followWindows) {
        const ThemeDef& base = BuiltinThemes()[SystemUsesLightTheme() ? 1 : 0];
        def = base;
    }
    Theme t;
    t.dark = def.dark;
    bool wantsBackdrop = def.backdrop != L"none";
    t.backdrop = wantsBackdrop && BackdropSupported();
    t.backdropType = def.backdrop == L"acrylic" ? 3 : 2;
    t.captionRgb = def.bg;

    D2D1_COLOR_F accent = Rgb(ResolveAccent(def, accentSetting));
    // keep the accent readable on the background
    if (t.dark && Luma(accent) < 0.30f) accent = Mix(accent, Rgb(0xFFFFFF), 0.35f);
    if (!t.dark && Luma(accent) > 0.55f) accent = Mix(accent, Rgb(0x000000), 0.30f);
    t.accent = accent;
    t.accentHover = t.dark ? Mix(accent, Rgb(0xFFFFFF), 0.12f) : Mix(accent, Rgb(0x000000), 0.10f);
    t.onAccent = Luma(accent) > 0.55f ? Rgb(0x101014) : Rgb(0xFFFFFF);

    float tr = t.backdrop ? def.transparency : 0.0f;
    t.bg = Rgb(def.bg, 1.0f - tr);
    t.sidebar = Rgb(def.sidebar, t.backdrop ? std::max(0.25f, 1.0f - tr * 2.2f) : 1.0f);
    if (def.surfaceOpacity < 1.0f) {
        // see-through cards; without a backdrop, blend them over the background instead
        float o = def.surfaceOpacity;
        auto layer = [&](uint32_t c, float k) {
            return t.backdrop ? Rgb(c, std::min(1.0f, o * k)) : Mix(Rgb(def.bg), Rgb(c), std::min(1.0f, o * k));
        };
        t.surface = layer(def.surface, 1.0f);
        t.surfaceHover = layer(def.surfaceHover, 1.6f);
        t.surfaceStrong = layer(def.surfaceStrong, 2.3f);
    } else {
        t.surface = Rgb(def.surface);
        t.surfaceHover = Rgb(def.surfaceHover);
        t.surfaceStrong = Rgb(def.surfaceStrong);
    }
    t.divider = t.dark ? Rgb(0xFFFFFF, 0.08f) : Rgb(0x000000, 0.08f);
    t.text = Rgb(def.text);
    t.subtext = Rgb(def.subtext);
    t.faint = Rgb(def.faint);
    if (t.dark) { t.good = Rgb(0x5BD18B); t.warn = Rgb(0xF2B84B); t.bad = Rgb(0xFF6B6B); t.info = Rgb(0x6CB4FF); }
    else { t.good = Rgb(0x1F9D57); t.warn = Rgb(0xB7791F); t.bad = Rgb(0xD64545); t.info = Rgb(0x2F6FDB); }
    // themes with a strong accent-colored identity use it for "info" too, so badges match
    if (def.accent && accentSetting == L"theme") t.info = accent;
    return t;
}
