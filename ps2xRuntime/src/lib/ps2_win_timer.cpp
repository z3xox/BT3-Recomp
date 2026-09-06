// [wintimer] Windows only: ask for a 1 ms scheduler tick for the life of the process.
//
// The runtime paces frames and polls for barrier work with short timed waits (a few hundred
// microseconds to a few milliseconds). On Windows every such wait rounds up to the timer tick,
// 15.6 ms by default, so the GL thread presented 3-30 frames a second in fights where Linux
// presents 50-60. timeBeginPeriod(1) is what every game/emulator does; it is process-wide
// and undone at exit. Lives in its own file because <windows.h> collides with raylib's names.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

extern "C" void ps2xWinTimerBegin() { timeBeginPeriod(1); }
extern "C" void ps2xWinTimerEnd() { timeEndPeriod(1); }
#endif

// [wincrash] Windows dies silently on an access violation; print what we can before it does.
#if defined(_WIN32)
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
// [wincrash] Which thread died? The stack usually says, but the one-line summary is what gets
// pasted into a bug report. Recorded at install time (called from main), so this needs no
// registration at any other thread's entry point -- deliberately, to keep this Windows-only
// file self-contained. File scope, NOT inside an extern "C" function: clang-cl rejects a C++
// global declared there and the Linux build cannot catch that class of error.
static DWORD g_ps2xMainThreadId = 0;
static const char *ps2xWinThreadRole()
{
    if (!g_ps2xMainThreadId) return "unknown (handler installed off-main?)";
    return (GetCurrentThreadId() == g_ps2xMainThreadId) ? "MAIN (GL / present / guest under PS2X_SCHED)"
                                                        : "NOT main -- worker (async kick, sound, or a host thread)";
}
static LONG WINAPI ps2xWinCrashFilter(EXCEPTION_POINTERS *ep)
{
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    HMODULE mod = nullptr; char name[MAX_PATH] = "?";
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod)
        GetModuleFileNameA(mod, name, sizeof name);
    const unsigned long long off = (mod && addr) ? (unsigned long long)((const char *)addr - (const char *)mod) : 0ull;
    void *fault = (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : nullptr;
    std::fprintf(stderr, "[wincrash] exception 0x%08lx at %p (%s+0x%llx) thread %lu%s%p\n", (unsigned long)code, addr, name, off, GetCurrentThreadId(),
                 fault ? " fault address " : "", fault);
    std::fprintf(stderr, "[wincrash] thread role: %s\n", ps2xWinThreadRole());

    // [wincrashstack] One faulting address is not diagnosable: two user crash reports (2026-09-07)
    // gave only "+0x49cf5ee" and "+0x49cd16e" in different builds, which resolve to nothing without
    // the matching PDB. Walk the stack so every report carries module+RVA per frame -- usable even
    // with no symbols, because the reporter's own build resolves it -- and symbol+line when a PDB
    // sits beside the exe. Everything here is best-effort: a crash handler that itself faults tells
    // us nothing, so every call is checked and failure just prints less.
    if (ep && ep->ContextRecord)
    {
        HANDLE proc = GetCurrentProcess(), thr = GetCurrentThread();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        const BOOL haveSym = SymInitialize(proc, nullptr, TRUE);
        CONTEXT ctx = *ep->ContextRecord;   // StackWalk64 MUTATES the context -- never pass the original
        STACKFRAME64 fr{};
        DWORD machine;
#if defined(_M_X64) || defined(__x86_64__)
        machine = IMAGE_FILE_MACHINE_AMD64;
        fr.AddrPC.Offset = ctx.Rip; fr.AddrFrame.Offset = ctx.Rbp; fr.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64) || defined(__aarch64__)
        machine = IMAGE_FILE_MACHINE_ARM64;
        fr.AddrPC.Offset = ctx.Pc; fr.AddrFrame.Offset = ctx.Fp; fr.AddrStack.Offset = ctx.Sp;
#else
        machine = IMAGE_FILE_MACHINE_I386;
        fr.AddrPC.Offset = ctx.Eip; fr.AddrFrame.Offset = ctx.Ebp; fr.AddrStack.Offset = ctx.Esp;
#endif
        fr.AddrPC.Mode = fr.AddrFrame.Mode = fr.AddrStack.Mode = AddrModeFlat;
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO *sym = (SYMBOL_INFO *)symBuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 511;
        for (int depth = 0; depth < 48; ++depth)
        {
            if (!StackWalk64(machine, proc, thr, &fr, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            const DWORD64 pc = fr.AddrPC.Offset;
            if (!pc) break;
            HMODULE fm = nullptr; char fn[MAX_PATH] = "?";
            unsigned long long frva = 0ull;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)pc, &fm) && fm)
            {
                GetModuleFileNameA(fm, fn, sizeof fn);
                frva = (unsigned long long)(pc - (DWORD64)(uintptr_t)fm);
                const char *slash = std::strrchr(fn, '\\');
                if (slash) std::memmove(fn, slash + 1, std::strlen(slash + 1) + 1);
            }
            char where[600] = "";
            DWORD64 disp = 0;
            if (haveSym && SymFromAddr(proc, pc, &disp, sym))
            {
                IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof line; DWORD ldisp = 0;
                if (SymGetLineFromAddr64(proc, pc, &ldisp, &line) && line.FileName)
                    std::snprintf(where, sizeof where, "  %s + 0x%llx  (%s:%lu)", sym->Name,
                                  (unsigned long long)disp, line.FileName, (unsigned long)line.LineNumber);
                else
                    std::snprintf(where, sizeof where, "  %s + 0x%llx", sym->Name, (unsigned long long)disp);
            }
            std::fprintf(stderr, "[wincrash]  #%02d %s+0x%llx%s\n", depth, fn, frva, where);
        }
        if (!haveSym)
            std::fprintf(stderr, "[wincrash] (no symbols loaded -- put the build's .pdb beside the .exe for names and line numbers)\n");
    }
    std::fflush(stderr); std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
extern "C" void ps2xWinCrashHandlerInstall()
{
    g_ps2xMainThreadId = GetCurrentThreadId();
    SetUnhandledExceptionFilter(ps2xWinCrashFilter);
}
#endif

// [wincpu] per-thread CPU time (the Linux build uses CLOCK_THREAD_CPUTIME_ID) and a one-line host description.
#if defined(_WIN32)
extern "C" unsigned long long ps2xWinThreadCpuNs()
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0ull;
    const unsigned long long kt = ((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime;
    const unsigned long long ut = ((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kt + ut) * 100ull;   // 100 ns units -> ns
}
extern "C" void ps2xWinHostInfo()
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    char cpu[128] = "?"; DWORD len = sizeof cpu; DWORD mhz = 0; DWORD mlen = sizeof mhz; HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
        RegQueryValueExA(k, "ProcessorNameString", nullptr, nullptr, (LPBYTE)cpu, &len);
        RegQueryValueExA(k, "~MHz", nullptr, nullptr, (LPBYTE)&mhz, &mlen);
        RegCloseKey(k);
    }
    std::fprintf(stderr, "[host] Windows, %lu logical cpus, %s (~%lu MHz)\n", (unsigned long)si.dwNumberOfProcessors, cpu, (unsigned long)mhz);
}
#pragma comment(lib, "advapi32.lib")
#endif
