// TimeSplitters 2 (NTSC-U, SLUS-203.14) – Spiel-Override für PS2Recomp.
// Abgeleitet aus sotc_override.cpp (Shadow of the Colossus): allgemeine Teile (ISO-Zugriff, Testgeschirr mit
// Screenshots/Abbruch-Tick, Watchdog, Minutenprofil, RAM-Dumps) übernommen; alle SotC-Adressen entfernt.
// Umgebungsvariablen heißen vorerst weiter SOTC_* (gleiche Testfarm/Werkzeuge); TS2_ISO geht zusätzlich.
#if defined(__linux__)
#include <unistd.h>
#include <dlfcn.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <execinfo.h>
#include <dirent.h>
#endif
#include <cstring>
#include <cerrno>
#include <ctime>
#include <vector>
#include <cmath>
// Shadow of the Colossus (PAL, SCES-53326) – game override for PS2Recomp.
//
// Lives in ps2xRuntime/src/runner/ next to the generated code. Applied by the runtime when
// the boot ELF is named SCES_533.26 with entry 0x100010 (the statically linked image built by
// tools/link_sotc.py: loader + KERNEL/MANAGER/GAMECORE at their real load addresses).
//
// All addresses below are in the (unrelocated) loader SCES_533.26 and therefore stable.
//
// Replacing an entry in the runtime's function table is not enough: generated code reaches
// many functions by direct C++ calls (e.g. tail jumps). So every overridden function is defined
// here under its generated name (e.g. sceGetstat_0x10c2a8) and tools/build_game.sh removes the
// generated .cpp of the same name. The table entries are replaced as well, for indirect calls.

#include "ps2_runtime.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_runtime_macros.h"
#include "game_overrides.h"
#include "runtime/ee_scheduler.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "ps2_recompiled_functions.h"
bool ps2xGsBridgeMenuOpen();   // gs_frontend.cpp
namespace ts2_dtoa_test { void versuch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt); }   // TS2_DTOA_TEST
extern std::atomic<uint64_t> g_ps2xBildwechsel;   // gs_frontend.cpp: echte Bildwechsel
bool ps2xGsBridgePad(uint16_t &pressed, uint8_t axes[4]);   // gs_frontend.cpp
int ps2xGsBridgeMouse(int &dx, int &dy);                     // gs_frontend.cpp (Mauszielen)
int ps2xGsBridgeOption(int id, int fallback);               // gs_frontend.cpp
void ps2xGsBridgeCaptureEnd();                              // gs_frontend.cpp: Videoaufnahme abschliessen
extern std::atomic<unsigned> g_ps2xPadZusatz;               // gs_frontend.cpp: zusaetzliche PS2-Tasten Spieler 1

#include <chrono>
#include <thread>
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int); // winmm, ohne windows.h (Makros near/far ...)
#endif

#include <algorithm>
#include <array>
#include <utility>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <map>
#include <unordered_map>
#include <mutex>
#include <string>
#include <string>
#include <cstdlib>

void ps2xSetPadOverridePort2(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
void ps2xSetPadOverridePlayer(int player, uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
bool ps2xMultitapActive();
namespace { void sotcRamDumpCheck(const uint8_t *r, uint64_t vs); void sotcRamDump(const uint8_t *r, uint64_t vs, const char *tag); }

namespace {
inline const char *sotcDbgDir()
{
    static const char *d = [] { const char *e = std::getenv("SOTC_DEBUG_DIR"); return (e && *e) ? e : "/tmp"; }();
    return d;
}
inline std::string sotcDbgFile(const char *name) { return std::string(sotcDbgDir()) + "/" + name; }
}

extern std::atomic<bool> g_ps2xSkipDraws;
extern std::atomic<uint64_t> g_ps2xPrimCount[8];
extern std::atomic<uint64_t> g_ps2xPrimTex;
extern std::atomic<bool> g_ps2xStateHist;
extern std::mutex g_ps2xStateHistMutex;
extern std::map<std::string, uint64_t> g_ps2xStateHistMap;
extern std::vector<std::string> g_ps2xStateHistOrder;
extern std::atomic<uint64_t> g_ps2xFogHist[256];
extern std::atomic<uint32_t> g_ps2xFogColor;
extern std::atomic<uint64_t> g_ps2xZMin, g_ps2xZMax;
extern std::mutex g_ps2xVtxSampleMutex;
extern std::vector<std::string> g_ps2xVtxSamples;
extern std::atomic<int> g_ps2xVu1DumpArm;
extern std::mutex g_ps2xVu1ProgMutex;
extern std::map<uint32_t, uint64_t> g_ps2xVu1ProgCount;
extern std::mutex g_ps2xCallHistMutex;
extern std::unordered_map<uint32_t, uint64_t> g_ps2xCallHist;

// VU0-Zufallsregister R: auf der PS2 genau ein Hardware-Register, nicht pro Thread gesichert (PCSX2: VU0.VI[REG_R]).
// Die generierten VRNEXT/VRINIT/VRXOR-Stellen (patch_026) lesen/schreiben dieses globale Register.
uint32_t g_ps2xVu0R = 0x3F800000u;

// SOTC_PROFILE=<us>: eingebauter Stichproben-Profiler (SIGPROF über CPU-Zeit aller Threads). Speichert die unterbrochene
// Befehlsadresse (RIP) als Versatz zur Ladeadresse + Thread-ID. Beim Laufende -> $SOTC_DEBUG_DIR/profil_roh.txt,
// Auflösung offline mit nm (tools/profil_aufloesen.py). Ersatz für perf, das auf dem PC nicht installiert ist.
#if defined(__linux__)
#include <ucontext.h>
#include <sys/time.h>
#include <link.h>
#endif

// GS bridge hooks (lib/gs/gs_frontend.cpp)
void ps2xGsBridgeRumble(int smallOn, int large);
int ps2xGsBridgeWideMode();
void ps2xGsBridgeGameWide(int wide);
#ifdef SOTC_PGO_GEN
extern "C" void __gcov_dump(void); // nur im PGO-Messbuild (libgcov)
#endif
#if defined(__linux__)
namespace sotc_prof
{
    constexpr size_t kMax = 1u << 22;
    static uint64_t *g_rip = nullptr; static uint32_t *g_tid = nullptr;
    static std::atomic<size_t> g_n{0}; static uintptr_t g_base = 0; static bool g_on = false;
    static void handler(int, siginfo_t *, void *uc)
    {
        const size_t i = g_n.fetch_add(1, std::memory_order_relaxed);
        if (i >= kMax) return;
        g_rip[i] = static_cast<uint64_t>(static_cast<ucontext_t *>(uc)->uc_mcontext.gregs[REG_RIP]);
        g_tid[i] = static_cast<uint32_t>(syscall(SYS_gettid));
    }
    static int phdrCb(struct dl_phdr_info *info, size_t, void *) { if (!g_base) g_base = info->dlpi_addr; return 1; }
    inline void dump(bool stop, const char *name);
    inline void start()
    {
        const char *e = std::getenv("SOTC_PROFILE"); if (!e || !*e) return;
        const long us = std::max(200L, std::strtol(e, nullptr, 10));
        g_rip = static_cast<uint64_t *>(std::calloc(kMax, 8)); g_tid = static_cast<uint32_t *>(std::calloc(kMax, 4));
        dl_iterate_phdr(phdrCb, nullptr);
        struct sigaction sa{}; sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
        itimerval tv{}; tv.it_interval.tv_usec = us; tv.it_value.tv_usec = us; setitimer(ITIMER_PROF, &tv, nullptr);
        g_on = true; std::fprintf(stderr, "[SotC profil] an, alle %ld us CPU-Zeit, Basis 0x%lx\n", us, (unsigned long)g_base);
        // SOTC_PROFILE_MINUTEN=1 (Spielbetrieb): jede Minute ein kumulativer Zwischenstand profil_mNNN.txt; die Differenz
        // zweier Staende ist das Profil dieser Minute (tools/profil_aufloesen.py auf die Differenz anwenden)
        if (const char *m = std::getenv("SOTC_PROFILE_MINUTEN"); m && *m == '1')
            std::thread([] { for (unsigned k = 1;; ++k) { std::this_thread::sleep_for(std::chrono::seconds(60)); char nm[32]; std::snprintf(nm, sizeof(nm), "profil_m%03u.txt", k); dump(false, nm); } }).detach();
    }
    // stop=false: Zwischenstand (Profiler laeuft weiter), name = Dateiname im Ausgabeordner
    inline void dump(bool stop = true, const char *name = "profil_roh.txt");
    inline void dump(bool stop, const char *name)
    {
        if (!g_on) return;
        if (stop) { itimerval tv{}; setitimer(ITIMER_PROF, &tv, nullptr); }
        const size_t n = std::min(g_n.load(), kMax);
        const char *dd = std::getenv("SOTC_PROFILE_DIR"); if (!dd || !*dd) dd = std::getenv("SOTC_DEBUG_DIR");
        std::string path = std::string(dd ? dd : ".") + "/" + name;
        if (FILE *f = std::fopen(path.c_str(), "w"))
        {
            std::fprintf(f, "# basis 0x%lx proben %zu\n", (unsigned long)g_base, n);
            // Ausfuehrbare Abbildungen (Bibliotheken), damit Proben ausserhalb der Binary zugeordnet werden koennen
            if (FILE *m = std::fopen("/proc/self/maps", "r"))
            {
                char line[1024];
                while (std::fgets(line, sizeof(line), m))
                {
                    unsigned long lo = 0, hi = 0, off = 0; char perm[8] = {0}; char pathbuf[700] = {0};
                    if (std::sscanf(line, "%lx-%lx %7s %lx %*s %*s %699[^\n]", &lo, &hi, perm, &off, pathbuf) >= 4 && perm[2] == 'x')
                        std::fprintf(f, "# map %lx %lx %lx %s\n", lo - (unsigned long)g_base, hi - (unsigned long)g_base, off, pathbuf[0] ? pathbuf : "[anon]");
                }
                std::fclose(m);
            }
            std::unordered_map<uint64_t, uint32_t> h; h.reserve(1 << 16);
            for (size_t i = 0; i < n; ++i) ++h[((g_rip[i] - g_base) << 20) ^ (g_tid[i] & 0xFFFFFu)];
            for (const auto &[k, c] : h) std::fprintf(f, "%lx %u %u\n", (unsigned long)(k >> 20), (unsigned)(k & 0xFFFFFu), c);
            std::fclose(f);
            std::fprintf(stderr, "[SotC profil] %zu Proben -> %s\n", n, path.c_str());
        }
    }
}
#else
namespace sotc_prof { inline void start() {} inline void dump(bool = true, const char * = nullptr) {} } // Windows: kein SIGPROF
#endif
bool ps2xGsBridgeScreenshot(const char *path);   // gs_frontend.cpp (PCSX2-GS-Brücke)
namespace sotc_vif_trace { void dump(); }

namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;

    inline uint32_t arg(R5900Context *ctx, int reg) { return GPR_U32(ctx, reg); }
    inline void ret(R5900Context *ctx, int32_t v) { SET_GPR_S32(ctx, 2, v); }

    // Return to the caller like a leaf function: the generated dispatcher expects pc = $ra.
    inline void leave(R5900Context *ctx) { ctx->pc = GPR_U32(ctx, 31); }

    // --- Debug console (DECI2 / TTY) -----------------------------------------------------
    // The SDK routes printf through sceTtyWrite -> DECI2, which waits forever for a dev-kit
    // host. On a retail-like system we just print to stdout.
    void sotc_sceTtyWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t buf = arg(ctx, 5), len = arg(ctx, 6);
        std::string s;
        s.reserve(len);
        for (uint32_t i = 0; i < len && i < 4096; ++i)
            s.push_back(static_cast<char>(rdram[(buf + i) & kRamMask]));
        std::fwrite("[SotC] ", 1, 7, stdout);
        std::fwrite(s.data(), 1, s.size(), stdout);
        if (s.empty() || s.back() != '\n') std::fputc('\n', stdout);
        std::fflush(stdout);
        ret(ctx, static_cast<int32_t>(len));
        leave(ctx);
    }
    void sotc_ret0(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 0); leave(ctx); }
    void sotc_ret1(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 1); leave(ctx); }
    void sotc_retm1(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, -1); leave(ctx); }

    // --- CD file table ---------------------------------------------------------------------
    // The PAL data archive spans NICO.DAT + XAB..XAE, which sit back to back on the disc; the
    // game computes sector numbers across all five. The runtime's own sceCdSearchFile hands
    // out pseudo LSNs per file, so we answer with the real LSNs of the disc and let sceCdRead
    // read them from the ISO (PS2Runtime IoPaths::cdImage).
    struct CdEntry { const char *name; uint32_t lsn; uint32_t size; };
    constexpr CdEntry kCdFiles[] = {
        {"SYSTEM.CNF", 293, 56},           {"SCES_533.26", 294, 229888},
        {"IOPRP300.IMG", 407, 275345},     {"MODULES\\SIO2MAN.IRX", 542, 6641},
        {"MODULES\\DBCMAN.IRX", 546, 15653}, {"MODULES\\SIO2D.IRX", 554, 11289},
        {"MODULES\\DS1O_D.IRX", 560, 10469}, {"STARTUP.XFF", 566, 26},
        {"KERNEL.XFF", 567, 419668},       {"MODULES\\LIBSD.IRX", 772, 30085},
        {"SG2IOPM1.IRX", 787, 27717},      {"MODULES\\MC2_D.IRX", 801, 20269},
        {"MANAGER.XFF", 811, 1732264},     {"GAMECORE.XFF", 1657, 2157144},
        {"MODULES2\\MCMAN.IRX", 2711, 96181}, {"MODULES2\\MCSERV.IRX", 2758, 7385},
        {"NICO.DAT", 2762, 1073741824u},   {"XAB", 527050, 1073741824u},
        {"XAC", 1051338, 1073741824u},     {"XAD", 1575626, 1073741824u},
        {"XAE", 2099914, 314316264u},
    };

    std::string normCdName(std::string n)
    {
        for (auto &c : n) c = (c == '/') ? '\\' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const auto colon = n.find(':');
        if (colon != std::string::npos) n = n.substr(colon + 1);
        while (!n.empty() && n.front() == '\\') n.erase(0, 1);
        const auto semi = n.find(';');
        if (semi != std::string::npos) n.resize(semi);
        while (!n.empty() && n.back() == '.') n.pop_back();
        return n;
    }

    // int sceCdSearchFile(sceCdlFILE *fp, const char *name);  sceCdlFILE = {u32 lsn, u32 size, char name[16], u8 date[8]}
    void sotc_sceCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t fp = arg(ctx, 4), namePtr = arg(ctx, 5);
        std::string name;
        for (uint32_t i = 0; i < 255; ++i)
        {
            const char c = static_cast<char>(rdram[(namePtr + i) & kRamMask]);
            if (!c) break;
            name.push_back(c);
        }
        const std::string key = normCdName(name);
        for (const auto &e : kCdFiles)
        {
            if (key == e.name)
            {
                uint8_t *out = rdram + (fp & kRamMask);
                std::memset(out, 0, 32);
                std::memcpy(out + 0, &e.lsn, 4);
                std::memcpy(out + 4, &e.size, 4);
                const std::string leaf = key.substr(key.rfind('\\') == std::string::npos ? 0 : key.rfind('\\') + 1);
                std::strncpy(reinterpret_cast<char *>(out + 8), leaf.c_str(), 15);
                std::fprintf(stderr, "[SotC] sceCdSearchFile %s -> lsn %u size %u\n", name.c_str(), e.lsn, e.size);
                ret(ctx, 1);
                leave(ctx);
                return;
            }
        }
        std::fprintf(stderr, "[SotC] sceCdSearchFile %s -> not found\n", name.c_str());
        ret(ctx, 0);
        leave(ctx);
    }

    // Use the original disc image for sector reads: $SOTC_ISO, else the first *.iso next to the
    // boot ELF or one directory above it.
    // Dateizugriff auf das Disc-Abbild (bis 4,7 GB): unter Windows ist long 32 Bit -> 64-Bit-Seek und Unicode-Pfad
    std::FILE *ts2Oeffnen(const std::filesystem::path &p)
    {
#ifdef _WIN32
        return ::_wfopen(p.c_str(), L"rb");
#else
        return std::fopen(p.c_str(), "rb");
#endif
    }
    bool ts2Seek(std::FILE *f, uint64_t off)
    {
#ifdef _WIN32
        return ::_fseeki64(f, static_cast<__int64>(off), SEEK_SET) == 0;
#else
        return fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
    }
    void configureIso()
    {
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        std::filesystem::path iso;
        if (const char *env = std::getenv("TS2_ISO"); env && *env) iso = env;
        else if (const char *env2 = std::getenv("SOTC_ISO"); env2 && *env2) iso = env2;
        for (const auto &dir : {paths.elfDirectory, paths.elfDirectory.parent_path()})
        {
            if (!iso.empty() || dir.empty()) break;
            std::error_code ec;
            for (const auto &de : std::filesystem::directory_iterator(dir, ec))
            {
                auto ext = de.path().extension().string();
                for (auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".iso") { iso = de.path(); break; }
            }
        }
        if (iso.empty())
        {
            std::fprintf(stderr, "[SotC override] WARNING: no ISO found (set SOTC_ISO); sector reads will fail\n");
            return;
        }
        paths.cdImage = iso;
        PS2Runtime::setIoPaths(paths);
        std::fprintf(stderr, "[SotC override] CD image: %s\n", iso.string().c_str());
    }

    extern std::atomic<uint32_t> g_wakeups, g_iwakeups;
    std::atomic<uint32_t> g_vblankHits{0}, g_gsIrqHits{0};
    // --- lock-delta tracing (SOTC_LOCKDELTA=0xFUNC,0xFUNC,...): net change of the KERNEL
    //     object-system lock count (0x1DC9D0) across each call of the listed functions.
    struct DeltaSlot { uint32_t addr = 0; PS2Runtime::RecompiledFunction orig = nullptr; std::atomic<int64_t> delta{0}; std::atomic<uint32_t> calls{0}; };
    DeltaSlot g_deltaSlots[32];
    inline int32_t lockCount(uint8_t *r) { int32_t v; std::memcpy(&v, r + 0x1DC9D0u, 4); return v; }
    template <int I>
    void deltaWrapper(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        const int32_t before = lockCount(r);
        g_deltaSlots[I].orig(r, c, rt);
        g_deltaSlots[I].delta += lockCount(r) - before;
        ++g_deltaSlots[I].calls;
    }
    template <int... Is>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Is)> makeDeltaWrappers(std::integer_sequence<int, Is...>)
    { return {deltaWrapper<Is>...}; }

    // --- module placement report: after KERNEL's isysLaunchModule relocated a module, print
    //     where its sections ended up (compare with docs/placement.json). Always on.
    PS2Runtime::RecompiledFunction g_origLaunchModule = nullptr;
    void reportModule(uint8_t *r, uint32_t hdr)
    {
        auto rd = [&](uint32_t a) { uint32_t v; std::memcpy(&v, r + (a & kRamMask), 4); return v; };
        hdr &= kRamMask;
        const uint32_t nsec = rd(hdr + 64), sectab = rd(hdr + 48) & kRamMask, names = rd(hdr + 68) & kRamMask, shstr = rd(hdr + 72) & kRamMask;
        const uint32_t nsym = rd(hdr + 36), symtab = rd(hdr + 40) & kRamMask;
        std::fprintf(stderr, "[SotC placement] module header 0x%08X sections:", hdr);
        int bssIdx = -1;
        for (uint32_t i = 0; i < nsec && i < 64; ++i)
        {
            const char *nm = reinterpret_cast<const char *>(r + ((shstr + rd(names + 4 * i)) & kRamMask));
            const uint32_t ptr = rd(sectab + 0x20 * i + 4) & kRamMask, size = rd(sectab + 0x20 * i + 8);
            if (std::strcmp(nm, ".bss") == 0) bssIdx = static_cast<int>(i);
            if (size && (std::strcmp(nm, ".text") == 0 || std::strcmp(nm, ".data") == 0 || std::strcmp(nm, ".rodata") == 0 ||
                         std::strcmp(nm, ".vutext") == 0))
                std::fprintf(stderr, " %s=0x%08X(0x%X)", nm, ptr, size);
        }
        // .bss is not in the section table: report relocated values of the first bss symbols
        int shown = 0;
        for (uint32_t i = 0; i < nsym && shown < 2; ++i)
        {
            const uint32_t value = rd(symtab + 16 * i + 4);
            uint16_t shndx; std::memcpy(&shndx, r + ((symtab + 16 * i + 14) & kRamMask), 2);
            if (bssIdx >= 0 && shndx == static_cast<uint16_t>(bssIdx))
            { std::fprintf(stderr, " bsssym#%u=0x%08X", i, value & kRamMask); ++shown; }
        }
        std::fprintf(stderr, "\n");
    }

    // lock-balance tracing for the KERNEL object system (SOTC_TRACE_OBJLOCK=1)
    std::mutex g_objLockMutex;
    std::map<uint32_t, uint32_t> g_objWaitByRa, g_objSigByRa;
    // --- Debug watchdog (SOTC_WATCHDOG=<seconds>): periodically dump guest thread states ---
