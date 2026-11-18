// StopWatch teacher portraits follow the existing Live voice and touch states.
#include "muse_pixel.h"

#include <math.h>
#include <string.h>

enum teacher_pose { READY, LISTEN, EXPLAIN, ENCOURAGE };
extern const uint16_t portraits[] asm("_binary_teacher_poses_rgb565_start");
static enum teacher_pose portrait;
static muse_mode_t previous_mode;
static float encouraged_until;

#define S 224
#define MAP_MAX 512

typedef struct {
    uint32_t acc;
} scheme_t;

// Accents for the UI around the picture; idle takes the avatar's own chat
// theme colour (#af591a), warmed up a little for the dark screen.
static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = {0xd98a4a},
    [MUSE_MODE_IDLE]      = {0xe08a45},
    [MUSE_MODE_LISTENING] = {0x5cb8ff},
    [MUSE_MODE_THINKING]  = {0xe07bff},
    [MUSE_MODE_SPEAKING]  = {0x6ff0bf},
    [MUSE_MODE_ERROR]     = {0xff5c5c},
    [MUSE_MODE_OFF]       = {0x7c72d0},
};

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return SCHEMES[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE].acc;
}

uint32_t muse_pixel_working_accent(void)
{
    return 0xffb54d;
}

static int s_size = 320;
// Per frame: which source column / row each screen column / row shows, or
// -1 for black. Built in muse_pixel_render(), read by muse_pixel_scale().
static int16_t s_xmap[MAP_MAX], s_ymap[MAP_MAX];
static bool s_dim;
static uint32_t s_frame_key, s_drawn_key = 1;

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? (px > 0 ? px : 1) : MAP_MAX;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void muse_pixel_render(const muse_pose_t *p)
{
    const float pi2 = 6.2831853f;
    float t = p->t, mt = p->mode_t, lvl = clampf(p->level, 0, 1);
    float dx = 0, dy = 0, zoom = 1.0f;   // in fractions of the canvas
    if (previous_mode == MUSE_MODE_SPEAKING && p->mode != previous_mode)
        encouraged_until = t + 1.2f;
    previous_mode = p->mode;
    portrait = READY;
    if (p->mode == MUSE_MODE_LISTENING)
        portrait = LISTEN;
    else if (p->mode == MUSE_MODE_SPEAKING)
        portrait = EXPLAIN;
    else if (p->mode == MUSE_MODE_IDLE && (p->happy > 0.05f || t < encouraged_until))
        portrait = ENCOURAGE;
    s_dim = false;
    switch (p->mode) {
    case MUSE_MODE_BOOT:
        zoom = 0.6f + 0.4f * clampf(mt / 0.6f, 0, 1);
        break;
    case MUSE_MODE_IDLE:
        dy = 0.015f * sinf(pi2 * t / 3.2f);
        break;
    case MUSE_MODE_LISTENING:
        zoom = 1.0f + 0.05f * lvl;
        dy = -0.01f;
        break;
    case MUSE_MODE_THINKING:
        dx = 0.025f * sinf(pi2 * t / (p->working ? 0.9f : 1.6f));
        dy = 0.01f * sinf(pi2 * t / 0.8f);
        break;
    case MUSE_MODE_SPEAKING:
        dy = -0.025f * lvl + 0.008f * sinf(pi2 * t / 0.5f);
        zoom = 1.0f + 0.02f * lvl;
        break;
    case MUSE_MODE_ERROR:
        dx = 0.01f * sinf(pi2 * t * 6) * expf(-mt * 2);
        break;
    case MUSE_MODE_OFF:
        s_dim = true;
        zoom = 0.9f;
        break;
    default:
        break;
    }
    // A pet (happy) hops: up and back down in about half a second.
    float h = clampf(p->happy, 0, 1);
    dy -= 0.035f * h * fabsf(sinf(pi2 * t * 1.5f));

    // Sized and placed (tools/make_avatar's mock of the 466 px face) to
    // clear the speaker button, the mic icon, the labels and the meter, with
    // room left for the moves above.
    float base = 0.96f * zoom;
    dy += 6.0f / 320;
    int n = s_size;
    float c = (n - 1) * 0.5f;
    float k = (float)S / (n * base);   // source pixels per screen pixel
    for (int i = 0; i < n; i++) {
        float sx = ((i - c) - dx * n) * k + (S - 1) * 0.5f;
        float sy = ((i - c) - dy * n) * k + (S - 1) * 0.5f;
        s_xmap[i] = (sx < 0 || sx > S - 1) ? -1 : (int16_t)(sx + 0.5f);
        s_ymap[i] = (sy < 0 || sy > S - 1) ? -1 : (int16_t)(sy + 0.5f);
    }
    uint32_t key = 2166136261u ^ (uint32_t)s_dim ^ ((uint32_t)n << 1) ^ ((uint32_t)portrait << 16);
    for (int i = 0; i < n; i++) {
        key = (key ^ (uint16_t)s_xmap[i]) * 16777619u;
        key = (key ^ (uint16_t)s_ymap[i]) * 16777619u;
    }
    s_frame_key = key;
}

/*
 * muse_ui.c redraws only the cells whose first pixel changed, which suits
 * flat pixel art but leaves trails of a picture behind. Tell it to redraw the
 * whole canvas instead, and only on frames where the picture moved.
 */
bool muse_pixel_whole_frame(bool *changed)
{
    *changed = s_frame_key != s_drawn_key;
    s_drawn_key = s_frame_key;
    return true;
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        int sy = y < MAP_MAX ? s_ymap[y] : -1;
        if (sy < 0) {
            memset(dst, 0, n * sizeof(uint16_t));
            continue;
        }
        const uint16_t *row = &portraits[(portrait * S + sy) * S];
        for (int i = 0; i < n; i++) {
            int x = x0 + i;
            int sx = x < MAP_MAX ? s_xmap[x] : -1;
            uint16_t v = sx < 0 ? 0 : row[sx];
            dst[i] = s_dim ? (uint16_t)((v >> 1) & 0x7bef) : v;
        }
    }
}
