// Outpost - a server: its settings (server.json), its folder, and turning settings into the game's
// own config files. Shared by the window and the host process that runs the server.
#pragma once
#include "common.h"
#include "template.h"

struct ModEntry {
    std::wstring source;        // modrinth | curseforge | file
    std::wstring projectId, versionId, title, versionName, file, hash;   // file: name inside the mods folder
    bool enabled = true;
};

struct ServerDef {
    std::wstring id, name, game, variant;
    std::wstring version = L"latest";        // requested game version ("latest" follows new releases)
    std::wstring installedVersion;           // what's on disk (game version, or Steam build id)
    std::wstring root;                       // the server's folder; game files are in root\files
    std::wstring created;
    std::map<std::wstring, std::wstring> settings;
    std::map<std::wstring, std::wstring> vars;   // set by the installer: java, launchArgs, jar…
    int ram = 0;                             // MB; 0 = template default (Java) or no limit
    // automation
    bool autoRestart = true;
    int restartHours = 0;                    // restart every N hours (0 = off)
    std::wstring restartAt;                  // or every day at "HH:MM" ("" = off)
    bool sleepWhenEmpty = false;
    int sleepMinutes = 15;
    bool startOnBoot = false;
    int backupMinutes = 30;                  // automatic backups while running (0 = off)
    int keepBackups = 10;
    std::wstring backupDir;                  // "" = DataDir()\backups\<id>
    bool upnp = true;
    bool firewall = true;
    bool lowerPriorityWhileGaming = true;
    std::vector<ModEntry> mods;

    std::wstring FilesDir() const { return root + L"\\files"; }
    std::wstring Get(const std::wstring& key) const { auto it = settings.find(key); return it == settings.end() ? L"" : it->second; }
    int Port(const std::wstring& id = L"port") const;
};

namespace servers {
std::wstring BaseDir();                                   // DataDir()\servers
std::vector<ServerDef> LoadAll();
bool Load(const std::wstring& root, ServerDef& out);
bool Save(const ServerDef& s);
std::wstring NewId(const std::wstring& name);
// A copy of the template is kept in the server's folder so a running server never changes under you.
bool LoadTemplate(const ServerDef& s, Template& t);
bool SaveTemplate(const ServerDef& s, const Template& t);
// Fills defaults for every setting the server doesn't have yet ({random} → a password).
void ApplyDefaults(ServerDef& s, const Template& t);
std::wstring RandomToken(int n);
}

// {setting}, {setting|prefix} (prefix + value, or nothing when empty), and the built-ins:
// {dir} {root} {name} {ram} {ramMin} {java} {launchArgs} {jar} plus any installer var.
std::wstring Expand(const std::wstring& text, const ServerDef& s, const Template& t);

// Writes the template's "files" and "configFiles" with the server's settings.
bool WriteConfigFiles(const ServerDef& s, const Template& t, std::wstring* error);
// After the raw editor changed a config file: copies mapped values back into the settings.
void ReadBackConfig(ServerDef& s, const Template& t);
// Config files the raw editor can open (template config files, plus common ones that exist).
std::vector<std::wstring> EditableConfigFiles(const ServerDef& s, const Template& t);

// Player list files (whitelist / ops / bans) for editing while the server is stopped.
std::vector<std::wstring> ReadPlayerList(const ServerDef& s, const Template& t, const std::wstring& list);
bool WritePlayerList(const ServerDef& s, const Template& t, const std::wstring& list, const std::vector<std::wstring>& names);

// Low-level config helpers (also used by tests)
namespace cfg {
bool SetProperties(std::wstring& text, const std::wstring& key, const std::wstring& value);
std::wstring GetProperties(const std::wstring& text, const std::wstring& key, bool* found = nullptr);
bool SetIni(std::wstring& text, const std::wstring& section, const std::wstring& key, const std::wstring& value);
std::wstring GetIni(const std::wstring& text, const std::wstring& section, const std::wstring& key, bool* found = nullptr);
bool SetUeOption(std::wstring& text, const std::wstring& section, const std::wstring& key, const std::wstring& opt, const std::wstring& value);
std::wstring GetUeOption(const std::wstring& text, const std::wstring& key, const std::wstring& opt, bool* found = nullptr);
bool SetXmlProperty(std::wstring& text, const std::wstring& name, const std::wstring& value);
std::wstring GetXmlProperty(const std::wstring& text, const std::wstring& name, bool* found = nullptr);
}
