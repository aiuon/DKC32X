/* 32X music output (sh/music.c) */
#ifndef MUSIC_H
#define MUSIC_H
#include <stdint.h>
#define MUS_RATE 22050
extern volatile uint32_t music_req, music_ack, music_ticks, music_refills, music_light;
void music_init(const uint8_t *music_blob, int8_t *echo);
/* master: a game command for the sound engine (sound effects), played from the next refill */
void music_sfx(uint32_t cmd, uint32_t param);
uint32_t music_echo_bytes(void);
#endif
