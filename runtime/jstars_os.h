// Host OS abstraction: the only place that differs between the Linux development harness and the
// Windows x64 host. Everything above it (loader, kernel/memory/system services) is shared.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define OS_PROT_R 1
#define OS_PROT_W 2
#define OS_PROT_X 4
enum { OS_MAP_ANYWHERE = 0, OS_MAP_FIXED = 1, OS_MAP_FIXED_NOREPLACE = 2 };

size_t os_page_size(void);            // host page size
void os_va_window(uint64_t lo, uint64_t hi); // place non-FIXED guest allocations inside [lo, hi) (0,0 = anywhere); see jstars_os.c
void* os_map_anon(void* hint, size_t len, int prot, int mode); // NULL on failure
int os_unmap(void* p, size_t len);    // 0 on success; Windows can only release whole reservations or views
int os_protect(void* p, size_t len, int prot); // 0 on success
// Direct memory: one shared backing store addressed by physical offset, mapped many times (aliasing).
// Offsets and (on Windows) addresses must be multiples of os_dm_granularity().
int os_dm_init(uint64_t size);
size_t os_dm_granularity(void);
void* os_dm_map(void* hint, size_t len, int prot, uint64_t phys, int mode);
void* os_alloc_aligned(size_t align, size_t size);
void os_free_aligned(void* p);

typedef struct {
    int signal;            // POSIX signal number, or Windows exception code
    int code;              // si_code / access type (0 read, 1 write, 8 execute on Windows)
    uintptr_t address;     // faulting data address (0 if none)
    uintptr_t rip, rsp;
    uint64_t gpr[16];      // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    void* native;          // host exception context: EXCEPTION_POINTERS* (Windows) / ucontext_t* (Linux)
} OsFault;
// The callback must not return (the loader logs and exits).
void os_install_fault_handler(void (*cb)(const OsFault*));
// Runs before the fatal callback for memory-access faults. Returning non-zero means the fault was handled
// (page-tracking fault of the GPU library) and the faulting instruction is retried.
void os_set_fault_prehook(int (*hook)(const OsFault*));
// Copies up to len bytes; stops at the first unreadable page. Returns the bytes copied.
size_t os_safe_read(void* dst, const void* src, size_t len);

// ---- files (paths are UTF-8; '/' separators are accepted on both hosts) ---------------------------------
// Status codes are host-neutral; the console file layer (jstars_fs.c) maps them to console errno values.
enum { OS_OK = 0, OS_E_NOENT, OS_E_ACCES, OS_E_EXIST, OS_E_NOTDIR, OS_E_ISDIR, OS_E_INVAL, OS_E_NOSPC, OS_E_NOTEMPTY, OS_E_IO, OS_E_MFILE, OS_E_BUSY, OS_E_NAMETOOLONG };
#define OS_F_READ 1
#define OS_F_WRITE 2
#define OS_F_CREATE 4
#define OS_F_TRUNC 8
#define OS_F_EXCL 16
typedef struct { uint64_t size; int is_dir; int64_t atime_ns, mtime_ns, ctime_ns; } OsStat;
void* os_file_open(const char* path, int flags, int* err);                 // NULL on failure (err set)
int64_t os_file_pread(void* f, void* buf, size_t n, uint64_t off, int* err);  // bytes read, or -1
int64_t os_file_pwrite(void* f, const void* buf, size_t n, uint64_t off, int* err);
int os_file_size(void* f, uint64_t* size);
int os_file_truncate(void* f, uint64_t size);
int os_file_sync(void* f);
void os_file_close(void* f);
int os_stat(const char* path, OsStat* st);                                  // OS_OK or OS_E_*
int os_mkdir(const char* path);
#include <stdio.h>
FILE* os_fopen(const char* utf8_path, const char* mode); // fopen with a UTF-8 path (Windows: _wfopen); NULL on failure
int os_unlink(const char* path);
int os_rmdir(const char* path);
int os_rename(const char* from, const char* to);
void* os_dir_open(const char* path, int* err);
int os_dir_next(void* d, char* name, size_t cap, int* is_dir);              // 1 = entry, 0 = end
void os_dir_close(void* d);

