// Host audio device. Windows: WASAPI shared mode, event driven, default render endpoint, 48 kHz stereo float with the system's
// sample-rate converter (AUTOCONVERTPCM). Every console audio port owns a ring of stereo frames; the render thread sums the
// rings into the device buffer. Nothing here fabricates sound: it only forwards samples the game submitted, and the counters in
// OsAudioStats say how much was submitted, how much reached the device, and how often the device ran dry.
#include "jstars_os.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <pthread.h>

#define PORTS 8
#define RING 4096                      // ring capacity in stereo frames per port (about 85 ms at 48 kHz); the queue limit below is lower
// Latency. The game blocks in sceAudioOutOutput until its buffer is queued, so everything queued here is delay between the
// game producing a sound and hearing it. A console queues a few of the game's own buffers; the 0.5.x runtime let each port
// fill the whole ring (about 85 ms) on top of a 30 ms device buffer. Defaults are now a 20 ms port queue and a 10 ms device
// buffer request; --audio-queue-ms 85 --audio-buffer-ms 30 restores the old behaviour if a PC crackles (underruns are logged).
static unsigned g_queue_frames = 20 * 48;
static int g_device_ms = 10;
void os_audio_configure(int queue_ms, int device_ms) {
    if (queue_ms > 0) { unsigned f = (unsigned)queue_ms * 48u; g_queue_frames = f < 96 ? 96 : f > RING ? RING : f; }
    if (device_ms > 0) g_device_ms = device_ms < 3 ? 3 : device_ms > 200 ? 200 : device_ms;
}
typedef struct { int used; float buf[RING * 2]; unsigned rd, wr; unsigned count; uint64_t last_data_ms; } Port;
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out here to avoid depending on ksmedia.h's conditional GUID definitions
static const GUID k_subtype_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static Port g_port[PORTS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_space = PTHREAD_COND_INITIALIZER;
static int g_state;                     // 0 not tried, 1 running, -1 failed
static OsAudioStats g_stats;
static HANDLE g_event;
static IAudioClient* g_client; static IAudioRenderClient* g_render; static UINT32 g_bufframes;

static void* render_thread(void* unused) {
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        if (WaitForSingleObject(g_event, 200) == WAIT_FAILED) break;
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(g_client, &pad))) continue;
        UINT32 n = g_bufframes - pad;
        if (!n) continue;
        BYTE* data = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(g_render, n, &data))) continue;
        float* out = (float*)data;
        memset(out, 0, (size_t)n * 2 * sizeof(float));
        uint64_t now = GetTickCount64();
        pthread_mutex_lock(&g_mu);
        for (int p = 0; p < PORTS; ++p) {
            Port* pt = &g_port[p];
            if (!pt->used) continue;
            unsigned take = pt->count < n ? pt->count : n;
            for (unsigned i = 0; i < take; ++i) {
                unsigned r = (pt->rd + i) % RING;
                out[i * 2] += pt->buf[r * 2]; out[i * 2 + 1] += pt->buf[r * 2 + 1];
            }
            pt->rd = (pt->rd + take) % RING; pt->count -= take;
            if (take < n && now - pt->last_data_ms < 250) g_stats.underruns++; // an active port had too little queued
        }
        g_stats.frames_rendered += n;
        pthread_cond_broadcast(&g_space);
        pthread_mutex_unlock(&g_mu);
        for (UINT32 i = 0; i < n * 2; ++i) { float v = out[i]; out[i] = v > 1.f ? 1.f : v < -1.f ? -1.f : v; }
        IAudioRenderClient_ReleaseBuffer(g_render, n, 0);
    }
    return NULL;
}