#if defined(__linux__)
    // ---- SOTC_HWWATCH: in-process hardware write watchpoint (perf_event breakpoint) on one guest word ----
    int g_hwWatchFd = -1;
    volatile uint32_t *g_hwWatchHost = nullptr;
    volatile uint32_t g_hwWatchGuest = 0;
    volatile int g_hwWatchHits = 0;
    volatile uint32_t *g_hwFixedHost[4] = {};
    uint32_t g_hwFixedGuest[4] = {};
    uint32_t g_hwFixedLast[4] = {};
    void hwWatchSignal(int sig, siginfo_t *info, void *uctx)
    {
        // fixed watches: report every change (with value) regardless of filter
        for (int k = 0; k < 4; ++k)
        {
            if (!g_hwFixedHost[k]) continue;
            const uint32_t v = *g_hwFixedHost[k];
            if (v == g_hwFixedLast[k]) continue;
            g_hwFixedLast[k] = v;
            if (g_hwWatchHits >= 4000) return;
            ++g_hwWatchHits;
            char head[128];
            const int n = std::snprintf(head, sizeof(head), "[SotC HWFIX] guest 0x%08X <- 0x%08X bt:", g_hwFixedGuest[k], v);
            (void)!write(2, head, static_cast<size_t>(n));
            void *frames[8];
            const int depth = backtrace(frames, 8);
            char line[256];
            int len = 0;
            for (int f = 2; f < depth; ++f)
            {
                Dl_info dinfo{};
                const uintptr_t base = (dladdr(frames[f], &dinfo) && dinfo.dli_fbase) ? reinterpret_cast<uintptr_t>(dinfo.dli_fbase) : 0u;
                len += std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " +0x%lx", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(frames[f]) - base));
            }
            line[len++] = '\n';
            (void)!write(2, line, static_cast<size_t>(len));
        }
        (void)sig; (void)info; (void)uctx;
        if (!g_hwWatchHost || g_hwWatchHits >= 2000)
            return;
        const uint32_t v = *g_hwWatchHost;
        // only interesting: values that are neither 0 nor a plausible code address
        if (v == 0u || (v >= 0x00100000u && v < 0x01600000u && (v & 3u) == 0u))
            return;
        ++g_hwWatchHits;
        char head[128];
        const int n = std::snprintf(head, sizeof(head), "[SotC HWWATCH] guest 0x%08X <- 0x%08X bt:", g_hwWatchGuest, v);
        (void)!write(2, head, static_cast<size_t>(n));
        void *frames[8];
        const int depth = backtrace(frames, 8);
        char line[256];
        int len = 0;
        for (int k = 2; k < depth; ++k)
        {
            Dl_info info{};
            const uintptr_t base = (dladdr(frames[k], &info) && info.dli_fbase) ? reinterpret_cast<uintptr_t>(info.dli_fbase) : 0u;
            len += std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " +0x%lx", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(frames[k]) - base));
        }
        line[len++] = '\n';
        (void)!write(2, line, static_cast<size_t>(len));
    }
    void hwWatchArmFixed(uint8_t *rdram, const char *list)
    {
        struct sigaction sa{};
        sa.sa_sigaction = hwWatchSignal;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGIO, &sa, nullptr);
        int k = 0;
        for (const char *p = list; *p && k < 4;)
        {
            char *end = nullptr;
            const uint32_t guest = static_cast<uint32_t>(std::strtoul(p, &end, 0));
            p = (*end == ',') ? end + 1 : end;
            struct perf_event_attr attr{};
            attr.type = PERF_TYPE_BREAKPOINT;
            attr.size = sizeof(attr);
            attr.bp_type = HW_BREAKPOINT_W;
            attr.bp_addr = reinterpret_cast<uint64_t>(rdram + (guest & kRamMask));
            attr.bp_len = HW_BREAKPOINT_LEN_4;
            attr.sample_period = 1;
            attr.wakeup_events = 1;
            attr.exclude_kernel = 1;
            attr.exclude_hv = 1;
            const int fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
            if (fd < 0) { std::fprintf(stderr, "[SotC HWFIX] perf_event_open failed: %s\n", std::strerror(errno)); return; }
            fcntl(fd, F_SETFL, O_ASYNC);
            struct f_owner_ex owner{F_OWNER_TID, static_cast<pid_t>(syscall(SYS_gettid))};
            fcntl(fd, F_SETOWN_EX, &owner);
            fcntl(fd, F_SETSIG, SIGIO);
            g_hwFixedHost[k] = reinterpret_cast<volatile uint32_t *>(rdram + (guest & kRamMask));
            g_hwFixedGuest[k] = guest;
            g_hwFixedLast[k] = *g_hwFixedHost[k];
            ioctl(fd, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
            std::fprintf(stderr, "[SotC HWFIX] armed 0x%08X\n", guest);
            ++k;
            if (end == p && *p) break;
        }
    }

    void hwWatchArm(uint8_t *rdram, uint32_t guest)
    {
        static bool init = false;
        if (!init)
        {
            init = true;
            struct sigaction sa{};
            sa.sa_sigaction = hwWatchSignal;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGIO, &sa, nullptr);
        }
        if (g_hwWatchFd >= 0) { close(g_hwWatchFd); g_hwWatchFd = -1; }
        struct perf_event_attr attr{};
        attr.type = PERF_TYPE_BREAKPOINT;
        attr.size = sizeof(attr);
        attr.bp_type = HW_BREAKPOINT_W;
        attr.bp_addr = reinterpret_cast<uint64_t>(rdram + (guest & kRamMask));
        attr.bp_len = HW_BREAKPOINT_LEN_4;
        attr.sample_period = 1;
        attr.wakeup_events = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        const int fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
        if (fd < 0)
        {
            static bool warned = false;
            if (!warned) { warned = true; std::fprintf(stderr, "[SotC HWWATCH] perf_event_open failed: %s\n", std::strerror(errno)); }
            return;
        }
        fcntl(fd, F_SETFL, O_ASYNC);
        struct f_owner_ex owner{F_OWNER_TID, static_cast<pid_t>(syscall(SYS_gettid))};
        fcntl(fd, F_SETOWN_EX, &owner);
        fcntl(fd, F_SETSIG, SIGIO);
        {
            static uint32_t lastArm = 0; static int armLogs = 0;
            if (guest != lastArm && armLogs < 60) { ++armLogs; lastArm = guest; std::fprintf(stderr, "[SotC HWWATCH] arm 0x%08X\n", guest); }
        }
        g_hwWatchHost = reinterpret_cast<volatile uint32_t *>(rdram + (guest & kRamMask));
        g_hwWatchGuest = guest;
        g_hwWatchFd = fd;
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }

