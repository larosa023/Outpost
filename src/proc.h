// Outpost - child processes: run-and-capture, the game server process itself, Ctrl+C, CPU and memory
#pragma once
#include "common.h"
#include <functional>
#include <atomic>

namespace proc {

// Runs a program hidden, calling onLine for each line of output (stdout+stderr). Returns the exit code, or -1.
int RunCapture(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
               const std::function<void(const std::string&)>& onLine, std::atomic<bool>* cancel = nullptr,
               const std::vector<std::pair<std::wstring, std::wstring>>& env = {});

// A long-running child with stdin/stdout pipes, placed in a job object so it (and anything it starts)
// ends when Outpost's host ends, and can be measured as a whole.
struct Child {
    HANDLE process = nullptr, job = nullptr, in = nullptr, out = nullptr;
    void* pty = nullptr;                            // pseudo-console (for games that only read a real console)
    DWORD pid = 0;
    bool Start(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
               const std::vector<std::pair<std::wstring, std::wstring>>& env, uint64_t memoryLimit, std::wstring* error,
               bool usePty = false);
    void ClosePty();                                // after the game exits, so the output pipe ends
    bool Write(const std::string& line);            // sends line + "\r\n" to stdin
    bool Running() const;
    DWORD ExitCode() const;
    void SendCtrlC();                               // like pressing Ctrl+C in the server's console
    void Kill();                                    // whole job (last resort)
    void SetLowPriority(bool low);
    void Close();
};

struct Usage { double cpu = 0; uint64_t ram = 0; };   // cpu: percent of the whole PC
class Meter {
public:
    Usage Sample(HANDLE job);
private:
    ULONGLONG lastCpu_ = 0, lastTick_ = 0;
};

// Whole-PC stats for the "This PC" panel
struct SysStats { double cpu = 0; uint64_t ramUsed = 0, ramTotal = 0; };
SysStats SampleSystem();

// True while a full-screen game or app is in front (for "lower priority while I'm playing")
bool GameInForeground();

std::wstring Quote(const std::wstring& s);
}
