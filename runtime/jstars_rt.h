// JStarsReborn execution runtime: shared declarations.
// Guest code is native x86-64 System V code; every function the guest can call is declared
// GUEST_ABI so the same sources build for a Linux host (development harness, where the System V
// ABI is native) and a Windows x64 host (where the attribute selects the System V convention).
// Parts adapted from HayatoG/iss_pc and Castlesea/infamous-second-son-pc-port are GPL-2.0-or-later.
#pragma once
#define JSTARS_RUNTIME_VERSION "0.5.1"
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define GUEST_ABI __attribute__((sysv_abi))
#else
#define GUEST_ABI
#endif

typedef struct RtImport {
    const char* nid_lib; // "NID#library", canonical
    const char* name;    // symbol name from the NID table, or ""
    uint32_t index;
    int is_data;
} RtImport;

// Returns a host implementation of an import, or NULL when the runtime does not implement it.
// A NULL result makes the loader bind a trap that stops the process with a diagnostic.
void* rt_resolve(const char* nid_lib, const char* name, int is_data);

void rt_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_log_open(const char* path);
const char* rt_import_name(uint32_t index);
void* rt_procparam(void);
void* rt_image(void);
int rt_tls_template(uint64_t module_id, const void** init, size_t* filesz, size_t* memsz);
void rt_thread_attach_main(void);
const char* rt_current_thread_name(void);
int rt_spawn_guest_thread(void* (GUEST_ABI* entry)(void*), void* arg, const char* name); // detached; 0 on success
void rt_set_stage(int stage);
void rt_nid(const char* name, char out[12]); // NID of a symbol name (selftest and resolution)

// ---- GPU library hooks (runtime/jstars_gpu_host.c; no-ops when built without JSTARS_GPU) -------------
int rt_gpu_enabled(void);                          // 1 when the GPU library is linked and initialized
void* rt_gpu_resolve(const char* nid_lib);         // GPU library export for "NID#library", or NULL
void rt_gpu_mem_map(void* addr, uint64_t len);     // guest memory became mapped (GPU-visible range)
void rt_gpu_mem_unmap(void* addr, uint64_t len);
void rt_fs_configure(const char* game_root, const char* user_root); // /app0 (read-only) and the writable mounts
void rt_fs_save_mount(int slot, const char* host_dir, int read_only); // /savedataN -> host folder (NULL/"" unmounts)
const char* rt_fs_user_root(void);
void rt_fs_report(void);                                  // diagnostics: files read since the previous report
int rt_fs_host_path(const char* console_path, char* host, size_t cap); // 0 and the host path, or a console errno
void rt_set_title_id(const char* title_id);           // e.g. "CUSA01593"; names the save-data folder
int rt_fd_alloc(void);                             // kernel descriptor numbers shared by files and event queues
void rt_fd_free(int fd);
int* rt_errno_cell(void);                          // the calling thread's errno cell (what the guest's __error returns)
uint64_t rt_read_tsc(void);
uint64_t rt_tsc_frequency(void);
uint64_t rt_process_time_us(void);
int rt_compiled_sdk_version(void);                 // SDK version from the eboot's process parameters, or -1
int rt_gpu_start(const char* title, const char* data_dir, const char* capture_dir, int width, int height); // 0 on success
void rt_gpu_capture(int n);                        // request PNG captures of the next n presented frames
void rt_gpu_stats(uint64_t out[5]);
void rt_gpu_timing_configure(int frame_stats, int flip_rate_override, int flip_rate_after);
void rt_gpu_timing_tick(int sec);
void rt_gpu_fsr_probe(void);                // draws, dispatches, flips, gnm frames, GPU-visible maps

