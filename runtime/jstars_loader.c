// JStarsReborn boot loader: maps the prepared J-Stars image (eboot + bundled libc/Fios2 modules),
// applies relocations, binds imports, runs module initializers and enters the original entry point.
//
// Imports the runtime does not implement are bound to traps that stop the process with the import's
// NID, library, symbol name and calling address. Nothing returns a fake success value.
//
// Structure follows the approach of HayatoG/iss_pc (probe.c) and Castlesea's inFAMOUS port; this file
// is written for J-Stars and carries none of their title-specific patches. GPL-2.0-or-later.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } Module;
typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value; int64_t addend; } Reloc;
typedef struct { uint64_t index, address, kind; } Binding;

static uint8_t* image;
static size_t image_size, total_size;
static uint8_t *traps, *data_traps, *thunks;
static uint64_t import_count, module_count, seg_count, page;
static char (*import_names)[128];
static char** import_symbol; // from import-names.tsv
static Module modules[8];
static Segment* segs;
static int g_stage = 0; // 0 loading, 1 module init, 2 entry
static uint64_t g_entry, g_procparam;
void* rt_procparam(void) { return image + g_procparam; }
void* rt_image(void) { return image; }
// Per-module TLS template (PT_TLS image): module id -> {initial bytes, file size, memory size}.
int rt_tls_template(uint64_t module_id, const void** init, size_t* filesz, size_t* memsz) {
    for (uint64_t m = 0; m < module_count; ++m)
        if (modules[m].tls_module == module_id) {
            *init = image + modules[m].tls_address; *filesz = modules[m].tls_filesz; *memsz = modules[m].tls_memsz;
            return 1;
        }
    return 0;
}
void rt_set_stage(int s) { g_stage = s; }

// ---- logging ---------------------------------------------------------------------------------
static FILE* g_log;
static struct timespec g_t0;
void rt_log_open(const char* path) {
    g_log = os_fopen(path, "w");
    if (g_log) setvbuf(g_log, NULL, _IOLBF, 0);
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
}
void rt_logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double ms = (double)(t.tv_sec - g_t0.tv_sec) * 1e3 + (double)(t.tv_nsec - g_t0.tv_nsec) / 1e6;
    fprintf(stderr, "[%9.3f] %s\n", ms, buf);
    if (g_log) { fprintf(g_log, "[%9.3f] %s\n", ms, buf); fflush(g_log); }   // Windows C runtime ignores _IOLBF: flush so a crash keeps the log
}
const char* rt_import_name(uint32_t i) {
    return i < import_count && import_symbol[i] && *import_symbol[i] ? import_symbol[i] : "(unnamed NID)";
}

[[noreturn]] static void fail(const char* msg) {
    rt_logf("FATAL: %s", msg);
    rt_audio_flush_dump();
    fflush(NULL);
    _exit(2);
}

const char* rt_locate(uintptr_t a, char* out, size_t n);
static const char* locate(uintptr_t a, char* out, size_t n) { return rt_locate(a, out, n); }
const char* rt_locate(uintptr_t a, char* out, size_t n) {
    if (a >= (uintptr_t)image && a < (uintptr_t)image + image_size) {
        uint64_t off = a - (uintptr_t)image;
        const char* mod = "eboot.bin";
        uint64_t base = 0;
        static const char* names[] = {"libc.prx", "libSceFios2.prx"};
        for (uint64_t m = 0; m < module_count; ++m)
            if (off >= modules[m].base && off < modules[m].base + modules[m].size) {
                mod = names[m < 2 ? m : 1];
                base = modules[m].base;
            }
        snprintf(out, n, "%s+0x%" PRIx64, mod, off - base);
    } else {
        char w[200];
        snprintf(out, n, "host/other %s", os_where(a, w, sizeof w)); // names the host DLL (or the plain pointer)
    }
    return out;
}

