# Outpost

Host game servers on your PC with one click. Pick a game, press **Start**, and send your friends the address. Outpost downloads the server, writes the config, sets up port forwarding and the firewall, backs up your world, and keeps the server running even when you close the window or update Outpost.

Part of the larosa023 suite (Hangar, Foyer, Strata, Breaker, Ember…). It's the same lightweight native stack and shares the suite's themes, and Hangar can install and live-update it.

**Status: v1.0.0.**

## Games

| Game | Server types | Where the files come from |
|---|---|---|
| Minecraft: Java Edition | Vanilla, Paper, Fabric, Forge, NeoForge | Mojang, PaperMC, Fabric, Forge, NeoForge (+ Java, automatically) |
| Minecraft: Bedrock Edition | | Mojang |
| Valheim · Palworld · Satisfactory · Project Zomboid · Enshrouded · ARK: Survival Ascended · Rust · Counter-Strike 2 · 7 Days to Die | | Steam, through SteamCMD (automatic) |
| Terraria | | terraria.org |

Every game is a template file ([docs/TEMPLATES.md](docs/TEMPLATES.md)). To add a game, add a template; no code changes.

## Install

Download from [Releases](https://github.com/larosa023/Outpost/releases):

- **Outpost-Setup.exe**: installs for you only (no admin), with a Start menu shortcut and an uninstaller. Silent: `/S`.
- **Outpost-portable.zip**: unzip anywhere. Data stays in a `data` folder next to the exe.
- **Outpost.exe**: the app by itself (what Hangar installs).

Windows may show SmartScreen the first time because the exe isn't signed yet: **More info → Run anyway**. When you first start a server, Windows asks once to add firewall rules so friends can connect.

## Using it

| | |
|---|---|
| **New server** | Game → version/loader → name → world settings → memory → Create and start. Or **Import** an existing server folder or world |
| **Overview** | Players online, CPU / memory / TPS, last backup with **Back up now** and **Restore**, live console, player list |
| **Console** | Colored log, filters, command box with history (↑ ↓) and autocomplete (Tab, including player names) |
| **Players** | Kick, ban, make admin, message everyone, whitelist / admins / bans (also while stopped), join and leave history |
| **Mods** | Minecraft: search Modrinth (or CurseForge with your API key), compatible versions only, one-click updates. Other games: their mods folder. Turn mods off without deleting them |
| **Backups** | Automatic while running (every 15 min to 6 h), keep the newest N, any folder (an external drive or OneDrive), restore with a safety backup first |
| **Settings** | Every setting as a friendly control, plus a raw editor for the config files. Automation: restart after a crash, scheduled restarts, sleep when empty and wake on join, start with Windows, go easy while you game. Network: UPnP, firewall, a connection check with router steps. Game files: version, update (backs up first), repair |

The header shows the address to give friends, with **Copy**; the eye button hides your IP on screen. For a name that doesn't change when your IP does, add a free [DuckDNS](https://www.duckdns.org) name in Outpost settings.

**Closing Outpost doesn't stop your servers.** They keep running in the background (see the tray). Use **Stop**, or the tray's **Stop all servers and quit**.

Keyboard: **Ctrl+N** new server · **Ctrl+1–6** tabs · **↑ ↓** switch servers · **Ctrl+,** settings · **F5** refresh.

## Updates

Outpost checks GitHub on startup and every few hours (Stable or Beta channel). It downloads the new version and checks its SHA-256 against both GitHub's digest and `SHA256SUMS.txt`. Then it restarts only the window; **game servers keep running** and the new window reconnects to them. If the new version doesn't start within 20 seconds, Outpost puts the old one back and skips that version. You can also turn on silent automatic updates, or roll back from Settings. With Hangar installed, Hangar can do all of this through the suite's IPC protocol instead.

## Command line

```bat
Outpost.exe --cli list
Outpost.exe --cli create --game minecraft-java --variant paper --name "Survival" --set eula=true --ram 4096
Outpost.exe --cli start survival
Outpost.exe --cli send survival say Hello
Outpost.exe --cli backup survival
Outpost.exe --cli stop survival
Outpost.exe --cli e2e valheim          :: the full end-to-end test used in CI
```

## Build

```bash
./build.sh                         # Linux/macOS: Zig cross-compile (pip install ziglang)
cmake -S . -B build -A x64 && cmake --build build --config Release   # Windows, VS 2022 (run sync-version.sh first)
```

A release is published when you push a tag `vX.Y.Z` that matches `OUTPOST_VERSION` in `src/common.h` (or run the Build workflow with "release" ticked). Versions with a `-` are pre-releases (the Beta channel).

## Docs

- [Architecture: processes, data, testing](docs/ARCHITECTURE.md)
- [Game templates](docs/TEMPLATES.md)
- [Hangar manifest entry](docs/HANGAR.md)