// ---- controller sources -----------------------------------------------------------------------------------
// jstars_input.c builds the console pad state from one of these host sources. Buttons use the console layout.
enum {
    PADBTN_L3 = 0x2, PADBTN_R3 = 0x4, PADBTN_OPTIONS = 0x8, PADBTN_UP = 0x10, PADBTN_RIGHT = 0x20, PADBTN_DOWN = 0x40,
    PADBTN_LEFT = 0x80, PADBTN_L2 = 0x100, PADBTN_R2 = 0x200, PADBTN_L1 = 0x400, PADBTN_R1 = 0x800,
    PADBTN_TRIANGLE = 0x1000, PADBTN_CIRCLE = 0x2000, PADBTN_CROSS = 0x4000, PADBTN_SQUARE = 0x8000, PADBTN_TOUCHPAD = 0x100000
};
typedef struct { uint32_t buttons; uint8_t lx, ly, rx, ry, l2, r2; } OsPadState; // sticks 0..255 (128 = centre), triggers 0..255
enum { OS_PAD_NONE = 0, OS_PAD_XINPUT = 1, OS_PAD_KEYBOARD = 2 };
// Fills `out` from the first connected XInput controller, else (allow_keyboard and window_focused) from the keyboard.
// Returns the source used, or OS_PAD_NONE (out is neutral). Linux builds have no controller source yet.
int os_pad_poll(OsPadState* out, int allow_keyboard, int window_focused);
void os_pad_rumble(int large, int small); // 0..255 motor speeds on the XInput controller in use (no-op without one)

// ---- thread sampling (diagnostics: where is each guest thread right now) -----------------------------------------
typedef struct { uintptr_t rip, rsp, rbp; int nwords; uintptr_t words[256]; } OsThreadSample;
// Briefly suspends the thread (host pthread_t value), reads its registers and the top of its stack, resumes it.
// Nothing is allocated or logged while it is suspended. Returns 0 on success; Linux builds return -1 (not provided).
int os_thread_sample(uintptr_t host_thread, OsThreadSample* out);
const char* os_where(uintptr_t addr, char* out, size_t n); // "module+0xRVA" for a host module, else the pointer
void os_setenv(const char* name, const char* value); // environment of this process (read by the GPU library)

// ---- audio output device (Windows: WASAPI shared mode on the default render endpoint; Linux harness: none) -----------------
// Ports carry stereo float frames at 48000 Hz (the caller converts/downmixes the console format first). A render thread owned by
// this layer mixes all ports into the device; os_audio_port_write blocks while the port's queue is full, so the caller is paced
// by the real device clock. Linux builds have no device: os_audio_start fails and callers fall back to a timed null sink.
typedef struct {
    char device[160];            // friendly name of the endpoint, or "" when none is open
    uint64_t frames_submitted;   // stereo frames accepted from all ports
    uint64_t frames_rendered;    // frames the device callback has mixed and handed to the device
    uint64_t underruns;          // device callbacks that needed more frames from an active port than it had queued
    unsigned device_buffer_frames; // buffer size the device granted (frames)
    unsigned queue_frames;       // per-port queue limit (frames)
    int ports_open;
} OsAudioStats;
void os_audio_configure(int queue_ms, int device_ms); // before os_audio_start: per-port queue limit and requested device buffer, ms (0 = keep)
int os_audio_start(void);                    // opens the default render endpoint (idempotent); 0 on success
int os_audio_port_new(void);                 // port index (0..7) or -1
void os_audio_port_free(int port);
int os_audio_port_write(int port, const float* stereo, unsigned frames); // blocks until all frames are queued; frames, or -1 on failure
void os_audio_stats(OsAudioStats* out);