#endif // __linux__ (HWWATCH)

    // SOTC_MEMWATCH=0xADDR[,0xADDR]: poll guest words every ~200us and report changes with the running thread.
    void startMemWatch(PS2Runtime &runtime)
    {
        const char *mw = std::getenv("SOTC_MEMWATCH");
        if (!mw || !*mw)
            return;
        std::vector<uint32_t> addrs;
        for (const char *p = mw; *p;)
        {
            char *end = nullptr;
            addrs.push_back(static_cast<uint32_t>(std::strtoul(p, &end, 0)));
            p = (*end == ',') ? end + 1 : end;
            if (end == p && *p != 0) break;
        }
        PS2Runtime *rt = &runtime;
        std::thread([rt, addrs]() {
            const uint8_t *ram = nullptr;
            while (!(ram = rt->memory().getRDRAM()))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::vector<uint32_t> last(addrs.size(), 0u);
            for (size_t i = 0; i < addrs.size(); ++i) std::memcpy(&last[i], ram + (addrs[i] & kRamMask), 4);
            int reports = 0;
            while (reports < 200)
            {
                for (size_t i = 0; i < addrs.size(); ++i)
                {
                    uint32_t v; std::memcpy(&v, ram + (addrs[i] & kRamMask), 4);
                    if (v != last[i])
                    {
                        const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
                        int pc = 0;
                        for (const auto &t : snap.threads) if (t.id == snap.runningThreadId) pc = (int)t.pc;
                        std::fprintf(stderr, "[SotC memwatch] 0x%08X: %08X -> %08X (running thread %d, last pc 0x%08X, vsync %llu)\n",
                                     addrs[i], last[i], v, snap.runningThreadId, pc, (unsigned long long)rt->memory().gs().vsyncTick.load());
                        last[i] = v; ++reports;
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }).detach();
    }

#if defined(__linux__)
    // SOTC_WPROT=0xADDR: write-protect the host page holding this guest address and print a host
    // backtrace on the first write into [addr, addr+0x40). Used to find stray DMA/HLE writes.
    uintptr_t g_wprotPage = 0, g_wprotLo = 0, g_wprotHi = 0;
    std::atomic<int> g_wprotHits{0};
    void wprotHandler(int, siginfo_t *si, void *)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
        const long ps = sysconf(_SC_PAGESIZE);
        if (a < g_wprotPage || a >= g_wprotPage + ps)
        {
            signal(SIGSEGV, SIG_DFL);
            return;
        }
        mprotect(reinterpret_cast<void *>(g_wprotPage), ps, PROT_READ | PROT_WRITE);
        if (a >= g_wprotLo && a < g_wprotHi && g_wprotHits.fetch_add(1) < 40)
        {
            static const char msg[] = "[SotC wprot] write into watched range, backtrace:\n";
            write(2, msg, sizeof(msg) - 1);
            void *bt[48];
            const int n = backtrace(bt, 48);
            backtrace_symbols_fd(bt, n, 2);
        }
    }
    void startWriteProtect(PS2Runtime &runtime)
    {
        const char *w = std::getenv("SOTC_WPROT");
        if (!w || !*w)
            return;
        const uint32_t guest = static_cast<uint32_t>(std::strtoul(w, nullptr, 0));
        PS2Runtime *rt = &runtime;
        std::thread([rt, guest]() {
            uint8_t *ram = nullptr;
            while (!(ram = rt->memory().getRDRAM()))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::this_thread::sleep_for(std::chrono::seconds(std::getenv("SOTC_WPROT_DELAY") ? std::atoi(std::getenv("SOTC_WPROT_DELAY")) : 20));
            const long ps = sysconf(_SC_PAGESIZE);
            g_wprotLo = reinterpret_cast<uintptr_t>(ram + guest);
            g_wprotHi = g_wprotLo + 0x10;
            g_wprotPage = g_wprotLo & ~static_cast<uintptr_t>(ps - 1);
            struct sigaction sa {};
            sa.sa_sigaction = wprotHandler;
            sa.sa_flags = SA_SIGINFO;
            sigaction(SIGSEGV, &sa, nullptr);
            std::fprintf(stderr, "[SotC wprot] watching guest 0x%08X (host %p), exe maps:\n", guest, (void *)g_wprotLo);
            if (FILE *m = std::fopen("/proc/self/maps", "r"))
            {
                char line[512];
                while (std::fgets(line, sizeof(line), m))
                    if (std::strstr(line, "ps2EntryRunner") && std::strstr(line, "r-xp")) std::fputs(line, stderr);
                std::fclose(m);
            }
            while (g_wprotHits.load() < 40)
            {
                mprotect(reinterpret_cast<void *>(g_wprotPage), ps, PROT_READ);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }).detach();
    }

#else
    void startWriteProtect(PS2Runtime &) {} // Windows: kein mprotect/SIGSEGV
#endif // __linux__ (WPROT)

    // ---- Test harness ----
    // SOTC_SHOT_TICKS=330,480,...  dump the render target (fbp 232, 512x416) at these vsync ticks
    // SOTC_SHOT_DIR=/path          where to put shot_<tick>.ppm (default /tmp)
    // SOTC_EXIT_TICK=N             print a summary line and exit at this vsync tick
    std::atomic<uint32_t> g_abortCount{0}, g_assertCount{0};
    void dumpFramePpm(PS2Runtime *rt, const std::string &path, unsigned fbp, unsigned fbw, unsigned psm, unsigned w, unsigned h)
    {
        // PCSX2-GS-Brücke aktiv: Bild so speichern, wie PCSX2 es ausgibt (PNG neben der .ppm-Endung)
        if (ps2xGsBridgeScreenshot((path.substr(0, path.size() - 4) + ".png").c_str()))
            return;
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            std::fprintf(f, "P6\n%u %u\n255\n", w, h);
            std::vector<unsigned char> row(w * 3u);
            // Konsistentes Abbild: ganzes VRAM auf einmal kopieren (wartet bei MTGS auf den Rasterizer), dann aus der
            // Kopie lesen. Pixelweises ReadVram würde bei MTGS ein halb neu gezeichnetes Bild erwischen.
            rt->gs().refreshDisplaySnapshot();
            uint32_t snapSize = 0;
            uint8_t *snap = const_cast<uint8_t *>(rt->gs().lockDisplaySnapshot(snapSize));
            const bool useSnap = snap && psm == 0u && snapSize >= 4u * 1024u * 1024u;
            for (unsigned y = 0; y < h; ++y)
            {
                for (unsigned x = 0; x < w; ++x)
                {
                    const uint32_t c = useSnap ? GSMem::ReadCT32(snap, fbp * 32u, fbw, x, y) : rt->gs().ReadVram(psm, fbp * 32u, fbw, x, y);
                    row[x * 3u] = (unsigned char)c;
                    row[x * 3u + 1u] = (unsigned char)(c >> 8);
                    row[x * 3u + 2u] = (unsigned char)(c >> 16);
                }
                std::fwrite(row.data(), 1, row.size(), f);
            }
            rt->gs().unlockDisplaySnapshot();
            std::fclose(f);
        }
    }
    std::vector<uint64_t> g_shotTicks;
    size_t g_shotNext = 0;
    std::string g_shotDir = "/tmp";
    std::atomic<uint64_t> g_clipCalls[2], g_clipOut[2], g_clipNan[2], g_clipNanOut[2];
    int g_clipSamples = 0;
    std::mutex g_clipHistMutex;
    uint64_t g_clipHist[2][4] = {};
    std::mutex g_texTraceMutex;
    uint64_t g_texTraceCount = 0;
    int g_texTraceSamples = 0;
    uint64_t g_texTraceBytes = 0;
    std::map<uint32_t, uint64_t> g_texTraceByRa;
    uint64_t g_skipDrawUntil = 0;
    void (*g_subThreadReport)(uint64_t) = nullptr; // SOTC_TRACE_SUBTHREAD: Bericht pro 500 Bilder
    void (*g_oomReportNow)(uint64_t) = nullptr;    // SOTC_TRACE_OOM: Bericht beim Absturz
    // Eigener Partitions-Durchlauf nach dem Vorbild von iosDebugPartitionDump (0x1AB358 -> 0x1AA720), nur lesend:
    // Partition: +0x08 erster Block (Wörter). Block-Kopf 0x20 Bytes: +0x00 Größe (Wörter), +0x08 nächster Block,
    // +0x0C Bit24 belegt / Bit25 Unterpartition (Kopf bei Block+0x24), +0x18 Aufrufer-PC (Wörter).
    void sotcDumpPartition(const uint8_t *r, uint32_t part, int depth, const char *why)
    {
        if (depth > 6 || part >= 0x2000000u) return;
        struct Agg { uint64_t bytes = 0; uint32_t n = 0; };
        std::unordered_map<uint32_t, Agg> perPc;
        uint64_t used = 0, freeB = 0, largestFree = 0;
        uint32_t nUsed = 0, nFree = 0, nSub = 0;
        std::vector<uint32_t> subs;
        uint32_t b = (Ps2FastRead32(r, part + 0x8u) & 0xFFFFFFu) << 2;
        for (int guard = 0; b != 0u && b < 0x2000000u && guard < 400000; ++guard)
        {
            const uint32_t w0 = Ps2FastRead32(r, b), wC = Ps2FastRead32(r, b + 0xCu);
            const uint32_t size = (w0 & 0xFFFFFFu) << 2;
            if ((wC >> 24) & 1u)
            {
                used += size; ++nUsed;
                const uint32_t pc = (Ps2FastRead32(r, b + 0x18u) & 0xFFFFFFu) << 2;
                auto &a = perPc[pc]; a.bytes += size; ++a.n;
                if ((wC >> 25) & 1u) { ++nSub; subs.push_back(b + 0x24u); }
            }
            else { freeB += size; ++nFree; largestFree = std::max<uint64_t>(largestFree, size); }
            const uint32_t next = (Ps2FastRead32(r, b + 0x8u) & 0xFFFFFFu) << 2;
            if (next <= b) break;
            b = next;
        }
        std::vector<std::pair<uint32_t, Agg>> v(perPc.begin(), perPc.end());
        std::sort(v.begin(), v.end(), [](auto &x, auto &y) { return x.second.bytes > y.second.bytes; });
        std::fprintf(stderr, "[SotC ptn] %s %*sPartition 0x%08X: belegt %.2f MB/%u, frei %.2f MB/%u (groesster %u B), Unterpartitionen %u; Halter:",
                     why, depth * 2, "", part, used / 1048576.0, nUsed, freeB / 1048576.0, nFree, (unsigned)largestFree, nSub);
        for (size_t i = 0; i < v.size() && i < 8; ++i)
            std::fprintf(stderr, " pc=0x%08X:%.2fMB/%u", v[i].first, v[i].second.bytes / 1048576.0, v[i].second.n);
        std::fprintf(stderr, "\n");
        for (uint32_t sp : subs)
        {
            // nur Unterpartitionen ab 256 KB einzeln ausgeben
            const uint32_t blk = sp - 0x24u;
            if (((Ps2FastRead32(r, blk) & 0xFFFFFFu) << 2) >= 0x40000u) sotcDumpPartition(r, sp, depth + 1, why);
        }
    }
    // SOTC_COUNT_FUNCS=addr,addr,... : Aufrufe beliebiger Gastfunktionen zählen (Laden vs. Entladen).
    constexpr int kCountSlots = 32;
    PS2Runtime::RecompiledFunction g_countOrig[kCountSlots] = {};
    uint32_t g_countAddr[kCountSlots] = {};
    std::atomic<uint64_t> g_countHits[kCountSlots] = {};
    int g_countN = 0;
    // SOTC_CALLERS=1 zusätzlich: je gezählter Funktion ein Histogramm der Rücksprungadressen ($ra) -> wer ruft wie oft?
    bool g_countCallers = false;
    std::mutex g_callerMutex;
    std::unordered_map<uint32_t, uint64_t> g_callerHist[kCountSlots];
    template <int N>
    void countTrampoline(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        g_countHits[N].fetch_add(1, std::memory_order_relaxed);
        if (g_countCallers)
        {
            std::lock_guard<std::mutex> lk(g_callerMutex);
            ++g_callerHist[N][GPR_U32(c, 31)];
        }
        g_countOrig[N](r, c, rt);
    }
    template <int... I>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> makeCountTramps(std::integer_sequence<int, I...>)
    { return {&countTrampoline<I>...}; }
    const auto g_countTramps = makeCountTramps(std::make_integer_sequence<int, kCountSlots>{});
    uint64_t g_drawWindow = 0;
    void harnessOnVBlank(PS2Runtime *rt)
    {
        const uint64_t tick = rt->memory().gs().vsyncTick.load();
        if (g_subThreadReport && tick % 500u == 0u)
            g_subThreadReport(tick);
        if (g_countN > 0 && tick % 500u == 0u)
        {
            std::fprintf(stderr, "[SotC zaehler] vsync=%llu", (unsigned long long)tick);
            for (int i = 0; i < g_countN; ++i)
                std::fprintf(stderr, " 0x%X=%llu", g_countAddr[i], (unsigned long long)g_countHits[i].load());
            std::fprintf(stderr, "\n");
            if (g_countCallers)
            {
                std::lock_guard<std::mutex> lk(g_callerMutex);
                for (int i = 0; i < g_countN; ++i)
                {
                    if (g_callerHist[i].empty()) continue;
                    std::vector<std::pair<uint32_t, uint64_t>> v(g_callerHist[i].begin(), g_callerHist[i].end());
                    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
                    std::fprintf(stderr, "[SotC aufrufer] 0x%X:", g_countAddr[i]);
                    for (size_t k = 0; k < v.size() && k < 8; ++k) std::fprintf(stderr, " ra=0x%08X:%llu", v[k].first, (unsigned long long)v[k].second);
                    std::fprintf(stderr, "\n");
                }
            }
        }
        if (g_skipDrawUntil != 0u)
        {
            // Auch im Vorlauf vor jedem Screenshot ein Zeichenfenster öffnen (sonst sind alle Vorlauf-Bilder schwarz).
            const uint64_t win = g_drawWindow ? g_drawWindow : 20u;
            const bool nearShot = g_shotNext < g_shotTicks.size() && tick + win >= g_shotTicks[g_shotNext];
            const bool skip = tick < g_skipDrawUntil && !nearShot;
            if (nearShot && tick < g_skipDrawUntil)
                g_ps2xSkipDraws.store(false, std::memory_order_relaxed);
            else if (g_ps2xSkipDraws.load(std::memory_order_relaxed) != skip)
            {
                g_ps2xSkipDraws.store(skip, std::memory_order_relaxed);
                std::fprintf(stderr, "[SotC test] skip draws %s at tick=%llu\n", skip ? "on" : "off", (unsigned long long)tick);
            }
            if (tick >= g_skipDrawUntil)
                g_skipDrawUntil = 0;
        }
        // SOTC_DRAW_WINDOW=N: nach dem Vorlauf nur in den N Bildern vor jedem Screenshot zeichnen.
        else if (g_drawWindow != 0u)
        {
            const bool near = g_shotNext < g_shotTicks.size() && tick + g_drawWindow >= g_shotTicks[g_shotNext];
            const bool skip = !near;
            if (g_ps2xSkipDraws.load(std::memory_order_relaxed) != skip)
                g_ps2xSkipDraws.store(skip, std::memory_order_relaxed);
        }
        // SOTC_STATE_HIST=<tick>: histogram of GS draw states for exactly one frame (tick .. tick+1), in draw order.
        static const uint64_t histTick = [] { const char *e = std::getenv("SOTC_STATE_HIST"); return e ? std::strtoull(e, nullptr, 10) : 0ull; }();
        if (histTick)
        {
            if (tick == histTick)
            {
                g_ps2xStateHist.store(true);
                if (const char *vd = std::getenv("SOTC_VU1_DUMP"); vd && *vd)
                    g_ps2xVu1DumpArm.store(std::atoi(vd));
            }
            else if (tick == histTick + 1u && g_ps2xStateHist.load())
            {
                g_ps2xStateHist.store(false);
                std::lock_guard<std::mutex> hl(g_ps2xStateHistMutex);
                std::fprintf(stderr, "[SotC STATES] frame tick=%llu distinct=%zu\n", (unsigned long long)histTick, g_ps2xStateHistOrder.size());
                if (std::getenv("SOTC_DUMP_LAYOUT"))
                    if (FILE *f = std::fopen(sotcDbgFile("sotc_ram.bin").c_str(), "wb")) { std::fwrite(rt->memory().getRDRAM(), 1, 0x2000000u, f); std::fclose(f); }
                for (const auto &k : g_ps2xStateHistOrder)
                    std::fprintf(stderr, "[SotC STATES] %6llu x %s\n", (unsigned long long)g_ps2xStateHistMap[k], k.c_str());
                {
                    std::lock_guard<std::mutex> cl(g_ps2xCallHistMutex);
                    if (FILE *f = std::fopen(sotcDbgFile("sotc_callhist.txt").c_str(), "w"))
                    {
                        for (const auto &[pc, n] : g_ps2xCallHist) std::fprintf(f, "%08x %llu\n", pc, (unsigned long long)n);
                        std::fclose(f);
                    }
                    std::fprintf(stderr, "[SotC STATES] call histogram: %zu distinct targets -> /tmp/sotc_callhist.txt\n", g_ps2xCallHist.size());
                }
                {
                    std::lock_guard<std::mutex> pl(g_ps2xVu1ProgMutex);
                    std::fprintf(stderr, "[SotC STATES] VU1 programs (start pc: runs):");
                    for (const auto &[pc, n] : g_ps2xVu1ProgCount) std::fprintf(stderr, " 0x%X:%llu", pc, (unsigned long long)n);
                    std::fprintf(stderr, "\n");
                }
                std::fprintf(stderr, "[SotC STATES] fogged tri vertices: fogcol=%06X z=[%llu..%llu] fog histogram (value:count):",
                             g_ps2xFogColor.load(), (unsigned long long)g_ps2xZMin.load(), (unsigned long long)g_ps2xZMax.load());
                for (int f = 0; f < 256; ++f)
                    if (const uint64_t n = g_ps2xFogHist[f].load()) std::fprintf(stderr, " %d:%llu", f, (unsigned long long)n);
                std::fprintf(stderr, "\n");
                {
                    std::lock_guard<std::mutex> sl(g_ps2xVtxSampleMutex);
                    for (const auto &line : g_ps2xVtxSamples)
                        std::fprintf(stderr, "[SotC STATES] tri %s\n", line.c_str());
                }
            }
        }
        static const bool kEnv_SOTC_TRACE_MODELCLIP = std::getenv("SOTC_TRACE_MODELCLIP") != nullptr;
        if (kEnv_SOTC_TRACE_MODELCLIP && tick % 50u == 0u)
        {
            std::lock_guard<std::mutex> lock(g_clipHistMutex);
            std::fprintf(stderr, "[SotC MODELCLIP] tick=%llu skin: <=-2:%llu (-2,0):%llu 0:%llu >0:%llu | solid: <=-2:%llu (-2,0):%llu 0:%llu >0:%llu\n",
                         (unsigned long long)tick,
                         (unsigned long long)g_clipHist[0][0], (unsigned long long)g_clipHist[0][1], (unsigned long long)g_clipHist[0][2], (unsigned long long)g_clipHist[0][3],
                         (unsigned long long)g_clipHist[1][0], (unsigned long long)g_clipHist[1][1], (unsigned long long)g_clipHist[1][2], (unsigned long long)g_clipHist[1][3]);
            std::memset(g_clipHist, 0, sizeof(g_clipHist));
        }
        static const bool tracePrims = std::getenv("SOTC_TRACE_PRIMS") != nullptr;
        if (tracePrims && tick % 25u == 0u)
        {
            uint64_t c[8];
            for (int k = 0; k < 8; ++k) c[k] = g_ps2xPrimCount[k].exchange(0);
            std::fprintf(stderr, "[SotC PRIMS] tick=%llu (25 frames) point=%llu line=%llu strip=%llu tri=%llu tristrip=%llu trifan=%llu sprite=%llu textured=%llu\n",
                         (unsigned long long)tick, (unsigned long long)c[0], (unsigned long long)(c[1] + c[2]), (unsigned long long)c[2],
                         (unsigned long long)c[3], (unsigned long long)c[4], (unsigned long long)c[5], (unsigned long long)c[6],
                         (unsigned long long)g_ps2xPrimTex.exchange(0));
        }
        if (g_shotNext >= g_shotTicks.size())
            return;
        while (g_shotNext < g_shotTicks.size() && tick >= g_shotTicks[g_shotNext])
        {
            char name[64];
            std::snprintf(name, sizeof(name), "/shot_%05llu.ppm", (unsigned long long)g_shotTicks[g_shotNext]);
            dumpFramePpm(rt, g_shotDir + name, 232, 8, 0, 512, 416);
            std::fprintf(stderr, "[SotC test] shot tick=%llu\n", (unsigned long long)g_shotTicks[g_shotNext]);
            ++g_shotNext;
        }
    }
    void startTestHarness(PS2Runtime &runtime)
    {
        if (const char *sk = std::getenv("SOTC_SKIP_DRAW_UNTIL"); sk && *sk)
        {
            g_skipDrawUntil = std::strtoull(sk, nullptr, 10);
            g_ps2xSkipDraws.store(g_skipDrawUntil != 0u);
        }
        if (const char *dw = std::getenv("SOTC_DRAW_WINDOW"); dw && *dw)
            g_drawWindow = std::strtoull(dw, nullptr, 10);
        const char *shots = std::getenv("SOTC_SHOT_TICKS");
        const char *exitTick = std::getenv("SOTC_EXIT_TICK");
        if ((!shots || !*shots) && (!exitTick || !*exitTick))
            return;
        std::vector<uint64_t> ticks;
        if (shots)
            for (const char *p = shots; *p;)
            {
                char *end = nullptr;
                ticks.push_back(std::strtoull(p, &end, 10));
                p = (*end == ',') ? end + 1 : end;
                if (end == p && *p) break;
            }
        std::sort(ticks.begin(), ticks.end());
        const uint64_t stopAt = exitTick ? std::strtoull(exitTick, nullptr, 10) : 0u;
        const std::string dir = std::getenv("SOTC_SHOT_DIR") ? std::getenv("SOTC_SHOT_DIR") : "/tmp";
        g_shotTicks = ticks;
        g_shotDir = dir;
        PS2Runtime *rt = &runtime;
        std::thread([rt, stopAt]() {
            const auto t0 = std::chrono::steady_clock::now();
            for (;;)
            {
                const uint64_t tick = rt->memory().gs().vsyncTick.load();
                if (stopAt && tick >= stopAt)
                {
                    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    std::fprintf(stderr, "[SotC test] done tick=%llu seconds=%.1f fps=%.2f aborts=%u asserts=%u\n",
                                 (unsigned long long)tick, secs, secs > 0 ? tick / secs : 0.0, g_abortCount.load(), g_assertCount.load());
                    sotc_prof::dump();
                    ::ps2xGsBridgeCaptureEnd();
                    std::fflush(stderr);
                    std::fflush(stdout);
                    #ifdef SOTC_PGO_GEN
                    __gcov_dump(); // PGO-Lauf: Profil schreiben
#endif
                    std::_Exit(0);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }).detach();
    }

#if defined(__linux__)
    // Haenger-Bericht (immer aktiv, ohne gdb, auch auf dem Steam Deck): wechselt der angezeigte Bildpuffer (DISPFB) laenger
    // nicht (8 s, Ladepausen sind kuerzer), obwohl VBlanks weiterlaufen und das Port-Menue zu ist, werden einmal die EE-Threads (Status/Warten/PC),
    // Semaphoren und die Host-Stapel aller Threads (aufgeloest ueber die Symboltabelle des Programms) ins Protokoll
    // geschrieben. Danach erst wieder nach dem naechsten Bildwechsel. TS2_HAENGERBERICHT=0 schaltet ihn ab.
    namespace haenger
    {
        constexpr int kMaxTiefe = 40;
        struct Abzug { std::atomic<int> tid{0}; int n = 0; void *adr[kMaxTiefe]; std::atomic<bool> fertig{false}; };
        Abzug g_abzug[96];
        std::atomic<int> g_slot{-1};
        void sigHandler(int)
        {
            const int i = g_slot.load();
            if (i < 0 || i >= 96) return;
            g_abzug[i].n = backtrace(g_abzug[i].adr, kMaxTiefe);
            g_abzug[i].fertig.store(true);
        }
        struct Sym { uintptr_t a; uintptr_t g; std::string n; };
        std::vector<Sym> g_syms; uintptr_t g_basis = 0, g_symBasis = 0;
        void ladeSymbole()
        {
            Dl_info di{};
            if (dladdr(reinterpret_cast<void *>(&ladeSymbole), &di)) g_basis = reinterpret_cast<uintptr_t>(di.dli_fbase);
            std::FILE *f = std::fopen("/proc/self/exe", "rb");
            if (!f) return;
            std::vector<uint8_t> d; uint8_t buf[1 << 16]; size_t k;
            while ((k = std::fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + k);
            std::fclose(f);
            if (d.size() < 64 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0 || d[4] != 2) return;
            // ET_EXEC (Zig-Paket): Symbolwerte sind absolute Adressen; ET_DYN (PIE): relativ zur Ladeadresse
            g_symBasis = (d[0x10] == 2) ? 0u : g_basis;
            auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, d.data() + o, 2); return v; };
            auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, d.data() + o, 4); return v; };
            auto u64 = [&](size_t o) { uint64_t v; std::memcpy(&v, d.data() + o, 8); return v; };
            const uint64_t shoff = u64(0x28); const uint16_t shentsize = u16(0x3A), shnum = u16(0x3C);
            for (uint16_t i = 0; i < shnum; ++i)
            {
                const size_t sh = shoff + size_t(i) * shentsize;
                if (sh + 64 > d.size() || u32(sh + 4) != 2) continue;   // SHT_SYMTAB
                const uint64_t off = u64(sh + 0x18), size = u64(sh + 0x20), ent = u64(sh + 0x38); const uint32_t link = u32(sh + 0x28);
                const size_t strsh = shoff + size_t(link) * shentsize; const uint64_t stroff = u64(strsh + 0x18);
                for (uint64_t o = off; ent && o + ent <= off + size && o + 24 <= d.size(); o += ent)
                {
                    if ((d[o + 4] & 0xF) != 2) continue;   // STT_FUNC
                    const uint64_t val = u64(o + 8), sz = u64(o + 16);
                    if (!val) continue;
                    const char *name = reinterpret_cast<const char *>(d.data() + stroff + u32(o));
                    g_syms.push_back({uintptr_t(val), uintptr_t(sz ? sz : 1), name});
                }
            }
            std::sort(g_syms.begin(), g_syms.end(), [](const Sym &a, const Sym &b) { return a.a < b.a; });
        }
        std::string aufloesen(void *p)
        {
            const uintptr_t a = reinterpret_cast<uintptr_t>(p) - g_symBasis;
            auto it = std::upper_bound(g_syms.begin(), g_syms.end(), a, [](uintptr_t v, const Sym &s) { return v < s.a; });
            char t[64];
            if (it != g_syms.begin()) { --it; if (a - it->a < (it->g > 1 ? it->g : uintptr_t(0x1000))) { std::snprintf(t, sizeof t, "+0x%lx", (unsigned long)(a - it->a)); return it->n + t; } }
            Dl_info di{};
            if (dladdr(p, &di) && di.dli_sname) { std::snprintf(t, sizeof t, "+0x%lx", (unsigned long)(reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(di.dli_saddr))); return std::string(di.dli_sname) + t; }
            if (g_basis && reinterpret_cast<uintptr_t>(p) >= g_basis && reinterpret_cast<uintptr_t>(p) - g_basis < (uintptr_t(1) << 31))
                std::snprintf(t, sizeof t, "exe+0x%lx", (unsigned long)(reinterpret_cast<uintptr_t>(p) - g_basis));
            else std::snprintf(t, sizeof t, "%p", p);
            return t;
        }
        void bericht(PS2Runtime *rt, uint64_t seitTick)
        {
            std::fprintf(stderr, "===== TS2 HAENGER-BERICHT: kein neues Bild seit VBlank %llu (jetzt %llu) =====\n",
                         (unsigned long long)seitTick, (unsigned long long)rt->memory().gs().vsyncTick.load());
            const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
            std::fprintf(stderr, "EE: running=%d cycle=%llu\n", snap.runningThreadId, (unsigned long long)snap.eeCycle);
            for (const auto &t : snap.threads)
                std::fprintf(stderr, "  EE-Thread %d status=%d warten=%d/%d prio=%d pc=0x%08X ra=0x%08X entry=0x%08X\n", t.id, int(t.status),
                             int(t.waitReason), t.waitId, t.currentPriority, t.pc, t.ra, t.entry);
            for (const auto &se : snap.semaphores)
                if (se.waiters) std::fprintf(stderr, "  Sema %d count=%d/%d wartende=%u\n", se.id, se.count, se.maxCount, se.waiters);
            if (g_syms.empty()) ladeSymbole();
            std::fprintf(stderr, "Symbole: %zu (Programm-Basis %p)\n", g_syms.size(), reinterpret_cast<void *>(g_basis));
            std::vector<int> tids;
            if (DIR *dir = opendir("/proc/self/task")) { while (dirent *e = readdir(dir)) if (e->d_name[0] != '.') tids.push_back(std::atoi(e->d_name)); closedir(dir); }
            const int self = int(syscall(SYS_gettid));
            int slot = 0;
            for (int tid : tids)
            {
                if (tid == self || slot >= 96) continue;
                char name[64] = "?"; { char pfad[64]; std::snprintf(pfad, sizeof pfad, "/proc/self/task/%d/comm", tid);
                  if (std::FILE *cf = std::fopen(pfad, "r")) { if (std::fgets(name, sizeof name, cf)) name[std::strcspn(name, "\n")] = 0; std::fclose(cf); } }
                Abzug &z = g_abzug[slot]; z.fertig = false; z.n = 0; g_slot.store(slot);
                if (syscall(SYS_tgkill, getpid(), tid, SIGRTMIN + 3) != 0) continue;
                for (int w = 0; w < 60 && !z.fertig.load(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                g_slot.store(-1);
                std::fprintf(stderr, "-- Host-Thread %d (%s)%s\n", tid, name, z.fertig.load() ? "" : " (keine Antwort)");
                for (int i = 2; i < z.n; ++i) std::fprintf(stderr, "     #%d %s\n", i - 2, aufloesen(z.adr[i]).c_str());
                ++slot;
            }
            std::fprintf(stderr, "===== Ende Haenger-Bericht (Spiel laeuft weiter) =====\n");
            std::fflush(stderr);
        }
        void start(PS2Runtime &runtime)
        {
            if (const char *e = std::getenv("TS2_HAENGERBERICHT"); e && *e == '0') return;
            { void *tmp[4]; backtrace(tmp, 4); }   // libgcc vorladen (backtrace im Signal-Handler)
            struct sigaction sa{}; sa.sa_handler = sigHandler; sa.sa_flags = SA_RESTART; sigemptyset(&sa.sa_mask);
            sigaction(SIGRTMIN + 3, &sa, nullptr);
            PS2Runtime *rt = &runtime;
            std::thread([rt]() {
                uint64_t letzterFb = ~0ull, seitTick = 0; auto seit = std::chrono::steady_clock::now(); bool gemeldet = false;
                // TS2_HAENGER_TEST=<VBlank>: Bericht einmal erzwingen (Test des Berichts selbst)
                uint64_t testTick = 0; if (const char *e = std::getenv("TS2_HAENGER_TEST")) testTick = std::strtoull(e, nullptr, 10);
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    GSRegisters &g = rt->memory().gs();
                    const uint64_t fb = ::g_ps2xBildwechsel.load(std::memory_order_relaxed), tick = g.vsyncTick.load();
                    const auto jetzt = std::chrono::steady_clock::now();
                    if (testTick && tick >= testTick) { testTick = 0; bericht(rt, tick); }
                    if (fb != letzterFb || ::ps2xGsBridgeMenuOpen() || tick < 1200) { letzterFb = fb; seit = jetzt; seitTick = tick; gemeldet = false; continue; }
                    if (!gemeldet && jetzt - seit > std::chrono::seconds(8) && tick > seitTick + 400) { gemeldet = true; bericht(rt, seitTick); }
                }
            }).detach();
        }
    }
#endif

    void startWatchdog(PS2Runtime &runtime)
    {
        const char *env = std::getenv("SOTC_WATCHDOG");
        if (!env || !*env) return;
        const int period = std::max(1, std::atoi(env));
        PS2Runtime *rt = &runtime;
        std::thread([rt, period]() {
            for (int tick = 1;; ++tick)
            {
                std::this_thread::sleep_for(std::chrono::seconds(period));
                const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
                std::fprintf(stderr, "[SotC watchdog t=%ds] running=%d cycle=%llu threads=%zu wakeups=%u iwakeups=%u vblankIrq=%u gsIrq=%u vsyncTick=%llu\n", tick * period,
                             snap.runningThreadId, static_cast<unsigned long long>(snap.eeCycle), snap.threads.size(),
                             g_wakeups.load(), g_iwakeups.load(), g_vblankHits.load(), g_gsIrqHits.load(),
                             (unsigned long long)rt->memory().gs().vsyncTick.load());
                for (const auto &t : snap.threads)
                    std::fprintf(stderr, "  thread %d status=%d wait=%d/%d prio=%d pc=0x%08X ra=0x%08X entry=0x%08X sp=0x%08X stack=[0x%08X..0x%08X)\n",
                                 t.id, static_cast<int>(t.status), static_cast<int>(t.waitReason), t.waitId,
                                 t.currentPriority, t.pc, t.ra, t.entry, t.sp, t.stack, t.stack + t.stackSize);
                if (std::getenv("SOTC_TRACE_VIF"))
                    sotc_vif_trace::dump();
                // SOTC_DUMP_CLUT=cbp : 16 CT32 entries in 8x2 layout
                if (const char *cl = std::getenv("SOTC_DUMP_CLUT"); cl && *cl)
                {
                    const unsigned cbp = (unsigned)std::strtoul(cl, nullptr, 0);
                    std::fprintf(stderr, "  clut %u:", cbp);
                    for (unsigned i = 0; i < 16; ++i)
                        std::fprintf(stderr, " %08x", rt->gs().ReadVram(0, cbp, 1, i & 7u, i >> 3));
                    if (FILE *tf = std::fopen(sotcDbgFile("sotc_tex4.pgm").c_str(), "wb"))
                    {
                        std::fprintf(tf, "P5\n128 128\n255\n");
                        for (unsigned ty = 0; ty < 128; ++ty)
                            for (unsigned tx = 0; tx < 128; ++tx)
                            {
                                unsigned char idx = (unsigned char)(rt->gs().ReadVram(20, 10752, 2, tx, ty) * 17u);
                                std::fwrite(&idx, 1, 1, tf);
                            }
                        std::fclose(tf);
                    }
                    if (FILE *tf = std::fopen(sotcDbgFile("sotc_tex4_ct32.ppm").c_str(), "wb"))
                    {
                        std::fprintf(tf, "P6\n64 32\n255\n");
                        for (unsigned ty = 0; ty < 32; ++ty)
                            for (unsigned tx = 0; tx < 64; ++tx)
                            {
                                uint32_t c = rt->gs().ReadVram(0, 10752, 1, tx, ty);
                                unsigned char rgb[3] = {(unsigned char)c, (unsigned char)(c >> 8), (unsigned char)(c >> 16)};
                                std::fwrite(rgb, 1, 3, tf);
                            }
                        std::fclose(tf);
                    }
                    std::fprintf(stderr, "\n  tex4 tbp=10752 row109:");
                    for (unsigned u = 96; u < 120; ++u)
                        std::fprintf(stderr, " %x", rt->gs().ReadVram(20, 10752, 2, u, 109));
                    std::fprintf(stderr, "\n");
                }
                // SOTC_DUMP_FB=fbp:fbw:psm:w:h,... -> /tmp/sotc_fb_<fbp>_<t>.ppm (fbp in 2048-word pages)
                if (const char *fbs = std::getenv("SOTC_DUMP_FB"); fbs && *fbs)
                {
                    std::string list(fbs);
                    size_t at = 0;
                    while (at < list.size())
                    {
                        size_t comma = list.find(',', at);
                        std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                        at = (comma == std::string::npos) ? list.size() : comma + 1;
                        unsigned fbp = 0, fbw = 8, psm = 0, w = 512, h = 448;
                        if (std::sscanf(item.c_str(), "%u:%u:%u:%u:%u", &fbp, &fbw, &psm, &w, &h) < 1)
                            continue;
                        char path[512];
                        std::snprintf(path, sizeof(path), "%s/sotc_fb_%u_%d.ppm", sotcDbgDir(), fbp, tick * period);
                        if (FILE *f = std::fopen(path, "wb"))
                        {
                            std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                            for (unsigned y = 0; y < h; ++y)
                                for (unsigned x = 0; x < w; ++x)
                                {
                                    uint32_t c = rt->gs().ReadVram(psm, fbp * 32u, fbw, x, y);
                                    unsigned char rgb[3];
                                    if (psm == 2 || psm == 10)
                                    {
                                        rgb[0] = (unsigned char)((c & 31u) << 3);
                                        rgb[1] = (unsigned char)(((c >> 5) & 31u) << 3);
                                        rgb[2] = (unsigned char)(((c >> 10) & 31u) << 3);
                                    }
                                    else
                                    {
                                        rgb[0] = (unsigned char)c; rgb[1] = (unsigned char)(c >> 8); rgb[2] = (unsigned char)(c >> 16);
                                    }
                                    std::fwrite(rgb, 1, 3, f);
                                }
                            std::fclose(f);
                            std::fprintf(stderr, "  dumped %s\n", path);
                        }
                    }
                }
                if (std::getenv("SOTC_GSREGS"))
                {
                    auto &g = rt->memory().gs();
                    std::fprintf(stderr, "  gs: pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx bgcolor=%llx csr=%llx imr=%llx\n",
                                 (unsigned long long)g.pmode, (unsigned long long)g.smode2, (unsigned long long)g.dispfb1,
                                 (unsigned long long)g.display1, (unsigned long long)g.dispfb2, (unsigned long long)g.display2,
                                 (unsigned long long)g.bgcolor, (unsigned long long)g.csr.load(), (unsigned long long)g.imr);
                }
                // SOTC_FIND_PAL: search guest RAM for the suspicious 4-bit palette bytes
                if (std::getenv("SOTC_FIND_PAL"))
                {
                    static const uint8_t needle[8] = {0x07, 0x0c, 0x05, 0x76, 0x0b, 0x2d, 0x05, 0x58};
                    const uint8_t *ram = rt->memory().getRDRAM();
                    int found = 0;
                    for (uint32_t a = 0; a + 64 < 0x2000000u && found < 4; ++a)
                        if (std::memcmp(ram + a, needle, 8) == 0)
                        {
                            ++found;
                            std::fprintf(stderr, "  pal@%08x:", a);
                            for (int k = -48; k < 80; ++k)
                                std::fprintf(stderr, "%s%02x", (k % 16 == 0) ? " | " : " ", ram[a + k]);
                            std::fprintf(stderr, "\n");
                        }
                    if (!found)
                        std::fprintf(stderr, "  pal not found in RAM\n");
                }
                // SOTC_FIND_WORD=0xVALUE : list RAM addresses holding this word (with neighbours)
                if (const char *fw = std::getenv("SOTC_FIND_WORD"); fw && *fw)
                {
                    const uint32_t needle = static_cast<uint32_t>(std::strtoul(fw, nullptr, 0));
                    const uint8_t *ram = rt->memory().getRDRAM();
                    int found = 0;
                    for (uint32_t a = 0; a < 0x2000000u && found < 24; a += 4)
                    {
                        uint32_t v; std::memcpy(&v, ram + a, 4);
                        if (v != needle) continue;
                        ++found;
                        std::fprintf(stderr, "  word@%08x:", a);
                        for (int k = -8; k < 8; ++k)
                        {
                            uint32_t w = 0;
                            const int64_t aa = static_cast<int64_t>(a) + k * 4;
                            if (aa >= 0 && aa < 0x2000000) std::memcpy(&w, ram + aa, 4);
                            std::fprintf(stderr, "%s%08x", k == 0 ? " >" : " ", w);
                        }
                        std::fprintf(stderr, "\n");
                    }
                    std::fprintf(stderr, "  find-word 0x%08X: %d hit(s)\n", needle, found);
                }
                // SOTC_PEEK=0xADDR,0xADDR,... : dump guest words
                if (const char *peek = std::getenv("SOTC_PEEK"); peek && *peek)
                {
                    const uint8_t *ram = rt->memory().getRDRAM();
                    std::string list(peek);
                    size_t pos = 0;
                    std::fprintf(stderr, "  peek:");
                    while (pos < list.size())
                    {
                        const size_t comma = list.find(',', pos);
                        const uint32_t addr = static_cast<uint32_t>(std::strtoul(list.substr(pos, comma - pos).c_str(), nullptr, 0));
                        uint32_t v = 0;
                        std::memcpy(&v, ram + (addr & kRamMask), 4);
                        std::fprintf(stderr, " [0x%08X]=0x%08X", addr, v);
                        if (comma == std::string::npos) break;
                        pos = comma + 1;
                    }
                    std::fprintf(stderr, "\n");
                }
                for (auto &slot : g_deltaSlots)
                    if (slot.addr && slot.calls)
                        std::fprintf(stderr, "  lockdelta 0x%08X calls=%u net=%lld\n", slot.addr, slot.calls.load(),
                                     static_cast<long long>(slot.delta.load()));
                if (std::getenv("SOTC_TRACE_OBJLOCK"))
                {
                    std::lock_guard<std::mutex> lock(g_objLockMutex);
                    std::fprintf(stderr, "  objlock wait:");
                    for (auto &[ra, n] : g_objWaitByRa) std::fprintf(stderr, " ra=0x%X x%u", ra, n);
                    std::fprintf(stderr, "\n  objlock signal:");
                    for (auto &[ra, n] : g_objSigByRa) std::fprintf(stderr, " ra=0x%X x%u", ra, n);
                    std::fprintf(stderr, "\n");
                }
                for (const auto &sm : snap.semaphores)
                    if (sm.waiters) std::fprintf(stderr, "  sema %d count=%d max=%d waiters=%u\n", sm.id, sm.count, sm.maxCount, sm.waiters);
            }
        }).detach();
    }

    void sotc_sceGetstat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioGetstat(rdram, ctx, runtime);
        leave(ctx);
    }
    // --- SIF tracing (SOTC_TRACE_SIF=1): log module upload to the IOP -----------------------
    bool traceSif() { static const bool on = std::getenv("SOTC_TRACE_SIF") != nullptr; return on; }

    void sotc_sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t type = arg(ctx, 4), size = arg(ctx, 5), addr = arg(ctx, 6);
        ps2_stubs::sceSifAllocSysMemory(rdram, ctx, runtime);
        if (traceSif()) std::fprintf(stderr, "[SotC SIF] AllocSysMemory(type=%u size=0x%x addr=0x%x) -> 0x%x\n", type, size, addr, GPR_U32(ctx, 2));
        leave(ctx);
    }
    // IOP destination -> EE source of recent SIF DMA uploads. The SDK's sceSifLoadModuleBuffer
    // takes an IOP address, while PS2Recomp's handler reads that address from EE RAM; we hand
    // it the EE source buffer the module was uploaded from (still intact at that point).
    std::mutex g_dmaMutex;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_dmaIopToEe;

    void sotc_sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t list = arg(ctx, 4), count = arg(ctx, 5);
        {
            std::lock_guard<std::mutex> lock(g_dmaMutex);
            for (uint32_t i = 0; i < count && i < 32; ++i)
            {
                uint32_t d[4];
                std::memcpy(d, rdram + ((list + 16 * i) & kRamMask), 16);
                if (d[2] > 0 && d[2] < 0x200000) g_dmaIopToEe[d[1] & 0x1FFFFF] = {d[0], d[2]};
            }
        }
        if (traceSif())
            for (uint32_t i = 0; i < count && i < 4; ++i)
            {
                uint32_t d[4];
                std::memcpy(d, rdram + ((list + 16 * i) & kRamMask), 16);
                std::fprintf(stderr, "[SotC SIF] SetDma[%u] src=0x%x dst=0x%x size=0x%x attr=0x%x\n", i, d[0], d[1], d[2], d[3]);
            }
        ps2_stubs::sceSifSetDma(rdram, ctx, runtime);
        if (traceSif()) std::fprintf(stderr, "[SotC SIF] SetDma -> %d\n", static_cast<int32_t>(GPR_U32(ctx, 2)));
        leave(ctx);
    }
    void sotc_sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t buf = arg(ctx, 4);
        uint8_t head[8] = {};
        const bool readable = runtime->readIopMemory(buf, head, sizeof(head));
        uint32_t eeSrc = 0;
        {
            std::lock_guard<std::mutex> lock(g_dmaMutex);
            auto it = g_dmaIopToEe.find(buf & 0x1FFFFF);
            if (it != g_dmaIopToEe.end()) eeSrc = it->second.first;
        }
        if (eeSrc && std::memcmp(rdram + (eeSrc & kRamMask), head, 4) == 0)
            SET_GPR_U32(ctx, 4, eeSrc);                    // module bytes as uploaded, in EE RAM
        ps2_stubs::sceSifLoadModuleBuffer(rdram, ctx, runtime);
        if (traceSif())
            std::fprintf(stderr, "[SotC SIF] LoadModuleBuffer(iop=0x%x ee=0x%x %s head=%02x %02x %02x %02x) -> %d\n", buf, eeSrc,
                         readable ? "ok" : "UNREADABLE", head[0], head[1], head[2], head[3], static_cast<int32_t>(GPR_U32(ctx, 2)));
        leave(ctx);
    }
    // --- Interrupt tracing (SOTC_TRACE_INTC=1) ------------------------------------------------
    bool traceIntc() { static const bool on = std::getenv("SOTC_TRACE_INTC") != nullptr; return on; }
    std::atomic<uint32_t> g_wakeups{0}, g_iwakeups{0};

    void sotc_AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = arg(ctx, 4), handler = arg(ctx, 5), next = arg(ctx, 6), harg = arg(ctx, 7);
        ps2_syscalls::AddIntcHandler(rdram, ctx, runtime);
        if (traceIntc()) std::fprintf(stderr, "[SotC INTC] AddIntcHandler(cause=%u handler=0x%x next=%u arg=0x%x) -> %d\n",
                                      cause, handler, next, harg, static_cast<int32_t>(GPR_U32(ctx, 2)));
        leave(ctx);
    }
    void sotc_EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = arg(ctx, 4);
        ps2_syscalls::EnableIntc(rdram, ctx, runtime);
        if (traceIntc()) std::fprintf(stderr, "[SotC INTC] _EnableIntc(cause=%u) -> %d\n", cause, static_cast<int32_t>(GPR_U32(ctx, 2)));
        leave(ctx);
    }
    void sotc_iWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_iwakeups;
        ps2_syscalls::iWakeupThread(rdram, ctx, runtime);
        leave(ctx);
    }
    void sotc_WakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_wakeups;
        ps2_syscalls::WakeupThread(rdram, ctx, runtime);
        leave(ctx);
    }
    // ---- libpad2 (scePad2*) HLE ----
    // The game talks to the controller through libpad2/DBC RPCs to the IOP, which the runtime
    // does not emulate. padSysTickProc only needs: GetState()==1 (stable), a 4-byte button
    // profile, and Read() data = [btn0, btn1 (active low), rx, ry, lx, ly, pressures...].
    std::atomic<int> g_pad2Sockets{0};

    void sotc_scePad2Init(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_pad2Sockets = 0;
        SET_GPR_S32(ctx, 2, 1);
        leave(ctx);
    }
    void sotc_scePad2End(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_S32(ctx, 2, 1);
        leave(ctx);
    }
    void sotc_scePad2CreateSocket(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_S32(ctx, 2, g_pad2Sockets.fetch_add(1));
        leave(ctx);
    }
    void sotc_scePad2GetState(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        // Only the first socket (port 1) has a controller attached.
        SET_GPR_S32(ctx, 2, arg(ctx, 4) == 0u ? 1 : 0);
        leave(ctx);
    }
    void sotc_scePad2GetButtonProfile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t buf = arg(ctx, 5);
        // DualShock 2: all 16 digital buttons, both sticks and pressure buttons.
        rdram[(buf + 0) & kRamMask] = 0xFF;
        rdram[(buf + 1) & kRamMask] = 0xFF;
        rdram[(buf + 2) & kRamMask] = 0xFF;
        rdram[(buf + 3) & kRamMask] = 0xFF;
        SET_GPR_S32(ctx, 2, 4);
        leave(ctx);
    }
    void sotc_scePad2Read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sock = arg(ctx, 4), buf = arg(ctx, 5);
        uint8_t raw[32] = {};
        if (sock != 0u || !runtime->padBackend().readState(0, 0, raw, sizeof(raw)))
        {
            SET_GPR_S32(ctx, 2, -1);
            leave(ctx);
            return;
        }
        // Eingabe-Aufnahme/-Wiedergabe je VSync (Fehler beim echten Spielen im Testlauf exakt nachstellen)
        {
            const uint64_t tickNow = runtime->memory().gs().vsyncTick.load();
            if (std::getenv("SOTC_RAMDUMP")) sotcRamDumpCheck(rdram, tickNow); // Speicherabbilder an festen Spielpunkten
            // Spielzaehler (iosGetISchedulerExecCnt, 0x1DC9EC): gemeinsamer Zeitmassstab mit PCSX2 (per PINE lesbar)
            uint32_t gameCnt = 0; std::memcpy(&gameCnt, rdram + 0x1DC9ECu, 4);
            {
                static const char *mapPath = std::getenv("SOTC_TICKMAP"); // "vsyncTick spielzaehler" je Pad-Abfrage
                static std::FILE *mf = (mapPath && *mapPath) ? std::fopen(mapPath, "w") : nullptr;
                static uint64_t lastT = ~0ull;
                if (mf && tickNow != lastT)
                {
                    lastT = tickNow;
                    // Spielerobjekt wie GAMEUTIL_get_player_obj / isysGroupForFirst(0x1477360) + Position (Diagnose/Abgleich)
                    auto r32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & 0x1FFFFFCu), 4); return v; };
                    auto r16 = [&](uint32_t a) { uint16_t v = 0; std::memcpy(&v, rdram + (a & 0x1FFFFFEu), 2); return v; };
                    uint32_t player = 0;
                    uint32_t id = r16(0x1477360u);
                    for (int guard = 0; id && guard < 4096; ++guard)
                    {
                        const uint32_t node = 0x216B00u + id * 24u;
                        if (rdram[node + 0xC]) { id = r16(node + 0xE); continue; }
                        const uint32_t obj = r32(node);
                        if (r16(obj + 0xA) & 1u) { id = r16(node + 0xE); continue; }
                        player = obj;
                        break;
                    }
                    std::fprintf(mf, "%llu %u %08x\n", static_cast<unsigned long long>(tickNow), gameCnt, player);
                }
            }
            {
                // SOTC_RAMDUMP_GC="1000,2000": Speicherabbild, sobald der Spielzaehler den Wert erreicht (Vergleich mit PCSX2-Savestates)
                static std::vector<uint32_t> todo = [] { std::vector<uint32_t> v; if (const char *e = std::getenv("SOTC_RAMDUMP_GC"))
                    { std::string q(e); size_t p = 0; while (p < q.size()) { v.push_back(static_cast<uint32_t>(std::strtoul(q.c_str() + p, nullptr, 10)));
                      p = q.find(',', p); if (p == std::string::npos) break; ++p; } std::sort(v.rbegin(), v.rend()); } return v; }();
                if (!todo.empty() && gameCnt >= todo.back()) { const uint32_t w = todo.back(); todo.pop_back(); sotcRamDump(rdram, w, "gc"); }
            }
            static const char *recPath = std::getenv("SOTC_INPUT_RECORD");
            static const char *repPath = std::getenv("SOTC_INPUT_REPLAY");
            if (repPath && *repPath)
            {
                static std::vector<std::pair<uint64_t, std::array<uint8_t, 6>>> rep;
                static bool loaded = false;
                if (!loaded)
                {
                    loaded = true;
                    if (std::FILE *f = std::fopen(repPath, "r"))
                    {
                        unsigned long long t; unsigned v[6];
                        while (std::fscanf(f, "%llu %x %x %x %x %x %x", &t, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 7)
                            rep.push_back({t, {uint8_t(v[0]), uint8_t(v[1]), uint8_t(v[2]), uint8_t(v[3]), uint8_t(v[4]), uint8_t(v[5])}});
                        std::fclose(f);
                    }
                    std::fprintf(stderr, "[Eingabe] Wiedergabe: %zu Eintraege aus %s\n", rep.size(), repPath);
                }
                std::array<uint8_t, 6> cur = {0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
                for (const auto &e : rep) { if (e.first > tickNow) break; cur = e.second; }
                for (int i = 0; i < 6; ++i) raw[2 + i] = cur[i];
            }
            else if (recPath && *recPath)
            {
                static std::FILE *rf = std::fopen(recPath, "w");
                static uint8_t last[6] = {0, 0, 0, 0, 0, 0};
                static bool first = true;
                if (rf && (first || std::memcmp(last, raw + 2, 6) != 0))
                {
                    first = false;
                    std::memcpy(last, raw + 2, 6);
                    std::fprintf(rf, "%llu %02x %02x %02x %02x %02x %02x\n", static_cast<unsigned long long>(tickNow),
                                 raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
                    std::fflush(rf);
                }
            }
        }
        // SOTC_INPUT="tick:key,tick:key,..." scripted presses (held 8 vsyncs), keys: x o s t start select up down left right
        if (const char *script = std::getenv("SOTC_INPUT"); script && *script)
        {
            // Testlauf: nur die Skript-Eingaben, kein echtes Gamepad/Tastatur dazwischen
            raw[2] = raw[3] = 0xFF;
            raw[4] = raw[5] = raw[6] = raw[7] = 0x80;
            static std::vector<std::pair<uint64_t, std::string>> events;
            static bool parsed = false;
            if (!parsed)
            {
                parsed = true;
                std::string all(script);
                size_t at = 0;
                while (at < all.size())
                {
                    size_t comma = all.find(',', at);
                    std::string item = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                    at = comma == std::string::npos ? all.size() : comma + 1;
                    const size_t colon = item.find(':');
                    if (colon != std::string::npos)
                        events.emplace_back(std::strtoull(item.c_str(), nullptr, 10), item.substr(colon + 1));
                }
            }
            const uint64_t tick = runtime->memory().gs().vsyncTick.load();
            for (const auto &[t, keySpec] : events)
            {
                // "key" holds 8 vsyncs, "key*N" holds N vsyncs. Analog: lup/ldown/lleft/lright, rup/rdown/rleft/rright.
                std::string key = keySpec;
                uint64_t hold = 8u;
                if (const size_t star = key.find('*'); star != std::string::npos)
                {
                    hold = std::strtoull(key.c_str() + star + 1, nullptr, 10);
                    key = key.substr(0, star);
                }
                if (tick < t || tick >= t + hold)
                    continue;
                if (key == "lup") raw[7] = 0x00;
                else if (key == "ldown") raw[7] = 0xFF;
                else if (key == "lleft") raw[6] = 0x00;
                else if (key == "lright") raw[6] = 0xFF;
                else if (key == "rup") raw[5] = 0x00;
                else if (key == "rdown") raw[5] = 0xFF;
                else if (key == "rleft") raw[4] = 0x00;
                else if (key == "rright") raw[4] = 0xFF;
                static const struct { const char *name; int byte; int bit; } kKeys[] = {
                    {"select", 2, 0}, {"start", 2, 3}, {"up", 2, 4}, {"right", 2, 5}, {"down", 2, 6}, {"left", 2, 7},
                    {"l2", 3, 0}, {"r2", 3, 1}, {"l1", 3, 2}, {"r1", 3, 3}, {"t", 3, 4}, {"o", 3, 5}, {"x", 3, 6}, {"s", 3, 7}};
                for (const auto &k : kKeys)
                    if (key == k.name)
                        raw[k.byte] = static_cast<uint8_t>(raw[k.byte] & ~(1u << k.bit));
            }
        }
        // raw = classic libpad layout: [status, 0x73, btn0, btn1, rx, ry, lx, ly]
        uint8_t out[18];
        std::memset(out, 0, sizeof(out));
        out[0] = raw[2];
        out[1] = raw[3];
        out[2] = raw[4];
        out[3] = raw[5];
        out[4] = raw[6];
        out[5] = raw[7];
        // Pressure bytes: full pressure for pressed buttons (right, left, up, down, tri, circle, cross, square, L1, R1, L2, R2)
        static const struct { int byte; int bit; } kPressure[12] = {
            {0, 5}, {0, 7}, {0, 4}, {0, 6}, {1, 4}, {1, 5}, {1, 6}, {1, 7}, {1, 2}, {1, 3}, {1, 0}, {1, 1}};
        for (int i = 0; i < 12; ++i)
            out[6 + i] = (out[kPressure[i].byte] & (1u << kPressure[i].bit)) ? 0x00 : 0xFF;
        for (size_t i = 0; i < sizeof(out); ++i)
            rdram[(buf + i) & kRamMask] = out[i];
        static const bool kEnv_SOTC_TRACE_PAD = std::getenv("SOTC_TRACE_PAD") != nullptr;
        if (kEnv_SOTC_TRACE_PAD && (out[0] != 0xFF || out[1] != 0xFF))
            std::fprintf(stderr, "[SotC pad] buttons %02x %02x\n", out[0], out[1]);
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(sizeof(out)));
        leave(ctx);
    }
    // ---- sceCdReadIOPm: read disc sectors into IOP RAM ----
    // The runtime forwards this to sceCdRead, which writes into EE RAM at the IOP address. Sound
    // streaming (ADPCM) uses it, and that overwrote the loader at 0x1064C0 (relocation jump table).
    void sotc_sceCdReadIOPm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t lbn = arg(ctx, 4), sectors = arg(ctx, 5), iopBuf = arg(ctx, 6);
        const std::filesystem::path iso = PS2Runtime::getIoPaths().cdImage;
        uint8_t *iop = runtime->memory().getIOPRAM();
        constexpr uint32_t kIopMask = 0x1FFFFFu;
        bool ok = false;
        if (iop && !iso.empty())
        {
            if (std::FILE *f = ts2Oeffnen(iso))
            {
                std::vector<uint8_t> tmp(static_cast<size_t>(sectors) * 2048u);
                if (ts2Seek(f, static_cast<uint64_t>(lbn) * 2048u) &&
                    std::fread(tmp.data(), 1, tmp.size(), f) == tmp.size())
                {
                    // Der IOP laeuft im ps2xIOP-Emulator mit eigenem RAM: dorthin schreiben (sonst bleiben die
                    // Musik-Streampuffer leer = Stille). Altes Runtime-IOP-RAM zusaetzlich fuer Altpfade.
                    static const bool oldPath = std::getenv("PS2X_CDREAD_OLD") != nullptr; // Diagnose: Musik-Fix aus
                    if (!oldPath && runtime->isIopMemoryRange(iopBuf & kIopMask, static_cast<uint32_t>(tmp.size())))
                        runtime->writeIopMemory(iopBuf & kIopMask, tmp.data(), tmp.size());
                    for (size_t i = 0; i < tmp.size(); ++i)
                        iop[(iopBuf + i) & kIopMask] = tmp[i];
                    ok = true;
                }
                std::fclose(f);
            }
        }
        static const bool kEnv_SOTC_TRACE_CDREAD = std::getenv("SOTC_TRACE_CDREAD") != nullptr;
        if (kEnv_SOTC_TRACE_CDREAD)
            std::fprintf(stderr, "[SotC cd] ReadIOPm lbn=%u sectors=%u iop=0x%X ok=%d\n", lbn, sectors, iopBuf, ok);
        (void)rdram;
        SET_GPR_S32(ctx, 2, ok ? 1 : 0);
        leave(ctx);
    }

    // ---- Memory card (libmc2 low level) ----
    // libmc2 keeps the whole PS2 card file system on the EE and only asks the IOP for raw pages
    // through _sceMcCore*. We answer those calls from a standard 8 MB card image in PCSX2 format
    // (16384 pages x (512 data + 16 spare/ECC)), so saves are interchangeable with PCSX2.
    constexpr uint32_t kMcPageSize = 512, kMcSpare = 16, kMcRaw = kMcPageSize + kMcSpare;
    constexpr uint32_t kMcPagesPerBlock = 16, kMcPages = 16384;
    constexpr uint32_t kMcSocketTable = 0x001609C8u; // 2 x 0x24 bytes: used, dbc socket, pagesize, blocksize, cardsize, ...

    std::mutex g_mcMutex;
    std::FILE *g_mcFile = nullptr;
    bool g_mcNewCard[2] = {true, true};
    std::string g_mcPath;

    uint8_t mcParity(uint8_t b) { return static_cast<uint8_t>(__builtin_popcount(b) & 1); }

    void mcEcc128(const uint8_t *d, uint8_t out[3])
    {
        static const uint8_t cpmasks[7] = {0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0};
        uint8_t col = 0x77, line0 = 0x7F, line1 = 0x7F;
        for (int i = 0; i < 128; ++i)
        {
            uint8_t mask = 0;
            for (int k = 0; k < 7; ++k)
                mask |= static_cast<uint8_t>(mcParity(d[i] & cpmasks[k]) << k);
            col ^= mask;
            if (mcParity(d[i]))
            {
                line0 ^= static_cast<uint8_t>(~i);
                line1 ^= static_cast<uint8_t>(i);
            }
        }
        out[0] = col;
        out[1] = line0 & 0x7F;
        out[2] = line1;
    }

    bool mcOpen()
    {
        if (g_mcFile)
            return true;
        if (g_mcPath.empty())
        {
            const char *env = std::getenv("SOTC_MEMCARD");
            g_mcPath = env && *env ? env : "Mcd001.ps2";
        }
        g_mcFile = std::fopen(g_mcPath.c_str(), "r+b");
        if (!g_mcFile)
        {
            g_mcFile = std::fopen(g_mcPath.c_str(), "w+b");
            if (!g_mcFile)
            {
                std::fprintf(stderr, "[SotC mc] cannot open %s\n", g_mcPath.c_str());
                return false;
            }
            std::vector<uint8_t> blank(kMcRaw * kMcPagesPerBlock, 0xFF);
            for (uint32_t b = 0; b < kMcPages / kMcPagesPerBlock; ++b)
                std::fwrite(blank.data(), 1, blank.size(), g_mcFile);
            std::fflush(g_mcFile);
            std::fprintf(stderr, "[SotC mc] created blank card %s\n", g_mcPath.c_str());
        }
        else
            std::fprintf(stderr, "[SotC mc] using card %s\n", g_mcPath.c_str());
        return true;
    }

    bool mcCardPresent(uint32_t sock) { return sock == 0u && mcOpen(); }

    void mcSet(R5900Context *ctx, int32_t v)
    {
        SET_GPR_S32(ctx, 2, v);
        leave(ctx);
    }

    void sotc_mcCreateSocket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        uint32_t port;
        std::memcpy(&port, rdram + ((arg(ctx, 4) + 4u) & kRamMask), 4);
        const uint32_t idx = (port >= 2u) ? port - 2u : 0u;
        if (idx > 1u) { mcSet(ctx, -1); return; }
        const uint32_t e = kMcSocketTable + idx * 0x24u;
        const uint32_t vals[5] = {1u, 0x4D430000u + idx, kMcPageSize, kMcPagesPerBlock, kMcPages};
        for (int k = 0; k < 5; ++k) std::memcpy(rdram + ((e + 4u * k) & kRamMask), &vals[k], 4);
        const uint32_t zero = 0;
        for (uint32_t off : {0x14u, 0x18u, 0x1Cu, 0x20u}) std::memcpy(rdram + ((e + off) & kRamMask), &zero, 4);
        g_mcNewCard[idx] = true;
        mcSet(ctx, static_cast<int32_t>(idx));
    }
    void sotc_mcDeleteSocket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t idx = arg(ctx, 4);
        if (idx > 1u) { mcSet(ctx, 0); return; }
        const uint32_t zero = 0;
        std::memcpy(rdram + ((kMcSocketTable + idx * 0x24u) & kRamMask), &zero, 4);
        mcSet(ctx, 1);
    }
    void sotc_mcGetConnection(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        mcSet(ctx, mcCardPresent(arg(ctx, 4)) ? 1 : 0);
    }
    void sotc_mcGetCardSpec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4), out = arg(ctx, 5);
        if (!mcCardPresent(sock)) { mcSet(ctx, 0); return; }
        const uint32_t e = kMcSocketTable + sock * 0x24u;
        const uint32_t vals[4] = {kMcPageSize, kMcPagesPerBlock, kMcPages, 1u};
        for (int k = 0; k < 4; ++k)
        {
            std::memcpy(rdram + ((e + 8u + 4u * k) & kRamMask), &vals[k], 4);
            if (out) std::memcpy(rdram + ((out + 4u * k) & kRamMask), &vals[k], 4);
        }
        mcSet(ctx, 1);
    }
    void sotc_mcCheckNewCard(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4);
        if (!mcCardPresent(sock)) { mcSet(ctx, 0); return; }
        mcSet(ctx, g_mcNewCard[sock & 1u] ? 2 : 1);
    }
    void sotc_mcClearNewCardFlag(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4);
        if (sock < 2u) g_mcNewCard[sock] = false;
        mcSet(ctx, mcCardPresent(sock) ? 1 : 0);
    }
    void sotc_mcPresentOk(uint8_t *, R5900Context *ctx, PS2Runtime *) // Authentication, WriteBack
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        mcSet(ctx, mcCardPresent(arg(ctx, 4)) ? 1 : 0);
    }
    void sotc_mcReadClock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t out = arg(ctx, 5);
        auto bcd = [](int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); };
        const std::time_t now = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        const uint8_t clk[8] = {0, bcd(tmv.tm_sec), bcd(tmv.tm_min), bcd(tmv.tm_hour), 0,
                                bcd(tmv.tm_mday), bcd(tmv.tm_mon + 1), bcd(tmv.tm_year % 100)};
        for (int k = 0; k < 8; ++k) rdram[(out + k) & kRamMask] = clk[k];
        mcSet(ctx, 1);
    }
    void sotc_mcReadPage2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4), buf = arg(ctx, 5), page = arg(ctx, 6), count = arg(ctx, 7), ecc = arg(ctx, 8);
        if (!mcCardPresent(sock) || page + count > kMcPages) { mcSet(ctx, 0); return; }
        uint8_t raw[kMcRaw];
        for (uint32_t i = 0; i < count; ++i)
        {
            std::fseek(g_mcFile, static_cast<long>((page + i) * kMcRaw), SEEK_SET);
            if (std::fread(raw, 1, kMcRaw, g_mcFile) != kMcRaw) std::memset(raw, 0xFF, kMcRaw);
            for (uint32_t b = 0; b < kMcPageSize; ++b) rdram[(buf + i * kMcPageSize + b) & kRamMask] = raw[b];
            if (ecc)
                for (uint32_t b = 0; b < kMcSpare; ++b) rdram[(ecc + i * kMcSpare + b) & kRamMask] = raw[kMcPageSize + b];
        }
        mcSet(ctx, static_cast<int32_t>(count));
    }
    void sotc_mcReadPage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        SET_GPR_U32(ctx, 8, 0u);
        sotc_mcReadPage2(rdram, ctx, rt);
    }
    void sotc_mcWritePage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4), buf = arg(ctx, 5), page = arg(ctx, 6), count = arg(ctx, 7);
        if (!mcCardPresent(sock) || page + count > kMcPages) { mcSet(ctx, 0); return; }
        uint8_t raw[kMcRaw];
        for (uint32_t i = 0; i < count; ++i)
        {
            for (uint32_t b = 0; b < kMcPageSize; ++b) raw[b] = rdram[(buf + i * kMcPageSize + b) & kRamMask];
            std::memset(raw + kMcPageSize, 0, kMcSpare);
            for (int c = 0; c < 4; ++c) mcEcc128(raw + c * 128, raw + kMcPageSize + c * 3);
            std::fseek(g_mcFile, static_cast<long>((page + i) * kMcRaw), SEEK_SET);
            std::fwrite(raw, 1, kMcRaw, g_mcFile);
        }
        std::fflush(g_mcFile);
        mcSet(ctx, static_cast<int32_t>(count));
    }
    void sotc_mcBlockErase(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::lock_guard<std::mutex> lock(g_mcMutex);
        const uint32_t sock = arg(ctx, 4), block = arg(ctx, 5);
        if (!mcCardPresent(sock) || block >= kMcPages / kMcPagesPerBlock) { mcSet(ctx, 0); return; }
        std::vector<uint8_t> ff(kMcRaw * kMcPagesPerBlock, 0xFF);
        std::fseek(g_mcFile, static_cast<long>(block * kMcPagesPerBlock * kMcRaw), SEEK_SET);
        std::fwrite(ff.data(), 1, ff.size(), g_mcFile);
        std::fflush(g_mcFile);
        mcSet(ctx, 1);
    }
    void sotc_mcWriteSameData(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::fprintf(stderr, "[SotC mc] WriteSameData(%u, 0x%X, %u) not implemented\n", arg(ctx, 4), arg(ctx, 5), arg(ctx, 6));
        mcSet(ctx, static_cast<int32_t>(arg(ctx, 6)));
    }
} // namespace

