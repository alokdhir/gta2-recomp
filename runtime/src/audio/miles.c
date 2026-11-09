/* The subset of the Miles Sound System (mss32.dll) gta2.exe imports, on our
 * Core Audio mixer. Signatures follow the Miles SDK (all stdcall; argument
 * sizes match the @N decorations). The game parses its own sample banks and
 * hands over raw PCM pointers; music and speech are ADPCM WAV streams.
 *
 * Handles are small guest objects ({magic, voice id, ...}) so the game gets
 * ordinary non-NULL pointers.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/mixer.h"
#include "guest_memory.h"
#include "winapi/imports.h"
#include "winapi/win32.h"

#define H_MAGIC 0x4D53534Bu /* 'MSSK' */
#define H_SAMPLE 1u
#define H_3D 2u
#define H_STREAM 3u

/* handle layout: +0 magic, +4 kind, +8 voice, +12.. 3D state */
static uint32_t new_handle(uint32_t kind)
{
    int v = mixer_voice_alloc();
    if (v < 0) return 0;
    uint32_t h = guest_alloc(64, 1);
    MEM32(h) = H_MAGIC;
    MEM32(h + 4) = kind;
    MEM32(h + 8) = (uint32_t)v;
    return h;
}

static int voice_of(uint32_t h)
{
    if (!h || MEM32(h) != H_MAGIC) return -1;
    return (int)MEM32(h + 8);
}

static void free_handle(uint32_t h)
{
    int v = voice_of(h);
    if (v < 0) return;
    mixer_voice_free(v);
    MEM32(h) = 0;
    guest_free(h);
}

static uint32_t status_of(uint32_t h)
{
    switch (mixer_voice_state(voice_of(h))) {
    case MX_PLAYING: return 4; /* SMP_PLAYING */
    case MX_DONE: return 2;    /* SMP_DONE */
    case MX_STOPPED: return 8; /* SMP_STOPPED */
    default: return 1;         /* SMP_FREE */
    }
}

