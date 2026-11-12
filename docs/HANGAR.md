# Outpost in Hangar

Hangar's catalog entry (in [larosa023/Hangar `manifest.json`](https://github.com/larosa023/Hangar/blob/main/manifest.json)):

```json
{
  "id": "outpost",
  "name": "Outpost",
  "repo": "larosa023/Outpost",
  "description": "Host game servers on your PC with one click.",
  "category": "gaming",
  "icon": "https://raw.githubusercontent.com/larosa023/Outpost/main/res/outpost.png",
  "exe": "Outpost.exe",
  "install": { "type": "portable", "asset": "Outpost.exe", "checksums": "SHA256SUMS.txt", "dir": "%LOCALAPPDATA%\\Programs\\Outpost" },
  "args": { "quit": "--quit" },
  "liveUpdate": true
}
```

- `portable` with `dir` = `%LOCALAPPDATA%\Programs\Outpost`, the same folder the installer uses. A copy installed with `Outpost-Setup.exe` shows up in Hangar as installed, and Outpost keeps updating itself when Hangar isn't there.
- `liveUpdate: true`: Outpost speaks Hangar's IPC protocol 1 (`docs/IPC.md` in Hangar).
  - Its pipe is `\\.\pipe\larosa023.outpost.<session>`, and its discovery file is `%LOCALAPPDATA%\Hangar\run\outpost.json`.
  - It answers `hello`, `status` (busy while game files download), `prepare-update` (saves the page and server you're on), `restart`, `cancel-update`, `apply-theme` (live, in the shared theme format), `apply-settings` and `export-settings`.
  - After starting it sends `health` to Hangar's pipe, and `restored` ("2 running servers") after a live update.
- `--quit` closes the window only. Game servers keep running in their host processes, so a Hangar update never interrupts players.