namespace
{
    // SOTC_RAMDUMP="5000,7000,8500": schreibt bei Erreichen jedes vsync-Werts das 32-MB-EE-RAM nach
    // $SOTC_RAMDUMP_DIR/sotc_ram_<vsync>.bin (gleicher Auswerter wie für PCSX2-Savestates). Zusätzlich beim Abbruch.
    void sotcRamDump(const uint8_t *r, uint64_t vs, const char *tag)
    {
        const char *dir = std::getenv("SOTC_RAMDUMP_DIR");
        if (!dir) dir = std::getenv("SOTC_DEBUG_DIR");   // Testfarm: Ergebnisordner des Laufs
        char path[512];
        std::snprintf(path, sizeof(path), "%s/sotc_ram_%s%05llu.bin", dir ? dir : ".", tag, (unsigned long long)vs);
        if (FILE *f = std::fopen(path, "wb")) { std::fwrite(r, 1, 32u * 1024u * 1024u, f); std::fclose(f);
            std::fprintf(stderr, "[SotC ramdump] %s\n", path); }
    }
    void sotcRamDumpCheck(const uint8_t *r, uint64_t vs)
    {
        static std::vector<uint64_t> todo = [] { std::vector<uint64_t> v; if (const char *e = std::getenv("SOTC_RAMDUMP"))
            { std::string s(e); size_t p = 0; while (p < s.size()) { v.push_back(std::strtoull(s.c_str() + p, nullptr, 10));
              p = s.find(',', p); if (p == std::string::npos) break; ++p; } std::sort(v.rbegin(), v.rend()); } return v; }();
        static std::mutex m; if (todo.empty()) return;
        std::lock_guard<std::mutex> lk(m);
        if (!todo.empty() && vs >= todo.back()) { todo.pop_back(); sotcRamDump(r, vs, ""); }
    }
}

