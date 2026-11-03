# JStarsReborn execution runtime

Runs the user's own decrypted CUSA01593 dump: the prepared image (eboot + the game's bundled `libc.prx` and
`libSceFios2.prx`) executes as native x86-64 code; imports are bound to the runtime in this directory.

Build: `make -C runtime jstars-boot` (Linux) or `make -C runtime jstars-boot.exe` (Windows x64, cross-compiled; static). Run with `--boot <work>/prepared/boot-linked.bin`. Linux harness driver:

    tools/boot_smoke.sh <game-folder> <work-root> [aerolib.inl]

The script never writes to the game folder. `aerolib.inl` (shadPS4's NID table) is optional and only adds
symbol names to log lines.

## What executes what (checkpoint 0.3.0)

| Part | Source of behaviour |
|---|---|
| eboot.bin game code | **Original code, unmodified** (relocated only) |
| libc.prx, libSceFios2.prx | **Original code from the dump** (94 imports resolve into them) |
| libkernel threads/sync/time/memory/TLS (`jstars_kernel.c`, `jstars_memory.c`, `jstars_evf.c`, `jstars_fs.c`) | Native replacement for the console OS. Mutex/cond/rwlock/attr slots point at padded blocks because the game's libc writes inside them |
| SystemService/UserService/Sysmodule/Rtc (`jstars_system.c`) | Native replacement; defaults configurable by environment |
| Pad (`jstars_input.c`) | Native: scripted test input (`--pad-script`, automated), XInput, keyboard. **Not yet exercised by a human**; Mouse is a diagnostic stub |
| AudioOut (`jstars_av.c`, `jstars_audiohost.c`) | Native: stereo downmix (LFE folded in) with a stereo-linked peak limiter to WASAPI (shared mode), 20 ms port queue and 10 ms device buffer (`--audio-queue-ms`, `--audio-buffer-ms`; 85 and 30 restore 0.5.x); `--dump-audio` records what the game submitted |
| VideoOut + Gnm (GPU) | **shadPS4-derived** command processor, shader translator and Vulkan renderer (`../gpu`, GPL-2.0-or-later), run on Windows. Upstream stubs stay stubs (see `docs/IMPORT_AUDIT.md`) |
| SaveData (`jstars_savedata.c`), Trophy (`jstars_platform.c`) | Save data implemented over a per-user folder; trophies are an explicitly **empty** registered set (the trophy pack is encrypted and not read) |
| AvPlayer (`jstars_avplayer.c`) | Diagnostic stub with the game's observed protocol; **movies are not decoded or shown** |
| Np*/Net/Http/Ssl, dialogs, DiscMap, ... | Offline answers where a diagnostic stub exists; the 92 imports classed *Unsupported* trap (exit 3, named) if reached |

Counts are **unique import-table entries** (477 in this eboot); the 26,919 relocation records are the places those entries are used
and are not a count of functions. Status of each: `docs/IMPORT_AUDIT.md`.

## Stop behaviour and exit codes

Any import without an implementation is bound to a trap. When reached, the process logs
`UNIMPLEMENTED IMPORT #n NID#lib [name] called from module+offset` and exits with status 3. Faults log
registers, the faulting thread, image-relative stack return addresses and host module+offset frames and exit with status 4.
Exit codes: 3 unimplemented import, 4 fault, 5 GPU fatal, 6 GPU init failure, 7 `--max-seconds` reached, 8 stack-check failure.
`--check-imports` lists every trapped import without running the game (see `docs/IMPORT_STATUS.md`).

`--stop-at-first-gnm` is the baseline regression mode inherited from checkpoint 0.2.2: the GPU library is not started and
the run must exit 3 at `0H2vBYbTLHI#libSceGnmDriver` (`sceGnmDrawInitDefaultHardwareState200`) called from `eboot.bin+0x45e3a9`.

## Diagnostic build: heap guard

`make jstars-boot-gpu-hg.exe` links `jstars_heapguard.c` and routes every `malloc/calloc/realloc/free` of the whole program
through a guard-page allocator (each block ends at a no-access page; freed blocks are decommitted and never reused). A stray
write past a block, a use after free or a bad/double free faults at the offending instruction and the fault report names the
block and where it was allocated. It found the pthread-object overflow described in `docs/ADR-006`. It is for short
diagnostic runs only and is not part of the normal build.

Licence: GPL-2.0-or-later (adapted from HayatoG/iss_pc, itself GPL-2.0-or-later; see THIRD_PARTY_NOTICES.md).

## Verification status

| Host | Result |
|---|---|
| Linux x86-64 | Reaches the first Gnm call (`evidence/execution/`); host self-test 134 checks, 0 failed. Valgrind reports no heap error on the start-up path, but Linux's larger pthread types hid the overflow that crashed Windows (see `docs/ADR-006`), so that is not evidence about Windows |
| Windows PE build under Wine 9 | Structural checks only (CPU path to the first Gnm call). Wine is a compatibility layer, not Windows |
| Windows 11 hardware (Ryzen 7 9800X3D / RX 9070 XT) | Build w: start-up check exits 3 (0/200 failures in the loop; before the pthread-object fix build v failed 8/430 and build t 5/230), self-test 134/134, a 620 s Free Battle with scripted input ran to its time limit; see `evidence/windows-gpu-0.3.0/` |
