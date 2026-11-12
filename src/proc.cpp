// Outpost - child processes
#include "proc.h"
#include "util.h"
#include <shellapi.h>
#include <psapi.h>
#include <thread>
#include <algorithm>

namespace proc {

std::wstring Quote(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    return L"\"" + s + L"\"";
}

static std::vector<wchar_t> BuildEnv(const std::vector<std::pair<std::wstring, std::wstring>>& extra) {
    std::vector<wchar_t> block;
    std::map<std::wstring, std::wstring> vars;   // case-insensitive keys via lower-case index
    std::map<std::wstring, std::wstring> names;
    if (wchar_t* e = GetEnvironmentStringsW()) {
        for (wchar_t* p = e; *p; p += wcslen(p) + 1) {
            std::wstring kv = p;
            size_t eq = kv.find(L'=', 1);
            if (eq == std::wstring::npos) continue;
            std::wstring k = kv.substr(0, eq);
            vars[Lower(k)] = kv.substr(eq + 1);
            names[Lower(k)] = k;
        }
        FreeEnvironmentStringsW(e);
    }
    for (auto& kv : extra) { vars[Lower(kv.first)] = kv.second; names[Lower(kv.first)] = kv.first; }
    for (auto& kv : vars) {
        std::wstring line = names[kv.first] + L"=" + kv.second;
        block.insert(block.end(), line.begin(), line.end());
        block.push_back(0);
    }
    block.push_back(0);
    return block;
}

int RunCapture(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
               const std::function<void(const std::string&)>& onLine, std::atomic<bool>* cancel,
               const std::vector<std::pair<std::wstring, std::wstring>>& env) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Quote(exe) + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    auto envBlock = BuildEnv(env);
    BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             envBlock.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { Log(L"couldn't run %ls (error %lu)", exe.c_str(), GetLastError()); CloseHandle(rd); return -1; }
    CloseHandle(pi.hThread);
    std::thread watchdog;
    std::atomic<bool> done{false};
    if (cancel) watchdog = std::thread([&] {
        while (!done) { if (*cancel) { TerminateProcess(pi.hProcess, 1); break; } Sleep(200); }
    });
    std::string pending;
    char chunk[4096];
    DWORD n;
    while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n) {
        pending.append(chunk, n);
        size_t p;
        while ((p = pending.find_first_of("\r\n")) != std::string::npos) {
            std::string line = pending.substr(0, p);
            pending.erase(0, p + 1);
            if (!line.empty() && onLine) onLine(line);
        }
    }
    if (!pending.empty() && onLine) onLine(pending);
    WaitForSingleObject(pi.hProcess, INFINITE);
    done = true;
    if (watchdog.joinable()) watchdog.join();
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return (int)code;
}

// ConPTY (Windows 10 1809+), loaded at run time
typedef HRESULT(WINAPI* CreatePCFn)(COORD, HANDLE, HANDLE, DWORD, void**);
typedef void(WINAPI* ClosePCFn)(void*);

static bool StartPty(Child& c, const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
                     const std::vector<wchar_t>& envBlock, std::wstring* error) {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    auto create = (CreatePCFn)GetProcAddress(k, "CreatePseudoConsole");
    if (!create) { *error = L"This game needs Windows 10 (1809) or newer."; return false; }
    HANDLE inR, inW, outR, outW;
    if (!CreatePipe(&inR, &inW, nullptr, 0) || !CreatePipe(&outR, &outW, nullptr, 1 << 16)) { *error = L"Couldn't create pipes."; return false; }
    void* hpc = nullptr;
    COORD size{1000, 60};   // wide, so long lines aren't wrapped
    if (FAILED(create(size, inR, outW, 0, &hpc))) { *error = L"Couldn't create a console for the server."; CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW); return false; }
    CloseHandle(inR); CloseHandle(outW);
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, 0x00020016 /*PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE*/, hpc, sizeof(hpc), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Quote(exe) + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                             (LPVOID)envBlock.data(), cwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        *error = L"Couldn't start " + PathName(exe) + L" (error " + std::to_wstring(GetLastError()) + L").";
        ((ClosePCFn)GetProcAddress(k, "ClosePseudoConsole"))(hpc);
        CloseHandle(inW); CloseHandle(outR);
        return false;
    }
    AssignProcessToJobObject(c.job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    c.process = pi.hProcess; c.pid = pi.dwProcessId; c.in = inW; c.out = outR; c.pty = hpc;
    return true;
}

void Child::ClosePty() {
    if (!pty) return;
    auto close = (ClosePCFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ClosePseudoConsole");
    if (close) close(pty);
    pty = nullptr;
}

bool Child::Start(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
                  const std::vector<std::pair<std::wstring, std::wstring>>& env, uint64_t memoryLimit, std::wstring* error, bool usePty) {
    if (usePty) {
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (memoryLimit) { li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY; li.JobMemoryLimit = (SIZE_T)memoryLimit; }
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        auto envBlock = BuildEnv(env);
        if (StartPty(*this, exe, args, cwd, envBlock, error)) return true;
        CloseHandle(job); job = nullptr;
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 1 << 16)) { *error = L"Couldn't create pipes."; return false; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (memoryLimit) { li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY; li.JobMemoryLimit = (SIZE_T)memoryLimit; }
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = outW;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Quote(exe) + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    auto envBlock = BuildEnv(env);
    // CREATE_NO_WINDOW still gives the server a (hidden) console, which is what lets us send it Ctrl+C
    BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                             envBlock.data(), cwd.c_str(), &si, &pi);
    DWORD err = GetLastError();
    CloseHandle(inR); CloseHandle(outW);
    if (!ok) {
        CloseHandle(inW); CloseHandle(outR); CloseHandle(job); job = nullptr;
        wchar_t msg[512] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, msg, 512, nullptr);
        *error = L"Couldn't start " + PathName(exe) + L": " + Trim(msg);
        return false;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process = pi.hProcess;
    pid = pi.dwProcessId;
    in = inW;
    out = outR;
    return true;
}