// ---- ISO9660: echte LSNs direkt aus dem Disc-Image (allgemein, ersetzt SotCs feste Dateitabelle) ----
namespace ts2_iso
{
    std::mutex g_m;
    std::FILE *g_f = nullptr;
    bool open()
    {
        if (g_f) return true;
        const auto p = PS2Runtime::getIoPaths().cdImage;
        if (p.empty()) return false;
        g_f = ts2Oeffnen(p);
        if (!g_f)
        {
            static bool gemeldet = false;
            if (!gemeldet) std::fprintf(stderr, "[TS2] FEHLER: Disc-Abbild nicht lesbar (verschoben oder umbenannt?): %s\n", p.string().c_str());
            gemeldet = true;
        }
        return g_f != nullptr;
    }
    bool sector(uint32_t lsn, uint8_t *buf)
    {
        return ts2Seek(g_f, static_cast<uint64_t>(lsn) * 2048u) && std::fread(buf, 1, 2048, g_f) == 2048;
    }
    // Pfad wie "\PAK\CHR.PAK;1" oder "cdrom0:\MUSIC\LEVEL1\3.VAG;1" -> lsn/size; Groß-/Kleinschreibung egal
    bool lookup(const std::string &path, uint32_t &lsn, uint32_t &size)
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (!open()) return false;
        uint8_t b[2048];
        if (!sector(16, b) || b[0] != 1 || std::memcmp(b + 1, "CD001", 5) != 0) return false;
        uint32_t dLsn, dSize; std::memcpy(&dLsn, b + 156 + 2, 4); std::memcpy(&dSize, b + 156 + 10, 4);
        std::string rest = normCdName(path);
        while (!rest.empty())
        {
            const auto cut = rest.find('\\');
            const std::string part = rest.substr(0, cut);
            rest = cut == std::string::npos ? std::string() : rest.substr(cut + 1);
            bool found = false;
            for (uint32_t off = 0; off < dSize && !found; off += 2048)
            {
                if (!sector(dLsn + off / 2048, b)) return false;
                for (uint32_t i = 0; i < 2048 && b[i];)
                {
                    const uint8_t len = b[i];
                    std::string n(reinterpret_cast<char *>(b + i + 33), b[i + 32]);
                    if (const auto sc = n.find(';'); sc != std::string::npos) n.resize(sc);
                    while (!n.empty() && n.back() == '.') n.pop_back();
                    for (auto &c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    if (n == part)
                    {
                        std::memcpy(&dLsn, b + i + 2, 4); std::memcpy(&dSize, b + i + 10, 4);
                        found = true; break;
                    }
                    i += len;
                }
            }
            if (!found) return false;
        }
        lsn = dLsn; size = dSize;
        return true;
    }
}
namespace
{
    // int sceCdSearchFile(sceCdlFILE *fp, const char *name) / sceCdLayerSearchFile(fp, name, layer)
    // sceCdlFILE = {u32 lsn, u32 size, char name[16], u8 date[8]}
    void ts2_sceCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rtp)
    {
        ts2_dtoa_test::versuch(rdram, ctx, rtp);
        const uint32_t fp = arg(ctx, 4), namePtr = arg(ctx, 5);
        std::string name;
        for (uint32_t i = 0; i < 255; ++i) { const char c = static_cast<char>(rdram[(namePtr + i) & kRamMask]); if (!c) break; name.push_back(c); }
        uint32_t lsn = 0, size = 0;
        if (ts2_iso::lookup(name, lsn, size))
        {
            uint8_t *out = rdram + (fp & kRamMask);
            std::memset(out, 0, 32);
            std::memcpy(out + 0, &lsn, 4); std::memcpy(out + 4, &size, 4);
            const std::string key = normCdName(name);
            const std::string leaf = key.substr(key.rfind('\\') == std::string::npos ? 0 : key.rfind('\\') + 1);
            std::strncpy(reinterpret_cast<char *>(out + 8), leaf.c_str(), 15);
            static int n = 0;
            if (n++ < 200) std::fprintf(stderr, "[TS2] sceCdSearchFile %s -> lsn %u size %u\n", name.c_str(), lsn, size);
            ret(ctx, 1);
        }
        else
        {
            std::fprintf(stderr, "[TS2] sceCdSearchFile %s -> nicht gefunden\n", name.c_str());
            ret(ctx, 0);
        }
        leave(ctx);
    }
}
// Definitionen unter den generierten Namen (tools/ts2_bauen.sh leert die generierten Dateien gleichen Namens)
#define TS2_DEFINE(GEN_NAME, IMPL) \
    void GEN_NAME(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { IMPL(rdram, ctx, runtime); }
TS2_DEFINE(sceCdLayerSearchFile_0x488dc0, ts2_sceCdSearchFile)

// libmtap (Multitap): Original fragt MTAPMAN.IRX, das ueber SIO2 die Hardware abfragt (im Port nicht nachgebildet).
// Die Pad-Eingabe ist ohnehin HLE (Pad.cpp), daher hier: Multitap an Port 1, wenn ein 3. Gamepad steckt.
namespace
{
    void ts2_sceMtapPortOpen(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 1); leave(ctx); }
    void ts2_sceMtapPortClose(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 1); leave(ctx); }
    void ts2_sceMtapGetConnection(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t port = arg(ctx, 4);
        const int r = (port == 0u && ::ps2xMultitapActive()) ? 1 : 0;
        static int n = 0;
        if (n++ < 20) std::fprintf(stderr, "[MTAP] GetConnection port=%u -> %d\n", port, r);
        ret(ctx, r);
        leave(ctx);
    }
}
TS2_DEFINE(sceMtapPortOpen_0x44e248, ts2_sceMtapPortOpen)
TS2_DEFINE(sceMtapPortClose_0x44e2b8, ts2_sceMtapPortClose)
TS2_DEFINE(sceMtapGetConnection_0x44e328, ts2_sceMtapGetConnection)

// TS2_STUBCOUNT=1 (nur mit instrumentiertem Code, tools/ts2_stub_zaehlen.py): wie oft wird welche HLE-Ersatzfunktion
// aufgerufen? Ausgabe beim Ende und jede Minute nach stderr ("[STUBS] name=anzahl ...").
#include <unordered_map>
extern uint32_t g_ps2xGameGParamAddr;   // Laufzeit GS.cpp: sceGsGParam des Spiels (sceGsGetGParam 0x4416E0 -> 0x56A250)
void ts2StubCount(const char *name)
{
    static std::mutex m; static std::unordered_map<const char *, uint64_t> cnt; static auto last = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l(m);
    ++cnt[name];
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(20))
    {
        last = now;
        std::vector<std::pair<std::string, uint64_t>> v(cnt.begin(), cnt.end());
        std::sort(v.begin(), v.end());
        std::fprintf(stderr, "[STUBS]");
        for (const auto &[n, c] : v) std::fprintf(stderr, " %s=%llu", n.c_str(), (unsigned long long)c);
        std::fprintf(stderr, "\n");
    }
}

// TS2_DTOA_TEST=1 (nur Testlaeufe): einmal im laufenden Spiel die ORIGINALEN Funktionen malloc (0x48EE78) und
// sprintf (0x490180) des Spiels aufrufen, wie am Geschuetz (printf("%f") -> _dtoa_r -> _Balloc -> malloc).
// Laeuft im EE-Kontext (beim ersten sceCdSearchFile), Register werden danach wiederhergestellt.
namespace ts2_dtoa_test
{
    bool rufe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t fn, uint32_t magic)
    {
        ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(magic));
        ctx->pc = fn;
        for (int i = 0; i < 2000000 && ctx->pc != magic; ++i)
        {
            auto f = rt->lookupFunction(ctx->pc);
            if (!f) { std::fprintf(stderr, "[DTOA-TEST] keine Funktion fuer pc=0x%08X\n", ctx->pc); return false; }
            f(rdram, ctx, rt);
        }
        return ctx->pc == magic;
    }
    void versuch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        static const bool aktiv = [] { const char *e = std::getenv("TS2_DTOA_TEST"); return e && *e == '1'; }();
        static bool fertig = false;
        if (aktiv && !fertig)
        {
            fertig = true;
            const R5900Context sicher = *ctx;
            const uint32_t sp = (GPR_U32(ctx, 29) - 0x800u) & ~0xFu, buf = sp + 0x100u, fmt = sp + 0x300u, magic = 0x00200000u;
            ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(sp));
            std::memset(rdram + buf, 0, 0x100);
            const char *f = "%f|%.3f|%d";
            std::memcpy(rdram + fmt, f, std::strlen(f) + 1);
            ctx->r[4] = _mm_set_epi64x(0, 64);
            const bool ok1 = rufe(rdram, ctx, rt, 0x0048EE78u, magic);
            const uint32_t m = GPR_U32(ctx, 2);
            std::fprintf(stderr, "[DTOA-TEST] malloc(64) -> 0x%08X (%s)\n", m, ok1 ? "zurueck" : "NICHT zurueck");
            // Softfloat-Routinen (libgcc fp-bit) einzeln gegen den Host: dpadd/dpsub/dpmul/dpdiv (a0,a1 -> v0)
            {
                struct R { const char *n; uint32_t ad; int op; };
                const R rs[] = {{"dpadd", 0x0048C9F8u, 0}, {"dpsub", 0x0048CA50u, 1}, {"dpmul", 0x0048CAB8u, 2}, {"dpdiv", 0x0048CD68u, 3}};
                const double paare[][2] = {{1.5, 2.25}, {1234.5678, 10.0}, {0.5, 3.0}, {3.14159, 2.71828}, {1e10, 1e-5}, {-2.25, 0.1}, {7.0, 0.0001}};
                int fehler = 0;
                for (const R &r : rs)
                    for (const auto &pq : paare)
                    {
                        int64_t a, b; std::memcpy(&a, &pq[0], 8); std::memcpy(&b, &pq[1], 8);
                        ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(sp));
                        ctx->r[4] = _mm_set_epi64x(0, a); ctx->r[5] = _mm_set_epi64x(0, b);
                        rufe(rdram, ctx, rt, r.ad, magic);
                        const uint64_t v = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[2])); double g; std::memcpy(&g, &v, 8);
                        const double h = r.op == 0 ? pq[0] + pq[1] : r.op == 1 ? pq[0] - pq[1] : r.op == 2 ? pq[0] * pq[1] : pq[0] / pq[1];
                        uint64_t hb; std::memcpy(&hb, &h, 8);
                        if (hb != v) { ++fehler; std::fprintf(stderr, "[DTOA-TEST] %s(%.17g, %.17g) = %.17g, Host %.17g FALSCH (%016llx vs %016llx)\n", r.n, pq[0], pq[1], g, h,
                                                              (unsigned long long)v, (unsigned long long)hb); }
                    }
                std::fprintf(stderr, "[DTOA-TEST] Softfloat: %d Abweichungen\n", fehler);
            }
            // mehrere Werte einzeln: "%f" mit genau einem double (in $a2), dazu "%d"-Kontrolle
            const double werte[] = {1.5, 0.5, 100.0, 1234.5678, 3.14159, -2.25, 0.001, 1e10, 123456789.0};
            const char *f1 = "%f";
            std::memcpy(rdram + fmt, f1, 3);
            for (double w : werte)
            {
                int64_t bits; std::memcpy(&bits, &w, 8);
                std::memset(rdram + buf, 0, 0x100);
                ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(sp));
                ctx->r[4] = _mm_set_epi64x(0, buf); ctx->r[5] = _mm_set_epi64x(0, fmt); ctx->r[6] = _mm_set_epi64x(0, bits);
                const bool ok2 = rufe(rdram, ctx, rt, 0x00490180u, magic);
                char host[64]; std::snprintf(host, sizeof host, "%f", w);
                const char *g = reinterpret_cast<const char *>(rdram + buf);
                std::fprintf(stderr, "[DTOA-TEST] %%f %-14s -> %-22.40s %s%s\n", host, g, std::strcmp(host, g) == 0 ? "OK" : "FALSCH", ok2 ? "" : " (NICHT zurueck)");
            }
            *ctx = sicher;
        }
    }
}

