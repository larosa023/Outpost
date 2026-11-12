# Game templates

Every game Outpost can host is described by one JSON file. **Adding a game means adding a template. You don't change any code.**

Where Outpost looks (later ones win when two have the same `id`):

1. Built into `Outpost.exe` (this repo's `templates/` folder at build time)
2. Downloaded from this repo's `templates/` on GitHub (Outpost checks `templates/index.json` at startup, so a new or fixed template reaches everyone without an app update; bump `version`)
3. **Your templates folder**: `%LOCALAPPDATA%\Outpost\templates\` (Settings → About → Game templates). Drop a file in and press **Reload templates**.

Each server keeps its own copy (`template.json` in its folder), so a template change never alters a server that's already running. It's picked up the next time you install or update that server's files.

## A complete example

```json
{
  "schema": 1,
  "id": "valheim",
  "name": "Valheim",
  "short": "VH",
  "color": "#3E6B8A",
  "version": 1,
  "description": "Viking survival for up to 10 players.",
  "source": { "type": "steamcmd", "appId": 896660 },
  "launch": {
    "exe": "valheim_server.exe",
    "args": "-nographics -batchmode -name \"{serverName}\" -port {port} -world \"{world}\" -password \"{password}\" -public {public} -savedir \"{dir}\\saves\" {crossplay}",
    "env": { "SteamAppId": "892970" }
  },
  "ports": [ { "id": "port", "name": "Game", "default": 2456, "protocol": "udp", "count": 3 } ],
  "settings": [
    { "id": "serverName", "label": "Server name", "type": "text", "default": "Outpost Valheim", "wizard": true },
    { "id": "password", "label": "Password", "type": "password", "default": "", "required": "minlen:5", "wizard": true,
      "help": "At least 5 characters." },
    { "id": "public", "label": "List in community servers", "type": "bool", "default": "0", "values": ["0", "1"] },
    { "id": "crossplay", "label": "Crossplay", "type": "bool", "default": "", "values": ["", "-crossplay"] }
  ],
  "saves": [ "saves" ],
  "stop": { "ctrlC": true, "timeout": 120 },
  "console": { "input": false },
  "log": {
    "ready": "Game server connected",
    "join": "Got character ZDOID from (.+?) :",
    "joinId": "Got connection SteamID (\\d+)",
    "leaveId": "Closing socket (\\d+)",
    "error": "(?i)exception"
  },
  "mods": { "type": "folder", "folder": "BepInEx\\plugins" },
  "sleep": { "wake": "udp" }
}
```

## Fields

| field | what it does |
|---|---|
| `id`, `name`, `short`, `color` | Identity. `short` (2 letters) and `color` draw the game's tile. Outpost uses no game logos |
| `version` | The template's own revision. Raise it when you change a published template |
| `description`, `notes` | Shown in the wizard. `notes` also appears on a new server's overview (for example "claim the server in-game") |
| `source` | Where the server files come from (below) |
| `variants` | Optional server types to choose from, such as Minecraft's Vanilla, Paper, Fabric, Forge and NeoForge: `[{ "id", "name", "description" }]` |
| `launch.exe`, `launch.args` | What to run, relative to the server's `files` folder. Both use placeholders. `launch.batch` instead reads the java line out of a game's own `.bat` (Project Zomboid) |
| `launch.env` | Extra environment variables |
| `ram` | `{ "default": 4096, "min": 1024, "java": true }`. Shows the memory slider. With `java`, `{ram}` goes into `-Xmx` |
| `ports` | `{ "id", "name", "default", "protocol": "tcp|udp|both", "count", "local" }`. Each port is also a setting (`{port}`). `local: true` ports (RCON, admin APIs) are never forwarded or opened in the firewall |
| `settings` | Friendly controls: `type` is `text`, `password`, `number` (`min`, `max`, `step`), `bool` (`values: [off, on]`), `choice` (`options: [{ value, label }]`) or `seed`. `wizard: true` asks in the new server wizard; `group` sorts the Settings tab; `hidden` hides it; `required: "true"` or `"minlen:5"`; a `default` of `{random}` makes a password |
| `files` | Files written before every start: `[{ "path", "content", "ifMissing" }]` (Minecraft's `eula.txt`) |
| `configFiles` | The game's config files, written from the settings before every start (below) |
| `saves` | What a backup contains: paths relative to `files`, or absolute (`%LOCALAPPDATA%\…`) |
| `stop` | Graceful shutdown, tried in this order: `command` (typed into the console), `rcon: [...]`, `telnet: [...]`, `api: "shutdown"` (Palworld's REST API), then `ctrlC`. After `timeout` seconds it's closed |
| `commands` | Player actions: `say`, `kick`, `ban`, `unban`, `op`, `deop`, `whitelistAdd`, `whitelistRemove`, `save`. Use `{player}` and `{text}`. `via`: `rcon` or `telnet` (default: the console). `api:announce` style names call Palworld's REST API |
| `console` | `input` (does the console take commands), `via`, `note`, `autocomplete: [...]` |
| `query` | How Outpost reads players and performance while it runs: `type` is `rcon`, `telnet`, `palworld` or `stdin`; `port`, `password`, `list` (the command), `listPattern`, `listMulti`, `listNextLine`, `perf: [{ "cmd", "pattern", "kind": "tps|mspt|fps" }]`, `ready` (the server is ready once this answers) |
| `log` | Regular expressions on console lines: `ready`, `join` and `leave` (group 1 = player), `joinId`/`leaveId` (for games that log IDs on leave), `chat`, `warn`, `error`. Start a pattern with `(?i)` to ignore case |
| `logFile` | For servers that write their log to a file instead of the console (ARK, Satisfactory, 7 Days to Die) |
| `backup` | Before a live backup: `before` commands, `waitFor` (a pattern), `poll` (repeat a command until `waitFor` matches), `after` commands, `timeout` |
| `players` | Lists Outpost can edit while the server is stopped: `whitelist`, `ops`, `bans`, each `{ "file", "format": "mc-json|lines", "label" }` |
| `mods` | `type`: `modrinth` (with `variants: { "<variant>": { "loader", "folder", "kind": "mod|plugin" } }`), `folder` (`folder`, `label`, `note`), `settings` (mods go in settings fields), or `none` |
| `sleep` | `wake`: `minecraft` (answers the server-list ping while asleep, wakes on join), `tcp` or `udp` (wakes on any connection). Leave it out if the game can't wake this way |
| `performance` | `{ "label": "TPS", "max": 20 }` for the Resources card |
| `prerequisites` | Things the server needs on the PC, installed once: `[{ "name", "check" (a file that exists when it's installed), "url", "file", "sha256", "run", "args" ({file}), "elevated" }]`. Terraria uses it for Microsoft XNA Framework 4.0 |
| `logFile`, `import` | `import.detect` lists files that identify a server folder for **Import** |

### `source` types

| type | fields | notes |
|---|---|---|
| `steamcmd` | `appId`, `branch` (optional) | SteamCMD is downloaded and run for you, anonymously. Updates compare the Steam build id |
| `minecraft` | (none) | Mojang, PaperMC (Fill v3), Fabric, Forge, NeoForge by variant. The right Java (Eclipse Temurin) is downloaded automatically. Sets `{java}` and `{launchArgs}` |
| `bedrock` | (none) | Minecraft's official Bedrock Dedicated Server. Updates keep your worlds and settings |
| `terraria` | (none) | terraria.org's dedicated server zip |
| `url` | `url`, `sha256`, `zipFolder`, `saveAs`, `version` | Anything else: a zip is unpacked into the server folder; any other file is saved there |
| `manual` | (none) | You put the files in the server's `files` folder yourself |

### `configFiles` formats

```json
{ "path": "server.properties", "format": "properties", "values": { "max-players": "{maxPlayers}" } }
```

| format | for | extra fields |
|---|---|---|
| `properties` | `key=value` files (Minecraft, Terraria, Project Zomboid) | |
| `ini` | `[Section]` + `key=value` | `section` |
| `ue-options` | Unreal's one-line option lists, e.g. Palworld's `OptionSettings=(A=1,B="x")` | `section`, `key`, `copyFrom` (a default file to start from) |
| `xml-properties` | `<property name="…" value="…"/>` (7 Days to Die) | |
| `json` | JSON files; dotted paths (`userGroups.1.password`), and a `#` prefix writes a number (`"#{port}"`) | |

Values that are exactly one placeholder (`"{maxPlayers}"`, `"\"{serverName}\""`, `"#{port}"`) are read back into the settings when you save a config file in the raw editor, so the two never disagree.

### Placeholders

`{settingId}` is any setting or port; `{setting|prefix}` writes `prefix` + value, or nothing when the value is empty (`{mods|-mods=}`). Built-in: `{dir}` (the server's game files), `{root}`, `{name}`, `{ram}`, `{ramMin}`, plus what the installer sets (`{java}`, `{launchArgs}`, `{mcVersion}`).

## Testing a template

```bat
Outpost.exe --cli templates                     :: is it loaded?
Outpost.exe --cli e2e my-game                    :: create, download, start, wait for "ready", back up, stop, restore, start, stop
```

`--cli e2e` is what CI runs on a real Windows machine for Minecraft, Valheim, Palworld and Terraria.
