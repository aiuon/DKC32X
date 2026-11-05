/* Portable music player core: music pack (tools/audio/mkmusic.py) + translated DKC sound engine
 * (spceng_gen.c) + S-DSP subset mixer (spcdsp.c).  Used by the 32X (sh/music.c) and the PC test. */
#ifndef MUSPLAY_H
#define MUSPLAY_H
#include <stdint.h>
#include "spceng.h"
#include "spcdsp.h"

#define MUS_NSRC 40
#define MUS_IMG_LO 0x0600

typedef struct {
    char magic[4];
    uint16_t nsongs, nsamples;
    uint32_t off_songs, off_samples;
    uint8_t level_song[256];
    uint8_t menu_song, pad[3];
    uint8_t id_song[32];    /* DKC music id -> song (0xFF: not in the pack) */
} mus_hdr_t;

typedef struct {
    uint32_t off_img, off_ram;
    uint16_t smp[MUS_NSRC];
    uint8_t pad[8];
} mus_song_t;

typedef struct {
    spc_t spc;
    spcdsp_t dsp;
    const dsp_sample_t *dir[MUS_NSRC];
    int32_t until;          /* output frames until the next engine tick, 16.16 */
    uint32_t fpt;           /* output frames per 32 kHz sample, 16.16 */
    int playing;
    int sfx_only;           /* (I23) SOUND SFX ONLY: the music's voices are not mixed (spcdsp vmask) */
    int fm;                 /* (I24) SOUND FM: + the voices playing the song's 32X samples (drums...) */
    uint32_t spcm[2];       /* (I24) those samples (SRCN bit mask, the song record's pad 0-4) */
    uint32_t swapm[2];      /* (I24) the song's samples an FM page replaces (mp_fmguard): never mixed */
} musplay_t;

/* (I24) song record pad (tools/audio/fm/fmbuild.py): 0-4 the samples the 32X plays in SOUND FM (SRCN bits,
   LSB first), 5 bit 0 = the song has FM data, 6-7 the engine's tick grid offset (32 kHz samples, BE) */
#define MUS_FM_OK(sg) ((sg)->pad[5] & 1)
#define MUS_FM_GRID(sg) ((uint32_t)(sg)->pad[6] << 8 | (sg)->pad[7])
/* (I24) SOUND FM may have an FM page in cart window 5: the voices playing the samples it replaces (the pack
   header's pad: their start, BE u24 - tools/audio/mkmusic.py puts them last) are not mixed */
void mp_fmguard(musplay_t *m, const uint8_t *blob);
/* (I24) the engine's ticks for n output frames, nothing mixed (SOUND FM: a skipped buffer keeps the time) */
void mp_advance(musplay_t *m, int n);
/* start song 'song' of the pack at 'blob' (0xFF = silence); echo: (echo_cap + 8) * 2 bytes */
void mp_start(musplay_t *m, const uint8_t *blob, int song, int8_t *echo, uint32_t echo_cap, uint32_t rate);
/* a command the game sent to the SPC700 (port 1 = cmd, port 2 = param): sound effects (cmd < $FC)
   and the $FD / $FC settings; songs ($FE / $FF uploads) are chosen by the 32X side */
void mp_cmd(musplay_t *m, int cmd, int param);
/* render n stereo frames (see spcdsp_mix for the output format) */
void mp_render(musplay_t *m, void *out, int n);

#endif