namespace ps2_stubs { void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry); void clearPadOverrideState(); }
namespace
{
    // SOTC_INPUT="tick:key,tick:key*N,..." (gleiches Format wie SotC): "key" hält 8 VSyncs, "key*N" N VSyncs.
    // Tasten: x o s t start select up down left right l1 l2 r1 r2, analog lup/ldown/lleft/lright, rup/rdown/rleft/rright.
    // Über setPadOverrideState der Laufzeit (libpad-HLE), Port 0. Ohne SOTC_INPUT bleibt das echte Gamepad aktiv.
    // TS2_WIDESCREEN=1: echtes 16:9 - Seitenverhaeltnis-Konstanten wie PCSX2-Patch SLUS-20314_12532F1C.pnach
    // ("Widescreen 16:9", Autor No.47, patch=1 = jedes Bild): 0x3f955555 (1.1667) -> 0x3fc71c71 (1.5556).
    // Nur Daten, kein Code -> kein Neuuebersetzen noetig. Renderer dazu auf 16:9 (PS2X_PCSX2_WIDESCREEN=1).
    // ---- Mauszielen (PC): Kamera-Drehung direkt im Spielerobjekt setzen ----
    // Verfahren und Objekt-Layout wie der Mouse Injector (GameCube-Fassung von TS2, GPL-2.0+, Carnivorous/garungorp):
    // Gier +0x148 und Nick +0x14C (Grad), Fadenkreuz-Schwanken +0x8B8/+0x8BC (-1..1). Die PS2-Adressen wurden per
    // RAM-Vergleich (rechter Stick dreht/hebt) gefunden: Spielerzeiger 0x59DC0C, Nick-Grenze 0x59DC1C (40 Grad).
    // Letzter Pad-Zustand aus SOTC_INPUT (Testlaeufe): die freie Kamera liest ihn statt des echten Pads
    static uint16_t g_skriptPad = 0; static uint8_t g_skriptAx[4] = {0x80, 0x80, 0x80, 0x80}; static bool g_skriptPadDa = false;
    namespace maus
    {
        constexpr uint32_t kSpielerZeiger = 0x0059DC0Cu;
        constexpr uint32_t kNickGrenze = 0x0059DC1Cu;
        inline float rf(const uint8_t *r, uint32_t a) { float v; std::memcpy(&v, r + (a & 0x1FFFFFCu), 4); return v; }
        inline void wf(uint8_t *r, uint32_t a, float v) { std::memcpy(r + (a & 0x1FFFFFCu), &v, 4); }
        // TS2_MAUS_TEST="tick:dx:dy*N,..." (Testlaeufe ohne echte Maus): N VBlanks lang dx/dy Zaehler je VBlank
        bool testDelta(uint64_t tick, int &dx, int &dy)
        {
            static const char *e = std::getenv("TS2_MAUS_TEST");
            if (!e || !*e) return false;
            struct T { uint64_t t, n; int dx, dy; };
            static std::vector<T> v = [] { std::vector<T> r; std::string s(e); size_t p = 0;
                while (p < s.size()) { T x{0, 1, 0, 0}; unsigned long long t = 0, n = 1; int a = 0, b = 0;
                    const size_t c = s.find(',', p); const std::string it = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
                    if (std::sscanf(it.c_str(), "%llu:%d:%d*%llu", &t, &a, &b, &n) >= 3) { x.t = t; x.dx = a; x.dy = b; x.n = n; r.push_back(x); }
                    if (c == std::string::npos) break; p = c + 1; }
                return r; }();
            dx = dy = 0;
            for (const auto &x : v) if (tick >= x.t && tick < x.t + x.n) { dx += x.dx; dy += x.dy; }
            return true;
        }
        void tick(uint8_t *r, uint64_t tick)
        {
            int dx = 0, dy = 0;
            const bool test = testDelta(tick, dx, dy);
            const bool gefangen = test || ::ps2xGsBridgeMouse(dx, dy) != 0;
            // Ego-Ansicht? (Spielerobjekt da, Gier/Nick im gueltigen Bereich) - sonst nichts anfassen
            uint32_t pb = 0; std::memcpy(&pb, r + kSpielerZeiger, 4);
            bool ego = pb >= 0x100000u && pb < 0x1FF0000u;
            float gier = 0.f, nick = 0.f, grenze = 0.f;
            if (ego)
            {
                gier = rf(r, pb + 0x148); nick = rf(r, pb + 0x14C); grenze = rf(r, kNickGrenze);
                ego = gier >= 0.f && gier < 360.f && grenze > 1.f && grenze <= 90.f && nick >= -grenze - 1.f && nick <= grenze + 1.f;
            }
            // Original-Fadenkreuz: beim Maus-Spielen den Zielmodus (L2) halten. Das Spiel zielt dann selbst genau ins Kreuz.
            const bool zielmodus = gefangen && ego && ::ps2xGsBridgeOption(5, 1) != 0;
            g_ps2xPadZusatz.store(zielmodus ? 0x0100u : 0u, std::memory_order_relaxed);
            static bool s_aktiv = false; static float s_gier = 0.f, s_nick = 0.f;
            if (!gefangen || !ego) { s_aktiv = false; return; }
            if (zielmodus)
            {
                // Kreuz fest in der Bildmitte (kein Zurueckfedern), die Maus dreht die Kamera
                wf(r, pb + 0x8B8, 0.f);
                wf(r, pb + 0x8BC, 0.f);
            }
            // Kamera gehoert der Maus: kleine Aenderungen des Spiels (Auto-Lookahead zieht den Blick beim Laufen zur
            // Waagerechten) werden zurueckgenommen, grosse (Rueckstoss, Skript, Tod) uebernommen.
            if (s_aktiv)
            {
                // Auto-Lookahead zieht den Nick proportional zur Waagerechten (bis ~1 Grad je Bild); Rueckstoss kommt nur
                // beim Feuern -> waehrend R1/R2 gedrueckt ist, alles uebernehmen, sonst Drift Richtung 0 zuruecknehmen.
                uint16_t pt = 0; uint8_t ax[4] = {0x80, 0x80, 0x80, 0x80};
                const bool feuert = ::ps2xGsBridgePad(pt, ax) && (pt & 0x0A00u) != 0;
                const float dn = nick - s_nick;
                float dg = gier - s_gier; while (dg > 180.f) dg -= 360.f; while (dg < -180.f) dg += 360.f;
                const bool zurNull = std::fabs(nick) < std::fabs(s_nick) - 0.001f;
                if (!feuert && zurNull && std::fabs(dn) < 3.0f && std::fabs(dg) < 0.6f)
                { nick = s_nick; gier = s_gier; }
            }
            s_aktiv = true;
            if (dx != 0 || dy != 0)
            {
                // Empfindlichkeit in Zehntel (Menue: 0.1-10.0, Standard 3.0) -> Grad je Mauszaehler wie bei PC-Shootern;
                // beim Zoomen (rechte Maustaste) zusaetzlich die Zoom-Empfindlichkeit in %
                const float sens = static_cast<float>(::ps2xGsBridgeOption(3, 30)) / 10.f;
                const bool invert = ::ps2xGsBridgeOption(4, 0) != 0;
                const float zoom = ::ps2xGsBridgeOption(7, 0) ? static_cast<float>(::ps2xGsBridgeOption(6, 50)) / 100.f : 1.f;
                const float k = 0.022f * sens * zoom;
                gier -= static_cast<float>(dx) * k;
                nick += static_cast<float>(invert ? dy : -dy) * k;
                while (gier < 0.f) gier += 360.f;
                while (gier >= 360.f) gier -= 360.f;
                nick = std::clamp(nick, -grenze, grenze);
            }
            wf(r, pb + 0x148, gier);
            wf(r, pb + 0x14C, nick);
            s_gier = gier; s_nick = nick;
        }

        // ---- Freie Kamera (Dev-Modus, Bridge-Option 8 / F7 / TS2_FREECAM=1) ----
        // Spielerposition im Spielerobjekt: +0x1A0/+0x1A4/+0x1A8 (x, Augenhoehe ~1.62, z), Bodenkopie +0x194..+0x19C,
        // Vorbild +0xF0..+0xF8, Lauftempo +0x1B4/+0x1B8. Vorwaerts = (sin(gier), 0, cos(gier)) (RAM-Diff ts2_344/346).
        // Jedes Bild wird die Position neu gesetzt; die Spieleingabe wird eingefroren (Pad-Override), damit die Figur
        // weder laeuft noch schiesst. Blickrichtung kommt weiter von der Maus (oben).
        void flug(uint8_t *r, uint64_t tick)
        {
            static const bool envAn = [] { const char *e = std::getenv("TS2_FREECAM"); return e && *e == '1'; }();
            static const uint64_t ab = [] { const char *e = std::getenv("TS2_FREECAM_AB"); return e && *e ? std::strtoull(e, nullptr, 10) : 0ull; }();
            const bool an = (envAn && tick >= ab) || ::ps2xGsBridgeOption(8, 0) != 0;
            static bool war = false;
            static float px = 0.f, py = 0.f, pz = 0.f;
            uint32_t pb = 0; std::memcpy(&pb, r + kSpielerZeiger, 4);
            // Nur im Level (Nick-Grenze gesetzt, Blickwinkel gueltig) - im Titel/Menue existiert das Spielerobjekt auch
            const float grenze = rf(r, kNickGrenze);
            const bool ego = pb >= 0x100000u && pb < 0x1FF0000u && rf(r, pb + 0x148) >= 0.f && rf(r, pb + 0x148) < 360.f && grenze > 1.f && grenze <= 90.f;
            if (!an || !ego)
            {
                if (war) { ps2_stubs::clearPadOverrideState(); war = false; std::fprintf(stderr, "[TS2] Freie Kamera aus\n"); }
                return;
            }
            if (!war)
            {
                px = rf(r, pb + 0x1A0); py = rf(r, pb + 0x1A4); pz = rf(r, pb + 0x1A8);
                std::fprintf(stderr, "[TS2] Freie Kamera an bei (%.1f %.1f %.1f)\n", px, py, pz);
                war = true;
            }
            // Eingabe: linker Stick / WASD (Vorwaerts/Seitwaerts), Kreuz (Space/E) hoch, L1 (Strg/C) runter, L2 (Shift) schnell
            uint16_t pt = 0; uint8_t ax[4] = {0x80, 0x80, 0x80, 0x80};
            if (g_skriptPadDa) { pt = g_skriptPad; std::memcpy(ax, g_skriptAx, 4); }
            else ::ps2xGsBridgePad(pt, ax);
            float vor = -(static_cast<float>(ax[1]) - 128.f) / 128.f, seit = (static_cast<float>(ax[0]) - 128.f) / 128.f;
            if (std::fabs(vor) < 0.15f) vor = 0.f;
            if (std::fabs(seit) < 0.15f) seit = 0.f;
            float hoch = ((pt & 0x4000u) ? 1.f : 0.f) - ((pt & 0x0400u) ? 1.f : 0.f);
            static const float tempoBasis = [] { const char *e = std::getenv("TS2_FREECAM_TEMPO"); return e && *e ? static_cast<float>(std::atof(e)) : 0.12f; }();
            const float tempo = tempoBasis * ((pt & 0x0100u) ? 4.f : 1.f);
            const float gier = rf(r, pb + 0x148) * 3.14159265f / 180.f, nick = rf(r, pb + 0x14C) * 3.14159265f / 180.f;
            // Blickrichtung (mit Nick), Seitwaerts rechtwinklig in der Ebene
            const float fx = std::sin(gier) * std::cos(nick), fy = std::sin(nick), fz = std::cos(gier) * std::cos(nick);
            const float sx = std::cos(gier), sz = -std::sin(gier);
            px += (fx * vor + sx * seit) * tempo;
            py += (fy * vor + hoch) * tempo;
            pz += (fz * vor + sz * seit) * tempo;
            wf(r, pb + 0x1A0, px); wf(r, pb + 0x1A4, py); wf(r, pb + 0x1A8, pz);
            wf(r, pb + 0x194, px); wf(r, pb + 0x198, py - 1.62f); wf(r, pb + 0x19C, pz);
            wf(r, pb + 0x0F0, px); wf(r, pb + 0x0F4, py - 1.62f); wf(r, pb + 0x0F8, pz);
            wf(r, pb + 0x1B4, 0.f); wf(r, pb + 0x1B8, 0.f);
            // Kamerakopien an festen Adressen (RAM-Suche ts2_350): Augenpunkt (y = Boden + 1.62) und Bodenpunkt; ohne sie
            // bleibt die Hoehe am Boden kleben, weil das Spiel den Augenpunkt je Bild aus dem Bodenkontakt neu setzt
            static const bool kopien = [] { const char *e = std::getenv("TS2_FREECAM_KOPIEN"); return !(e && *e == '0'); }();
            if (kopien)
            {
                for (uint32_t a : {0x784708u, 0x784750u, 0x7E82B0u, 0x7E837Cu})
                { wf(r, a, px); wf(r, a + 4, py); wf(r, a + 8, pz); }
                for (uint32_t a : {0x7849A8u, 0x7849F0u, 0x7B2368u, 0x7B23B0u})
                { wf(r, a, px); wf(r, a + 4, py - 1.62f); wf(r, a + 8, pz); }
            }
            // Spieleingabe einfrieren (kein Laufen, kein Feuern); Maus-Blick laeuft weiter ueber tick()
            ps2_stubs::setPadOverrideState(0xFFFFu, 0x80, 0x80, 0x80, 0x80);
            (void)tick;
        }
    }

    void ts2WidescreenTick(uint8_t *rdram)
    {
        static const bool an = [] { const char *e = std::getenv("TS2_WIDESCREEN"); return e && *e == '1'; }();
        if (!rdram) return;
        // TS2_POKE="adresse=wert[/16],..." (hex, nur fuer Testlaeufe): Speicherwoerter je Bild setzen, z. B. die
        // Freischaltungen aus den CodeBreaker-Codes (entschluesselt): 5cc268/5cc26c/5cc270 Story Easy/Normal/Hard,
        // 5cc280/16 Challenges, 59d6fc "alles freischalten".
        {
            struct Poke { uint32_t ad, val; bool half; };
            static const std::vector<Poke> pokes = [] {
                std::vector<Poke> v; const char *e = std::getenv("TS2_POKE");
                for (const char *p = e; p && *p;)
                {
                    char *end = nullptr; Poke k{static_cast<uint32_t>(std::strtoul(p, &end, 16)), 0u, false};
                    if (end && *end == '=') k.val = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
                    if (end && *end == '/') { k.half = std::strtoul(end + 1, &end, 10) == 16; }
                    if (k.ad && k.ad + 4 <= 0x2000000u) v.push_back(k);
                    p = (end && *end == ',') ? end + 1 : nullptr;
                }
                return v; }();
            for (const auto &k : pokes) { if (k.half) { const uint16_t h = static_cast<uint16_t>(k.val); std::memcpy(rdram + k.ad, &h, 2); } else std::memcpy(rdram + k.ad, &k.val, 4); }
        }
        // Divisor der fest eingebauten 4:3-Projektionen (Befehls-Patch in tools/ts2_rec_elf.py): 3.0 = Original, 2.25 = 16:9
        { const uint32_t v = an ? 0x40100000u : 0x40400000u; std::memcpy(rdram + 0x0059e9f0u, &v, 4); }
        // Letterbox in Zwischensequenzen (Befehls-Patch, Werte wie Original bzw. PCSX2-Patch "No Cutscene Letterbox").
        // TS2_LETTERBOX=1 Balken wie im Original, 0 ohne Balken; Standard: ohne Balken bei 16:9, Original bei 4:3.
        {
            static const bool balken = [] { const char *e = std::getenv("TS2_LETTERBOX"); const char *w = std::getenv("TS2_WIDESCREEN");
                                            if (e && *e) return *e == '1'; return !(w && *w == '1'); }();
            static const uint16_t orig[7] = {0x40, 0x180, 0x200, 0x40, 0x180, 0x200, 0x1C0};
            static const uint16_t ohne[7] = {0x00, 0x1C0, 0x000, 0x00, 0x000, 0x000, 0x000};
            std::memcpy(rdram + 0x0059e9d0u, balken ? orig : ohne, sizeof(orig));
        }
        if (!an) return;
        static const uint32_t adr[] = {0x005995dc, 0x00598a08, 0x00599664, 0x0059a304, 0x0059a308, 0x0059a39c, 0x0059a3ac, 0x0059ca10};
        for (uint32_t a : adr) { const uint32_t v = 0x3fc71c71u; std::memcpy(rdram + a, &v, 4); }
        // TS2_WS_EXTRA="adresse[=wert],..." (hex): weitere Datenwoerter je Bild setzen (Suche nach HUD-/Waffen-Konstanten)
        static const std::vector<std::pair<uint32_t, uint32_t>> extra = [] {
            std::vector<std::pair<uint32_t, uint32_t>> v; const char *e = std::getenv("TS2_WS_EXTRA");
            for (const char *p = e; p && *p;)
            {
                char *end = nullptr; const uint32_t ad = static_cast<uint32_t>(std::strtoul(p, &end, 16)); uint32_t val = 0x3fc71c71u;
                if (end && *end == '=') val = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
                if (ad && ad + 4 <= 0x2000000u) v.emplace_back(ad, val);
                p = (end && *end == ',') ? end + 1 : nullptr;
            }
            return v; }();
        for (const auto &[ad, val] : extra) std::memcpy(rdram + ad, &val, 4);
    }
    void ts2InputTickScript(uint64_t tick, const char *envName, int port);
    // TS2_EINGABE_AUFNAHME=datei: echte Pad-Eingaben je VBlank mitschreiben (nur bei Aenderung: "tick tasten lx ly rx ry",
    // tasten 1 = gedrueckt). TS2_EINGABE_WIEDERGABE=datei spielt sie ueber den Pad-Override wieder ab (Fehler nachstellen).
    void ts2EingabeAufnahme(uint64_t tick)
    {
        static const char *rep = std::getenv("TS2_EINGABE_WIEDERGABE");
        static const char *rec = std::getenv("TS2_EINGABE_AUFNAHME");
        if (rep && *rep)
        {
            struct E { uint64_t t; uint16_t p; uint8_t a[4]; };
            static std::vector<E> ev = [] {
                std::vector<E> v; if (std::FILE *f = std::fopen(rep, "r")) { unsigned long long t; unsigned p, a0, a1, a2, a3;
                    while (std::fscanf(f, "%llu %x %x %x %x %x", &t, &p, &a0, &a1, &a2, &a3) == 6)
                        v.push_back({t, uint16_t(p), {uint8_t(a0), uint8_t(a1), uint8_t(a2), uint8_t(a3)}}); std::fclose(f); }
                std::fprintf(stderr, "[TS2] Eingabe-Wiedergabe: %zu Eintraege\n", v.size()); return v; }();
            static size_t i = 0;
            while (i + 1 < ev.size() && ev[i + 1].t <= tick) ++i;
            if (!ev.empty() && ev[0].t <= tick)
                ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~ev[i].p), ev[i].a[0], ev[i].a[1], ev[i].a[2], ev[i].a[3]);
            return;
        }
        if (!rec || !*rec) return;
        static std::FILE *f = std::fopen(rec, "w");
        if (!f) return;
        uint16_t p = 0; uint8_t a[4] = {0x80, 0x80, 0x80, 0x80};
        if (!::ps2xGsBridgePad(p, a)) return;
        static uint16_t lp = 0xFFFF; static uint8_t la[4] = {0, 0, 0, 0};
        if (p == lp && std::memcmp(a, la, 4) == 0) return;
        lp = p; std::memcpy(la, a, 4);
        std::fprintf(f, "%llu %04x %02x %02x %02x %02x\n", static_cast<unsigned long long>(tick), p, a[0], a[1], a[2], a[3]);
        if ((tick & 63u) == 0u) std::fflush(f);
        static bool atexitReg = [] { std::atexit([] { if (f) std::fflush(f); }); return true; }(); (void)atexitReg;
    }
    void ts2InputTick(uint64_t tick)
    {
        ts2EingabeAufnahme(tick);
        ts2InputTickScript(tick, "SOTC_INPUT", 0);
        ts2InputTickScript(tick, "TS2_INPUT2", 1);   // Splitscreen: Spieler 2 (Port 2)
        ts2InputTickScript(tick, "TS2_INPUT3", 2);   // Multitap: Spieler 3
        ts2InputTickScript(tick, "TS2_INPUT4", 3);   // Multitap: Spieler 4
    }
    void ts2InputTickScript(uint64_t tick, const char *envName, int port)
    {
        const char *script = std::getenv(envName);
        if (!script || !*script) return;
        static std::vector<std::pair<uint64_t, std::string>> evPorts[4];
        static bool parsedPorts[4] = {false, false, false, false};
        if (!parsedPorts[port]) { parsedPorts[port] = true; evPorts[port] = [script] {
            std::vector<std::pair<uint64_t, std::string>> ev; std::string all(script); size_t at = 0;
            while (at < all.size())
            {
                const size_t comma = all.find(',', at);
                const std::string item = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                at = comma == std::string::npos ? all.size() : comma + 1;
                if (const size_t colon = item.find(':'); colon != std::string::npos)
                    ev.emplace_back(std::strtoull(item.c_str(), nullptr, 10), item.substr(colon + 1));
            }
            return ev; }(); }
        const auto &events = evPorts[port];
        uint8_t b2 = 0xFF, b3 = 0xFF, rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
        for (const auto &[t, spec] : events)
        {
            std::string key = spec; uint64_t hold = 8u;
            if (const size_t star = key.find('*'); star != std::string::npos) { hold = std::strtoull(key.c_str() + star + 1, nullptr, 10); key = key.substr(0, star); }
            if (tick < t || tick >= t + hold) continue;
            if (key == "lup") ly = 0x00; else if (key == "ldown") ly = 0xFF; else if (key == "lleft") lx = 0x00; else if (key == "lright") lx = 0xFF;
            else if (key == "rup") ry = 0x00; else if (key == "rdown") ry = 0xFF; else if (key == "rleft") rx = 0x00; else if (key == "rright") rx = 0xFF;
            static const struct { const char *name; int byte; int bit; } kKeys[] = {
                {"select", 2, 0}, {"start", 2, 3}, {"up", 2, 4}, {"right", 2, 5}, {"down", 2, 6}, {"left", 2, 7},
                {"l2", 3, 0}, {"r2", 3, 1}, {"l1", 3, 2}, {"r1", 3, 3}, {"t", 3, 4}, {"o", 3, 5}, {"x", 3, 6}, {"s", 3, 7}};
            for (const auto &k : kKeys)
                if (key == k.name) { if (k.byte == 2) b2 &= static_cast<uint8_t>(~(1u << k.bit)); else b3 &= static_cast<uint8_t>(~(1u << k.bit)); }
        }
        if (port == 0)
        {
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(b2 | (b3 << 8)), lx, ly, rx, ry);
            g_skriptPad = static_cast<uint16_t>(~(b2 | (b3 << 8))); g_skriptAx[0] = lx; g_skriptAx[1] = ly; g_skriptAx[2] = rx; g_skriptAx[3] = ry; g_skriptPadDa = true;
        }
        else
        {
            ::ps2xSetPadOverridePlayer(port, static_cast<uint16_t>(b2 | (b3 << 8)), lx, ly, rx, ry);
        }
    }
}

namespace
{
    void applyTs2(PS2Runtime &runtime)
    {
        // Protokoll vollstaendig: ist stdout eine Pipe (Launcher schreibt spielen.log), puffert die C-Laufzeit sonst
        // blockweise, und bei einem Haenger/Abbruch fehlt das Ende. Zeilenweise reicht (stderr ist ungepuffert).
#ifdef _WIN32
        std::setvbuf(stdout, nullptr, _IONBF, 0);   // Windows kennt kein Zeilenpuffern (_IOLBF = Blockpuffer)
        // Windows weckt wartende Threads nur im Takt der Systemuhr (Standard 15,6 ms). Der VBlank-Takt (16,7 ms) und
        // die Bildsynchronisation warten ueber condition_variable/Sleep -> ohne 1-ms-Aufloesung halbe Bildrate.
        ::timeBeginPeriod(1);
#else
        std::setvbuf(stdout, nullptr, _IOLBF, 1 << 14);
#endif
        configureIso();
        // TS2_MCROOT=/pfad: eigenes Memory-Card-Verzeichnis (Testlaeufe reproduzierbar, nicht disc/inhalt/mc0 teilen)
        if (const char *mc = std::getenv("TS2_MCROOT"); mc && *mc)
        {
            PS2Runtime::IoPaths p = PS2Runtime::getIoPaths();
            p.mcRoot = mc;
            PS2Runtime::setIoPaths(p);
            std::fprintf(stderr, "[TS2] Memory Card: %s\n", mc);
        }
        runtime.registerFunction(0x00488DC0u, sceCdLayerSearchFile_0x488dc0);
        runtime.registerFunction(0x0044E248u, sceMtapPortOpen_0x44e248);
        runtime.registerFunction(0x0044E2B8u, sceMtapPortClose_0x44e2b8);
        runtime.registerFunction(0x0044E328u, sceMtapGetConnection_0x44e328);
        g_ps2xGameGParamAddr = 0x0056A250u;   // sceGsResetGraph laeuft als Originalcode und schreibt den Bildmodus dorthin   // libcdvd ohne Server -> ISO9660
        startWatchdog(runtime);
#if defined(__linux__)
        haenger::start(runtime);   // Haenger-Bericht ins Protokoll (ohne gdb)
#endif
        startMemWatch(runtime);      // SOTC_MEMWATCH=0xADDR,...
        startWriteProtect(runtime);  // SOTC_WPROT=0xADDR
        startTestHarness(runtime);
        sotc_prof::start();
        // Testgeschirr: bis der VBlank-Handler des Spiels bekannt ist, die VSync-Zähler aus eigenem Thread abfragen.
        PS2Runtime *rt = &runtime;
        std::thread([rt]() {
            uint64_t last = 0;
            for (;;)
            {
                const uint64_t t = rt->memory().gs().vsyncTick.load();
                if (t != last) { last = t; ++g_vblankHits; ts2WidescreenTick(rt->memory().getRDRAM()); ts2InputTick(t); maus::tick(rt->memory().getRDRAM(), t); maus::flug(rt->memory().getRDRAM(), t); harnessOnVBlank(rt); { static const bool vt = std::getenv("SOTC_TRACE_VIF") != nullptr; if (vt && t % 100u == 0u) { std::fprintf(stderr, "[VIF-Statistik] tick=%llu\n", (unsigned long long)t); sotc_vif_trace::dump(); } } sotcRamDumpCheck(rt->memory().getRDRAM(), t); }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }).detach();
        // Diagnose: wie oft feuern GS-Handler (Bild fertig, INTC 0) und Render-Timeout-Alarm?
        static PS2Runtime::RecompiledFunction s_origGs = runtime.lookupFunction(0x0020267Cu);
        static PS2Runtime::RecompiledFunction s_origAlarm = runtime.lookupFunction(0x00202150u);
        static std::atomic<uint64_t> s_gsHits{0}, s_alarmHits{0};
        if (s_origGs)
            runtime.registerFunction(0x0020267Cu, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) { ++s_gsHits; ++g_gsIrqHits; s_origGs(r, c, rt); });
        if (s_origAlarm)
            runtime.registerFunction(0x00202150u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                const uint64_t n = ++s_alarmHits;
                if (n <= 5 || n % 100 == 0)
                    std::fprintf(stderr, "[TS2] Render-Timeout-Alarm #%llu bei vsync %llu (GS-Handler bisher %llu)\n", (unsigned long long)n,
                                 (unsigned long long)rt->memory().gs().vsyncTick.load(), (unsigned long long)s_gsHits.load());
                s_origAlarm(r, c, rt); });
        // TS2_FPUTEST=1: uebersetzte libm-Funktionen des Spiels mit Grenzwerten aufrufen (PS2-FPU: kein Inf/NaN, x/0 = +-Fmax)
        if (std::getenv("TS2_FPUTEST"))
        {
            struct T { const char *name; uint32_t addr; } fns[] = {{"acosf", 0x00484870u}, {"sinf", 0x00483DA8u}, {"cosf", 0x00483CA8u}};
            const uint32_t ins[] = {0xC1233333u /*-10.2*/, 0x3F800001u /*1.0000001*/, 0x3F800000u, 0x7F7FFFFFu /*+Fmax*/, 0xFF7FFFFFu,
                                    0x7F800000u /*Inf-Muster*/, 0x7FC00000u /*NaN-Muster*/, 0x4B800000u /*2^24*/, 0x40490FDBu /*pi*/};
            for (const auto &fn : fns)
            {
                auto f = runtime.lookupFunction(fn.addr);
                if (!f) { std::fprintf(stderr, "[FPUTEST] %s fehlt\n", fn.name); continue; }
                for (uint32_t in : ins)
                {
                    R5900Context c{};
                    c.r[29] = _mm_set_epi64x(0, 0x01F80000);
                    std::memcpy(&c.f[12], &in, 4);
                    f(runtime.memory().getRDRAM(), &c, &runtime);
                    uint32_t out; std::memcpy(&out, &c.f[0], 4);
                    float fi, fo; std::memcpy(&fi, &in, 4); std::memcpy(&fo, &out, 4);
                    std::fprintf(stderr, "[FPUTEST] %s(0x%08X = %g) = 0x%08X = %g\n", fn.name, in, (double)fi, out, (double)fo);
                }
            }
        }
        std::fprintf(stderr, "[TS2 override] applied (SLUS-203.14)\n");
    }
}

