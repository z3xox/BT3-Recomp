// [guestprof] PS2X_GUESTPROF=1: where the guest thread's time goes, by phase. Exclusive accounting: entering a
// nested phase (XGKICK's GIF packet inside a VU1 run inside a VIF transfer) pauses the enclosing one. Off = one
// predictable branch per site. ps2_runtime.cpp calibrates ticks against the wall clock per print.
#pragma once
#include <atomic>
#include <cstdint>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#elif defined(_M_IX86) || defined(_M_X64)
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#else
#include <chrono>
#endif

namespace gprof
{
inline uint64_t ticks()
{
#if defined(__APPLE__)
    return mach_absolute_time();
#elif defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    return __rdtsc();
#else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
enum Phase { GAME = 0, GIF = 1, VU1 = 2, VIF = 3, DEC = 4, WAIT = 5, REC_PRE = 6, REC_TEX = 7, REC_BUILD = 8, PUSH = 9, SW = 10, XFER = 11, NPHASE = 16 };
static const char *const kPhaseName[NPHASE] = {"game", "gif", "vu1", "vif", "dec", "wait", "recpre", "rectex", "recbuild", "push", "sw", "xfer", "p12", "p13", "p14", "p15"};
extern bool g_on;
extern std::atomic<uint64_t> g_acc[NPHASE];
struct TL { int cur = GAME; uint64_t t0 = 0; int sp = 0; int stack[32]; uint64_t acc[NPHASE] = {}; };
extern thread_local TL t_tl;
inline void enter(int ph)
{
    if (!g_on) return;
    TL &t = t_tl; const uint64_t now = ticks();
    if (t.t0) t.acc[t.cur] += now - t.t0;
    t.t0 = now;
    if (t.sp < 32) t.stack[t.sp++] = t.cur;
    t.cur = ph;
}
inline void leave()
{
    if (!g_on) return;
    TL &t = t_tl; const uint64_t now = ticks();
    t.acc[t.cur] += now - t.t0; t.t0 = now;
    t.cur = t.sp ? t.stack[--t.sp] : GAME;
    if (t.sp == 0)   // back at the top level: publish this thread's totals
        for (int i = 0; i < NPHASE; ++i) if (t.acc[i]) { g_acc[i].fetch_add(t.acc[i], std::memory_order_relaxed); t.acc[i] = 0; }
}
inline void mark(int ph)   // switch the current phase in place (no push): sub-phases of one function under a Scope
{
    if (!g_on) return;
    TL &t = t_tl; const uint64_t now = ticks();
    t.acc[t.cur] += now - t.t0; t.t0 = now; t.cur = ph;
}
struct Scope { Scope(int ph) { enter(ph); } ~Scope() { leave(); } };
}