bool Child::Write(const std::string& line) {
    if (!in) return false;
    std::string l = line + (pty ? "\r" : "\r\n");   // a console gets Enter as \r
    DWORD w = 0;
    return WriteFile(in, l.data(), (DWORD)l.size(), &w, nullptr) && w == l.size();
}

bool Child::Running() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
DWORD Child::ExitCode() const { DWORD c = 0; if (process) GetExitCodeProcess(process, &c); return c; }

void Child::SendCtrlC() {
    if (!Running()) return;
    if (pty) { DWORD w; WriteFile(in, "\x03", 1, &w, nullptr); return; }
    // Join the server's hidden console for a moment and raise Ctrl+C there. We ignore it ourselves.
    FreeConsole();
    if (AttachConsole(pid)) {
        SetConsoleCtrlHandler(nullptr, TRUE);
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        Sleep(200);
        FreeConsole();
        // keep ignoring for a moment so a late event can't reach us
        Sleep(300);
        SetConsoleCtrlHandler(nullptr, FALSE);
    } else {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
    }
}

void Child::Kill() {
    if (job) TerminateJobObject(job, 1);
    else if (process) TerminateProcess(process, 1);
}

void Child::SetLowPriority(bool low) {
    if (!job) return;
    JOBOBJECT_BASIC_PROCESS_ID_LIST* list = (JOBOBJECT_BASIC_PROCESS_ID_LIST*)calloc(1, sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 64 * sizeof(ULONG_PTR));
    list->NumberOfAssignedProcesses = 65;
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 64 * sizeof(ULONG_PTR), nullptr)) {
        for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i)
            if (HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, (DWORD)list->ProcessIdList[i])) {
                SetPriorityClass(h, low ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
                CloseHandle(h);
            }
    }
    free(list);
}

void Child::Close() {
    ClosePty();
    if (in) { CloseHandle(in); in = nullptr; }
    if (out) { CloseHandle(out); out = nullptr; }
    if (process) { CloseHandle(process); process = nullptr; }
    if (job) { CloseHandle(job); job = nullptr; }
    pid = 0;
}

Usage Meter::Sample(HANDLE job) {
    Usage u;
    if (!job) return u;
    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION acc{};
    if (QueryInformationJobObject(job, JobObjectBasicAndIoAccountingInformation, &acc, sizeof(acc), nullptr)) {
        ULONGLONG cpu = acc.BasicInfo.TotalUserTime.QuadPart + acc.BasicInfo.TotalKernelTime.QuadPart;   // 100 ns units
        ULONGLONG tick = GetTickCount64();
        if (lastTick_ && tick > lastTick_) {
            SYSTEM_INFO si; GetSystemInfo(&si);
            double elapsed = (double)(tick - lastTick_) * 10000.0 * si.dwNumberOfProcessors;
            u.cpu = std::max(0.0, std::min(100.0, (double)(cpu - lastCpu_) * 100.0 / elapsed));
        }
        lastCpu_ = cpu; lastTick_ = tick;
    }
    // working set of every process in the job
    JOBOBJECT_BASIC_PROCESS_ID_LIST* list = (JOBOBJECT_BASIC_PROCESS_ID_LIST*)calloc(1, sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 64 * sizeof(ULONG_PTR));
    list->NumberOfAssignedProcesses = 65;
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 64 * sizeof(ULONG_PTR), nullptr)) {
        for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i)
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)list->ProcessIdList[i])) {
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) u.ram += pmc.PrivateUsage;
                CloseHandle(h);
            }
    }
    free(list);
    return u;
}

SysStats SampleSystem() {
    static ULONGLONG lastIdle = 0, lastTotal = 0;
    SysStats s;
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto u64 = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
        ULONGLONG i = u64(idle), total = u64(kernel) + u64(user);
        if (lastTotal && total > lastTotal) s.cpu = 100.0 * (1.0 - (double)(i - lastIdle) / (double)(total - lastTotal));
        lastIdle = i; lastTotal = total;
    }
    MEMORYSTATUSEX m{sizeof(m)};
    if (GlobalMemoryStatusEx(&m)) { s.ramTotal = m.ullTotalPhys; s.ramUsed = m.ullTotalPhys - m.ullAvailPhys; }
    return s;
}

bool GameInForeground() {
    QUERY_USER_NOTIFICATION_STATE st;
    if (SUCCEEDED(SHQueryUserNotificationState(&st)) && (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_BUSY)) return true;
    HWND fg = GetForegroundWindow();
    if (!fg || fg == GetDesktopWindow() || fg == GetShellWindow()) return false;
    wchar_t cls[64];
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman")) return false;
    RECT r;
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(fg, &r) || !GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    // borderless full-screen games cover the whole monitor
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

} // namespace proc