// ---- unresolved import traps -----------------------------------------------------------------
// The trap is reached by a tail jump, so returning from here returns to the game's caller with this value in rax.
// By default nothing returns: an unimplemented import stops the run (exit 3) at its exact call site. The
// `online-probe` experiment is the one exception, for mapping the online path in a single run (docs/EXPERIMENTS.md).
static const char* import_library(uint32_t index) {
    const char* p = strchr(import_names[index], '#');
    return p ? p + 1 : import_names[index];
}
static int is_online_library(const char* lib) {
    return !strncmp(lib, "libSceNp", 8) || !strncmp(lib, "libSceNet", 9) || !strcmp(lib, "libSceSsl") ||
           !strcmp(lib, "libSceHttp") || !strcmp(lib, "libSceRudp");
}
static uint8_t* g_probe_seen;   // one byte per import: how many times this import was already reported
static int64_t GUEST_ABI unresolved(uint32_t index, uintptr_t arg0) {
    char where[96];
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    const char* lib = import_library(index);
    if (g_probe_seen && is_online_library(lib)) {
        // Diagnosis only: report the console's "signed out" error instead of stopping, so one run records the whole
        // sequence the game tries. The answer is NOT a claim that this import behaves correctly.
        if (g_probe_seen[index] < 3) {
            g_probe_seen[index]++;
            rt_logf("ONLINE PROBE (experiment, not an implementation) %s [%s] called from %s (first arg 0x%" PRIxPTR ") -> 0x80550006%s",
                    import_names[index], rt_import_name(index), locate(ret, where, sizeof where), arg0,
                    g_probe_seen[index] == 3 ? " (further calls are not logged)" : "");
        }
        return (int64_t)(int32_t)0x80550006; // SCE_NP_ERROR_SIGNED_OUT
    }
    rt_logf("UNIMPLEMENTED IMPORT #%u %s [%s] called from %s (first arg 0x%" PRIxPTR ") stage=%s", index,
            import_names[index], rt_import_name(index), locate(ret, where, sizeof where), arg0,
            g_stage == 1 ? "module-init" : g_stage == 2 ? "entry" : "load");
    rt_audio_flush_dump();
    fflush(NULL);
    _exit(3);
}

