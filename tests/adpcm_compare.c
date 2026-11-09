/* Decode an IMA ADPCM WAV with the mixer's decoder and write raw s16le PCM.
 * Compared against afconvert in tests/adpcm_compare.sh. */
#include <stdio.h>
#include "../runtime/src/audio/mixer.c"
void rt_log(const char *f, ...) { (void)f; }
void rt_fatal(const char *f, ...) { (void)f; abort(); }

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int v = mixer_voice_alloc();
    if (!mixer_voice_open_wav(v, argv[1])) { fprintf(stderr, "open failed\n"); return 1; }
    Voice *x = &g_v[v];
    FILE *o = fopen(argv[2], "wb");
    for (uint32_t f = 0; f < x->total_frames; f++) {
        uint32_t blk = f / x->block_samples, off = f % x->block_samples;
        if (blk != x->block_index) ima_decode_block(x, blk);
        fwrite(&x->blockbuf[off * (uint32_t)x->channels], 2, (size_t)x->channels, o);
    }
    fclose(o);
    printf("%u frames, %d ch, %u Hz, block %u (%u samples)\n", x->total_frames, x->channels, x->src_rate,
           x->block_align, x->block_samples);
    return 0;
}
