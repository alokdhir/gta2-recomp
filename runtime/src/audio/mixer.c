/* Software mixer driven by the default Core Audio output unit. See mixer.h. */
#include "audio/mixer.h"

#include <AudioToolbox/AudioToolbox.h>
#include <fcntl.h>
#include <math.h>
#include <os/lock.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "winapi/imports.h"

#define MAX_VOICES 96
#define OUT_RATE 44100.0

typedef struct {
    MxState state;
    int in_use;
    /* source */
    const uint8_t *data;   /* PCM, or the WAV data chunk */
    uint32_t bytes;
    int bits, channels;
    uint32_t src_rate, play_rate;
    /* WAV streaming (mapped file) */
    void *map;
    size_t map_len;
    int adpcm;
    uint32_t block_align, block_samples;
    int16_t *blockbuf;     /* decoded current block, interleaved */
    uint32_t block_index;  /* which block is in blockbuf (UINT32_MAX = none) */
    uint32_t total_frames;
    /* playback */
    double pos;            /* in source frames */
    int loops_left;        /* 0 = forever */
    uint32_t loop_start, loop_end; /* frames */
    float vol, pan;
    char name[48]; /* for traces: stream file name, or "pcm" */
} Voice;

static Voice g_v[MAX_VOICES];
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;
static AudioUnit g_unit;
static int g_running, g_paused;
static float g_master = 1.0f;
static int g_trace;
static int g_mute; /* GTA2_HEADLESS / GTA2_MUTE: mix as usual, output silence */
static uint64_t g_out_frames; /* frames rendered so far */
uint64_t mixer_out_frames(void) { return g_out_frames; }
static FILE *g_dump; /* GTA2_AUDIO_DUMP=file: raw float32 stereo 44.1 kHz of the mix (first 2 min) */

/* ---- IMA ADPCM (WAV format 0x11) ----------------------------------------- */

static const int g_ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int g_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static int16_t ima_nibble(int nib, int *pred, int *idx)
{
    int step = g_ima_step[*idx];
    int diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    if (nib & 8) diff = -diff;
    *pred += diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += g_ima_index[nib];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

/* Decode one block into v->blockbuf (block_samples frames, interleaved). */
static void ima_decode_block(Voice *v, uint32_t b)
{
    const uint8_t *p = v->data + (size_t)b * v->block_align;
    uint32_t avail = v->bytes - b * v->block_align;
    if (avail < 4u * (uint32_t)v->channels) {
        memset(v->blockbuf, 0, sizeof(int16_t) * v->block_samples * (size_t)v->channels);
        v->block_index = b;
        return;
    }
    int ch = v->channels, pred[2], idx[2];
    for (int c = 0; c < ch; c++) {
        pred[c] = (int16_t)(p[4 * c] | p[4 * c + 1] << 8);
        idx[c] = p[4 * c + 2] > 88 ? 88 : p[4 * c + 2];
        v->blockbuf[c] = (int16_t)pred[c];
    }
    const uint8_t *d = p + 4 * ch, *end = p + (avail < v->block_align ? avail : v->block_align);
    uint32_t frame = 1;
    /* Groups of 4 bytes (8 samples) per channel, channels interleaved */
    while (d + 4u * (uint32_t)ch <= end && frame < v->block_samples) {
        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < 4; i++) {
                uint8_t byte = d[4 * c + i];
                uint32_t f0 = frame + 2u * (uint32_t)i, f1 = f0 + 1;
                int16_t s0 = ima_nibble(byte & 0xF, &pred[c], &idx[c]);
                int16_t s1 = ima_nibble(byte >> 4, &pred[c], &idx[c]);
                if (f0 < v->block_samples) v->blockbuf[f0 * (uint32_t)ch + (uint32_t)c] = s0;
                if (f1 < v->block_samples) v->blockbuf[f1 * (uint32_t)ch + (uint32_t)c] = s1;
            }
        }
        d += 4 * ch;
        frame += 8;
    }
    v->block_index = b;
}

/* One source frame as floats (L, R). */
static void fetch(Voice *v, uint32_t f, float *l, float *r)
{
    float a, b;
    if (v->adpcm) {
        uint32_t blk = f / v->block_samples, off = f % v->block_samples;
        if (blk != v->block_index) ima_decode_block(v, blk);
        a = v->blockbuf[off * (uint32_t)v->channels] / 32768.0f;
        b = v->channels == 2 ? v->blockbuf[off * 2 + 1] / 32768.0f : a;
    } else if (v->bits == 16) {
        const int16_t *s = (const int16_t *)v->data;
        a = s[(size_t)f * (size_t)v->channels] / 32768.0f;
        b = v->channels == 2 ? s[(size_t)f * 2 + 1] / 32768.0f : a;
    } else {
        const uint8_t *s = v->data;
        a = (s[(size_t)f * (size_t)v->channels] - 128) / 128.0f;
        b = v->channels == 2 ? (s[(size_t)f * 2 + 1] - 128) / 128.0f : a;
    }
    *l = a;
    *r = b;
}

