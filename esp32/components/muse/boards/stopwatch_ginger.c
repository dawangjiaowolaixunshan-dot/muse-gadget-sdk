// StopWatch gingerbread portraits, rendered directly from flash in display strips.
#include "muse_pixel.h"

#include <math.h>
#include <string.h>

enum { SIDE = 224, MAX_CANVAS = 512 };
enum portrait { REST, MOUTH_HALF, MOUTH_OPEN, LISTEN, TYPE_UP, TYPE_MID, TYPE_DOWN };
extern const uint16_t pictures[] asm("_binary_ginger_poses_rgb565_start");

static int canvas = 320;
static int16_t columns[MAX_CANVAS], rows[MAX_CANVAS];
static int last_size, last_y;
static enum portrait face, previous;
static bool working, dim, dirty = true;
static float switched;
static unsigned mix_step = 8;

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    static const uint32_t accents[MUSE_MODE_COUNT] = {
        [MUSE_MODE_BOOT]=0xd98a4a, [MUSE_MODE_IDLE]=0xe08a45, [MUSE_MODE_LISTENING]=0x5cb8ff,
        [MUSE_MODE_THINKING]=0xe07bff, [MUSE_MODE_SPEAKING]=0x6ff0bf,
        [MUSE_MODE_ERROR]=0xff5c5c, [MUSE_MODE_OFF]=0x7c72d0
    };
    return accents[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE];
}
uint32_t muse_pixel_working_accent(void) { return 0xffb54d; }

void muse_pixel_set_size(int px)
{
    canvas = px < 1 ? 1 : px > MAX_CANVAS ? MAX_CANVAS : px;
}

void muse_pixel_render(const muse_pose_t *p)
{
    // Live reports genuine background work explicitly; ordinary short speech gaps stay face-on.
    bool at_desk = p->mode == MUSE_MODE_THINKING && (p->working > .5f || p->mode_t >= 1.6f);
    enum portrait next = REST;
    if (at_desk) {
        static const enum portrait typing[] = {TYPE_MID, TYPE_DOWN, TYPE_MID, TYPE_UP};
        next = typing[(unsigned)(p->mode_t * 5) % 4];
    } else if (p->mode == MUSE_MODE_LISTENING) {
        next = LISTEN;
    } else if (p->mode == MUSE_MODE_SPEAKING) {
        next = p->level > .58f ? MOUTH_OPEN : p->level > .19f ? MOUTH_HALF : REST;
    }
    if (working != at_desk) {
        previous = face;
        switched = p->t;
        working = at_desk;
        mix_step = 0;
        dirty = true;
    }
    unsigned blend = (unsigned)fminf(8, fmaxf(0, (p->t - switched) * 36));
    int bob = (int)lroundf(1.6f * sinf(p->t * 1.8f) - 5 * p->happy);
    bool off = p->mode == MUSE_MODE_OFF;
    if (face != next || blend != mix_step || dim != off) dirty = true;
    face = next;
    dim = off;
    mix_step = blend;
    if (canvas != last_size || bob != last_y) {
        for (int i = 0; i < canvas; i++) {
            int x = (i - canvas / 2) * SIDE / canvas + SIDE / 2;
            int y = (i - canvas / 2 - bob) * SIDE / canvas + SIDE / 2;
            columns[i] = x >= 0 && x < SIDE ? x : -1;
            rows[i] = y >= 0 && y < SIDE ? y : -1;
        }
        last_size = canvas;
        last_y = bob;
        dirty = true;
    }
}

bool muse_pixel_whole_frame(bool *changed)
{
    *changed = dirty;
    dirty = false;
    return true;
}

static uint16_t dissolve(uint16_t a, uint16_t b)
{
    unsigned old = 8 - mix_step;
    unsigned rb = (((a & 0xf81f) * old + (b & 0xf81f) * mix_step) >> 3) & 0xf81f;
    unsigned g = (((a & 0x07e0) * old + (b & 0x07e0) * mix_step) >> 3) & 0x07e0;
    return rb | g;
}

void muse_pixel_scale(uint16_t *dst, int stride, int x0, int x1, int y0, int y1)
{
    for (int y = y0; y <= y1; y++, dst += stride) {
        int row = rows[y];
        if (row < 0) {
            memset(dst, 0, (x1 - x0 + 1) * sizeof(*dst));
            continue;
        }
        const uint16_t *now = pictures + (face * SIDE + row) * SIDE;
        const uint16_t *old = pictures + (previous * SIDE + row) * SIDE;
        for (int x = x0; x <= x1; x++) {
            int col = columns[x];
            uint16_t pixel = col < 0 ? 0 : now[col];
            if (mix_step < 8 && col >= 0) pixel = dissolve(old[col], pixel);
            dst[x-x0] = dim ? ((pixel >> 1) & 0x7bef) : pixel;
        }
    }
}
