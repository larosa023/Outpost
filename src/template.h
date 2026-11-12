// Outpost - game templates. A template is one JSON file that describes how to download, configure,
// run and talk to a game's dedicated server (docs/TEMPLATES.md). Adding a game = adding a template.
#pragma once
#include "common.h"
#include "json.h"

struct TOption { std::wstring value, label; };

struct TSetting {
    std::wstring id, label, type, def, help, group, required;   // type: text password number bool choice seed
    bool wizard = false, hidden = false;
    std::wstring valOff = L"false", valOn = L"true";            // bool: the two values written to config
    std::vector<TOption> options;
    double min = 0, max = 0, step = 1;
};

struct TPort {
    std::wstring id, name, protocol;   // tcp | udp | both
    int def = 0, count = 1;
    bool local = false;                // used only on this PC (RCON, admin API): never forwarded
};

struct TVariant { std::wstring id, name, description; };

struct Template {
    std::wstring id, name, shortName, description, notes, sourceType;
    uint32_t color = 0x5B8C32;
    int version = 1;
    std::vector<TVariant> variants;
    std::vector<TSetting> settings;
    std::vector<TPort> ports;
    bool hasRam = false, ramJava = false;
    int ramDefault = 4096, ramMin = 1024;
    bool consoleInput = true;
    std::wstring consoleVia, consoleNote;     // "" (stdin) | rcon | telnet
    std::vector<std::wstring> autocomplete;
    std::wstring perfLabel; double perfMax = 0;
    std::wstring modsType;                    // modrinth | folder | settings | none
    std::wstring sleepWake;                   // minecraft | tcp | udp | ""
    json::Value raw;                          // everything, for the less common fields
    std::wstring origin;                      // built-in | downloaded | your templates folder
    std::wstring file;

    const TSetting* Setting(const std::wstring& sid) const;
    const json::Value* Obj(const std::wstring& key) const { return raw.Get(key); }
    std::wstring Str(const std::wstring& section, const std::wstring& key) const;
    bool HasCommand(const std::wstring& action) const;
};

bool ParseTemplate(const std::wstring& text, Template& t, std::wstring* error);

namespace templates {
void Load();                                  // built-in + downloaded + DataDir()\templates (user wins)
const std::vector<Template>& All();
const Template* Find(const std::wstring& id);
std::wstring UserDir();
void FetchRemote(HWND notify);                // background: newer templates from the repo (posts WM_OP_TEMPLATES)
}