// ---- first-call tracing thunks and the import audit ---------------------------------------------------
size_t rt_thunk_bytes(size_t count);
void rt_thunks_init(void* area, size_t count);   // area: rt_thunk_bytes(count) readable/writable/executable bytes
void* rt_thunk_for(uint32_t index, void* target, const char* nid_lib, const char* name, const char* origin);
const char* rt_resolve_origin(void);             // "runtime" or "gpu": who implemented the last successful rt_resolve
const char* rt_locate(uintptr_t addr, char* out, size_t n); // "module+0xoffset" (loader)
// Audit status of an implemented import: "verified", "partial", "diagnostic-stub" ("unsupported" is a trap, not a thunk).
const char* rt_audit_status(const char* nid_lib, const char* name, const char* origin, const char** note);
const char* rt_resolve_name(void);              // symbol name of the last successful rt_resolve
const char* rt_gpu_symbol_name(const char* nid_lib);

// input sources (jstars_input.c)
int rt_input_load_script(const char* path);      // --pad-script: timed automated test input (logged as such)
void rt_input_set_none(int on);                 // --pad-none
void rt_input_set_keyboard(int enabled);        // --no-keyboard clears it
void rt_input_set_focus_hook(int (*fn)(void));  // reports whether the game window has keyboard focus (GPU builds)
void rt_sample_threads(void);                 // diagnostics: log where every guest thread is (--sample-threads)
void rt_thunk_set_counting(int on);           // --count-imports: count every import call (slower)
void rt_thunk_report(void);                     // log the imports called most since the previous report
void rt_audio_set_device_enabled(int on);    // --no-audio clears it: AudioOut then uses the timed null sink
void rt_audio_flush_dump(void);              // before process exit: finalize the --dump-audio WAV headers
void rt_audio_set_volume(int percent);       // --volume 0..100: output gain applied after the meter and the WAV dump (default 100)
void rt_audio_set_dump(const char* prefix);  // --dump-audio: per-port stereo S16 WAV of what the game submitted
void rt_audio_set_latency(int queue_ms, int device_ms); // --audio-queue-ms / --audio-buffer-ms (0 = keep the default 20 / 10)
// Network layer (jstars_net.c). Off by default: without --online the game gets the offline answers.
void rt_net_configure(int online, int trace);
int rt_net_online(void);

// LAN room directory (jstars_rooms.c; docs/ADR-007-online.md addendum)
#define ROOM_MAX_INT 8
#define ROOM_MAX_BIN 2
#define ROOM_MAX_BIN_LEN 64
typedef struct {
    uint64_t room_id;
    uint32_t world_id;
    uint16_t max_slot, cur_members;
    uint32_t flags;
    char owner[16];                 // owner's online id, not terminated when 16 characters long
    uint32_t owner_ip;              // network order, from the datagram's source (0 for a local room)
    uint16_t owner_port;            // the owner's P2P UDP port, host order (0 = unknown)
    uint8_t n_int, n_bin;
    struct { uint16_t id; uint32_t value; } ints[ROOM_MAX_INT];
    struct { uint16_t id; uint16_t len; uint8_t data[ROOM_MAX_BIN_LEN]; } bins[ROOM_MAX_BIN];
    uint64_t heard_ms;
} RoomInfo;
void rt_rooms_configure(uint16_t port, const char* peers_csv);
int rt_rooms_start(void);
void rt_rooms_report(void);
uint64_t rt_rooms_nonce(void);
uint64_t rt_rooms_new_id(void);
uint64_t rt_rooms_now_ms(void);
int rt_rooms_encode(const RoomInfo* r, uint64_t nonce, uint8_t* buf, int cap);
int rt_rooms_decode(const uint8_t* buf, int len, RoomInfo* r, uint64_t* nonce_out);
int rt_rooms_encode_query(uint64_t nonce, uint8_t* buf, int cap);
int rt_rooms_is_query(const uint8_t* buf, int len);
int rt_rooms_ingest(const uint8_t* buf, int len, uint32_t ip, uint64_t at_ms);
int rt_rooms_snapshot(RoomInfo* out, int max, uint64_t at_ms);
int rt_rooms_publish(const RoomInfo* r);
void rt_rooms_unpublish(uint64_t room_id);
void rt_net_report(void);
void rt_sleep_ms_for_selftest(int ms);
void rt_np_configure(int trace);   // --trace-np: log every call into the NP libraries with its answer
void rt_set_experiments(const char* list);   // --experiment a,b: diagnosis-only platform answers (logged loudly)
int rt_experiment(const char* name);
