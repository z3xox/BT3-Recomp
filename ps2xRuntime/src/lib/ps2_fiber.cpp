// [fibers] See include/runtime/ps2_fiber.h for why this exists.
// Darwin's ucontext_t only carries its mcontext storage (__mcontext_data) under _XOPEN_SOURCE, and
// getcontext writes there regardless; it must be set before the first system header.
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 700
#  define _DARWIN_C_SOURCE 1
#endif
#include "runtime/ps2_fiber.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define PS2X_FIBER_WIN 1
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
// ucontext is deprecated on macOS but still present and functional; silence the deprecation there
// rather than carrying a third backend. _XOPEN_SOURCE is defined at the top of the file.
#  include <ucontext.h>
#  include <sys/mman.h>
#  include <unistd.h>
#  define PS2X_FIBER_UCTX 1
#else
#  define PS2X_FIBER_NONE 1
#endif

struct Ps2xFiber
{
    uint8_t *stack = nullptr;       // null for the adopted entry fiber: we do not own its stack
    size_t   stackSize = 0;
    uint8_t *mapping = nullptr;     // ucontext: the whole mmap (guard page + stack); stack = mapping + guard
    size_t   mappingSize = 0;
    bool     entry = false;
    void   (*fn)(void *) = nullptr;
    void    *arg = nullptr;
#if defined(PS2X_FIBER_WIN)
    void    *handle = nullptr;      // LPVOID fiber
#elif defined(PS2X_FIBER_UCTX)
    ucontext_t ctx{};
#endif
};

bool ps2xFiberSupported()
{
#if defined(PS2X_FIBER_NONE)
    return false;
#else
    return true;
#endif
}

#if defined(PS2X_FIBER_WIN)
static VOID CALLBACK ps2xFiberTrampoline(PVOID p)
{
    Ps2xFiber *f = static_cast<Ps2xFiber *>(p);
    f->fn(f->arg);
    // A fiber entry must never return -- there is no stack to return to. A guest thread that ends
    // switches away from itself before this point; reaching here is a bug, so say so loudly.
    std::fprintf(stderr, "[fibers] FATAL: fiber entry returned\n");
    std::abort();
}
#elif defined(PS2X_FIBER_UCTX)
// makecontext passes int-sized arguments, so a 64-bit pointer travels as two halves.
static void ps2xFiberTrampoline(unsigned lo, unsigned hi)
{
    const uintptr_t p = (static_cast<uintptr_t>(hi) << 32) | static_cast<uintptr_t>(lo);
    Ps2xFiber *f = reinterpret_cast<Ps2xFiber *>(p);
    f->fn(f->arg);
    std::fprintf(stderr, "[fibers] FATAL: fiber entry returned\n");
    std::abort();
}
#endif

Ps2xFiber *ps2xFiberAdoptCurrent()
{
#if defined(PS2X_FIBER_NONE)
    return nullptr;
#else
    Ps2xFiber *f = new (std::nothrow) Ps2xFiber();
    if (!f) return nullptr;
    f->entry = true;
#  if defined(PS2X_FIBER_WIN)
    // Already a fiber if some other component converted this thread; ConvertThreadToFiber fails
    // with ERROR_ALREADY_FIBER in that case, and GetCurrentFiber is then the right handle.
    f->handle = ConvertThreadToFiber(nullptr);
    if (!f->handle)
    {
        if (GetLastError() == ERROR_ALREADY_FIBER) f->handle = GetCurrentFiber();
        if (!f->handle) { delete f; return nullptr; }
    }
#  endif
    return f;
#endif
}