#ifdef JSTARS_HEAPGUARD
void rt_heapguard_describe(uintptr_t addr, char* out, size_t cap);
#endif
static void fault(const OsFault* f) {
    char where[96];
    rt_logf("FAULT %s %#x (access %d) address %p at %s stage=%d",
#ifdef _WIN32
            "exception", (unsigned)f->signal,
#else
            "signal", (unsigned)f->signal,
#endif
            f->code, (void*)f->address, locate(f->rip, where, sizeof where), g_stage);
    uintptr_t fa = f->address;
#ifdef JSTARS_HEAPGUARD
    { char hg[400]; rt_heapguard_describe(fa, hg, sizeof hg); if (hg[0]) rt_logf("  HEAPGUARD: %s", hg); }
#endif
    if (data_traps && fa >= (uintptr_t)data_traps && fa < (uintptr_t)data_traps + (import_count + 1) * page) {
        uint32_t i = (uint32_t)((fa - (uintptr_t)data_traps) / page);
        rt_logf("  access to unimplemented DATA import #%u %s [%s]", i, import_names[i], rt_import_name(i));
    }
    static const char* rn[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    for (int i = 0; i < 16; ++i) rt_logf("  %-3s = 0x%016llx", rn[i], (unsigned long long)f->gpr[i]);
    rt_logf("  rip = 0x%016llx", (unsigned long long)f->rip);
    rt_logf("  faulting thread: %s", rt_current_thread_name());
    for (int i = 0; i < 24; ++i) {
        uintptr_t v = 0;
        if (os_safe_read(&v, (const uintptr_t*)f->rsp + i, sizeof v) != sizeof v) break;
        if (v >= (uintptr_t)image && v < (uintptr_t)image + image_size)
            rt_logf("  stack[%2d] -> %s", i, locate(v, where, sizeof where));
    }
    // host return addresses deeper on the stack (names the DLL and offset when the fault is inside a system or driver library)
    for (int i = 0, shown = 0; i < 128 && shown < 12; ++i) {
        uintptr_t v = 0;
        if (os_safe_read(&v, (const uintptr_t*)f->rsp + i, sizeof v) != sizeof v) break;
        if (v < 0x10000 || (v >= (uintptr_t)image && v < (uintptr_t)image + image_size)) continue;
        char w2[200]; const char* l = os_where(v, w2, sizeof w2);
        if (strchr(l, '+')) { rt_logf("  host stack[%3d] -> %s", i, l); ++shown; }
    }
    rt_audio_flush_dump();
    fflush(NULL);
    _exit(4);
}

// ---- boot file ---------------------------------------------------------------------------------
static uint64_t rd64(FILE* f) {
    uint64_t v;
    if (fread(&v, 8, 1, f) != 1) fail("truncated boot file");
    return v;
}
static int mapped(uint64_t a, uint64_t n) {
    for (uint64_t i = 0; i < seg_count; ++i)
        if (a >= segs[i].address && n <= segs[i].size && a - segs[i].address <= segs[i].size - n) return 1;
    return 0;
}
static int prot_of(uint64_t f) { return (f & 4 ? OS_PROT_R : 0) | (f & 2 ? OS_PROT_W : 0) | (f & 1 ? OS_PROT_X : 0); }

static void GUEST_ABI guest_exit(void) {
    rt_logf("guest called its exit function");
    rt_audio_flush_dump();
    fflush(NULL);
    _exit(0);
}

typedef struct { uint64_t argc; const char* argv[2]; } EntryParams;
static EntryParams g_params = {1, {"/app0/eboot.bin", NULL}};

static int g_sample_interval;
static int g_max_seconds, g_capture_flips, g_capture_interval = 60, g_capture_max = 3;
static void* monitor_main(void* arg);
// Periodic GPU statistics, optional frame capture and a wall-clock limit for evidence runs.
static void* monitor_main(void* arg) {
    (void)arg;
    uint64_t last[5] = {0}, captured = 0;
    for (int sec = 1;; ++sec) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
        uint64_t st[5];
        rt_gpu_stats(st);
        if (memcmp(st, last, sizeof st)) {
            rt_logf("GPU stats t=%ds: draws=%" PRIu64 " dispatches=%" PRIu64 " flips=%" PRIu64 " gnm_frames=%" PRIu64 " gpu_maps=%" PRIu64, sec, st[0], st[1], st[2], st[3], st[4]);
            memcpy(last, st, sizeof st);
        }
        if (g_capture_flips > 0 && st[2] >= (uint64_t)g_capture_flips && captured < g_capture_max && st[2] >= (uint64_t)captured * (uint64_t)g_capture_interval + (uint64_t)g_capture_flips) {
            rt_gpu_capture(1);
            ++captured;
            rt_logf("capture requested at flip %" PRIu64, st[2]);
        }
        rt_gpu_timing_tick(sec);
        if (g_sample_interval && sec % g_sample_interval == 0) { rt_sample_threads(); rt_thunk_report(); rt_fs_report(); }
        if (g_max_seconds && sec >= g_max_seconds) {
            rt_logf("--max-seconds %d reached; stopping (exit 7)", g_max_seconds);
            rt_audio_flush_dump();
    fflush(NULL);
            _exit(7);
        }
    }
    return NULL;
}

static void* guest_main(void* arg) {
    (void)arg;
    rt_thread_attach_main();
    const uint16_t cw = 0x037F; // the console's x87 control word (64-bit precision)
    __asm__ volatile("fldcw %0" : : "m"(cw));
    typedef int(GUEST_ABI * ModInit)(uint64_t, void*, void*);
    g_stage = 1;
    for (uint64_t m = 0; m < module_count; ++m) {
        rt_logf("module %" PRIu64 " initializer at image+0x%" PRIx64, m, modules[m].init);
        int r = ((ModInit)(image + modules[m].init))(0, NULL, NULL);
        rt_logf("module %" PRIu64 " initializer returned %d", m, r);
        if (r) fail("module initializer failed");
    }
    g_stage = 2;
    rt_logf("entering original x86-64 code at image+0x%" PRIx64, g_entry);
    typedef void(GUEST_ABI * Entry)(void*, void (*)(void));
    ((Entry)(image + g_entry))(&g_params, (void (*)(void))guest_exit);
    fail("entry returned");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Paths from the launcher may contain any character: take the arguments from the wide command line as UTF-8
    // (every file the runtime opens goes through the UTF-8 helpers in jstars_os.c).
    {
        int wc = 0;
        LPWSTR* wv = CommandLineToArgvW(GetCommandLineW(), &wc);
        if (wv && wc > 0) {
            char** u = calloc((size_t)wc + 1, sizeof *u);
            for (int i = 0; u && i < wc; ++i) {
                int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, NULL, 0, NULL, NULL);
                u[i] = malloc(n > 0 ? (size_t)n : 1);
                if (u[i]) { u[i][0] = 0; if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, u[i], n, NULL, NULL); }
            }
            if (u) { argc = wc; argv = u; }
            LocalFree(wv);
        }
    }
