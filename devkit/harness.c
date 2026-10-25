/* Minimal headless libretro frontend for automated testing.
 * usage: harness core.so rom.bin script.txt outdir
 * script lines:
 *   run N [BTN,BTN..]   run N frames holding buttons
 *   shot NAME           save last frame as NAME.ppm
 *   dump NAME ID SIZE   dump retro_get_memory_data(ID) (SIZE bytes, 0 = core size)
 *   state NAME          save serialized state
 *   load NAME           load serialized state
 * Buttons: B Y SELECT START UP DOWN LEFT RIGHT A X L R (Genesis: B=A? see core)
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "libretro.h"

static unsigned pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint8_t *frame; static unsigned fw, fh; static size_t fpitch;
static uint16_t buttons;
static const char *outdir;
static long frames_run;
static int npokes; static unsigned poke_addr[16]; static unsigned poke_val[16];
static void *(*get_mem)(unsigned);
static void apply_pokes(void){ unsigned char *ram = get_mem ? get_mem(2) : 0; if(!ram) return;
  for(int i=0;i<npokes;i++){ ram[poke_addr[i]] = poke_val[i] & 0xFF; ram[poke_addr[i]+1] = poke_val[i] >> 8; } }

static void logcb(enum retro_log_level l, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); if (l >= RETRO_LOG_WARN) vfprintf(stderr, fmt, ap); va_end(ap);
}
static bool env(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: pixfmt = *(unsigned *)data; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = outdir; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = logcb; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data; v->value = NULL;
        const char *e = getenv("HARNESS_OPTS"); /* key=value;key=value */
        if (e) {
            static char buf[4096]; strncpy(buf, e, sizeof buf - 1);
            char *save, *tok = strtok_r(buf, ";", &save);
            while (tok) { char *eq = strchr(tok, '=');
                if (eq && (size_t)(eq - tok) == strlen(v->key) && !strncmp(tok, v->key, eq - tok)) { v->value = eq + 1; return true; }
                tok = strtok_r(NULL, ";", &save); }
        }
        return false; }
    default: return false;
    }
}
static void video(const void *d, unsigned w, unsigned h, size_t pitch) {
    if (!d) return;
    fw = w; fh = h; fpitch = pitch;
    frame = realloc(frame, pitch * h);
    memcpy(frame, d, pitch * h);
}
/* audio: "audio FILE" starts appending raw s16le stereo, "audio -" stops */
static FILE *audf; static long loopframes = 60;
static void audio(int16_t l, int16_t r) { if (audf) { int16_t s[2] = { l, r }; fwrite(s, 2, 2, audf); } }
static size_t audio_batch(const int16_t *d, size_t n) { if (audf) fwrite(d, 4, n, audf); return n; }
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    if (port || dev != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return buttons;
    return (buttons >> id) & 1;
}
static void shot(const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(p, "wb"); fprintf(f, "P6 %u %u 255\n", fw, fh);
    for (unsigned y = 0; y < fh; y++) for (unsigned x = 0; x < fw; x++) {
        uint8_t rgb[3];
        if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
            uint32_t c = ((uint32_t *)(frame + y * fpitch))[x];
            rgb[0] = c >> 16; rgb[1] = c >> 8; rgb[2] = c;
        } else if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
            uint16_t c = ((uint16_t *)(frame + y * fpitch))[x];
            rgb[0] = (c >> 11) << 3; rgb[1] = ((c >> 5) & 63) << 2; rgb[2] = (c & 31) << 3;
        } else {
            uint16_t c = ((uint16_t *)(frame + y * fpitch))[x];
            rgb[0] = ((c >> 10) & 31) << 3; rgb[1] = ((c >> 5) & 31) << 3; rgb[2] = (c & 31) << 3;
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static uint16_t parse_buttons(const char *s) {
    static const char *names[] = {"B","Y","SELECT","START","UP","DOWN","LEFT","RIGHT","A","X","L","R"};
    uint16_t m = 0; char buf[256]; strncpy(buf, s, 255); buf[255] = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ","))
        for (int i = 0; i < 12; i++) if (!strcmp(t, names[i])) m |= 1 << i;
    return m;
}
#define SYM(n) n##_t n = (n##_t)dlsym(h, #n)
typedef void (*retro_init_t)(void);
typedef void (*retro_run_t)(void);
typedef bool (*retro_load_game_t)(const struct retro_game_info *);
typedef void (*retro_set_environment_t)(retro_environment_t);
typedef void (*retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void (*retro_set_audio_sample_t)(retro_audio_sample_t);
typedef void (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
typedef void (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void (*retro_set_input_state_t)(retro_input_state_t);
typedef void *(*retro_get_memory_data_t)(unsigned);
typedef size_t (*retro_get_memory_size_t)(unsigned);
typedef size_t (*retro_serialize_size_t)(void);
typedef bool (*retro_serialize_t)(void *, size_t);
typedef bool (*retro_unserialize_t)(const void *, size_t);
typedef void (*retro_set_controller_port_device_t)(unsigned, unsigned);

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage\n"); return 1; }
    outdir = argv[4];
    void *h = dlopen(argv[1], RTLD_NOW); if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    SYM(retro_init); SYM(retro_run); SYM(retro_load_game); SYM(retro_set_environment);
    SYM(retro_set_video_refresh); SYM(retro_set_audio_sample); SYM(retro_set_audio_sample_batch);
    SYM(retro_set_input_poll); SYM(retro_set_input_state); SYM(retro_get_memory_data);
    SYM(retro_get_memory_size); SYM(retro_serialize_size); SYM(retro_serialize); SYM(retro_unserialize);
    SYM(retro_set_controller_port_device);
    retro_set_environment(env); retro_set_video_refresh(video); retro_set_audio_sample(audio);
    retro_set_audio_sample_batch(audio_batch); retro_set_input_poll(poll); retro_set_input_state(input);
    retro_init();
    get_mem = retro_get_memory_data;
    FILE *rf = fopen(argv[2], "rb"); fseek(rf, 0, SEEK_END); long rs = ftell(rf); fseek(rf, 0, SEEK_SET);
    void *rd = malloc(rs); fread(rd, 1, rs, rf); fclose(rf);
    struct retro_game_info gi = { argv[2], rd, (size_t)rs, NULL };
    if (!retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
    if (retro_set_controller_port_device) retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    FILE *sf = fopen(argv[3], "r"); char line[512];
    while (fgets(line, sizeof line, sf)) {
        char cmd[32] = {0}, a1[256] = {0}, a2[64] = {0}, a3[64] = {0};
        if (sscanf(line, "%31s %255s %63s %63s", cmd, a1, a2, a3) < 1 || cmd[0] == '#') continue;
        if (!strcmp(cmd, "run")) {
            long n = atol(a1); buttons = a2[0] ? parse_buttons(a2) : 0;
            for (long i = 0; i < n; i++) { apply_pokes(); retro_run(); frames_run++; }
        } else if (!strcmp(cmd, "audio")) {
            if (audf) fclose(audf);
            audf = strcmp(a1, "-") ? fopen(a1, "wb") : NULL;
        } else if (!strcmp(cmd, "pokew")) { poke_addr[npokes] = strtoul(a1,0,0); poke_val[npokes++] = strtoul(a2,0,0); }
        else if (!strcmp(cmd, "unpoke")) npokes = 0;
        else if (!strcmp(cmd, "shot")) shot(a1);
        else if (!strcmp(cmd, "dumploop")) {
            unsigned id = strtoul(a2, 0, 0); size_t sz = strtoul(a3, 0, 0);
            char path[1024]; snprintf(path, sizeof path, "%s/%s", outdir, a1);
            FILE *f = fopen(path, "wb");
            for (long i = 0; i < loopframes; i++) { apply_pokes(); retro_run(); frames_run++;
                fwrite(retro_get_memory_data(id), 1, sz, f); }
            fclose(f);
        }
        else if (!strcmp(cmd, "loopframes")) loopframes = atol(a1);
        else if (!strcmp(cmd, "dump")) {
            unsigned id = strtoul(a2, 0, 0); size_t sz = strtoul(a3, 0, 0);
            void *p = retro_get_memory_data(id); if (!sz) sz = retro_get_memory_size(id);
            if (!p) { fprintf(stderr, "no mem %u\n", id); continue; }
            char path[1024]; snprintf(path, sizeof path, "%s/%s", outdir, a1);
            FILE *f = fopen(path, "wb"); fwrite(p, 1, sz, f); fclose(f);
        } else if (!strcmp(cmd, "loadmem")) {   /* loadmem FILE ID: file -> memory id (e.g. 0 = save RAM) */
            unsigned id = strtoul(a2, 0, 0);
            void *p = retro_get_memory_data(id); size_t sz = retro_get_memory_size(id);
            char path[1024]; snprintf(path, sizeof path, "%s/%s", outdir, a1);
            FILE *f = fopen(path, "rb");
            if (!p || !f) { fprintf(stderr, "loadmem failed\n"); if (f) fclose(f); continue; }
            size_t n = fread(p, 1, sz, f); fclose(f); fprintf(stderr, "loadmem %zu of %zu\n", n, sz);
        } else if (!strcmp(cmd, "state") || !strcmp(cmd, "load")) {
            char path[1024]; snprintf(path, sizeof path, "%s/%s", outdir, a1);
            size_t sz = retro_serialize_size(); void *buf = malloc(sz);
            if (cmd[0] == 's') { retro_serialize(buf, sz); FILE *f = fopen(path, "wb"); fwrite(buf, 1, sz, f); fclose(f); }
            else { FILE *f = fopen(path, "rb"); fread(buf, 1, sz, f); fclose(f); retro_unserialize(buf, sz); }
            free(buf);
        }
    }
    fprintf(stderr, "ran %ld frames\n", frames_run);
    return 0;
}
