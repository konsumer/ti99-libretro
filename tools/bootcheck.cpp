// Drives the libretro core's own entry points: loads the console ROMs, runs N
// frames and writes the last frame as a PPM. TI99_TYPE can type a script into
// the emulated keyboard (so a BASIC program can be entered and run) and the
// peak audio level is reported, which is how sound output is checked.
//
//   TI99_SYS=<dir>           directory with 994aROM.bin / 994aGROM.bin
//   TI99_PPM=<file>          where to write the last frame
//   TI99_TYPE=<script>       keys to type; '@' = pad A, '#' = pause one slot
//   TI99_DEBUG=1             per-frame VDP mode / registers on stderr
#include <libretro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t fb[256 * 192];
static unsigned w, h, frames;
static long   peak_audio;
static unsigned g_frameno;

// TI99_SAVETEST=1 serialises at frame 300, again at 400 (the state must have
// changed), then reloads the first state at 500 and re-serialises: the two
// images have to match byte for byte.
static int      g_save_test;
static size_t   g_state_size;
static uint8_t *g_state_a, *g_state_b, *g_state_c;
static uint64_t g_hash_at_320, g_hash_after_load;

static uint64_t fb_hash(void)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned i = 0; i < w * h; i++) {
        hash ^= fb[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// --- typing --------------------------------------------------------------

#define TYPE_HOLD 4                       // frames each key is held / released

struct sHeld { unsigned dev, id; };
static sHeld    g_held[8];
static int      g_nheld;
static const char *g_type = "";
static int      g_pos;
static int      g_phase;

static void release_all(void) { g_nheld = 0; }

static void hold_key(unsigned dev, unsigned id)
{
    if (g_nheld < (int)(sizeof(g_held) / sizeof(g_held[0]))) {
        g_held[g_nheld].dev = dev;
        g_held[g_nheld].id = id;
        g_nheld++;
    }
}

static void hold_char(char c)
{
    unsigned sh = RETROK_LSHIFT;
    release_all();
    if (c >= 'a' && c <= 'z')      hold_key(RETRO_DEVICE_KEYBOARD, RETROK_a + (c - 'a'));
    else if (c >= 'A' && c <= 'Z') hold_key(RETRO_DEVICE_KEYBOARD, RETROK_a + (c - 'A'));
    else if (c >= '0' && c <= '9') hold_key(RETRO_DEVICE_KEYBOARD, RETROK_0 + (c - '0'));
    else switch (c) {
        case '\n': hold_key(RETRO_DEVICE_KEYBOARD, RETROK_RETURN); break;
        case ' ':  hold_key(RETRO_DEVICE_KEYBOARD, RETROK_SPACE); break;
        case ',':  hold_key(RETRO_DEVICE_KEYBOARD, RETROK_COMMA); break;
        case '.':  hold_key(RETRO_DEVICE_KEYBOARD, RETROK_PERIOD); break;
        case ';':  hold_key(RETRO_DEVICE_KEYBOARD, RETROK_SEMICOLON); break;
        case '=':  hold_key(RETRO_DEVICE_KEYBOARD, RETROK_EQUALS); break;
        case '/':  hold_key(RETRO_DEVICE_KEYBOARD, RETROK_SLASH); break;
        case '(':  hold_key(RETRO_DEVICE_KEYBOARD, sh); hold_key(RETRO_DEVICE_KEYBOARD, RETROK_9); break;
        case ')':  hold_key(RETRO_DEVICE_KEYBOARD, sh); hold_key(RETRO_DEVICE_KEYBOARD, RETROK_0); break;
        case '*':  hold_key(RETRO_DEVICE_KEYBOARD, sh); hold_key(RETRO_DEVICE_KEYBOARD, RETROK_8); break;
        case '+':  hold_key(RETRO_DEVICE_KEYBOARD, sh); hold_key(RETRO_DEVICE_KEYBOARD, RETROK_EQUALS); break;
        case '-':  hold_key(RETRO_DEVICE_KEYBOARD, sh); hold_key(RETRO_DEVICE_KEYBOARD, RETROK_SLASH); break;
        case '"':  hold_key(RETRO_DEVICE_KEYBOARD, RETROK_LALT); hold_key(RETRO_DEVICE_KEYBOARD, RETROK_p); break;
        case '@':  hold_key(RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_A); break;
        default:   break;                 // '#': pause, anything else: no-op
    }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    w = width; h = height; frames++;
    if (data) {
        for (unsigned y = 0; y < height; y++)
            memcpy(&fb[y * width], (const uint8_t *)data + y * pitch, width * 2);
    }

    // Type one slot per frame: hold for TYPE_HOLD frames, release, next char.
    if (g_type[g_pos]) {
        char c = g_type[g_pos];
        if (g_phase < TYPE_HOLD)      hold_char(c);
        else                          release_all();
        if (++g_phase >= 2 * TYPE_HOLD) { g_phase = 0; g_pos++; }
    } else {
        release_all();
    }
    g_frameno++;
    return;
}

static size_t audio_cb(const int16_t *d, size_t n)
{
    for (size_t i = 0; i < n * 2; i++) {
        long v = d[i] < 0 ? -d[i] : d[i];
        if (v > peak_audio) peak_audio = v;
    }
    return n;
}
static void sample_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static void poll_cb(void) {}
static int16_t input_cb(unsigned p, unsigned d, unsigned i, unsigned id)
{
    (void)p; (void)i;
    for (int k = 0; k < g_nheld; k++) {
        if (g_held[k].dev == d && g_held[k].id == id) return 1;
    }
    return 0;
}
static bool env_cb(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
        *(const char **)data = getenv("TI99_SYS") ? getenv("TI99_SYS") : ".";
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
    if (cmd == RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES) {
        *(uint64_t *)data |= (uint64_t)1 << RETRO_DEVICE_KEYBOARD;
        return true;
    }
    (void)data;
    return false;
}

int main(int argc, char **argv)
{
    unsigned run = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 300;
    if (getenv("TI99_TYPE")) g_type = getenv("TI99_TYPE");

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(sample_cb);
    retro_set_audio_sample_batch(audio_cb);
    retro_set_input_poll(poll_cb);
    retro_set_input_state(input_cb);
    retro_init();

    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = argc > 1 ? argv[1] : "";

    if (!retro_load_game(&info)) { printf("load_game failed\n"); return 1; }

    g_save_test = getenv("TI99_SAVETEST") != NULL;
    if (g_save_test) {
        g_state_size = retro_serialize_size();
        g_state_a = (uint8_t *)malloc(g_state_size);
        g_state_b = (uint8_t *)malloc(g_state_size);
        g_state_c = (uint8_t *)malloc(g_state_size);
    }

    unsigned ok_a = 0, ok_b = 0, ok_load = 0, ok_c = 0;
    bool changed = false, roundtrip = false, long_replay = false;

    for (unsigned i = 0; i < run; i++) {
        retro_run();
        if (!g_save_test) continue;
        unsigned fr = i + 1;
        if (fr == 300) {
            ok_a = retro_serialize(g_state_a, g_state_size);
        } else if (fr == 320) {
            g_hash_at_320 = fb_hash();
        } else if (fr == 400) {
            retro_serialize(g_state_c, g_state_size);
            changed = memcmp(g_state_a, g_state_c, g_state_size) != 0;
        } else if (fr == 700) {
            // Reference state for the long replay, still a normal frame.
            ok_b = retro_serialize(g_state_b, g_state_size);
        } else if (fr == 800) {
            // Reload the frame-300 state: from here frame (800 + k) replays
            // frame (300 + k).
            ok_load = retro_unserialize(g_state_a, g_state_size);
            ok_c = retro_serialize(g_state_c, g_state_size);
            roundtrip = ok_load && ok_c && memcmp(g_state_a, g_state_c, g_state_size) == 0;
        } else if (fr == 820) {
            // Replays frame 320.
            g_hash_after_load = fb_hash();
        } else if (fr == 1200) {
            // Replays frame 700, so it must equal the reference byte for byte.
            retro_serialize(g_state_c, g_state_size);
            long_replay = memcmp(g_state_b, g_state_c, g_state_size) == 0;
            if (!long_replay) {
                size_t d = 0;
                while (d < g_state_size && g_state_b[d] == g_state_c[d]) d++;
                printf("  first difference at state offset %zu (ref %02x, replay %02x)\n",
                       d, g_state_b[d], g_state_c[d]);
            }
        }
    }

    if (g_save_test) {
        printf("save-state: size %zu, serialize %u/%u, load %u, reserialize %u, "
               "state changed %s, round-trip %s, replay %s, long replay %s\n",
               g_state_size, ok_a, ok_b, ok_load, ok_c,
               changed ? "yes" : "NO", roundtrip ? "exact" : "MISMATCH",
               (g_hash_after_load && g_hash_after_load == g_hash_at_320) ? "matches" : "DIFFERS",
               long_replay ? "matches" : "DIFFERS");
    }

    unsigned nonblack = 0, colours = 0;
    bool seen[32] = {0};
    for (unsigned i = 0; i < w * h; i++) {
        uint16_t p = fb[i];
        unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        if (r || g || b) nonblack++;
        unsigned idx = (r * 3 + g * 5 + b * 7) & 31;
        if (!seen[idx]) { seen[idx] = true; colours++; }
    }
    printf("%ux%u after %u frames: nonblack %u/%u (~%u colours), audio peak %ld\n",
           w, h, frames, nonblack, w * h, colours, peak_audio);

    FILE *f = fopen(getenv("TI99_PPM") ? getenv("TI99_PPM") : "/tmp/ti99.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (unsigned i = 0; i < w * h; i++) {
            uint16_t p = fb[i];
            unsigned char rgb[3] = {
                (unsigned char)((((p >> 11) & 0x1F) * 255) / 31),
                (unsigned char)((((p >> 5) & 0x3F) * 255) / 63),
                (unsigned char)(((p & 0x1F) * 255) / 31) };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    retro_unload_game();
    return 0;
}