PS2_REGISTER_GAME_OVERRIDE("TimeSplitters 2 (NTSC-U)", "SLUS_203.14", 0x00200008u, 0u, applyTs2)

// =====================================================================================================================
// Online-Mehrspieler (virtuelles LAN ueber einen TS2-Server, siehe src/server/ts2_server.cpp)
//
// TS2 hat einen LAN-Modus fuer den PS2-Netzwerkadapter. Kette im Original:
//   Boot: DEV9.IRX (Ergebnis 0 = Adapter da) -> Netzwerk-Menue -> INET/NETCNF/EZNETCNF laden -> Netzwerk-Konfiguration
//   von der Memory Card (ezNetCnf) -> INETCTL/SMAP/MSIFRPC/LIBNETB -> Verbindung (ezNetCtl) -> spieleigene Socket-
//   Schicht (0x479178 Open, 0x479378 SendTo, ...) auf libnet (Sony INET ueber SIF-RPC + Ringpuffer im IOP).
// Ersetzt wird:
//   * das Modul-Laden (0x44B000/0x44B020): DEV9 meldet "Adapter da", die Netzwerkmodule gelten als geladen
//   * ezNetCnf/ezNetCtl: eine Konfiguration "TS2 Online", Verbindung sofort "verbunden"
//   * das Backend der Socket-Schicht (0x479F90.., 16 Einstiege): statt libnet ein Tunnel zum TS2-Server.
//     Jeder Spieler bekommt dort eine virtuelle IP 10.13.x.y; Broadcasts (Spielsuche) gehen an alle auf dem Server.
// Spiel-Typen der Socket-Schicht: 0 TCP verbinden, 1 UDP, 2 TCP warten (Gegenstelle verbindet), 3 ICMP (Ping).
// Fehlercodes des Backends: 0 ok, 1 Fehler, 2 Parameter, 5 nicht initialisiert, 0xD Verbindung beendet.
//
// Einschalten: TS2_ONLINE_SERVER=host[:port] (Standardport 27713), TS2_ONLINE_NAME, TS2_ONLINE_PASSWORT
// oder ~/.config/ts2-native/online.ini (server=, name=, passwort=). TS2_NETLOG=1: jeden Aufruf protokollieren.
// =====================================================================================================================
#include <condition_variable>
#include <deque>
#include <fstream>
#if defined(__linux__)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#define TS2_NETZ_SOCKETS 1
namespace ts2netz
{
    inline int netzSchliessen(int s) { return ::close(s); }
    inline void nichtBlockierend(int s) { fcntl(s, F_SETFL, O_NONBLOCK); }
    inline int netzPoll(pollfd *p, int n, int ms) { return ::poll(p, nfds_t(n), ms); }
    constexpr int kSendeFlags = MSG_NOSIGNAL;
    inline bool netzStart() { return true; }
}
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define TS2_NETZ_SOCKETS 1
namespace ts2netz
{
    inline int netzSchliessen(int s) { return closesocket(SOCKET(s)); }
    inline void nichtBlockierend(int s) { u_long an = 1; ioctlsocket(SOCKET(s), FIONBIO, &an); }
    inline int netzPoll(pollfd *p, int n, int ms) { return WSAPoll(p, ULONG(n), ms); }
    constexpr int kSendeFlags = 0;
    inline bool netzStart() { static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }(); return ok; }
}
#else
#define TS2_NETZ_SOCKETS 0
#endif
void sub_0044ADD8_0x44add8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime); // Stub sceSifLoadModule (generiert)
void ps2xGsBridgeOnlineBrowser();                   // gs_frontend.cpp: Serverbrowser im Spielmenue oeffnen
void ps2xGsBridgeOnlineStatus(const char *text);    // gs_frontend.cpp: Verbindungsstatus im Serverbrowser

namespace ts2netz
{
    constexpr uint16_t kProtokoll = 1;
    constexpr uint16_t kStandardPort = 27713;
    constexpr uint32_t kBroadcast = 0x0A0DFFFFu;
    enum : uint8_t
    {
        M_HELLO = 1, M_WELCOME = 2, M_REJECT = 3, M_DGRAM = 10, M_SCONNECT = 20, M_SINCOMING = 21, M_SACCEPT = 22,
        M_SRESULT = 23, M_SDATA = 24, M_SCLOSE = 25, M_PING = 30, M_KEEPALIVE = 40, M_INFO = 41,
    };

    struct Konfig
    {
        bool aktiv = false;
        std::string host, name, passwort;
        uint16_t port = kStandardPort;
        bool log = false;
    };

    inline std::string trim(std::string s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    }

    // Einstellungsdatei des Spielmenues (ESC/F1 -> Online): <XDG_CONFIG_HOME|~/.config>/<PS2XGS_CFGNAME>/einstellungen.ini
    inline std::string iniPfad()
    {
        if (const char *c = std::getenv("PS2XGS_CFG"); c && *c) return c;
        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        const char *home = std::getenv("HOME");
        const char *name = std::getenv("PS2XGS_CFGNAME");
        const std::string basis = xdg && *xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.config";
        return basis + "/" + (name && *name ? name : "ts2-recomp") + "/einstellungen.ini";
    }

    Konfig lesen()
    {
        Konfig c;
        std::string server, name, pw;
#if TS2_NETZ_SOCKETS
        std::ifstream in(iniPfad());
        for (std::string z; std::getline(in, z);)
        {
            const auto p = z.find('=');
            if (z.empty() || z[0] == '#' || p == std::string::npos) continue;
            const std::string key = trim(z.substr(0, p)), val = trim(z.substr(p + 1));
            if (key == "online_server") server = val;
            else if (key == "online_name") name = val;
            else if (key == "online_passwort") pw = val;
        }
#endif
        if (const char *e = std::getenv("TS2_ONLINE_SERVER")) server = e;
        if (const char *e = std::getenv("TS2_ONLINE_NAME")) name = e;
        if (const char *e = std::getenv("TS2_ONLINE_PASSWORT")) pw = e;
        if (name.empty()) { const char *u = std::getenv("USER"); name = u && *u ? u : "Spieler"; }
        server = trim(server);
        if (!server.empty())
        {
            std::string h = server; // host, host:port, [v6]:port
            if (h[0] == '[' && h.find(']') != std::string::npos)
            {
                const size_t e = h.find(']');
                if (e + 1 < h.size() && h[e + 1] == ':') c.port = uint16_t(std::atoi(h.c_str() + e + 2));
                h = h.substr(1, e - 1);
            }
            else if (std::count(h.begin(), h.end(), ':') == 1)
            {
                c.port = uint16_t(std::atoi(h.c_str() + h.find(':') + 1));
                h = h.substr(0, h.find(':'));
            }
            c.host = h;
            c.aktiv = TS2_NETZ_SOCKETS && !h.empty() && c.port != 0;
        }
        c.name = name;
        c.passwort = pw;
        c.log = std::getenv("TS2_NETLOG") != nullptr;
        return c;
    }

    Konfig &konfig()
    {
        static Konfig k = lesen();
        return k;
    }

    // Netzwerkadapter melden: immer (Online-Modus waehlbar), ausser TS2_ONLINE=0 oder ohne Socket-Unterstuetzung
    inline bool adapterAn()
    {
        static const bool an = [] { const char *e = std::getenv("TS2_ONLINE"); return TS2_NETZ_SOCKETS && !(e && *e == '0'); }();
        return an;
    }

    inline std::string ipText(uint32_t ip)
    {
        char b[20];
        std::snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        return b;
    }
    inline bool ipLesen(const std::string &s, uint32_t &ip)
    {
        unsigned a, b, c, d;
        char rest;
        if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &rest) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
        ip = (a << 24) | (b << 16) | (c << 8) | d;
        return true;
    }
    inline void put16(std::string &s, uint16_t v) { s.push_back(char(v)); s.push_back(char(v >> 8)); }
    inline void put32(std::string &s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char(v >> (8 * i))); }
    inline uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
    inline uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

    struct Datagramm { uint32_t vip; uint16_t port; std::string daten; };
    struct PingAntwort { uint32_t vip; uint16_t id; };

    struct Sock
    {
        int typ = 1;
        uint16_t lport = 0;
        std::deque<Datagramm> rx;        // UDP
        uint32_t sid = 0, cid = 0;       // TCP-Strom
        int zustand = 0;                 // 0 wartet, 1 verbindet, 2 offen, 3 beendet/abgelehnt
        std::string srx;                 // TCP-Empfangspuffer
        uint32_t peerVip = 0;
        uint16_t peerPort = 0;
        std::deque<PingAntwort> prx;     // ICMP
    };

#if TS2_NETZ_SOCKETS
    class Tunnel
    {
    public:
        std::mutex m;
        std::condition_variable cv;
        int zustand = 0; // 0 aus, 1 verbindet, 2 online, 3 Fehler
        std::string fehler, servername, motd;
        uint32_t vip = 0, token = 0;
        int spieler = 0;
        std::map<int, Sock> socks;
        int naechsterHandle = 1;
        uint16_t naechsterPort = 50000;
        uint32_t naechsteCid = 1;

        // Start (idempotent); wartet bis online oder Fehler, hoechstens ms
        bool online(int ms)
        {
            std::unique_lock<std::mutex> l(m);
            if (zustand == 0 || zustand == 3)
            {
                zustand = 1;
                fehler.clear();
                if (thread.joinable()) { l.unlock(); thread.join(); l.lock(); }
                thread = std::thread([this] { lauf(); });
            }
            cv.wait_for(l, std::chrono::milliseconds(ms), [this] { return zustand == 2 || zustand == 3; });
            return zustand == 2;
        }

        // Verbindung zum Server trennen (z. B. anderer Server gewaehlt); Spiel-Sockets werden verworfen
        void beenden()
        {
            {
                std::lock_guard<std::mutex> l(m);
                if (zustand != 1 && zustand != 2) return;
                trennen = true;
            }
            wecken();
            if (thread.joinable()) thread.join();
            std::lock_guard<std::mutex> l(m);
            zustand = 0;
            trennen = false;
            socks.clear();
            vip = 0;
            std::fprintf(stderr, "[ONLINE] Verbindung zum Server getrennt\n");
        }

        void senden(uint8_t typ, const std::string &nutz)
        {
            std::string f;
            put32(f, uint32_t(nutz.size() + 1));
            f.push_back(char(typ));
            f += nutz;
            std::lock_guard<std::mutex> l(ausM);
            if (aus.size() < (16u << 20)) aus += f;
            wecken();
        }

        void datagramm(uint16_t srcport, uint32_t dstvip, uint16_t dstport, const uint8_t *d, size_t n)
        {
            if (dstvip == vip) // an sich selbst
            {
                std::lock_guard<std::mutex> l(m);
                zustellen(vip, srcport, dstport, d, n);
                return;
            }
            if (udpOk.load() && n <= 1400)
            {
                std::string p;
                put32(p, token); put32(p, vip); put16(p, srcport); put32(p, dstvip); put16(p, dstport);
                p.append(reinterpret_cast<const char *>(d), n);
                if (int(::sendto(udp, p.data(), int(p.size()), 0, reinterpret_cast<sockaddr *>(&srv), srvLen)) == int(p.size())) return;
            }
            std::string p;
            put16(p, srcport); put32(p, dstvip); put16(p, dstport);
            p.append(reinterpret_cast<const char *>(d), n);
            senden(M_DGRAM, p);
        }

        // m muss gehalten sein
        void zustellen(uint32_t srcvip, uint16_t srcport, uint16_t dstport, const uint8_t *d, size_t n)
        {
            for (auto &kv : socks)
            {
                Sock &s = kv.second;
                if (s.typ == 1 && s.lport == dstport)
                {
                    if (s.rx.size() >= 512) s.rx.pop_front();
                    s.rx.push_back({srcvip, srcport, std::string(reinterpret_cast<const char *>(d), n)});
                    return;
                }
            }
        }

    private:
        std::thread thread;
        int tcp = -1, udp = -1, weck = -1; // weck: UDP-Socket an 127.0.0.1, weckt den Netzwerk-Thread (statt pipe, auch Windows)
        sockaddr_in weckAdr{};
        sockaddr_storage srv{};
        socklen_t srvLen = 0;
        std::mutex ausM;
        std::string aus, ein;
        std::atomic<bool> udpOk{false};
        bool trennen = false; // m

        void wecken() { if (weck >= 0) { const char c = 1; (void)::sendto(weck, &c, 1, 0, reinterpret_cast<const sockaddr *>(&weckAdr), sizeof weckAdr); } }

        void scheitern(const std::string &grund)
        {
            std::lock_guard<std::mutex> l(m);
            if (zustand != 3) { std::fprintf(stderr, "[ONLINE] Verbindung: %s\n", grund.c_str()); ::ps2xGsBridgeOnlineStatus(("Nicht verbunden: " + grund).c_str()); }
            fehler = grund;
            zustand = 3;
            for (auto &kv : socks) if (kv.second.typ == 0 || kv.second.typ == 2) if (kv.second.zustand < 3 && kv.second.zustand > 0) kv.second.zustand = 3;
            cv.notify_all();
        }

        bool verbinden()
        {
            if (!netzStart()) { scheitern("Netzwerk (Winsock) nicht verfuegbar"); return false; }
            const Konfig &k = konfig();
            addrinfo hint{}, *res = nullptr;
            hint.ai_family = AF_UNSPEC;
            hint.ai_socktype = SOCK_STREAM;
            const std::string port = std::to_string(k.port);
            if (getaddrinfo(k.host.c_str(), port.c_str(), &hint, &res) != 0 || !res) { scheitern("Server \"" + k.host + "\" nicht gefunden"); return false; }
            for (addrinfo *a = res; a; a = a->ai_next)
            {
                const int fd = int(::socket(a->ai_family, SOCK_STREAM, 0));
                if (fd < 0) continue;
                nichtBlockierend(fd);
                ::connect(fd, a->ai_addr, a->ai_addrlen);
                pollfd p{decltype(pollfd::fd)(fd), POLLOUT, 0};
                int err = 0;
                socklen_t el = sizeof err;
                if (netzPoll(&p, 1, 4000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &el) == 0 && err == 0)
                {
                    tcp = fd;
                    std::memcpy(&srv, a->ai_addr, a->ai_addrlen);
                    srvLen = a->ai_addrlen;
                    break;
                }
                netzSchliessen(fd);
            }
            freeaddrinfo(res);
            if (tcp < 0) { scheitern("Server " + k.host + ":" + port + " antwortet nicht"); return false; }
            int ja = 1;
            setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&ja), sizeof ja);
            udp = int(::socket(srv.ss_family, SOCK_DGRAM, 0));
            if (udp >= 0) nichtBlockierend(udp);
            return true;
        }

        void nachricht(uint8_t typ, const uint8_t *p, size_t n)
        {
            std::unique_lock<std::mutex> l(m);
            switch (typ)
            {
            case M_WELCOME:
                if (n < 8) break;
                vip = get32(p);
                token = get32(p + 4);
                {
                    const std::string rest(reinterpret_cast<const char *>(p + 8), n - 8);
                    servername = rest.substr(0, rest.find('\0'));
                    motd = rest.find('\0') != std::string::npos ? rest.substr(rest.find('\0') + 1) : "";
                }
                zustand = 2;
                std::fprintf(stderr, "[ONLINE] verbunden mit \"%s\" als %s%s%s\n", servername.c_str(), ipText(vip).c_str(),
                             motd.empty() ? "" : " - ", motd.c_str());
                ::ps2xGsBridgeOnlineStatus(("Verbunden mit \"" + servername + "\"" + (motd.empty() ? "" : " - " + motd)).c_str());
                cv.notify_all();
                break;
            case M_REJECT:
                l.unlock();
                scheitern("abgelehnt: " + std::string(reinterpret_cast<const char *>(p), n));
                break;
            case M_INFO:
                if (n >= 4) spieler = int(get32(p));
                break;
            case M_DGRAM:
                if (n >= 8) zustellen(get32(p), get16(p + 4), get16(p + 6), p + 8, n - 8);
                break;
            case M_SINCOMING:
            {
                if (n < 12) break;
                const uint32_t sid = get32(p), src = get32(p + 4);
                const uint16_t sport = get16(p + 8), dport = get16(p + 10);
                bool ok = false;
                for (auto &kv : socks)
                {
                    Sock &s = kv.second;
                    if (s.typ == 2 && s.lport == dport && s.zustand == 0)
                    {
                        s.sid = sid; s.zustand = 2; s.peerVip = src; s.peerPort = sport;
                        ok = true;
                        break;
                    }
                }
                l.unlock();
                std::string r;
                put32(r, sid);
                r.push_back(ok ? 1 : 0);
                senden(M_SACCEPT, r);
                if (konfig().log) std::fprintf(stderr, "[ONLINE] eingehend von %s:%u an Port %u -> %s\n", ipText(src).c_str(), sport, dport, ok ? "angenommen" : "abgelehnt");
                break;
            }
            case M_SRESULT:
            {
                if (n < 9) break;
                const uint32_t cid = get32(p), sid = get32(p + 4);
                for (auto &kv : socks)
                    if (kv.second.cid == cid && kv.second.zustand == 1)
                    {
                        kv.second.sid = sid;
                        kv.second.zustand = p[8] ? 2 : 3;
                    }
                cv.notify_all();
                break;
            }
            case M_SDATA:
            {
                if (n < 4) break;
                const uint32_t sid = get32(p);
                for (auto &kv : socks)
                    if (kv.second.sid == sid && kv.second.zustand == 2 && kv.second.srx.size() < (4u << 20))
                        kv.second.srx.append(reinterpret_cast<const char *>(p + 4), n - 4);
                break;
            }
            case M_SCLOSE:
            {
                if (n < 4) break;
                const uint32_t sid = get32(p);
                for (auto &kv : socks)
                    if (kv.second.sid == sid) kv.second.zustand = 3;
                break;
            }
            case M_PING:
            {
                if (n < 11) break;
                const uint32_t von = get32(p);
                const uint16_t id = get16(p + 4);
                if (!p[10]) // Anfrage -> sofort beantworten
                {
                    l.unlock();
                    std::string r;
                    put32(r, von);
                    r.append(reinterpret_cast<const char *>(p + 4), 6);
                    r.push_back(1);
                    senden(M_PING, r);
                    break;
                }
                for (auto &kv : socks)
                    if (kv.second.typ == 3 && kv.second.prx.size() < 64) kv.second.prx.push_back({von, id});
                break;
            }
            default:
                break;
            }
        }

        void udpLesen()
        {
            uint8_t b[2048];
            for (;;)
            {
                const int n = int(::recv(udp, reinterpret_cast<char *>(b), sizeof b, 0));
                if (n < 0) return;
                if (n == 4 && std::memcmp(b, "T2OK", 4) == 0)
                {
                    if (!udpOk.exchange(true)) std::fprintf(stderr, "[ONLINE] UDP-Kanal aktiv\n");
                    continue;
                }
                if (n < 8) continue;
                std::lock_guard<std::mutex> l(m);
                zustellen(get32(b), get16(b + 4), get16(b + 6), b + 8, size_t(n) - 8);
            }
        }

        void lauf()
        {
            if (!verbinden()) return;
            weck = int(::socket(AF_INET, SOCK_DGRAM, 0));
            if (weck >= 0)
            {
                weckAdr.sin_family = AF_INET;
                weckAdr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                socklen_t wl = sizeof weckAdr;
                if (::bind(weck, reinterpret_cast<sockaddr *>(&weckAdr), sizeof weckAdr) == 0 && getsockname(weck, reinterpret_cast<sockaddr *>(&weckAdr), &wl) == 0)
                    nichtBlockierend(weck);
                else { netzSchliessen(weck); weck = -1; }
            }
            {
                const Konfig &k = konfig();
                std::string h;
                put16(h, kProtokoll);
                h += k.name;
                h.push_back('\0');
                h += k.passwort;
                senden(M_HELLO, h);
            }
            auto letzteReg = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            auto letzterKeep = std::chrono::steady_clock::now();
            for (;;)
            {
                const auto jetzt = std::chrono::steady_clock::now();
                if (udp >= 0 && vip && jetzt - letzteReg > std::chrono::milliseconds(udpOk ? 2000 : 500))
                {
                    letzteReg = jetzt;
                    std::string r;
                    put32(r, token); put32(r, vip); put16(r, 0); put32(r, 0); put16(r, 0);
                    (void)::sendto(udp, r.data(), 12, 0, reinterpret_cast<sockaddr *>(&srv), srvLen);
                }
                if (jetzt - letzterKeep > std::chrono::seconds(5)) { letzterKeep = jetzt; senden(M_KEEPALIVE, ""); }
                bool schreiben;
                { std::lock_guard<std::mutex> l(ausM); schreiben = !aus.empty(); }
                using fd_t = decltype(pollfd::fd);
                pollfd p[3] = {{fd_t(tcp), short(POLLIN | (schreiben ? POLLOUT : 0)), 0}, {fd_t(udp), POLLIN, 0}, {fd_t(weck), POLLIN, 0}};
                if (netzPoll(p, weck >= 0 ? 3 : 2, weck >= 0 ? 250 : 5) < 0) continue;
                { std::lock_guard<std::mutex> l(m); if (trennen) break; }
                if (weck >= 0 && (p[2].revents & POLLIN)) { char d[64]; while (::recv(weck, d, sizeof d, 0) > 0) {} }
                if (p[1].revents & POLLIN) udpLesen();
                if (p[0].revents & (POLLERR | POLLHUP)) { scheitern("Verbindung zum Server verloren"); break; }
                if (p[0].revents & POLLIN)
                {
                    char b[16384];
                    const int n = int(::recv(tcp, b, sizeof b, 0));
                    if (n <= 0) { scheitern(zustand == 2 ? "Verbindung zum Server verloren" : "Server hat die Verbindung beendet"); break; }
                    ein.append(b, size_t(n));
                    size_t pos = 0;
                    while (ein.size() - pos >= 5)
                    {
                        const uint32_t len = get32(reinterpret_cast<const uint8_t *>(ein.data() + pos));
                        if (len == 0 || len > (1u << 20)) { scheitern("ungueltige Daten vom Server"); pos = ein.size(); break; }
                        if (ein.size() - pos < 4 + len) break;
                        const uint8_t *msg = reinterpret_cast<const uint8_t *>(ein.data() + pos + 4);
                        nachricht(msg[0], msg + 1, len - 1);
                        pos += 4 + len;
                    }
                    ein.erase(0, pos);
                    std::lock_guard<std::mutex> l(m);
                    if (zustand == 3) break;
                }
                {
                    std::lock_guard<std::mutex> l(ausM);
                    while (!aus.empty())
                    {
                        const int n = int(::send(tcp, aus.data(), int(aus.size()), kSendeFlags));
                        if (n <= 0) break;
                        aus.erase(0, size_t(n));
                    }
                }
            }
            netzSchliessen(tcp); tcp = -1;
            if (udp >= 0) { netzSchliessen(udp); udp = -1; }
            if (weck >= 0) { netzSchliessen(weck); weck = -1; }
            udpOk = false;
            std::lock_guard<std::mutex> l(ausM);
            aus.clear();
            ein.clear();
        }
    };

    Tunnel &tunnel()
    {
        static Tunnel *t = new Tunnel(); // bewusst nie zerstoert (Thread laeuft bis Prozessende)
        return *t;
    }