static float f32(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static float vol127(uint32_t v) { return (float)(v > 127 ? 127 : v) / 127.0f; }

/* ---- system ------------------------------------------------------------------ */

static uint32_t g_dig; /* the one digital driver handle */

static void AIL_startup(void) { RETURN_STDCALL(1, 0); }
static void AIL_shutdown(void) { mixer_shutdown(); RETURN_STDCALL(0, 0); }
static void AIL_set_preference(void) { RETURN_STDCALL(0, 2); }

/* AIL_waveOutOpen(HDIGDRIVER *drvr, LPHWAVEOUT *out, S32 device, LPWAVEFORMAT fmt) -> 0 ok */
static void AIL_waveOutOpen(void)
{
    if (!mixer_init()) {
        rt_log("miles: no audio output");
        RETURN_STDCALL(1, 4);
    }
    if (!g_dig) g_dig = guest_alloc(64, 1);
    if (ARG(0)) MEM32(ARG(0)) = g_dig;
    if (ARG(1)) MEM32(ARG(1)) = 0;
    RETURN_STDCALL(0, 4);
}

static void AIL_waveOutClose(void) { RETURN_STDCALL(0, 1); }
static void AIL_digital_handle_release(void) { mixer_pause(1); RETURN_STDCALL(1, 1); }
static void AIL_digital_handle_reacquire(void) { mixer_pause(0); RETURN_STDCALL(1, 1); }
static void AIL_set_digital_master_volume(void) { mixer_master_volume(vol127(ARG(1))); RETURN_STDCALL(0, 2); }

static void AIL_delay(void)
{
    usleep(ARG(0) * 1000000u / 60u);
    RETURN_STDCALL(0, 1);
}

static void AIL_mem_alloc_lock(void) { RETURN_STDCALL(guest_alloc(ARG(0), 0), 1); }
static void AIL_mem_free_lock(void) { guest_free(ARG(0)); RETURN_STDCALL(0, 1); }

/* ---- 2D samples ---------------------------------------------------------------- */

static void apply_pcm(uint32_t h)
{
    /* +12 format, +16 address, +20 length, +24 rate */
    int fmt = (int)MEM32(h + 12);
    MxPcm p = { MEM32(h + 16) ? GPTR(MEM32(h + 16)) : NULL, MEM32(h + 20),
                (fmt & 1) ? 16 : 8, (fmt & 2) ? 2 : 1, MEM32(h + 24) };
    mixer_voice_set_pcm(voice_of(h), &p);
}

static void AIL_allocate_sample_handle(void) { RETURN_STDCALL(new_handle(H_SAMPLE), 1); }
static void AIL_release_sample_handle(void) { free_handle(ARG(0)); RETURN_STDCALL(0, 1); }

static void AIL_init_sample(void)
{
    uint32_t h = ARG(0);
    int v = voice_of(h);
    if (v >= 0) {
        mixer_voice_stop(v);
        MEM32(h + 12) = 1; /* mono 16 */
        MEM32(h + 16) = MEM32(h + 20) = 0;
        MEM32(h + 24) = 22050;
        mixer_voice_volume(v, 1.0f);
        mixer_voice_pan(v, 0);
        mixer_voice_rate(v, 0);
        mixer_voice_loop(v, 1, 0, 0);
    }
    RETURN_STDCALL(0, 1);
}

/* AIL_set_sample_type(s, format, flags) */
static void AIL_set_sample_type(void)
{
    if (voice_of(ARG(0)) >= 0) {
        MEM32(ARG(0) + 12) = ARG(1);
        apply_pcm(ARG(0));
    }
    RETURN_STDCALL(0, 3);
}

/* AIL_set_sample_address(s, start, len) */
static void AIL_set_sample_address(void)
{
    if (voice_of(ARG(0)) >= 0) {
        MEM32(ARG(0) + 16) = ARG(1);
        MEM32(ARG(0) + 20) = ARG(2);
        apply_pcm(ARG(0));
    }
    RETURN_STDCALL(0, 3);
}

static void AIL_set_sample_playback_rate(void)
{
    if (voice_of(ARG(0)) >= 0) {
        MEM32(ARG(0) + 24) = ARG(1);
        mixer_voice_rate(voice_of(ARG(0)), ARG(1));
    }
    RETURN_STDCALL(0, 2);
}

static void AIL_set_sample_volume(void) { mixer_voice_volume(voice_of(ARG(0)), vol127(ARG(1))); RETURN_STDCALL(0, 2); }

static void AIL_set_sample_pan(void)
{
    mixer_voice_pan(voice_of(ARG(0)), ((float)(ARG(1) > 127 ? 127 : ARG(1)) - 64.0f) / 64.0f);
    RETURN_STDCALL(0, 2);
}

static void AIL_set_sample_loop_count(void) { mixer_voice_loop(voice_of(ARG(0)), (int)ARG(1), 0, 0); RETURN_STDCALL(0, 2); }

static void AIL_set_sample_loop_block(void)
{
    /* keeps the loop count; Miles treats end -1 as "end of sample" */
    int v = voice_of(ARG(0));
    if (v >= 0) mixer_voice_loop(v, 0, (int32_t)ARG(1), (int32_t)ARG(2));
    RETURN_STDCALL(0, 3);
}

static void trace_start(const char *what, uint32_t h)
{
    static int n;
    if (!getenv("GTA2_TRACE_AUDIO") || n++ >= 400) return;
    rt_log("%s %08X fmt=%u addr=%08X len=%u rate=%u at %.3f s", what, h, MEM32(h + 12), MEM32(h + 16), MEM32(h + 20),
           MEM32(h + 24), (double)mixer_out_frames() / 44100.0);
}

static void AIL_start_sample(void) { trace_start("start_sample", ARG(0)); mixer_voice_start(voice_of(ARG(0))); RETURN_STDCALL(0, 1); }
static void AIL_end_sample(void) { mixer_voice_stop(voice_of(ARG(0))); RETURN_STDCALL(0, 1); }
static void AIL_sample_status(void) { RETURN_STDCALL(status_of(ARG(0)), 1); }

/* ---- 3D providers & samples ------------------------------------------------------- */

static uint32_t g_provider_name;

/* AIL_enumerate_3D_providers(HPROENUM *next, HPROVIDER *dest, C8 **name) -> 1 while more */
static void AIL_enumerate_3D_providers(void)
{
    uint32_t next = ARG(0);
    if (!g_provider_name) g_provider_name = guest_strdup("Miles Fast 2D Positional Audio");
    if (MEM32(next) != 0) RETURN_STDCALL(0, 3);
    MEM32(next) = 1;
    MEM32(ARG(1)) = 1;
    MEM32(ARG(2)) = g_provider_name;
    RETURN_STDCALL(1, 3);
}

static void AIL_open_3D_provider(void) { RETURN_STDCALL(0, 1); } /* M3D_NOERR */
static void AIL_close_3D_provider(void) { RETURN_STDCALL(0, 1); }

static void AIL_3D_provider_attribute(void)
{
    if (ARG(2)) MEM32(ARG(2)) = 0;
    RETURN_STDCALL(0, 3);
}

static void AIL_set_3D_provider_preference(void) { RETURN_STDCALL(0, 3); }
static void AIL_allocate_3D_sample_handle(void) { RETURN_STDCALL(new_handle(H_3D), 1); }
static void AIL_release_3D_sample_handle(void) { free_handle(ARG(0)); RETURN_STDCALL(0, 1); }

/* 3D state in the handle: +28 x, +32 y, +36 z, +40 max dist, +44 min dist, +48 volume */
static void update_3d(uint32_t h)
{
    int v = voice_of(h);
    if (v < 0) return;
    float x = f32(MEM32(h + 28)), y = f32(MEM32(h + 32)), z = f32(MEM32(h + 36));
    float maxd = f32(MEM32(h + 40)), mind = f32(MEM32(h + 44)), vol = f32(MEM32(h + 48));
    float d = sqrtf(x * x + y * y + z * z), att = 1.0f;
    if (mind <= 0) mind = 1;
    if (d > mind) att = mind / d;
    if (maxd > 0 && d > maxd) att = 0;
    mixer_voice_volume(v, vol * att);
    mixer_voice_pan(v, d > 0.001f ? x / d * 0.8f : 0);
}

/* AIL_set_3D_sample_info(h, const AILSOUNDINFO *info)
 * AILSOUNDINFO: format, data_ptr, data_len, rate, bits, channels, samples, block_size, initial_ptr */
static void AIL_set_3D_sample_info(void)
{
    uint32_t h = ARG(0), info = ARG(1);
    int v = voice_of(h);
    if (v >= 0 && info) {
        uint32_t fmt = MEM32(info), bits = MEM32(info + 16), ch = MEM32(info + 20);
        if (fmt != 1) rt_log("miles: 3D sample format %u not supported", fmt);
        MxPcm p = { GPTR(MEM32(info + 4)), MEM32(info + 8), bits == 8 ? 8 : 16, ch == 2 ? 2 : 1, MEM32(info + 12) };
        mixer_voice_set_pcm(v, &p);
        if (!MEM32(h + 48)) MEM32(h + 48) = 0x3F800000u; /* volume 1.0 */
        update_3d(h);
    }
    RETURN_STDCALL(1, 2);
}

/* AIL_set_3D_position(obj, x, y, z) */
static void AIL_set_3D_position(void)
{
    uint32_t h = ARG(0);
    static int n;
    if (getenv("GTA2_TRACE_AUDIO") && n++ < 30)
        rt_log("3D position %08X: %.1f %.1f %.1f", h, f32(ARG(1)), f32(ARG(2)), f32(ARG(3)));
    if (voice_of(h) >= 0) {
        MEM32(h + 28) = ARG(1);
        MEM32(h + 32) = ARG(2);
        MEM32(h + 36) = ARG(3);
        update_3d(h);
    }
    RETURN_STDCALL(0, 4);
}

/* AIL_set_3D_sample_float_distances(h, max, min, rear_max, rear_min) */
static void AIL_set_3D_sample_float_distances(void)
{
    uint32_t h = ARG(0);
    if (voice_of(h) >= 0) {
        MEM32(h + 40) = ARG(1);
        MEM32(h + 44) = ARG(2);
        update_3d(h);
    }
    RETURN_STDCALL(0, 5);
}

static void AIL_set_3D_sample_volume(void)
{
    uint32_t h = ARG(0);
    if (voice_of(h) >= 0) {
        float v = vol127(ARG(1));
        memcpy(GPTR(h + 48), &v, 4);
        update_3d(h);
    }
    RETURN_STDCALL(0, 2);
}

static void AIL_set_3D_sample_playback_rate(void) { mixer_voice_rate(voice_of(ARG(0)), ARG(1)); RETURN_STDCALL(0, 2); }
static void AIL_set_3D_sample_loop_count(void) { mixer_voice_loop(voice_of(ARG(0)), (int)ARG(1), 0, 0); RETURN_STDCALL(0, 2); }
static void AIL_set_3D_sample_loop_block(void) { mixer_voice_loop(voice_of(ARG(0)), 0, (int32_t)ARG(1), (int32_t)ARG(2)); RETURN_STDCALL(0, 3); }
static void AIL_start_3D_sample(void) { trace_start("start_3D", ARG(0)); mixer_voice_start(voice_of(ARG(0))); RETURN_STDCALL(0, 1); }
static void AIL_end_3D_sample(void) { mixer_voice_stop(voice_of(ARG(0))); RETURN_STDCALL(0, 1); }
static void AIL_3D_sample_status(void) { RETURN_STDCALL(status_of(ARG(0)), 1); }

/* ---- streams ------------------------------------------------------------------------ */

/* AIL_open_stream(dig, filename, stream_mem) -> HSTREAM */
static void AIL_open_stream(void)
{
    const char *name = GSTR(ARG(1));
    char *path = win32_host_path(name);
    uint32_t h = new_handle(H_STREAM);
    if (getenv("GTA2_TRACE_AUDIO")) rt_log("open_stream %s", name);
    if (h && !mixer_voice_open_wav(voice_of(h), path)) {
        rt_log("miles: cannot stream %s", name);
        free_handle(h);
        h = 0;
    }
    free(path);
    RETURN_STDCALL(h, 3);
}

static void AIL_close_stream(void) { free_handle(ARG(0)); RETURN_STDCALL(0, 1); }
static void AIL_start_stream(void) { mixer_voice_start(voice_of(ARG(0))); RETURN_STDCALL(0, 1); }
static void AIL_stream_status(void) { RETURN_STDCALL(status_of(ARG(0)), 1); }

static void AIL_set_stream_volume(void)
{
    if (voice_of(ARG(0)) >= 0) MEM32(ARG(0) + 48) = ARG(1);
    mixer_voice_volume(voice_of(ARG(0)), vol127(ARG(1)));
    RETURN_STDCALL(0, 2);
}

static void AIL_stream_volume(void) { RETURN_STDCALL(voice_of(ARG(0)) >= 0 ? MEM32(ARG(0) + 48) : 0, 1); }
static void AIL_set_stream_loop_count(void) { mixer_voice_loop(voice_of(ARG(0)), (int)ARG(1), 0, 0); RETURN_STDCALL(0, 2); }
static void AIL_set_stream_playback_rate(void) { mixer_voice_rate(voice_of(ARG(0)), ARG(1)); RETURN_STDCALL(0, 2); }
static void AIL_stream_playback_rate(void) { RETURN_STDCALL(mixer_voice_get_rate(voice_of(ARG(0))), 1); }

/* AIL_stream_ms_position(s, S32 *total_ms, S32 *current_ms) */
static void AIL_stream_ms_position(void)
{
    uint32_t total = 0, cur = mixer_voice_position_ms(voice_of(ARG(0)), &total);
    if (ARG(1)) MEM32(ARG(1)) = total;
    if (ARG(2)) MEM32(ARG(2)) = cur;
    RETURN_STDCALL(0, 3);
}

static void AIL_set_stream_ms_position(void) { mixer_voice_seek_ms(voice_of(ARG(0)), ARG(1)); RETURN_STDCALL(0, 2); }

#define X(name) { "AIL_" #name, AIL_##name }
const NativeExport g_miles_exports[] = {
    X(startup), X(shutdown), X(set_preference), X(waveOutOpen), X(waveOutClose),
    X(digital_handle_release), X(digital_handle_reacquire), X(set_digital_master_volume), X(delay),
    X(mem_alloc_lock), X(mem_free_lock),
    X(allocate_sample_handle), X(release_sample_handle), X(init_sample), X(set_sample_type),
    X(set_sample_address), X(set_sample_playback_rate), X(set_sample_volume), X(set_sample_pan),
    X(set_sample_loop_count), X(set_sample_loop_block), X(start_sample), X(end_sample), X(sample_status),
    X(enumerate_3D_providers), X(open_3D_provider), X(close_3D_provider), X(3D_provider_attribute),
    X(set_3D_provider_preference), X(allocate_3D_sample_handle), X(release_3D_sample_handle),
    X(set_3D_sample_info), X(set_3D_position), X(set_3D_sample_float_distances), X(set_3D_sample_volume),
    X(set_3D_sample_playback_rate), X(set_3D_sample_loop_count), X(set_3D_sample_loop_block),
    X(start_3D_sample), X(end_3D_sample), X(3D_sample_status),
    X(open_stream), X(close_stream), X(start_stream), X(stream_status), X(set_stream_volume),
    X(stream_volume), X(set_stream_loop_count), X(set_stream_playback_rate), X(stream_playback_rate),
    X(stream_ms_position), X(set_stream_ms_position),
    { NULL, NULL },
};