int os_audio_start(void) {
    pthread_mutex_lock(&g_mu);
    if (g_state) { int r = g_state > 0 ? 0 : -1; pthread_mutex_unlock(&g_mu); return r; }
    g_state = -1;
    IMMDeviceEnumerator* en = NULL; IMMDevice* dev = NULL;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&en))) goto done;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) goto done;
    IPropertyStore* ps = NULL;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
            WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, g_stats.device, (int)sizeof g_stats.device, NULL, NULL);
        PropVariantClear(&v); IPropertyStore_Release(ps);
    }
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&g_client))) goto done;
    WAVEFORMATEXTENSIBLE wf; memset(&wf, 0, sizeof wf);
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wf.Format.nChannels = 2; wf.Format.nSamplesPerSec = 48000;
    wf.Format.wBitsPerSample = 32; wf.Format.nBlockAlign = 8; wf.Format.nAvgBytesPerSec = 48000 * 8; wf.Format.cbSize = 22;
    wf.Samples.wValidBitsPerSample = 32; wf.dwChannelMask = 0x3; /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */
    wf.SubFormat = k_subtype_float;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(IAudioClient_Initialize(g_client, AUDCLNT_SHAREMODE_SHARED, flags, (REFERENCE_TIME)g_device_ms * 10000 /* 100 ns units */, 0, &wf.Format, NULL))) goto done;
    g_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_event || FAILED(IAudioClient_SetEventHandle(g_client, g_event))) goto done;
    if (FAILED(IAudioClient_GetBufferSize(g_client, &g_bufframes))) goto done;
    if (FAILED(IAudioClient_GetService(g_client, &IID_IAudioRenderClient, (void**)&g_render))) goto done;
    { // pre-fill with silence so the first period does not underrun
        BYTE* d = NULL;
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(g_render, g_bufframes, &d))) IAudioRenderClient_ReleaseBuffer(g_render, g_bufframes, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    pthread_t th;
    if (pthread_create(&th, NULL, render_thread, NULL)) goto done;
    pthread_detach(th);
    if (FAILED(IAudioClient_Start(g_client))) goto done;
    g_state = 1;
done:
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    int ok = g_state > 0;
    pthread_mutex_unlock(&g_mu);
    return ok ? 0 : -1;
}
int os_audio_port_new(void) {
    pthread_mutex_lock(&g_mu);
    int id = -1;
    for (int i = 0; i < PORTS; ++i) if (!g_port[i].used) { memset(&g_port[i], 0, sizeof g_port[i]); g_port[i].used = 1; id = i; break; }
    pthread_mutex_unlock(&g_mu);
    return id;
}
void os_audio_port_free(int port) {
    if (port < 0 || port >= PORTS) return;
    pthread_mutex_lock(&g_mu); g_port[port].used = 0; pthread_cond_broadcast(&g_space); pthread_mutex_unlock(&g_mu);
}
int os_audio_port_write(int port, const float* stereo, unsigned frames) {
    if (port < 0 || port >= PORTS || g_state <= 0) return -1;
    unsigned done = 0;
    pthread_mutex_lock(&g_mu);
    while (done < frames) {
        Port* pt = &g_port[port];
        if (!pt->used) { pthread_mutex_unlock(&g_mu); return -1; }
        unsigned cap = g_queue_frames > 2 * frames ? g_queue_frames : 2 * frames; // never less than two of the game's buffers
        if (cap > RING) cap = RING;
        unsigned room = pt->count < cap ? cap - pt->count : 0;
        if (!room) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50 * 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_nsec -= 1000000000; ts.tv_sec++; }
            pthread_cond_timedwait(&g_space, &g_mu, &ts);
            continue;
        }
        unsigned n = frames - done < room ? frames - done : room;
        for (unsigned i = 0; i < n; ++i) {
            unsigned w = (pt->wr + i) % RING;
            pt->buf[w * 2] = stereo[(done + i) * 2]; pt->buf[w * 2 + 1] = stereo[(done + i) * 2 + 1];
        }
        pt->wr = (pt->wr + n) % RING; pt->count += n; done += n;
        pt->last_data_ms = GetTickCount64();
        g_stats.frames_submitted += n;
    }
    pthread_mutex_unlock(&g_mu);
    return (int)frames;
}
void os_audio_stats(OsAudioStats* out) {
    pthread_mutex_lock(&g_mu);
    *out = g_stats;
    out->device_buffer_frames = g_bufframes;
    out->queue_frames = g_queue_frames;
    out->ports_open = 0;
    for (int i = 0; i < PORTS; ++i) out->ports_open += g_port[i].used;
    pthread_mutex_unlock(&g_mu);
}
#else
int os_audio_start(void) { return -1; }
int os_audio_port_new(void) { return -1; }
void os_audio_port_free(int port) { (void)port; }
int os_audio_port_write(int port, const float* stereo, unsigned frames) { (void)port; (void)stereo; (void)frames; return -1; }
void os_audio_configure(int queue_ms, int device_ms) { (void)queue_ms; (void)device_ms; }
void os_audio_stats(OsAudioStats* out) { memset(out, 0, sizeof *out); }
#endif