static uint32_t frames_of(const Voice *v)
{
    if (v->adpcm) return v->total_frames;
    uint32_t fb = (uint32_t)(v->bits / 8 * v->channels);
    return fb ? v->bytes / fb : 0;
}

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                       UInt32 bus, UInt32 nframes, AudioBufferList *out)
{
    (void)ref; (void)flags; (void)ts; (void)bus;
    float *o = out->mBuffers[0].mData;
    memset(o, 0, sizeof(float) * 2 * nframes);
    if (g_paused) return noErr;
    os_unfair_lock_lock(&g_lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &g_v[i];
        if (v->state != MX_PLAYING || !v->data) continue;
        uint32_t n = frames_of(v);
        if (!n) { v->state = MX_DONE; continue; }
        uint32_t lend = v->loop_end && v->loop_end <= n ? v->loop_end : n;
        double step = (double)(v->play_rate ? v->play_rate : v->src_rate) / OUT_RATE;
        float gl = v->vol * g_master * (v->pan > 0 ? 1.0f - v->pan : 1.0f);
        float gr = v->vol * g_master * (v->pan < 0 ? 1.0f + v->pan : 1.0f);
        for (UInt32 k = 0; k < nframes; k++) {
            if (v->pos >= lend) {
                if (v->loops_left == 1) { v->state = MX_DONE; break; }
                if (v->loops_left > 1) v->loops_left--;
                v->pos = v->loop_start + (v->pos - lend);
                if (v->pos >= lend) v->pos = v->loop_start;
            }
            uint32_t f = (uint32_t)v->pos;
            float l0, r0, l1, r1, t = (float)(v->pos - f);
            fetch(v, f, &l0, &r0);
            if (f + 1 < lend) fetch(v, f + 1, &l1, &r1); else { l1 = l0; r1 = r0; }
            o[2 * k] += (l0 + (l1 - l0) * t) * gl;
            o[2 * k + 1] += (r0 + (r1 - r0) * t) * gr;
            v->pos += step;
        }
    }
    os_unfair_lock_unlock(&g_lock);
    static double acc;
    static uint64_t frames, clipped;
    /* Limiter: when overlapping sounds (level-start voice + music + effects)
     * add up past full scale, turn the whole mix down smoothly (instant
     * attack, ~150 ms release) instead of hard-clipping it into harsh noise. */
    static float gain = 1.0f;
    const float ceiling = 0.89f;                       /* about -1 dBFS */
    const float release = 1.0f / (0.15f * (float)OUT_RATE);
    for (UInt32 k = 0; k < nframes; k++) {
        float l = o[2 * k], r = o[2 * k + 1];
        float peak = fmaxf(fabsf(l), fabsf(r));
        if (peak * gain > ceiling) gain = ceiling / peak;
        else gain = fminf(1.0f, gain + release);
        l *= gain, r *= gain;
        if (fabsf(l) > 1 || fabsf(r) > 1) clipped++;
        o[2 * k] = l > 1 ? 1 : l < -1 ? -1 : l;
        o[2 * k + 1] = r > 1 ? 1 : r < -1 ? -1 : r;
        acc += (double)o[2 * k] * o[2 * k] + (double)o[2 * k + 1] * o[2 * k + 1];
    }
    frames += nframes;
    g_out_frames += nframes;
    static uint64_t dumped;
    if (g_dump && dumped < 120 * (uint64_t)OUT_RATE) {
        fwrite(o, sizeof(float) * 2, nframes, g_dump);
        dumped += nframes;
        if (dumped >= 120 * (uint64_t)OUT_RATE) fflush(g_dump);
    }
    if (g_mute) memset(o, 0, sizeof(float) * 2 * nframes);
    if (g_trace && frames >= (uint64_t)OUT_RATE) {
        int active = 0;
        for (int i = 0; i < MAX_VOICES; i++) active += g_v[i].state == MX_PLAYING;
        rt_log("mixer: rms %.4f clipped %llu voices %d", sqrt(acc / (2.0 * frames)), (unsigned long long)clipped, active);
        for (int i = 0; i < MAX_VOICES; i++)
            if (g_v[i].state == MX_PLAYING)
                rt_log("mixer:   voice %d %s vol %.2f rate %u", i, g_v[i].name[0] ? g_v[i].name : "pcm", g_v[i].vol,
                       g_v[i].play_rate ? g_v[i].play_rate : g_v[i].src_rate);
        acc = 0; frames = 0; clipped = 0;
    }
    return noErr;
}

