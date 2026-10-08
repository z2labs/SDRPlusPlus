#include "crashlog.h"
#include <utils/flog.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unwind.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

namespace crashlog {
    static int fd = -1;
    static std::atomic<uint64_t> frames{ 0 };
    static std::atomic<const char*> curStep{ "start-up" };
    static std::atomic<pid_t> uiTid{ 0 };
    static uintptr_t uiStackLo = 0, uiStackHi = 0;
    static struct sigaction oldAct[NSIG];
    static const int FATAL[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP };
    static const int STACK_SIG = SIGURG; // watchdog -> UI thread: "write your stack"
    static std::atomic<bool> inFatal{ false };

    // ---- async-signal-safe output helpers ----
    static void out(const char* s) {
        if (fd < 0 || !s) { return; }
        size_t n = strlen(s);
        while (n) {
            ssize_t w = write(fd, s, n);
            if (w <= 0) { return; }
            s += w; n -= (size_t)w;
        }
    }
    static void outHex(uintptr_t v) {
        char b[2 + 2 * sizeof(uintptr_t) + 1];
        int i = sizeof(b) - 1;
        b[i] = 0;
        do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v && i > 2);
        b[--i] = 'x'; b[--i] = '0';
        out(b + i);
    }
    static void outDec(long v) {
        char b[24];
        int i = sizeof(b) - 1;
        bool neg = v < 0;
        unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
        b[i] = 0;
        do { b[--i] = '0' + (u % 10); u /= 10; } while (u && i > 1);
        if (neg) { b[--i] = '-'; }
        out(b + i);
    }

    // One frame: "  #03 pc 0x... libsdrpp_core.so+0x1234 (symbol+0x10)"
    static void outFrame(int n, uintptr_t pc) {
        out("  #"); if (n < 10) { out("0"); } outDec(n);
        out(" pc "); outHex(pc);
        Dl_info info;
        if (pc && dladdr((void*)pc, &info) && info.dli_fname) {
            const char* base = strrchr(info.dli_fname, '/');
            out(" "); out(base ? base + 1 : info.dli_fname);
            out("+"); outHex(pc - (uintptr_t)info.dli_fbase);
            if (info.dli_sname) {
                out(" ("); out(info.dli_sname); out("+"); outHex(pc - (uintptr_t)info.dli_saddr); out(")");
            }
        }
        out("\n");
    }

    struct UnwindState { uintptr_t* pcs; int n, max; };
    static _Unwind_Reason_Code unwindCb(struct _Unwind_Context* ctx, void* arg) {
        auto* st = (UnwindState*)arg;
        uintptr_t pc = _Unwind_GetIP(ctx);
        if (pc) {
            if (st->n >= st->max) { return _URC_END_OF_STACK; }
            st->pcs[st->n++] = pc;
        }
        return _URC_NO_REASON;
    }

    static void dumpStack(void* uctx, bool walkFp) {
        uintptr_t pc = 0, lr = 0, fp = 0;
#if defined(__aarch64__)
        if (uctx) {
            auto* uc = (ucontext_t*)uctx;
            pc = uc->uc_mcontext.pc; lr = uc->uc_mcontext.regs[30]; fp = uc->uc_mcontext.regs[29];
        }
#elif defined(__arm__)
        if (uctx) {
            auto* uc = (ucontext_t*)uctx;
            pc = uc->uc_mcontext.arm_pc; lr = uc->uc_mcontext.arm_lr; fp = uc->uc_mcontext.arm_fp;
        }
#elif defined(__x86_64__)
        if (uctx) { pc = ((ucontext_t*)uctx)->uc_mcontext.gregs[REG_RIP]; fp = ((ucontext_t*)uctx)->uc_mcontext.gregs[REG_RBP]; }
#endif
        out(" registers: pc "); outHex(pc); out(" lr "); outHex(lr); out(" fp "); outHex(fp); out("\n");
        out(" interrupted at:\n");
        outFrame(0, pc);
        if (lr) { outFrame(1, lr); }

#if defined(__aarch64__)
        // Frame-pointer walk (AArch64 keeps x29 chains): reliable through the signal frame
        if (walkFp && uiStackLo && fp) {
            out(" frame-pointer chain:\n");
            for (int i = 0; i < 48; i++) {
                if (fp < uiStackLo || fp + 16 > uiStackHi || (fp & 7)) { break; }
                uintptr_t next = ((uintptr_t*)fp)[0];
                uintptr_t ret = ((uintptr_t*)fp)[1];
                if (!ret) { break; }
                outFrame(i, ret);
                if (next <= fp) { break; }
                fp = next;
            }
        }
#endif
        static uintptr_t pcs[64];
        UnwindState st{ pcs, 0, 64 };
        _Unwind_Backtrace(unwindCb, &st);
        out(" unwinder:\n");
        for (int i = 0; i < st.n; i++) { outFrame(i, pcs[i]); }
    }

    static void outTime() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        out("[t="); outDec((long)ts.tv_sec); out(".");
        long ms = ts.tv_nsec / 1000000;
        if (ms < 100) { out("0"); } if (ms < 10) { out("0"); }
        outDec(ms); out("] ");
    }

    static void fatalHandler(int sig, siginfo_t* si, void* uctx) {
        if (!inFatal.exchange(true)) {
            out("\n");
            outTime();
            out("*** FATAL SIGNAL "); outDec(sig);
            const char* name = (sig == SIGSEGV) ? "SIGSEGV" : (sig == SIGABRT) ? "SIGABRT" : (sig == SIGBUS) ? "SIGBUS" : (sig == SIGFPE) ? "SIGFPE" : (sig == SIGILL) ? "SIGILL" : "SIGTRAP";
            out(" ("); out(name); out(") code "); outDec(si ? si->si_code : 0);
            out(" fault addr "); outHex(si ? (uintptr_t)si->si_addr : 0);
            out(" thread "); outDec((long)gettid());
            out(gettid() == uiTid.load() ? " (UI thread)" : " (worker thread)");
            out("\n last UI step: "); out(curStep.load()); out("\n");
            dumpStack(uctx, gettid() == uiTid.load());
            out("*** end of crash report\n");
            fsync(fd);
        }
        // Hand over to the previous handler (debuggerd / ART) so the system still sees the crash
        sigaction(sig, &oldAct[sig], NULL);
        if (sig == SIGABRT) { raise(sig); }
    }

    static void stackHandler(int, siginfo_t*, void* uctx) {
        if (gettid() != uiTid.load()) { return; }
        out(" UI thread stack (watchdog):\n");
        dumpStack(uctx, true);
        fsync(fd);
    }

    static void watchdog() {
        uint64_t last = frames.load();
        auto lastChange = std::chrono::steady_clock::now();
        bool reported = false;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            uint64_t f = frames.load();
            auto now = std::chrono::steady_clock::now();
            double stalled = std::chrono::duration<double>(now - lastChange).count();
            if (f != last) {
                if (reported) { flog::warn("WATCHDOG: UI thread running again after {:.1f} s", stalled); }
                last = f; lastChange = now; reported = false;
                continue;
            }
            if (!reported && stalled > 2.0 && uiTid.load()) {
                reported = true;
                flog::error("WATCHDOG: UI thread stalled for {:.1f} s, last step: {}", stalled, curStep.load());
                syscall(SYS_tgkill, getpid(), uiTid.load(), STACK_SIG);
            }
        }
    }

    static bool openLog(const std::string& dir) {
        mkdir(dir.c_str(), 0775);
        std::string base = dir + "/sdrpp-log";
        rename((base + ".1.txt").c_str(), (base + ".2.txt").c_str());
        rename((base + ".txt").c_str(), (base + ".1.txt").c_str());
        fd = open((base + ".txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0664);
        return fd >= 0;
    }

    void init() {
        static bool done = false;
        if (done) { return; }
        done = true;

        const char* dirs[] = { "/storage/emulated/0/Download", "/sdcard/Download",
                               "/storage/emulated/0/Android/data/io.z2labs.z2sdr/files" };
        for (auto d : dirs) { if (openLog(d)) { break; } }
        if (fd < 0) { flog::warn("crashlog: no writable log directory"); return; }
        flog::setAndroidFileFd(fd);
        outTime(); out("Z2 SDR log start, pid "); outDec(getpid()); out("\n");

        // Signal handlers on their own stack (a stack overflow must still be reported)
        static char altStack[64 * 1024];
        stack_t ss{};
        ss.ss_sp = altStack; ss.ss_size = sizeof(altStack);
        sigaltstack(&ss, NULL);
        struct sigaction sa{};
        sa.sa_sigaction = fatalHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        for (int s : FATAL) { sigaction(s, &sa, &oldAct[s]); }
        struct sigaction ss2{};
        ss2.sa_sigaction = stackHandler;
        ss2.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&ss2.sa_mask);
        sigaction(STACK_SIG, &ss2, NULL);

        std::thread(watchdog).detach();
        flog::info("crashlog: writing to fd {} (Download/sdrpp-log.txt)", fd);
    }

    void frame() {
        if (!uiTid.load()) {
            uiTid = gettid();
            pthread_attr_t a;
            if (pthread_getattr_np(pthread_self(), &a) == 0) {
                void* lo; size_t sz;
                if (pthread_attr_getstack(&a, &lo, &sz) == 0) { uiStackLo = (uintptr_t)lo; uiStackHi = uiStackLo + sz; }
                pthread_attr_destroy(&a);
            }
        }
        frames++;
    }

    void step(const char* what) { curStep = what; }
}