#endif
    const char *boot = NULL, *logpath = "jstars-boot.log", *gpu_dir = NULL, *capture_dir = NULL, *game_root = NULL, *user_dir = NULL, *pad_script = NULL, *experiments = NULL;
    int online = 0, trace_net = 0, trace_np = 0;
    uint16_t lobby_port = 0;
    int frame_stats = 0, flip_rate = -1, flip_rate_after = 0, ffx_probe = 0;
    const char *vblank_hz = NULL, *present_mode = NULL;
    const char* lobby_peers = NULL;
    int check_only = 0, selftest = 0, audit_dump = 0, stop_at_gnm = 0, win_w = 1280, win_h = 720;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--version")) { printf("JStarsReborn runtime %s\n", JSTARS_RUNTIME_VERSION); return 0; }
        else if (!strcmp(argv[i], "--boot") && i + 1 < argc) boot = argv[++i];
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) rt_audio_set_volume(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logpath = argv[++i];
        else if (!strcmp(argv[i], "--check-imports")) check_only = 1;
        else if (!strcmp(argv[i], "--selftest-host")) selftest = 1;
        else if (!strcmp(argv[i], "--audit-dump")) audit_dump = 1;
        // Diagnostic mode of checkpoint 0.2.2: no GPU library; the process stops (exit 3) at the first Gnm call.
        else if (!strcmp(argv[i], "--stop-at-first-gnm")) stop_at_gnm = 1;
        else if (!strcmp(argv[i], "--gpu-dir") && i + 1 < argc) gpu_dir = argv[++i];
        else if (!strcmp(argv[i], "--game-root") && i + 1 < argc) game_root = argv[++i];
        else if (!strcmp(argv[i], "--user-dir") && i + 1 < argc) user_dir = argv[++i];
        else if (!strcmp(argv[i], "--title-id") && i + 1 < argc) rt_set_title_id(argv[++i]);
        else if (!strcmp(argv[i], "--capture-dir") && i + 1 < argc) capture_dir = argv[++i];
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &win_w, &win_h);
        else if (!strcmp(argv[i], "--max-seconds") && i + 1 < argc) g_max_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pad-script") && i + 1 < argc) pad_script = argv[++i];
        else if (!strcmp(argv[i], "--sample-threads") && i + 1 < argc) g_sample_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-keyboard")) rt_input_set_keyboard(0);
        else if (!strcmp(argv[i], "--no-audio")) rt_audio_set_device_enabled(0);
        else if (!strcmp(argv[i], "--dump-audio") && i + 1 < argc) rt_audio_set_dump(argv[++i]);
        else if (!strcmp(argv[i], "--audio-queue-ms") && i + 1 < argc) rt_audio_set_latency(atoi(argv[++i]), 0);
        else if (!strcmp(argv[i], "--audio-buffer-ms") && i + 1 < argc) rt_audio_set_latency(0, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--pad-none")) rt_input_set_none(1);
        else if (!strcmp(argv[i], "--count-imports")) rt_thunk_set_counting(1);
        else if (!strcmp(argv[i], "--experiment") && i + 1 < argc) experiments = argv[++i];
        else if (!strcmp(argv[i], "--online")) online = 1;
        else if (!strcmp(argv[i], "--frame-stats")) frame_stats = 1;
        else if (!strcmp(argv[i], "--ffx-probe")) ffx_probe = 1;
        else if (!strcmp(argv[i], "--flip-rate") && i + 1 < argc) flip_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flip-rate-after") && i + 1 < argc) flip_rate_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vblank-hz") && i + 1 < argc) vblank_hz = argv[++i];
        else if (!strcmp(argv[i], "--present-mode") && i + 1 < argc) present_mode = argv[++i];
        else if (!strcmp(argv[i], "--lobby-port") && i + 1 < argc) lobby_port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lobby-peer") && i + 1 < argc) lobby_peers = argv[++i];
        else if (!strcmp(argv[i], "--trace-net")) trace_net = 1;
        else if (!strcmp(argv[i], "--trace-np")) trace_np = 1;
        else if (!strcmp(argv[i], "--trace-draws") && i + 1 < argc) { ++i; os_setenv("JGPU_TRACE_DRAWS", argv[i]); }
        else if (!strcmp(argv[i], "--capture-flips") && i + 1 < argc) g_capture_flips = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture-interval") && i + 1 < argc) g_capture_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture-max") && i + 1 < argc) g_capture_max = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s --boot boot-linked.bin [--log file] [--check-imports] [--stop-at-first-gnm] [--selftest-host] [--audit-dump]\n"
                               "       [--game-root dir] [--user-dir dir] [--gpu-dir dir] [--capture-dir dir] [--window WxH] [--max-seconds n] [--capture-flips n]\n"
                               "       [--capture-interval flips] [--capture-max n] [--pad-script file] [--no-keyboard] [--no-audio] [--volume 0-100] [--dump-audio prefix] [--pad-none] [--sample-threads seconds] [--count-imports] [--trace-draws first,count] [--experiment names] [--frame-stats] [--ffx-probe] [--flip-rate n] [--flip-rate-after s] [--vblank-hz n] [--present-mode Fifo|Mailbox|Immediate] [--online] [--lobby-port n] [--lobby-peer ip[,ip]] [--trace-net] [--trace-np]\n", argv[0]); return 1; }
    }
    if (audit_dump) { extern void rt_audit_dump(void*); rt_audit_dump(stdout); return 0; }
    if (selftest) { rt_log_open(logpath); extern int rt_selftest_host(const char*); char sd[4096]; snprintf(sd, sizeof sd, "%s.selftest", logpath); return rt_selftest_host(sd); }
    if (!boot) { fprintf(stderr, "--boot is required\n"); return 1; }
    rt_log_open(logpath);
    rt_logf("JStarsReborn runtime %s", JSTARS_RUNTIME_VERSION);
    if (pad_script) rt_input_load_script(pad_script);
    if (experiments) rt_set_experiments(experiments);
    rt_net_configure(online, trace_net);
    rt_np_configure(trace_np);
    rt_rooms_configure(lobby_port, lobby_peers);
    rt_gpu_timing_configure(frame_stats, flip_rate, flip_rate_after);
    // The renderer reads these once at start (gpu/shim/core/emulator_settings.h). Experiments only; the defaults are unchanged.
    if (vblank_hz) { static char e1[64]; snprintf(e1, sizeof e1, "JSTARS_VBLANK_HZ=%s", vblank_hz); putenv(e1); rt_logf("experiment: vblank %s Hz", vblank_hz); }
    if (present_mode) { static char e2[64]; snprintf(e2, sizeof e2, "JSTARS_PRESENT_MODE=%s", present_mode); putenv(e2); rt_logf("experiment: present mode %s", present_mode); }
    if (flip_rate >= 0) rt_logf("experiment: --flip-rate %d replaces the flip rate the game asks for%s", flip_rate, flip_rate_after > 0 ? " (from --flip-rate-after seconds on)" : "");
    if (online) rt_logf("--online: the network layer is active. The game gets real UDP sockets on this host; "
                        "no PlayStation Network service is contacted and no account is used.");
    {
        char udir[4096];
        if (!user_dir) { snprintf(udir, sizeof udir, "%s.user", logpath); user_dir = udir; }
        rt_fs_configure(game_root, user_dir);
    }
    // Guest-visible memory is placed inside the console's 40-bit user space (see jstars_os.c): GPU shader/descriptor
    // addresses are 40-bit and the SDK code requires the high half of a shader address to be zero. This must happen
    // before the first guest allocation; it applies to every mode.
    os_va_window(0x800000000ull, 0x10000000000ull);
    setvbuf(stdout, NULL, _IONBF, 0);
    page = 16384; // the console page size: segment alignment in the boot file
    if (os_page_size() > page) fail("host page size exceeds the guest page size");
    rt_logf("jstars-boot: boot file %s", boot);

    FILE* f = os_fopen(boot, "rb");
    if (!f) fail("cannot open boot file");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "JSTBOOT5", 8)) fail("bad boot file signature (run tools/prepare)");
    uint64_t size = rd64(f), entry = rd64(f), ns = rd64(f), nr = rd64(f);
    import_count = rd64(f);
    (void)rd64(f); // flags
    g_procparam = rd64(f);
    for (int i = 0; i < 4; ++i) (void)rd64(f); // eboot TLS (empty for this title)
    module_count = rd64(f);
    if (!size || size > (512u << 20) || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000 ||
        !module_count || module_count > 8) fail("boot file limits exceeded");
    for (uint64_t m = 0; m < module_count; ++m) {
        Module* x = &modules[m];
        x->base = rd64(f); x->size = rd64(f); x->init = rd64(f); x->tls_address = rd64(f);
        x->tls_memsz = rd64(f); x->tls_filesz = rd64(f); x->tls_module = rd64(f);
    }
    uint64_t nb = rd64(f);
    if (nb > import_count) fail("bad binding count");
    Binding* bind = calloc(nb ? nb : 1, sizeof *bind);
    uint64_t* bound = calloc(import_count + 1, sizeof *bound);
    uint8_t* bound_kind = calloc(import_count + 1, 1);
    for (uint64_t i = 0; i < nb; ++i) {
        bind[i].index = rd64(f); bind[i].address = rd64(f); bind[i].kind = rd64(f);
        if (bind[i].index >= import_count) fail("bad binding index");
        bound[bind[i].index] = bind[i].address; bound_kind[bind[i].index] = (uint8_t)bind[i].kind;
    }
    segs = calloc(ns, sizeof *segs);
    seg_count = ns;
    for (uint64_t i = 0; i < ns; ++i) {
        segs[i].address = rd64(f); segs[i].size = rd64(f); segs[i].flags = rd64(f);
        if (segs[i].address > size || segs[i].size > size - segs[i].address || segs[i].address % page || segs[i].flags > 7)
            fail("bad segment");
    }
    import_names = calloc(import_count + 1, 128);
    if (fread(import_names, 128, import_count, f) != import_count) fail("truncated import names");
    Reloc* relocs = calloc(nr ? nr : 1, sizeof *relocs);
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = rd64(f); relocs[i].kind = rd64(f); relocs[i].value = rd64(f); relocs[i].addend = (int64_t)rd64(f);
        if (!mapped(relocs[i].target, 8) || relocs[i].kind > 2 || (relocs[i].kind && relocs[i].value >= import_count))
            fail("bad relocation");
    }
    // Symbol names (diagnostics only) from import-names.tsv beside the boot file.
    import_symbol = calloc(import_count + 1, sizeof *import_symbol);
    {
        char p[4096];
        snprintf(p, sizeof p, "%s", boot);
        char* slash = strrchr(p, '/');
        char* bslash = strrchr(p, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        snprintf(slash ? slash + 1 : p, sizeof p - (size_t)(slash ? slash + 1 - p : 0), "import-names.tsv");
        FILE* t = os_fopen(p, "r");
        if (t) {
            char line[512];
            uint64_t i = 0;
            while (i < import_count && fgets(line, sizeof line, t)) {
                char* tab = strchr(line, '\t');
                if (!tab) { ++i; continue; }
                *tab = 0;
                char* nl = strchr(tab + 1, '\n');
                if (nl) *nl = 0;
                if (strcmp(line, import_names[i]) == 0) import_symbol[i] = strdup(tab + 1);
                ++i;
            }
            fclose(t);
        } else {
            rt_logf("note: %s not found; imports will be reported by NID only", p);
        }
    }

    // Reserve image + import trap pages in one low-address mapping.
    image_size = (size + page - 1) & ~(page - 1);
    size_t trap_bytes = (import_count * 32 + page) & ~(page - 1);
    size_t data_bytes = (import_count + 1) * page;
    size_t thunk_bytes = (rt_thunk_bytes(import_count) + page - 1) & ~(page - 1);
    total_size = image_size + trap_bytes + data_bytes + thunk_bytes;
    void* want = (void*)0x200000000ull;
    void* mem = os_map_anon(want, total_size, OS_PROT_R | OS_PROT_W, OS_MAP_FIXED_NOREPLACE);
    if (!mem) mem = os_map_anon(NULL, total_size, OS_PROT_R | OS_PROT_W, OS_MAP_ANYWHERE);
    if (!mem) fail("cannot reserve guest image");
    image = mem;
    traps = image + image_size;
    data_traps = traps + trap_bytes;
    thunks = data_traps + data_bytes;
    rt_thunks_init(thunks, import_count);
    if (fread(image, 1, size, f) != size) fail("truncated image");
    fclose(f);
    rt_logf("image mapped at %p (%" PRIu64 " bytes, %" PRIu64 " segments, %" PRIu64 " relocations, %" PRIu64 " imports)",
            (void*)image, size, ns, nr, import_count);

    if (rt_experiment("online-probe")) g_probe_seen = calloc(import_count ? import_count : 1, 1);
    for (uint64_t i = 0; i < import_count; ++i) {
        uint8_t* t = traps + i * 32;
        uintptr_t h = (uintptr_t)unresolved;
        uint32_t idx = (uint32_t)i;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe;       // mov rsi, rdi
        t[3] = 0xbf; memcpy(t + 4, &idx, 4);          // mov edi, index
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &h, 8); // movabs rax, handler
        t[18] = 0xff; t[19] = 0xe0;                   // jmp rax
    }

    // The GPU library must be running before imports are bound: its exports are registered at start-up.
    if (!stop_at_gnm && !check_only) {
        char gdir[4096], cdir[4096];
        if (!gpu_dir) { snprintf(gdir, sizeof gdir, "%s.gpu", logpath); gpu_dir = gdir; }
        if (!capture_dir) { snprintf(cdir, sizeof cdir, "%s.captures", logpath); capture_dir = cdir; }
        if (rt_gpu_start("J-Stars Victory VS+", gpu_dir, capture_dir, win_w, win_h)) {
            rt_logf("GPU library did not start; run with --stop-at-first-gnm for the loader-only diagnostic");
            return 6;
        }
        if (ffx_probe) rt_gpu_fsr_probe();
    }

    uint64_t n_host = 0, n_module = 0, n_trap = 0;
    uint8_t* seen = calloc(import_count + 1, 1);
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t v;
        if (!relocs[i].kind) {
            v = (uintptr_t)image + relocs[i].value;
        } else {
            uint64_t k = relocs[i].value;
            int is_data = relocs[i].kind == 2;
            void* host = rt_resolve(import_names[k], import_symbol[k] ? import_symbol[k] : "", is_data);
            const char* how;
            if (host) {
                // Function imports go through a first-call tracing thunk (jstars_thunk.c); data stays direct.
                void* target = is_data ? host : rt_thunk_for((uint32_t)k, host, import_names[k], rt_resolve_name(), rt_resolve_origin());
                v = (uintptr_t)target + relocs[i].addend; how = "host";
            }
            else if (bound[k] && bound_kind[k] == relocs[i].kind) { v = (uintptr_t)image + bound[k] + relocs[i].addend; how = "module"; }
            else { v = is_data ? (uintptr_t)(data_traps + page * k + relocs[i].addend) : (uintptr_t)(traps + 32 * k); how = "trap"; }
            if (!seen[k]) {
                seen[k] = 1;
                if (how[0] == 'h') ++n_host; else if (how[0] == 'm') ++n_module; else ++n_trap;
                if (check_only && how[0] == 't') rt_logf("MISSING #%" PRIu64 " %s [%s]%s", k, import_names[k], rt_import_name((uint32_t)k), is_data ? " (data)" : "");
            }
        }
        memcpy(image + relocs[i].target, &v, 8);
    }
    rt_logf("imports bound: %" PRIu64 " host, %" PRIu64 " game modules, %" PRIu64 " unimplemented (trapped)", n_host, n_module, n_trap);
    if (check_only) return n_trap ? 1 : 0;

    if (os_protect(image, image_size, 0)) fail("protect image");
    for (uint64_t i = 0; i < ns; ++i)
        if (os_protect(image + segs[i].address, (segs[i].size + page - 1) & ~(page - 1), prot_of(segs[i].flags))) fail("protect segment");
    os_protect(traps, trap_bytes, OS_PROT_R | OS_PROT_X);
    os_protect(data_traps, data_bytes, 0);
    os_protect(thunks, thunk_bytes, OS_PROT_R | OS_PROT_W | OS_PROT_X);
    os_install_fault_handler(fault);

    if (!stop_at_gnm) {
        pthread_t mon;
        pthread_create(&mon, NULL, monitor_main, NULL);
    } else {
        rt_logf("diagnostic mode: GPU library not started (stops at the first Gnm call)");
    }

    g_entry = entry;
    // The guest main thread gets a large stack, as on the console.
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, guest_main, NULL)) fail("cannot start guest main thread");
    pthread_join(th, NULL);
    return 0;
}