int mixer_init(void)
{
    if (g_running) return 1;
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                    kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &g_unit) != noErr) return 0;
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = OUT_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = f.mBytesPerPacket = 8;
    AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = { render, NULL };
    AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(g_unit) != noErr || AudioOutputUnitStart(g_unit) != noErr) return 0;
    g_running = 1;
    g_trace = getenv("GTA2_TRACE_AUDIO") != NULL;
    g_mute = getenv("GTA2_HEADLESS") != NULL || getenv("GTA2_MUTE") != NULL;
    if (getenv("GTA2_AUDIO_DUMP")) {
        g_dump = fopen(getenv("GTA2_AUDIO_DUMP"), "wb");
        if (g_dump) setvbuf(g_dump, NULL, _IONBF, 0);
    }
    rt_log("mixer: Core Audio output running at %.0f Hz", OUT_RATE);
    return 1;
}

void mixer_shutdown(void)
{
    if (!g_running) return;
    AudioOutputUnitStop(g_unit);
    AudioUnitUninitialize(g_unit);
    AudioComponentInstanceDispose(g_unit);
    g_running = 0;
}

void mixer_pause(int paused) { g_paused = paused; }
void mixer_master_volume(float v) { g_master = v; }

/* ---- voices ----------------------------------------------------------------- */

static void release_source(Voice *v)
{
    if (v->map) munmap(v->map, v->map_len);
    free(v->blockbuf);
    v->map = NULL;
    v->blockbuf = NULL;
    v->adpcm = 0;
    v->data = NULL;
}

int mixer_voice_alloc(void)
{
    os_unfair_lock_lock(&g_lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!g_v[i].in_use) {
            memset(&g_v[i], 0, sizeof g_v[i]);
            g_v[i].in_use = 1;
            g_v[i].state = MX_STOPPED;
            g_v[i].vol = 1.0f;
            g_v[i].loops_left = 1;
            os_unfair_lock_unlock(&g_lock);
            return i;
        }
    }
    os_unfair_lock_unlock(&g_lock);
    return -1;
}

#define VOICE(v) if ((v) < 0 || (v) >= MAX_VOICES || !g_v[v].in_use) return

void mixer_voice_free(int v)
{
    VOICE(v);
    os_unfair_lock_lock(&g_lock);
    release_source(&g_v[v]);
    memset(&g_v[v], 0, sizeof g_v[v]);
    os_unfair_lock_unlock(&g_lock);
}

void mixer_voice_set_pcm(int v, const MxPcm *p)
{
    VOICE(v);
    os_unfair_lock_lock(&g_lock);
    Voice *x = &g_v[v];
    release_source(x);
    x->data = p->data;
    x->bytes = p->bytes;
    x->bits = p->bits == 8 ? 8 : 16;
    x->channels = p->channels == 2 ? 2 : 1;
    x->src_rate = p->rate ? p->rate : 22050;
    x->pos = 0;
    x->loop_start = x->loop_end = 0;
    x->state = MX_STOPPED;
    os_unfair_lock_unlock(&g_lock);
}

