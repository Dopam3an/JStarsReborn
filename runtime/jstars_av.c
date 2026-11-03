// VideoOut and AudioOut. Both keep the console-visible bookkeeping (handles, registered buffers,
// flip counters, blocking audio output paced at the device rate). Presentation and sound output are
// provided by backends: this build has none, so pixels are not shown and samples are discarded
// (AudioOut still blocks for the buffer duration, as a device would).
#define _GNU_SOURCE
#include "jstars_rt.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "jstars_os.h"

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

typedef struct { int32_t pixelFormat, tilingMode, aspectRatio; uint32_t width, height, pitchInPixel, option, reserved0; uint64_t reserved1; } VoBufferAttr;
typedef struct { uint64_t count, processTime, tsc; int64_t flipArg; uint64_t submitTsc; int32_t gcQueueNum, flipPendingNum, currentBuffer; uint32_t reserved; } VoFlipStatus;
#define VO_MAX_BUFFERS 16
static struct { int open; VoBufferAttr attr; void* buf[VO_MAX_BUFFERS]; int nbuf; VoFlipStatus flip; int rate; } vo;
static pthread_mutex_t vo_lock = PTHREAD_MUTEX_INITIALIZER;

static int GUEST_ABI v_open(int user, int bus, int index, const void* param) {
    (void)index; (void)param;
    if (bus != 0) return (int)0x80290001; // SCE_VIDEO_OUT_ERROR_INVALID_VALUE: only the main output exists
    pthread_mutex_lock(&vo_lock);
    int h = vo.open ? (int)0x80290009 /* RESOURCE_BUSY */ : (vo.open = 1);
    pthread_mutex_unlock(&vo_lock);
    rt_logf("sceVideoOutOpen(user %#x) -> %d", (unsigned)user, h);
    return h;
}
static int GUEST_ABI v_set_attr(VoBufferAttr* a, int pixel, int tiling, int aspect, uint32_t w, uint32_t h, uint32_t pitch) {
    memset(a, 0, sizeof *a);
    a->pixelFormat = pixel; a->tilingMode = tiling; a->aspectRatio = aspect; a->width = w; a->height = h; a->pitchInPixel = pitch;
    return 0;
}
static int GUEST_ABI v_register(int handle, int start, void* const* addrs, int num, const VoBufferAttr* attr) {
    if (handle != 1 || !addrs || !attr || start < 0 || num <= 0 || start + num > VO_MAX_BUFFERS) return (int)0x80290001;
    pthread_mutex_lock(&vo_lock);
    vo.attr = *attr;
    for (int i = 0; i < num; ++i) vo.buf[start + i] = addrs[i];
    if (start + num > vo.nbuf) vo.nbuf = start + num;
    pthread_mutex_unlock(&vo_lock);
    rt_logf("sceVideoOutRegisterBuffers: %d buffers from %d, %ux%u pitch %u fmt %#x tiling %d", num, start, attr->width, attr->height, attr->pitchInPixel, (unsigned)attr->pixelFormat, attr->tilingMode);
    return 0;
}
static int GUEST_ABI v_flip_rate(int handle, int rate) { if (handle != 1) return (int)0x80290001; vo.rate = rate; return 0; }
static int GUEST_ABI v_flip_status(int handle, VoFlipStatus* s) {
    if (handle != 1 || !s) return (int)0x80290001;
    pthread_mutex_lock(&vo_lock); *s = vo.flip; pthread_mutex_unlock(&vo_lock);
    return 0;
}

