// Outpost - the host: one small background Outpost process per running server ("Outpost.exe --host <folder>").
//
// It owns the game server process: starts it, reads its console, answers the window over a named pipe,
// takes scheduled backups, restarts it on a crash or a schedule, puts it to sleep when nobody's on,
// and wakes it when someone connects. Because it's a separate process (started from a copy of the exe
// in DataDir()\runtime), closing or updating Outpost never stops a server: the new window reconnects.
//
// Pipe: \\.\pipe\outpost.<serverId>.<session>, one JSON object per line.
//   window -> host: {"type":"subscribe"} {"type":"command","text":"say hi"} {"type":"action","action":"kick","player":"Bob","text":""}
//                   {"type":"stop"} {"type":"restart"} {"type":"backup","label":"manual"} {"type":"reload"} {"type":"sleep"}
//                   {"type":"wake"} {"type":"players"}
//   host -> window: {"ev":"line","n":12,"t":1731400000,"k":"info|warn|error|chat|join|leave|cmd|outpost","s":"text"}
//                   {"ev":"status", ...see Host::StatusJson} {"ev":"notify","kind":"crash|join|leave|backup|backup-failed|ready","text":"…"}
//                   {"ev":"result","ok":true,"text":"…"}
#pragma once
#include "server.h"

namespace host {
int Main(const std::wstring& root);
std::wstring PipeName(const std::wstring& serverId);
// Starts the host for a server (no-op if it's already running). Called by the window and at boot.
bool Launch(const ServerDef& s, std::wstring* error);
bool IsRunning(const std::wstring& serverId);     // a host holds the server's mutex
void CleanupRuntimeCopies();                       // removes old host copies nobody is using
}