#endif

    // ---- Helfer fuer Gastspeicher ----
    inline std::string gStr(uint8_t *rdram, uint32_t a, size_t max = 255)
    {
        std::string s;
        if (!a) return s;
        for (size_t i = 0; i < max; ++i) { const char c = char(rdram[(a + i) & kRamMask]); if (!c) break; s.push_back(c); }
        return s;
    }
    inline void gStrSchreiben(uint8_t *rdram, uint32_t a, const std::string &s)
    {
        if (!a) return;
        for (size_t i = 0; i < s.size(); ++i) rdram[(a + i) & kRamMask] = uint8_t(s[i]);
        rdram[(a + s.size()) & kRamMask] = 0;
    }
    inline void g8(uint8_t *rdram, uint32_t a, uint8_t v) { if (a) rdram[a & kRamMask] = v; }
    inline void g16(uint8_t *rdram, uint32_t a, uint16_t v) { if (a) std::memcpy(rdram + (a & kRamMask), &v, 2); }
    inline void g32(uint8_t *rdram, uint32_t a, uint32_t v) { if (a) std::memcpy(rdram + (a & kRamMask), &v, 4); }
    inline uint32_t r32(uint8_t *rdram, uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & kRamMask), 4); return v; }

    inline bool istNetzModul(const std::string &pfad)
    {
        static const char *namen[] = {"\\INET.IRX", "\\NETCNF.IRX", "\\EZNETCNF.IRX", "\\INETCTL.IRX", "\\SMAP.IRX",
                                      "\\MSIFRPC.IRX", "\\LIBNETB.IRX", "\\EZNETCTL.IRX"};
        std::string p = pfad;
        for (auto &c : p) c = char(std::toupper(static_cast<unsigned char>(c)));
        for (const char *n : namen)
            if (p.find(n) != std::string::npos) return true;
        return false;
    }

    // sceSifLoadModule(path, argsize, args) = 0x44B000, sceSifLoadStartModule(path, argsize, args, int *result) = 0x44B020
    void ladeModul(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, bool mitErgebnis)
    {
        const std::string pfad = gStr(rdram, arg(ctx, 4));
        const uint32_t ergebnis = mitErgebnis ? arg(ctx, 7) : 0u;
        std::string grossPfad = pfad;
        for (auto &c : grossPfad) c = char(std::toupper(static_cast<unsigned char>(c)));
        const bool dev9 = grossPfad.find("\\DEV9.IRX") != std::string::npos;
        if (adapterAn() && (dev9 || istNetzModul(pfad)))
        {
            static int naechsteId = 0x60;
            if (ergebnis) g32(rdram, ergebnis, 0); // Modul "laeuft resident"
            std::fprintf(stderr, "[ONLINE] Modul %s -> nachgebildet\n", pfad.c_str());
            ret(ctx, naechsteId++);
            leave(ctx);
            return;
        }
        SET_GPR_U32(ctx, 8, 0); // t0 = 0 wie im Original
        if (!mitErgebnis) SET_GPR_U32(ctx, 7, 0);
        sub_0044ADD8_0x44add8(rdram, ctx, rt); // setzt pc = ra, v0 = Modul-ID
        // Startergebnis: die Laufzeit schreibt es nicht. DEV9 ohne Online = kein Adapter (wie bisher praktisch auch).
        if (ergebnis) g32(rdram, ergebnis, dev9 ? 1u : 0u);
    }
    void ts2_LoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { ladeModul(rdram, ctx, rt, false); }
    void ts2_LoadStartModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { ladeModul(rdram, ctx, rt, true); }

    // ---- ezNetCnf / ezNetCtl (libezcnf.c / libezctl.c) ----
    void ts2_ezNetCnf_A(uint8_t *, R5900Context *ctx, PS2Runtime *) // 0x482150 Init, 0x482218 Exit, 0x482298 GetCount
    {
        const uint32_t pc = ctx->pc;
        const bool an = adapterAn();
        if (pc == 0x482298u) ret(ctx, an ? 1 : 0);
        else if (pc == 0x482218u) ret(ctx, 0);
        else ret(ctx, an ? 1 : 0);
        if (konfig().log) std::fprintf(stderr, "[ONLINE] ezNetCnf %06X\n", pc);
        leave(ctx);
    }
    void ts2_ezNetCnfGetCombinationList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // (path, list)
    {
        const uint32_t list = arg(ctx, 5);
        if (!adapterAn() || !list) { ret(ctx, 0); leave(ctx); return; }
        for (uint32_t i = 0; i < 0x30 + 0x68; i += 4) g32(rdram, list + i, 0);
        g32(rdram, list + 0, 1); // Anzahl
        g32(rdram, list + 4, 1); // Vorgabe (1-basiert)
        const uint32_t e = list + 0x30;
        g32(rdram, e + 0, 1);
        gStrSchreiben(rdram, e + 0x04, "TS2 Online");
        gStrSchreiben(rdram, e + 0x28, "TS2 Online");
        gStrSchreiben(rdram, e + 0x48, "Server");
        std::fprintf(stderr, "[ONLINE] Netzwerk-Konfiguration \"TS2 Online\" bereitgestellt\n");
        if (!std::getenv("TS2_ONLINE_SERVER")) ::ps2xGsBridgeOnlineBrowser(); // Serverbrowser zeigen (Spiel wartet im Menue)
        ret(ctx, 1);
        leave(ctx);
    }
    void ts2_ret0(uint8_t *, R5900Context *ctx, PS2Runtime *) { ret(ctx, 0); leave(ctx); }
    void ts2_ezNetCtl_Init(uint8_t *, R5900Context *ctx, PS2Runtime *) // 0x4826B8 Init, 0x482780 Exit
    {
        ret(ctx, ctx->pc == 0x482780u ? 0 : (adapterAn() ? 1 : 0));
        leave(ctx);
    }
    void ts2_ezNetCtl_Status(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // 0x4829E8 GetStatus(st), 0x482A88
    {
        if (ctx->pc == 0x4829E8u)
        {
            const uint32_t st = arg(ctx, 4);
            g32(rdram, st + 0, 0);
            g32(rdram, st + 4, 0);
            g32(rdram, st + 8, adapterAn() ? 1u : 0u); // 1 = verbunden
        }
        ret(ctx, 0);
        leave(ctx);
    }

    // ---- Backend der Socket-Schicht ----
    void backend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t pc = ctx->pc;
        const uint32_t a0 = arg(ctx, 4), a1 = arg(ctx, 5), a2 = arg(ctx, 6), a3 = arg(ctx, 7);
        const uint32_t t0 = arg(ctx, 8), t1 = arg(ctx, 9);
        int r = 1;
#if TS2_NETZ_SOCKETS
        Tunnel &T = tunnel();
        const bool log = konfig().log;
        auto handle = [&](uint32_t h) -> Sock * { auto it = T.socks.find(int(h)); return it == T.socks.end() ? nullptr : &it->second; };
        switch (pc)
        {
        case 0x479FC0u: // Init
        {
            {
                const Konfig neu = lesen();
                bool anders, online;
                {
                    std::lock_guard<std::mutex> l(T.m);
                    anders = neu.host != konfig().host || neu.port != konfig().port || neu.name != konfig().name || neu.passwort != konfig().passwort;
                    online = T.zustand == 1 || T.zustand == 2;
                }
                if (anders && online) T.beenden(); // anderer Server gewaehlt: neu verbinden
                if (anders || !online) konfig() = neu;
            }
            if (!konfig().aktiv)
            {
                std::fprintf(stderr, "[ONLINE] kein Server eingetragen (Spielmenue ESC/F1 -> Online)\n");
                ::ps2xGsBridgeOnlineStatus("Kein Server gewaehlt - im Reiter Online einen Server verbinden");
                r = 1;
                break;
            }
            std::fprintf(stderr, "[ONLINE] Server %s Port %u, Name \"%s\"\n", konfig().host.c_str(), konfig().port, konfig().name.c_str());
            if (std::getenv("TS2_NETHIST") && !g_ps2xStateHist.exchange(true)) // Diagnose: Aufrufe der Netzwerk-Spielschicht zaehlen
                std::thread([] {
                    std::map<uint32_t, uint64_t> vorher;
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        g_ps2xStateHist.store(true);
                        std::string z;
                        std::lock_guard<std::mutex> cl(g_ps2xCallHistMutex);
                        for (const auto &[pc, n] : g_ps2xCallHist)
                            if (pc >= 0x458000u && pc < 0x47C000u && n != vorher[pc])
                            {
                                char b[40]; std::snprintf(b, sizeof b, " %06X:%llu", pc, (unsigned long long)(n - vorher[pc]));
                                z += b; vorher[pc] = n;
                            }
                        std::fprintf(stderr, "[NETHIST]%s\n", z.c_str());
                    }
                }).detach();
            r = T.online(6000) ? 0 : 1;
            std::fprintf(stderr, "[ONLINE] Netzwerk-Start -> %s\n", r == 0 ? "ok" : T.fehler.c_str());
            break;
        }
        case 0x47A088u: // Term
        {
            std::lock_guard<std::mutex> l(T.m);
            for (auto &kv : T.socks)
                if (kv.second.sid && kv.second.zustand == 2) { std::string p; put32(p, kv.second.sid); T.senden(M_SCLOSE, p); }
            T.socks.clear();
            r = 0;
            break;
        }
        case 0x47A108u: // Open(int *out, const char *host, u16 port, cfg*)
        {
            const int typ = int(r32(rdram, a3));
            const std::string host = gStr(rdram, a1);
            const uint16_t port = uint16_t(a2);
            std::unique_lock<std::mutex> l(T.m);
            if (T.zustand != 2) { r = 1; break; }
            Sock s;
            s.typ = typ;
            s.lport = port;
            if (typ == 0)
            {
                uint32_t ziel = 0;
                if (!ipLesen(host, ziel)) { r = 1; break; }
                s.lport = T.naechsterPort++;
                if (T.naechsterPort < 50000) T.naechsterPort = 50000;
                s.cid = T.naechsteCid++;
                s.zustand = 1;
                s.peerVip = ziel;
                s.peerPort = port;
                const int h = T.naechsterHandle++;
                T.socks[h] = s;
                std::string p;
                put32(p, s.cid); put16(p, s.lport); put32(p, ziel); put16(p, port);
                l.unlock();
                T.senden(M_SCONNECT, p);
                l.lock();
                T.cv.wait_for(l, std::chrono::milliseconds(5000), [&] { auto it = T.socks.find(h); return it == T.socks.end() || it->second.zustand != 1; });
                auto it = T.socks.find(h);
                if (it != T.socks.end() && it->second.zustand == 2) { g32(rdram, a0, uint32_t(h)); r = 0; }
                else { if (it != T.socks.end()) T.socks.erase(it); r = 1; }
                if (log) std::fprintf(stderr, "[ONLINE] TCP verbinden %s:%u -> %s\n", host.c_str(), port, r ? "fehlgeschlagen" : "ok");
                break;
            }
            if (typ == 1 && port == 0) { s.lport = T.naechsterPort++; }
            if (typ < 1 || typ > 3) { r = 3; break; }
            const int h = T.naechsterHandle++;
            T.socks[h] = s;
            g32(rdram, a0, uint32_t(h));
            r = 0;
            if (log) std::fprintf(stderr, "[ONLINE] Open Typ %d Port %u -> Handle %d\n", typ, s.lport, h);
            break;
        }
        case 0x47A3C0u: // Close(h)
        {
            std::lock_guard<std::mutex> l(T.m);
            auto it = T.socks.find(int(a0));
            if (it == T.socks.end()) { r = 2; break; }
            if (it->second.sid && it->second.zustand == 2) { std::string p; put32(p, it->second.sid); T.senden(M_SCLOSE, p); }
            T.socks.erase(it);
            r = 0;
            break;
        }
        case 0x47A410u: // Send(h, buf, u16 len, u16 *sent)
        {
            std::unique_lock<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1 || !a3) { r = 2; break; }
            g16(rdram, a3, 0);
            if (s->zustand == 3) { r = 0xD; break; }
            if (s->zustand != 2) { r = 0; break; } // noch nicht verbunden: nichts gesendet
            const uint32_t sid = s->sid;
            l.unlock();
            std::string p;
            put32(p, sid);
            for (uint32_t i = 0; i < (a2 & 0xFFFF); ++i) p.push_back(char(rdram[(a1 + i) & kRamMask]));
            T.senden(M_SDATA, p);
            g16(rdram, a3, uint16_t(a2));
            r = 0;
            break;
        }
        case 0x47A4A8u: // SendTo(h, const char *host, u16 port, buf, u16 len, u16 *sent)
        {
            uint16_t lport = 0;
            {
                std::lock_guard<std::mutex> l(T.m);
                Sock *s = handle(a0);
                if (!s || !a1 || !a3 || !t1) { r = 2; break; }
                lport = s->lport;
            }
            g16(rdram, t1, 0);
            uint32_t ziel = 0;
            const std::string host = gStr(rdram, a1);
            if (!ipLesen(host, ziel)) { r = 1; break; }
            const uint16_t n = uint16_t(t0);
            std::vector<uint8_t> d(n);
            for (uint32_t i = 0; i < n; ++i) d[i] = rdram[(a3 + i) & kRamMask];
            T.datagramm(lport, ziel, uint16_t(a2), d.data(), d.size());
            g16(rdram, t1, n);
            r = 0;
            static int z = 0;
            if (log && z++ < 200) std::fprintf(stderr, "[ONLINE] SendTo %u -> %s:%u (%u Bytes)\n", lport, host.c_str(), uint16_t(a2), n);
            break;
        }
        case 0x47A598u: // PingSenden(h, u8 *ok, const char *host, u16 id)
        {
            g8(rdram, a1, 0);
            uint32_t ziel = 0;
            if (!ipLesen(gStr(rdram, a2), ziel)) { r = 1; break; }
            {
                std::lock_guard<std::mutex> l(T.m);
                if (!handle(a0)) { r = 2; break; }
                if (ziel == T.vip) { handle(a0)->prx.push_back({ziel, uint16_t(a3)}); g8(rdram, a1, 1); r = 0; break; }
            }
            std::string p;
            put32(p, ziel); put16(p, uint16_t(a3)); put32(p, 0); p.push_back(0);
            T.senden(M_PING, p);
            g8(rdram, a1, 1);
            r = 0;
            break;
        }
        case 0x47A658u: // Recv(h, buf, u16 len, u16 *got)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1 || !a3) { r = 2; break; }
            g16(rdram, a3, 0);
            const uint32_t n = std::min<uint32_t>(a2 & 0xFFFF, uint32_t(s->srx.size()));
            if (n == 0) { r = s->zustand == 3 ? 0xD : 0; break; }
            for (uint32_t i = 0; i < n; ++i) rdram[(a1 + i) & kRamMask] = uint8_t(s->srx[i]);
            s->srx.erase(0, n);
            g16(rdram, a3, uint16_t(n));
            r = 0;
            break;
        }
        case 0x47A708u: // RecvFrom(h, char *host, u16 *port, buf, u16 len, u16 *got)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1 || !a2 || !a3 || !t1) { r = 2; break; }
            g16(rdram, t1, 0);
            r = 0;
            if (s->rx.empty()) break;
            Datagramm d = std::move(s->rx.front());
            s->rx.pop_front();
            const uint32_t n = std::min<uint32_t>(t0 & 0xFFFF, uint32_t(d.daten.size()));
            for (uint32_t i = 0; i < n; ++i) rdram[(a3 + i) & kRamMask] = uint8_t(d.daten[i]);
            g16(rdram, a2, d.port);
            gStrSchreiben(rdram, a1, ipText(d.vip));
            g16(rdram, t1, uint16_t(n));
            static int z = 0;
            if (log && z++ < 200) std::fprintf(stderr, "[ONLINE] RecvFrom %s:%u (%u Bytes)\n", ipText(d.vip).c_str(), d.port, n);
            break;
        }
        case 0x47A810u: // PingEmpfangen(h, u8 *got, char *host, u16 *id)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1 || !a2) { r = 2; break; }
            g8(rdram, a1, 0);
            r = 0;
            if (s->prx.empty()) break;
            const PingAntwort p = s->prx.front();
            s->prx.pop_front();
            g8(rdram, a1, 1);
            gStrSchreiben(rdram, a2, ipText(p.vip));
            g16(rdram, a3, p.id);
            break;
        }
        case 0x47A8E0u: // Verfuegbar(h, u16 *out)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1) { r = 2; break; }
            uint32_t n = s->typ == 1 ? (s->rx.empty() ? 0u : uint32_t(s->rx.front().daten.size())) : uint32_t(s->srx.size());
            g16(rdram, a1, uint16_t(std::min<uint32_t>(n, 0xFFFF)));
            r = (n == 0 && s->zustand == 3) ? 0xD : 0;
            break;
        }
        case 0x47A978u: // EigeneIP(char *out)
            if (!a0) { r = 2; break; }
            gStrSchreiben(rdram, a0, ipText(T.vip));
            r = 0;
            break;
        case 0x47A9C0u: // Broadcast(char *out)
            if (!a0) { r = 2; break; }
            gStrSchreiben(rdram, a0, ipText(kBroadcast));
            r = 0;
            break;
        case 0x47AA08u: // Aufloesen(const char *name, char *out)
        {
            uint32_t ip = 0;
            if (!a0 || !a1) { r = 2; break; }
            if (!ipLesen(gStr(rdram, a0), ip)) { r = 1; break; }
            gStrSchreiben(rdram, a1, ipText(ip));
            r = 0;
            break;
        }
        case 0x479F90u: // Thunk -> 0x47AAB8
        case 0x47AAB8u: // Gegenstelle(h, char *out)
        {
            std::lock_guard<std::mutex> l(T.m);
            Sock *s = handle(a0);
            if (!s || !a1) { r = 2; break; }
            gStrSchreiben(rdram, a1, ipText(s->peerVip ? s->peerVip : T.vip));
            r = 0;
            break;
        }
        case 0x479FA8u: // Thunk -> 0x47AB68
        case 0x47AB68u: // Abfragen
            r = 0;
            break;
        default:
            std::fprintf(stderr, "[ONLINE] unbekannter Backend-Einstieg 0x%06X\n", pc);
            r = 1;
            break;
        }
        if (log && pc != 0x47A708u && pc != 0x47A658u && pc != 0x47A810u && pc != 0x47A8E0u && pc != 0x47A4A8u && pc != 0x47AB68u && pc != 0x479FA8u)
            std::fprintf(stderr, "[ONLINE] Backend %06X (%08X %08X %08X %08X %08X %08X) -> %d\n", pc, a0, a1, a2, a3, t0, t1, r);
#else
        (void)rdram; (void)a0; (void)a1; (void)a2; (void)a3; (void)t0; (void)t1;
        r = 5;
#endif
        ret(ctx, r);
        leave(ctx);
    }
}
TS2_DEFINE(sub_0044B000_0x44b000, ts2netz::ts2_LoadModule)
TS2_DEFINE(sub_0044B020_0x44b020, ts2netz::ts2_LoadStartModule)
TS2_DEFINE(sub_00482150_0x482150, ts2netz::ts2_ezNetCnf_A)
TS2_DEFINE(sub_00482358_0x482358, ts2netz::ts2_ezNetCnfGetCombinationList)
TS2_DEFINE(sub_00482518_0x482518, ts2netz::ts2_ret0)
TS2_DEFINE(sub_004825E0_0x4825e0, ts2netz::ts2_ret0)
TS2_DEFINE(sub_004826B8_0x4826b8, ts2netz::ts2_ezNetCtl_Init)
TS2_DEFINE(sub_00482800_0x482800, ts2netz::ts2_ret0)
TS2_DEFINE(sub_004829E8_0x4829e8, ts2netz::ts2_ezNetCtl_Status)
TS2_DEFINE(sub_00482B00_0x482b00, ts2netz::ts2_ret0)
TS2_DEFINE(sub_00482B78_0x482b78, ts2netz::ts2_ret0)
TS2_DEFINE(sub_00479F90_0x479f90, ts2netz::backend)