// AudioOut. Ports are console buffers of `len` frames in one of the console sample formats; each Output call converts the
// buffer to stereo float, hands it to the host audio device (jstars_audiohost.c) and blocks until the device has room, which
// paces the game by the real device clock. Without a device (Linux harness, --no-audio, or a failed open) the port falls back
// to a timed null sink that discards samples. The level meter logs what the game actually submitted.
typedef struct {
    int open; uint32_t len, freq, param;
    int host_port;               // port in jstars_audiohost.c, or -1 for the null sink
    unsigned channels, is_float;
    struct timespec next;
    float* stereo;               // len * 2 conversion buffer
    double sumsq; float peak, raw_peak, dm_peak; uint64_t metered_frames, total_frames, silent_buffers, limited;
    uint64_t last_report_ms;
    float lim_gain, lim_release;  // stereo-linked peak limiter: current gain and per-frame release coefficient
    FILE* wav; uint64_t wav_frames; float last_pk;   // --dump-audio: stereo S16 capture of what the game submitted on this port (starts at its first non-silent buffer)
} AoPort;
static AoPort ao[8];
static pthread_mutex_t ao_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_audio_device_wanted = 1;
void rt_audio_set_device_enabled(int on) { g_audio_device_wanted = on; }
// --dump-audio <prefix>: every port that submits a non-silent buffer writes <prefix>.portN.wav (the stereo downmix of exactly what
// the game submitted, S16 48 kHz) starting at that buffer (the log says how many frames of silence preceded it), capped at
// WAV_MAX_SECONDS per port so a long run cannot fill the results folder. The header is rewritten at every meter report.
#define WAV_MAX_SECONDS 420
static char g_dump_prefix[512];
static float g_volume = 1.0f;
void rt_audio_set_volume(int percent) { g_volume = (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f; }
void rt_audio_set_dump(const char* prefix) { snprintf(g_dump_prefix, sizeof g_dump_prefix, "%s", prefix); }
void rt_audio_set_latency(int queue_ms, int device_ms) { os_audio_configure(queue_ms, device_ms); }
static void wav_header(FILE* f, uint32_t rate, uint64_t frames) {
    uint32_t data = (uint32_t)(frames * 4), riff = 36 + data, br = rate * 4; uint16_t fmt = 1, ch = 2, ba = 4, bits = 16; uint32_t fl = 16;
    long pos = ftell(f); fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fseek(f, pos, SEEK_SET); fflush(f);
}
static void wav_write(int handle, AoPort* p) {
    if (!g_dump_prefix[0]) return;
    if (!p->wav) {
        if (p->last_pk < 1e-5f) return;   // still silent: nothing to record yet
        char path[640]; snprintf(path, sizeof path, "%s.port%d.wav", g_dump_prefix, handle);
        p->wav = os_fopen(path, "wb+");
        if (!p->wav) { rt_logf("audio port %d: cannot create %s", handle, path); g_dump_prefix[0] = 0; return; }
        wav_header(p->wav, p->freq, 0); fseek(p->wav, 44, SEEK_SET);
        rt_logf("audio port %d: first non-silent buffer after %.2f s of silence; writing the submitted audio from there to %s (stereo S16 %u Hz, first %d s)", handle, (double)(p->total_frames - p->len) / p->freq, path, p->freq, WAV_MAX_SECONDS);
    }
    if (p->wav_frames >= (uint64_t)WAV_MAX_SECONDS * p->freq) return;
    int16_t tmp[2048 * 2]; unsigned done = 0;
    while (done < p->len) {
        unsigned n = p->len - done; if (n > 2048) n = 2048;
        for (unsigned i = 0; i < n * 2; ++i) { float v = p->stereo[done * 2 + i]; v = v > 1 ? 1 : v < -1 ? -1 : v; tmp[i] = (int16_t)(v * 32767.0f); }
        fwrite(tmp, 4, n, p->wav); done += n;
    }
    p->wav_frames += p->len;
}
static void wav_finish(AoPort* p) { if (p->wav) { wav_header(p->wav, p->freq, p->wav_frames); } }
// Called right before the process exits (any exit path): makes the WAV headers match what was written. Best effort, no locks.
void rt_audio_flush_dump(void) { for (int i = 0; i < 8; ++i) if (ao[i].open && ao[i].wav) wav_finish(&ao[i]); }
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static int GUEST_ABI a_init(void) { return 0; }
static int GUEST_ABI a_open(int user, int type, int index, uint32_t len, uint32_t freq, uint32_t param) {
    (void)user; (void)index;
    if (!len || !freq) return (int)0x80260003; // SCE_AUDIO_OUT_ERROR_INVALID_SIZE / freq
    unsigned fmt = param & 0xff; // 0 S16 mono, 1 S16 stereo, 2 S16 8ch, 3 float mono, 4 float stereo, 5 float 8ch, 6 S16 8ch std, 7 float 8ch std
    if (fmt > 7) return (int)0x80260003;
    static const unsigned ch[8] = {1, 2, 8, 1, 2, 8, 8, 8};
    int h = -1;
    pthread_mutex_lock(&ao_lock);
    for (int i = 0; i < 8; ++i) if (!ao[i].open) {
        ao[i] = (AoPort){.open = 1, .len = len, .freq = freq, .param = param, .host_port = -1, .channels = ch[fmt], .is_float = fmt >= 3};
        ao[i].stereo = malloc((size_t)len * 2 * sizeof(float));
        ao[i].lim_gain = 1.0f; ao[i].lim_release = expf(-1.0f / (0.080f * (float)freq)); // 80 ms release
        h = i + 1; break;
    }
    pthread_mutex_unlock(&ao_lock);
    if (h < 0) return (int)0x80260005;
    AoPort* p = &ao[h - 1];
    const char* mode = "null sink: samples discarded";
    if (g_audio_device_wanted && freq == 48000 && os_audio_start() == 0) {
        p->host_port = os_audio_port_new();
        if (p->host_port >= 0) mode = "host audio device";
    } else if (g_audio_device_wanted && freq != 48000) mode = "null sink: the host device path only takes 48000 Hz ports";
    else if (g_audio_device_wanted) mode = "null sink: the host audio device could not be opened";
    OsAudioStats st; os_audio_stats(&st);
    rt_logf("sceAudioOutOpen(type %d, %u frames, %u Hz, format %u = %u ch %s) -> %d [%s%s%s]", type, len, freq, param, p->channels,
            p->is_float ? "float" : "s16", h, mode, p->host_port >= 0 ? ": " : "", p->host_port >= 0 ? st.device : "");
    if (p->host_port >= 0)
        rt_logf("audio latency: port queue limit %u frames (%.1f ms), device buffer %u frames (%.1f ms)", st.queue_frames, st.queue_frames / 48.0,
                st.device_buffer_frames, st.device_buffer_frames / 48.0);
    return h;
}
static void ao_convert(AoPort* p, const void* buf) {
    const unsigned n = p->len, c = p->channels;
    float* o = p->stereo;
    const float k = 0.70710678f;
    for (unsigned i = 0; i < n; ++i) {
        float f[8] = {0};
        if (p->is_float) { const float* s = (const float*)buf + (size_t)i * c; for (unsigned j = 0; j < c; ++j) f[j] = s[j]; }
        else { const int16_t* s = (const int16_t*)buf + (size_t)i * c; for (unsigned j = 0; j < c; ++j) f[j] = (float)s[j] * (1.0f / 32768.0f); }
        for (unsigned j = 0; j < c; ++j) { float a = f[j] < 0 ? -f[j] : f[j]; if (a > p->raw_peak) p->raw_peak = a; } // before the downmix: any channel
        float l, r;
        if (c == 1) { l = r = f[0]; }
        else if (c == 2) { l = f[0]; r = f[1]; }
        else { // 8 channels: front L/R, centre, LFE, then four surround channels in console order (L, R pairs)
            // LFE is folded into both sides at -6 dB; dropping it (0.5.x) removed the low end of hits and explosions.
            l = f[0] + k * f[2] + 0.5f * f[3] + k * f[4] + k * f[6];
            r = f[1] + k * f[2] + 0.5f * f[3] + k * f[5] + k * f[7];
        }
        // The 7.1 -> stereo sum regularly exceeds full scale in battle (downmix peaks around 3.2 were measured). 0.5.x bent every
        // sample above 0.9 through a tanh knee, which flattens loud passages into distortion. This is a stereo-linked peak
        // limiter instead: the gain drops at once to keep the louder side at 0.95 and recovers over about 80 ms, so the waveform
        // keeps its shape and both sides keep their balance. The pre-limiter peak and the limited sample count are still reported.
        float al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        if (al > p->dm_peak) p->dm_peak = al;
        if (ar > p->dm_peak) p->dm_peak = ar;
        const float thr = 0.95f;
        float pk = al > ar ? al : ar;
        float target = pk > thr ? thr / pk : 1.0f;
        if (target < p->lim_gain) p->lim_gain = target;
        else p->lim_gain = target - (target - p->lim_gain) * p->lim_release;
        if (p->lim_gain < 0.9999f) { l *= p->lim_gain; r *= p->lim_gain; p->limited += 2; }
        o[i * 2] = l; o[i * 2 + 1] = r;
    }
}
static void ao_meter(int handle, AoPort* p) {
    const unsigned n = p->len * 2;
    float pk = 0; double sq = 0;
    for (unsigned i = 0; i < n; ++i) { float v = p->stereo[i] < 0 ? -p->stereo[i] : p->stereo[i]; if (v > pk) pk = v; sq += (double)p->stereo[i] * p->stereo[i]; }
    p->total_frames += p->len; p->metered_frames += p->len; p->sumsq += sq; if (pk > p->peak) p->peak = pk;
    p->last_pk = pk;
    if (pk < 1e-5f) p->silent_buffers++;
    uint64_t now = mono_ms();
    if (!p->last_report_ms) p->last_report_ms = now;
    if (now - p->last_report_ms >= 5000) {
        OsAudioStats st; os_audio_stats(&st);
        rt_logf("audio port %d: last %.1f s submitted by the game: peak %.4f (any input channel %.4f; downmix before limiter %.4f, %llu samples soft-limited), rms %.5f (%llu frames total, %llu silent buffers of the last window)%s; device: %llu frames submitted, %llu rendered, %llu underruns",
                handle, (double)p->metered_frames / p->freq, (double)p->peak, (double)p->raw_peak, (double)p->dm_peak, (unsigned long long)p->limited, p->metered_frames ? sqrt(p->sumsq / (2.0 * (double)p->metered_frames)) : 0.0,
                (unsigned long long)p->total_frames, (unsigned long long)p->silent_buffers, p->host_port >= 0 ? "" : " [null sink]",
                (unsigned long long)st.frames_submitted, (unsigned long long)st.frames_rendered, (unsigned long long)st.underruns);
        p->sumsq = 0; p->peak = 0; p->raw_peak = 0; p->dm_peak = 0; p->limited = 0; p->metered_frames = 0; p->silent_buffers = 0; p->last_report_ms = now;
        wav_finish(p);
    }
}
static int GUEST_ABI a_output(int handle, const void* buf) {
    if (handle < 1 || handle > 8 || !ao[handle - 1].open) return (int)0x80260004;
    AoPort* p = &ao[handle - 1];
    if (buf) { ao_convert(p, buf); ao_meter(handle, p); wav_write(handle, p); }
    if (buf && p->host_port >= 0) {
        if (g_volume < 1.0f) for (unsigned i = 0; i < p->len * 2; ++i) p->stereo[i] *= g_volume; // player volume (after metering and the dump)
        if (os_audio_port_write(p->host_port, p->stereo, p->len) < 0) { rt_logf("audio port %d: host device write failed; switching this port to the null sink", handle); os_audio_port_free(p->host_port); p->host_port = -1; }
        else return (int)p->len;
    }
    // null sink: pace like a device of that rate
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (!p->next.tv_sec) p->next = now;
    uint64_t ns = (uint64_t)p->len * 1000000000ull / p->freq;
    p->next.tv_nsec += (long)(ns % 1000000000ull); p->next.tv_sec += (time_t)(ns / 1000000000ull);
    if (p->next.tv_nsec >= 1000000000L) { p->next.tv_nsec -= 1000000000L; p->next.tv_sec++; }
    if (buf) { // buf == NULL only waits for the device
        struct timespec n2; clock_gettime(CLOCK_MONOTONIC, &n2);
        long long d = (long long)(p->next.tv_sec - n2.tv_sec) * 1000000000ll + (p->next.tv_nsec - n2.tv_nsec);
        if (d > 0) { struct timespec s = {(time_t)(d / 1000000000ll), (long)(d % 1000000000ll)}; nanosleep(&s, NULL); }
    }
    return (int)p->len;
}
static int GUEST_ABI a_close(int handle) {
    if (handle < 1 || handle > 8) return (int)0x80260004;
    AoPort* p = &ao[handle - 1];
    if (p->host_port >= 0) os_audio_port_free(p->host_port);
    wav_finish(p); if (p->wav) { fclose(p->wav); p->wav = NULL; }
    p->open = 0; free(p->stereo); p->stereo = NULL;
    return 0;
}

const HostSymbol av_symbols[] = {
    {"sceVideoOutOpen", "libSceVideoOut", (void*)v_open, 0},
    {"sceVideoOutSetBufferAttribute", "libSceVideoOut", (void*)v_set_attr, 0},
    {"sceVideoOutRegisterBuffers", "libSceVideoOut", (void*)v_register, 0},
    {"sceVideoOutSetFlipRate", "libSceVideoOut", (void*)v_flip_rate, 0},
    {"sceVideoOutGetFlipStatus", "libSceVideoOut", (void*)v_flip_status, 0},
    {"sceAudioOutInit", "libSceAudioOut", (void*)a_init, 0},
    {"sceAudioOutOpen", "libSceAudioOut", (void*)a_open, 0},
    {"sceAudioOutOutput", "libSceAudioOut", (void*)a_output, 0},
    {"sceAudioOutClose", "libSceAudioOut", (void*)a_close, 0},
};
const size_t av_symbol_count = sizeof av_symbols / sizeof *av_symbols;