Ps2xFiber *ps2xFiberCreate(void (*fn)(void *), void *arg, size_t stackSize)
{
#if defined(PS2X_FIBER_NONE)
    (void)fn; (void)arg; (void)stackSize;
    return nullptr;
#else
    if (!fn) return nullptr;
    if (stackSize < (64u * 1024u)) stackSize = 64u * 1024u;
    Ps2xFiber *f = new (std::nothrow) Ps2xFiber();
    if (!f) return nullptr;
    f->fn = fn; f->arg = arg; f->stackSize = stackSize;

#  if defined(PS2X_FIBER_WIN)
    // Windows owns the stack for us; we keep stackSize for reporting only.
    f->handle = CreateFiber(stackSize, &ps2xFiberTrampoline, f);
    if (!f->handle) { delete f; return nullptr; }
#  elif defined(PS2X_FIBER_UCTX)
    // mmap with a PROT_NONE guard page below the stack. A host thread's stack has one; a malloc'd
    // fiber stack does not, so an overflow there would silently write into whatever mapping sits
    // below it (another fiber's stack, guest RAM...) instead of faulting -- the one fiber-specific
    // way to get a wild write with no trace. With the guard, an overflow is a SIGSEGV whose fault
    // address is just below the fiber's stack, which is unmistakable.
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t guard = page;
    stackSize = (stackSize + page - 1u) & ~(page - 1u);
    f->mappingSize = guard + stackSize;
    void *m = mmap(nullptr, f->mappingSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { delete f; return nullptr; }
    f->mapping = static_cast<uint8_t *>(m);
    (void)mprotect(f->mapping, guard, PROT_NONE);
    f->stack = f->mapping + guard;
    f->stackSize = stackSize;
    // Poison the stack so the portable ps2xFiberLiveStack fallback can find the high-water mark,
    // and so an accidental read of untouched stack is obvious rather than plausible zeroes.
    std::memset(f->stack, 0xA5, stackSize);
    if (getcontext(&f->ctx) != 0) { munmap(f->mapping, f->mappingSize); delete f; return nullptr; }
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = stackSize;
    f->ctx.uc_link = nullptr;           // the trampoline never returns
    const uintptr_t p = reinterpret_cast<uintptr_t>(f);
    makecontext(&f->ctx, reinterpret_cast<void (*)()>(&ps2xFiberTrampoline), 2,
                static_cast<unsigned>(p & 0xFFFFFFFFu), static_cast<unsigned>(p >> 32));
#  endif
    return f;
#endif
}

void ps2xFiberSwitch(Ps2xFiber *from, Ps2xFiber *to)
{
#if defined(PS2X_FIBER_NONE)
    (void)from; (void)to;
#else
    if (!from || !to || from == to) return;
#  if defined(PS2X_FIBER_WIN)
    SwitchToFiber(to->handle);
#  elif defined(PS2X_FIBER_UCTX)
    swapcontext(&from->ctx, &to->ctx);
#  endif
#endif
}

void ps2xFiberDestroy(Ps2xFiber *f)
{
#if !defined(PS2X_FIBER_NONE)
    if (!f) return;
#  if defined(PS2X_FIBER_WIN)
    if (!f->entry && f->handle) DeleteFiber(f->handle);
#  elif defined(PS2X_FIBER_UCTX)
    if (f->mapping) munmap(f->mapping, f->mappingSize);
#  endif
    delete f;
#else
    (void)f;
#endif
}

size_t ps2xFiberLiveStack(const Ps2xFiber *f, const uint8_t **outBase)
{
    if (outBase) *outBase = nullptr;
#if defined(PS2X_FIBER_UCTX)
    if (!f || !f->stack || f->entry) return 0;
    // The EXACT answer is the fiber's stack pointer, which swapcontext stored in its context when
    // it last switched away. Only valid while the fiber is parked -- which is precisely when a
    // savestate would copy it. A poison high-water scan was the first attempt and is both slower
    // (it walks the whole allocation) and wrong in the other direction: it reports every byte ever
    // touched, including dead frames far below the live region.
#  if defined(__linux__) && defined(__x86_64__) && defined(REG_RSP)
    const auto sp = static_cast<uintptr_t>(f->ctx.uc_mcontext.gregs[REG_RSP]);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(f->stack);
    const uintptr_t hi = lo + f->stackSize;
    if (sp > lo && sp <= hi)
    {
        if (outBase) *outBase = reinterpret_cast<const uint8_t *>(sp);
        return static_cast<size_t>(hi - sp);
    }
    return 0;   // never switched away yet, or a stack pointer outside its own allocation
#  else
    // Portable fallback: high-water mark from the 0xA5 poison laid down at creation.
    size_t untouched = 0;
    while (untouched < f->stackSize && f->stack[untouched] == 0xA5) ++untouched;
    if (outBase) *outBase = f->stack + untouched;
    return f->stackSize - untouched;
#  endif
#else
    (void)f;
    return 0;   // Windows owns the fiber stack; a snapshot there needs a different mechanism
#endif
}

// [rollback] blob = [ucontext_t][uint64_t liveSize][live stack bytes, lowest address first].
const void *ps2xFiberUContext(const Ps2xFiber *f)
{
#if defined(PS2X_FIBER_UCTX)
    return (f && !f->entry) ? static_cast<const void *>(&f->ctx) : nullptr;
#else
    (void)f; return nullptr;
#endif
}

size_t ps2xFiberSnapshotSize(const Ps2xFiber *f)
{
#if defined(PS2X_FIBER_UCTX)
    const uint8_t *base = nullptr;
    const size_t live = ps2xFiberLiveStack(f, &base);
    if (!f || f->entry || !base) return 0;
    return sizeof(ucontext_t) + sizeof(uint64_t) + live;
#else
    (void)f; return 0;
#endif
}

bool ps2xFiberSnapshot(const Ps2xFiber *f, void *buf, size_t size)
{
#if defined(PS2X_FIBER_UCTX)
    const uint8_t *base = nullptr;
    const size_t live = ps2xFiberLiveStack(f, &base);
    if (!f || f->entry || !base || !buf || size < sizeof(ucontext_t) + sizeof(uint64_t) + live) return false;
    uint8_t *o = static_cast<uint8_t *>(buf);
    std::memcpy(o, &f->ctx, sizeof(ucontext_t));            o += sizeof(ucontext_t);
    const uint64_t n = live; std::memcpy(o, &n, sizeof n);  o += sizeof n;
    std::memcpy(o, base, live);
    return true;
#else
    (void)f; (void)buf; (void)size; return false;
#endif
}

bool ps2xFiberRestore(Ps2xFiber *f, const void *buf, size_t size)
{
#if defined(PS2X_FIBER_UCTX)
    if (!f || f->entry || !f->stack || !buf || size < sizeof(ucontext_t) + sizeof(uint64_t)) return false;
    const uint8_t *i = static_cast<const uint8_t *>(buf);
    ucontext_t ctx; std::memcpy(&ctx, i, sizeof ctx);       i += sizeof ctx;
    uint64_t n = 0; std::memcpy(&n, i, sizeof n);           i += sizeof n;
    if (size < sizeof(ucontext_t) + sizeof(uint64_t) + n || n > f->stackSize) return false;
    // The context's fpregs (glibc) / uc_mcontext (Darwin) pointer refers into the ucontext_t itself; restoring into the same
    // object keeps it valid. The stack goes back to the same addresses it was copied from.
    std::memcpy(&f->ctx, &ctx, sizeof ctx);
#  if defined(__APPLE__)
    f->ctx.uc_mcontext = &f->ctx.__mcontext_data;
#  elif defined(__GLIBC__)
    f->ctx.uc_mcontext.fpregs = &f->ctx.__fpregs_mem;
#  endif
    std::memcpy(f->stack + f->stackSize - n, i, n);
    return true;
#else
    (void)f; (void)buf; (void)size; return false;
#endif
}

// ---------------------------------------------------------------------------------------------
// Self-test: two fibers handing control back and forth, plus a stack high-water reading. This is
// here so the primitive is proven on the actual toolchain before the scheduler depends on it.
namespace
{
    struct PingPong { Ps2xFiber *main_ = nullptr, *self = nullptr; int n = 0; bool deep = false; };
    PingPong g_pp;

    // noinline + a full memset: writing only the ends let the compiler fold the recursion into a
    // loop reusing one frame, and the first run of this test measured 96 bytes for what should
    // have been ~36 KB.
    #if defined(_MSC_VER)
    __declspec(noinline)
    #else
    __attribute__((noinline))
    #endif
    void burnStack(int depth, volatile uint8_t *keep)
    {
        volatile uint8_t pad[4096];
        for (size_t i = 0; i < sizeof pad; ++i) pad[i] = static_cast<uint8_t>(depth + (int)i);
        // Read pad AFTER the recursive call, or the compiler tail-calls and reuses one frame:
        // the first version of this test measured 4176 bytes where nine frames were intended.
        if (depth > 0) { burnStack(depth - 1, pad); pad[1] = pad[0]; (void)keep; return; }
        // Deepest frame: park the fiber HERE, mid-call-chain. That is the case a savestate has to
        // handle -- a guest thread suspended inside nested calls, not neatly at the top of its
        // entry function -- and it is the only way the measurement shows real depth.
        ps2xFiberSwitch(g_pp.self, g_pp.main_);
        (void)keep;
    }

    void pingPongEntry(void *)
    {
        for (;;)
        {
            ++g_pp.n;
            if (g_pp.deep) burnStack(8, nullptr);   // force a measurable stack high-water mark
            ps2xFiberSwitch(g_pp.self, g_pp.main_);
        }
    }
}

void ps2xFiberSelfTest()
{
    const char *v = std::getenv("PS2X_FIBERTEST");
    if (!v || !v[0] || v[0] == '0') return;

    if (!ps2xFiberSupported()) { std::fprintf(stderr, "[fibers] NOT SUPPORTED on this platform\n"); return; }

    g_pp.main_ = ps2xFiberAdoptCurrent();
    if (!g_pp.main_) { std::fprintf(stderr, "[fibers] adopt FAILED\n"); return; }
    g_pp.self = ps2xFiberCreate(&pingPongEntry, nullptr, 1u << 20);
    if (!g_pp.self) { std::fprintf(stderr, "[fibers] create FAILED\n"); return; }

    for (int i = 0; i < 1000; ++i) ps2xFiberSwitch(g_pp.main_, g_pp.self);
    const bool countOk = (g_pp.n == 1000);

    const uint8_t *base = nullptr;
    const size_t shallow = ps2xFiberLiveStack(g_pp.self, &base);
    g_pp.deep = true;
    ps2xFiberSwitch(g_pp.main_, g_pp.self);      // parks deep inside burnStack
    const size_t deep = ps2xFiberLiveStack(g_pp.self, &base);
    g_pp.deep = false;
    ps2xFiberSwitch(g_pp.main_, g_pp.self);      // let it unwind back to the loop

    std::fprintf(stderr,
                 "[fibers] self-test: %d switches %s | live stack parked-shallow=%zu "
                 "parked-deep=%zu %s\n",
                 g_pp.n, countOk ? "OK" : "MISMATCH", shallow, deep,
                 (deep > shallow + 32768u) ? "OK (9 x 4 KB frames live)" : "UNEXPECTED");
    std::fprintf(stderr, "[fibers] backend = %s\n",
#if defined(PS2X_FIBER_WIN)
                 "Windows Fibers"
#elif defined(PS2X_FIBER_UCTX)
                 "ucontext"
#else
                 "none"
#endif
    );
    // The ping-pong fiber is parked inside its loop, never finished, so it cannot be destroyed
    // safely; leak it deliberately (self-test only, runs once).
}
