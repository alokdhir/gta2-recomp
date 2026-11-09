/* Software mixer on a Core Audio output unit. Voices play PCM from memory
 * (the game's sample banks) or stream IMA ADPCM / PCM WAV files from disk. */
#pragma once
#include <stdint.h>

typedef enum { MX_FREE = 0, MX_STOPPED, MX_PLAYING, MX_DONE } MxState;

typedef struct {
    const void *data;   /* PCM (host pointer), for memory voices */
    uint32_t bytes;
    int bits;           /* 8 (unsigned) or 16 (signed) */
    int channels;       /* 1 or 2 */
    uint32_t rate;      /* source sample rate */
} MxPcm;

int mixer_init(void);
void mixer_shutdown(void);
void mixer_pause(int paused);
void mixer_master_volume(float v);

/* Voices are small integer ids. */
int mixer_voice_alloc(void);
void mixer_voice_free(int v);
void mixer_voice_set_pcm(int v, const MxPcm *pcm);
int mixer_voice_open_wav(int v, const char *host_path); /* 0 on failure */
void mixer_voice_start(int v);
/* Output frames rendered so far (44.1 kHz), for matching traces to a dump. */
uint64_t mixer_out_frames(void);
void mixer_voice_stop(int v);
MxState mixer_voice_state(int v);
void mixer_voice_rate(int v, uint32_t hz);           /* playback rate, 0 = source rate */
uint32_t mixer_voice_get_rate(int v);
void mixer_voice_volume(int v, float vol);           /* 0..1 */
void mixer_voice_pan(int v, float pan);              /* -1 left .. 1 right */
void mixer_voice_loop(int v, int count, int32_t start_byte, int32_t end_byte); /* count 0 = forever */
uint32_t mixer_voice_position_ms(int v, uint32_t *total_ms);
void mixer_voice_seek_ms(int v, uint32_t ms);
