# Outpost architecture

## Stack

**Native C++17, Win32 + Direct2D/DirectWrite, cross-compiled with Zig.** It's the same stack and toolchain as Hangar, Foyer, Strata and Breaker. Outpost reuses their JSON reader, theme engine, WinHTTP helper and self-updater.

- One exe of about 2 MB, with no runtime. It opens instantly, and the window uses about 20–30 MB of RAM. Game servers use what you give them.
- [miniz](https://github.com/richgel999/miniz) (MIT, in `src/miniz/`) handles zip: backups, and unpacking SteamCMD, Java, Bedrock and Terraria.

## Processes: why updating Outpost never stops a server

```
Outpost.exe (window, tray)  ──named pipe──►  Outpost host (one per running server)  ──stdin/stdout, RCON, REST──►  game server
                                              DataDir\runtime\host\Outpost-<ver>-<hash>.exe --host <server folder>
```

- **The host** is a copy of `Outpost.exe` started with `--host`. It runs from its own copy in `runtime\`, so the real exe can be replaced by the updater or the installer while servers run.
- It owns the game process, inside a job object so the whole tree is measured and nothing is orphaned. It reads the console and matches the template's log patterns (ready, joins, leaves). It answers RCON, telnet or REST queries for players and TPS/FPS.
- It also handles scheduled backups, scheduled restarts, auto-restart after a crash (3 tries in 15 minutes), UPnP leases, lower priority while you're gaming, and sleep when empty with wake on connect. For Minecraft, the sleeping host answers the server-list ping itself.
- It has a hidden window so Windows tells it about sign-out and shutdown. It then stops the server properly and saves the world.
- **The window** connects to each host's pipe (`\\.\pipe\outpost.<id>.<session>`, JSON lines; see `src/host.h`), gets the backlog plus live events, and sends commands. Closing the window, an Outpost update or a live update from Hangar restarts only the window. The new one reconnects and picks up the console where it left off.

## Code map

| file | |
|---|---|
| `template.*` | loading and parsing game templates (built-in, downloaded, user folder) |
| `server.*` | `server.json`, placeholders, writing/reading every config format, whitelist/ops/bans files |
| `provision.*` | downloads: Mojang, Paper (Fill v3), Fabric, Forge, NeoForge, Temurin Java, Bedrock, Terraria, SteamCMD, update checks |
| `host.*` | the host process |
| `hostlink.*` | the window's side of the host pipe |
| `remote.*` | Source RCON, telnet, small HTTP client (Palworld REST, UPnP SOAP) |
| `netx.*` | local/public IP, UPnP (SSDP + SOAP, no COM), Windows Firewall (elevated helper), NAT/CGNAT detection, reachability checks, DuckDNS |
| `backup.*`, `archive.*` | backups with a manifest, keep N, restore with a safety backup |
| `mods.*` | Modrinth (search, compatible versions, hash-verified installs, bulk update check), CurseForge with your key, mod folders |
| `ops.*` | create, install, import, update with a backup first, delete |
| `hangar.*` | Hangar IPC protocol 1: hello, status, prepare-update, restart, apply-theme, apply-settings, export-settings, health ping, discovery file |
| `selfupdate.*` | GitHub releases, Stable/Beta, SHA-256 (digest + `SHA256SUMS.txt`), swap and roll back |
| `cli.*` | `Outpost.exe --cli …` for scripts and CI |
| `ui*.cpp` | the window: `ui.cpp` (widgets, input, tray, Hangar, updater), `ui_server.cpp` (server pages), `ui_app.cpp` (sidebar, wizard, settings) |

## Data

`%LOCALAPPDATA%\Outpost\` (or `data\` next to the exe in portable mode):

```
settings.json  outpost.log  themes\  templates\  cache\templates\
servers\<id>\  server.json  template.json  state.json  history.jsonl  logs\console.log  files\ (the game)
backups\<id>\<id>-20261112-153000-automatic.zip
runtime\java-21\  runtime\host\  steamcmd\  downloads\  updates\  versions\
```

## Testing

- `--cli selftest`: config formats, placeholders, template patterns against real log lines, zip round trip.
- `.github/workflows/e2e.yml` runs `--cli e2e <game>` on GitHub's Windows machines. It downloads the real server, then: start → ready → console command → player action → live backup → graceful stop → world on disk → restore (with safety backup) → start again → stop.
- `.github/workflows/screens.yml` starts a real Paper server and screenshots every page.