int mixer_voice_open_wav(int v, const char *path)
{
    if (v < 0 || v >= MAX_VOICES || !g_v[v].in_use) return 0;
    const char *base = strrchr(path, '/');
    snprintf(g_v[v].name, sizeof g_v[v].name, "%s", base ? base + 1 : path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    fstat(fd, &st);
    uint8_t *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED || st.st_size < 44 || memcmp(m, "RIFF", 4) || memcmp(m + 8, "WAVE", 4)) {
        if (m != MAP_FAILED) munmap(m, (size_t)st.st_size);
        return 0;
    }
    /* walk chunks */
    const uint8_t *fmt = NULL, *data = NULL;
    uint32_t data_len = 0;
    for (size_t off = 12; off + 8 <= (size_t)st.st_size;) {
        uint32_t len = m[off + 4] | m[off + 5] << 8 | m[off + 6] << 16 | (uint32_t)m[off + 7] << 24;
        if (!memcmp(m + off, "fmt ", 4)) fmt = m + off + 8;
        if (!memcmp(m + off, "data", 4)) {
            data = m + off + 8;
            data_len = len;
            if (off + 8 + data_len > (size_t)st.st_size) data_len = (uint32_t)((size_t)st.st_size - off - 8);
        }
        off += 8 + len + (len & 1);
    }
    if (!fmt || !data) {
        munmap(m, (size_t)st.st_size);
        return 0;
    }
    uint16_t tag = (uint16_t)(fmt[0] | fmt[1] << 8), ch = (uint16_t)(fmt[2] | fmt[3] << 8);
    uint32_t rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24;
    uint16_t align = (uint16_t)(fmt[12] | fmt[13] << 8), bits = (uint16_t)(fmt[14] | fmt[15] << 8);
    if ((tag != 1 && tag != 0x11) || ch < 1 || ch > 2) {
        rt_log("mixer: %s: unsupported WAV format %u", path, tag);
        munmap(m, (size_t)st.st_size);
        return 0;
    }
    os_unfair_lock_lock(&g_lock);
    Voice *x = &g_v[v];
    release_source(x);
    x->map = m;
    x->map_len = (size_t)st.st_size;
    x->data = data;
    x->bytes = data_len;
    x->channels = ch;
    x->src_rate = rate;
    x->bits = bits == 8 ? 8 : 16;
    if (tag == 0x11) {
        x->adpcm = 1;
        x->block_align = align;
        x->block_samples = (align - 4u * ch) * 8u / (4u * ch) + 1u;
        x->blockbuf = calloc(x->block_samples * ch, sizeof(int16_t));
        x->block_index = UINT32_MAX;
        x->total_frames = data_len / align * x->block_samples;
    }
    x->pos = 0;
    x->state = MX_STOPPED;
    os_unfair_lock_unlock(&g_lock);
    return 1;
}

void mixer_voice_start(int v)
{
    VOICE(v);
    os_unfair_lock_lock(&g_lock);
    if (g_v[v].state != MX_PLAYING) {
        if (g_v[v].state == MX_DONE) g_v[v].pos = 0;
        g_v[v].state = MX_PLAYING;
    }
    os_unfair_lock_unlock(&g_lock);
}

void mixer_voice_stop(int v)
{
    VOICE(v);
    if (g_v[v].state == MX_PLAYING) g_v[v].state = MX_STOPPED;
}

MxState mixer_voice_state(int v)
{
    if (v < 0 || v >= MAX_VOICES || !g_v[v].in_use) return MX_FREE;
    return g_v[v].state;
}

void mixer_voice_rate(int v, uint32_t hz) { VOICE(v); g_v[v].play_rate = hz; }
uint32_t mixer_voice_get_rate(int v)
{
    if (v < 0 || v >= MAX_VOICES) return 0;
    return g_v[v].play_rate ? g_v[v].play_rate : g_v[v].src_rate;
}
void mixer_voice_volume(int v, float vol) { VOICE(v); g_v[v].vol = vol < 0 ? 0 : vol > 1 ? 1 : vol; }
void mixer_voice_pan(int v, float pan) { VOICE(v); g_v[v].pan = pan < -1 ? -1 : pan > 1 ? 1 : pan; }

void mixer_voice_loop(int v, int count, int32_t start_byte, int32_t end_byte)
{
    VOICE(v);
    Voice *x = &g_v[v];
    uint32_t fb = x->adpcm ? 0 : (uint32_t)(x->bits / 8 * x->channels);
    if (count >= 0) x->loops_left = count; /* count < 0: keep the current loop count */
    x->loop_start = fb && start_byte > 0 ? (uint32_t)start_byte / fb : 0;
    x->loop_end = fb && end_byte > 0 ? (uint32_t)end_byte / fb : 0;
}

uint32_t mixer_voice_position_ms(int v, uint32_t *total_ms)
{
    if (v < 0 || v >= MAX_VOICES || !g_v[v].in_use || !g_v[v].src_rate) {
        if (total_ms) *total_ms = 0;
        return 0;
    }
    Voice *x = &g_v[v];
    if (total_ms) *total_ms = (uint32_t)((uint64_t)frames_of(x) * 1000u / x->src_rate);
    return (uint32_t)(x->pos * 1000.0 / x->src_rate);
}

void mixer_voice_seek_ms(int v, uint32_t ms)
{
    VOICE(v);
    os_unfair_lock_lock(&g_lock);
    g_v[v].pos = (double)ms * g_v[v].src_rate / 1000.0;
    os_unfair_lock_unlock(&g_lock);
}
