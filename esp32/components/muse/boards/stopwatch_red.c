// The StopWatch Live avatar. Audio and input stay owned by the voice task;
// this renderer just follows the pose sampled by the existing LVGL timer.
#include "muse_pixel.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "miniz.h"
#include "stopwatch_red_data.h"

#define PIXELS (RED_W * RED_H)
#define MAX_CANVAS 512
#define RED_FPS 12
#define BLEND_S 0.16f
#define IDLE_GAP_S 16.0f

_Static_assert(PIXELS % sizeof(uint32_t) == 0, "word-aligned frames");

extern const uint8_t red_asset[] asm("_binary_stopwatch_red_zlib_start");

typedef struct {
    tinfl_decompressor inflate;
    uint32_t block[RED_GROUP][PIXELS / sizeof(uint32_t)];
    uint8_t shown[PIXELS];
    uint8_t old[PIXELS];
} decoder_t;

static const char *TAG = "stopwatch.avatar";
static const char *const CLIP_NAME[RED_COUNT] = {
    "rest", "wake-eyes", "wake-nod", "idle-hair", "idle-shift",
    "listen-ear", "listen-interrupt", "think-chin", "think-idea",
    "answer-palm", "answer-point", "touch-smile", "touch-kiss",
    "bye-slow", "bye-quick",
};
static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = 0xe08a45,
    [MUSE_MODE_IDLE] = 0xe08a45,
    [MUSE_MODE_LISTENING] = 0x5cb8ff,
    [MUSE_MODE_THINKING] = 0xe07bff,
    [MUSE_MODE_SPEAKING] = 0x6ff0bf,
    [MUSE_MODE_ERROR] = 0xff5c5c,
    [MUSE_MODE_OFF] = 0x7c72d0,
};

static decoder_t *s_decoder;
static bool s_init, s_failed, s_blending, s_dirty, s_prev_call, s_prev_ending;
static uint8_t s_clip = RED_REST, s_choices[RED_COUNT], s_blend_weight = 16;
static uint8_t s_lips, s_lip_x, s_lip_y;
static muse_mode_t s_prev_mode = MUSE_MODE_COUNT;
static int s_last_block = -1, s_prefix, s_frame = -1;
static float s_started, s_next_idle;
static int16_t s_xmap[MAX_CANVAS], s_ymap[MAX_CANVAS];

static unsigned s_total_frames, s_clip_frames, s_dropped, s_decode_max_us;

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return mode < MUSE_MODE_COUNT ? ACCENT[mode] : ACCENT[MUSE_MODE_IDLE];
}

uint32_t muse_pixel_working_accent(void)
{
    return 0xffb54d;
}

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static int16_t source_pos(int px, int canvas, int start, int span, int src)
{
    int pos = px * 320 / canvas - start;
    return pos < 0 || pos >= span ? -1 : pos * src / span;
}

void muse_pixel_set_size(int px)
{
    px = min_int(px, MAX_CANVAS);
    for (int i = 0; i < px; ++i) {
        s_xmap[i] = source_pos(i, px, RED_X, RED_DRAW_W, RED_W);
        s_ymap[i] = source_pos(i, px, RED_Y, RED_DRAW_H, RED_H);
    }
    for (int i = px; i < MAX_CANVAS; ++i) {
        s_xmap[i] = s_ymap[i] = -1;
    }
    s_dirty = true;
}

static bool decode(unsigned index)
{
    const red_clip_t *clip = &red_clips[s_clip];
    int head = index / RED_GROUP;
    int block = clip->block + head;
    if (block != s_last_block) {
        size_t expected = min_int(clip->count - head * RED_GROUP, RED_GROUP) * PIXELS;
        size_t input = red_offsets[block + 1] - red_offsets[block];
        size_t output = expected;
        uint8_t *dst = (uint8_t *)s_decoder->block;
        int64_t start = esp_timer_get_time();
        tinfl_init(&s_decoder->inflate);
        tinfl_status status = tinfl_decompress(&s_decoder->inflate, red_asset + red_offsets[block], &input,
            dst, dst, &output, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        unsigned elapsed = (unsigned)(esp_timer_get_time() - start);
        if (elapsed > s_decode_max_us) s_decode_max_us = elapsed;
        if (status != TINFL_STATUS_DONE || output != expected) {
            ESP_LOGE(TAG, "bad block for %s", CLIP_NAME[s_clip]);
            return false;
        }
        s_last_block = block;
        s_prefix = 0;
    }
    int slot = index % RED_GROUP;
    while (s_prefix < slot) {
        uint32_t *base = s_decoder->block[s_prefix];
        uint32_t *delta = s_decoder->block[++s_prefix];
        for (size_t n = 0; n < PIXELS / sizeof(uint32_t); ++n) {
            delta[n] ^= base[n];
        }
    }
    memcpy(s_decoder->shown, s_decoder->block[slot], PIXELS);
    uint16_t mouth = red_mouth[clip->frame + index];
    s_lip_x = mouth & 0xff;
    s_lip_y = mouth >> 8;
    return true;
}

static unsigned alternate(unsigned first)
{
    return first + (s_choices[first]++ % 2);
}

static void select_clip(unsigned clip, float now)
{
    bool had_frame = s_frame >= 0;
    if (had_frame) {
        memcpy(s_decoder->old, s_decoder->shown, PIXELS);
        ESP_LOGI(TAG, "%s -> %s, frames=%u skip=%u decode_max_us=%u",
                 CLIP_NAME[s_clip], CLIP_NAME[clip], s_clip_frames, s_dropped, s_decode_max_us);
    } else {
        ESP_LOGI(TAG, "start %s", CLIP_NAME[clip]);
    }
    s_blending = had_frame;
    s_blend_weight = had_frame ? 0 : 16;
    s_clip = clip;
    s_started = now;
    s_frame = -1;
    s_clip_frames = s_dropped = 0;
    s_next_idle = now + IDLE_GAP_S;
}

static bool leaving(void)
{
    return s_clip == RED_BYE_SLOW || s_clip == RED_BYE_QUICK;
}

void muse_stopwatch_red_screen(bool asleep)
{
    ESP_LOGI(TAG, "screen=%s frames_total=%u decode_max_us=%u",
             asleep ? "off" : "on", s_total_frames, s_decode_max_us);
}

void muse_pixel_render(const muse_pose_t *p)
{
    if (!s_init) {
        s_init = true;
        s_decoder = heap_caps_calloc(1, sizeof(*s_decoder), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_decoder) {
            ESP_LOGE(TAG, "no PSRAM for avatar");
            s_failed = true;
        }
    }
    if (s_failed) return;

    bool entered = p->mode != s_prev_mode || p->woke || p->in_call != s_prev_call;
    bool talk_cut = s_prev_mode == MUSE_MODE_SPEAKING && p->mode == MUSE_MODE_LISTENING && p->in_call;
    float age = p->t - s_started;
    bool complete = age * RED_FPS >= red_clips[s_clip].count;
    unsigned clip = s_clip;

    if (p->ending) {
        if (!s_prev_ending) clip = alternate(RED_BYE_SLOW);
    } else if (entered && p->mode == MUSE_MODE_LISTENING) {
        clip = talk_cut ? RED_LISTEN_INTERRUPT : RED_LISTEN_EAR;
    } else if (entered && p->mode == MUSE_MODE_THINKING) {
        clip = alternate(RED_THINK_CHIN);
    } else if (entered && p->mode == MUSE_MODE_SPEAKING) {
        clip = alternate(RED_ANSWER_PALM);
    } else if (p->mode == MUSE_MODE_OFF && entered) {
        clip = RED_BYE_QUICK;
    } else if (p->mode == MUSE_MODE_ERROR) {
        clip = RED_REST;
    } else if (p->mode == MUSE_MODE_BOOT || p->mode == MUSE_MODE_IDLE) {
        if (!p->in_call && s_prev_call && !leaving()) {
            clip = alternate(RED_BYE_SLOW);
        } else if (p->woke || s_prev_mode == MUSE_MODE_COUNT) {
            clip = alternate(RED_WAKE_EYES);
        } else if (p->touched && p->mode == MUSE_MODE_IDLE) {
            clip = (++s_choices[RED_TOUCH_SMILE] % 5 == 0) ? RED_TOUCH_KISS : RED_TOUCH_SMILE;
        } else if (entered && s_prev_mode != MUSE_MODE_BOOT && !leaving()) {
            clip = RED_REST;
        } else if (complete && s_clip != RED_REST) {
            clip = RED_REST;
        } else if (s_clip == RED_REST && p->mode == MUSE_MODE_IDLE && !p->in_call && p->t >= s_next_idle) {
            clip = alternate(RED_IDLE_HAIR);
        }
    }

    if (clip != s_clip || s_frame < 0 || (p->touched && clip == RED_TOUCH_SMILE)) {
        select_clip(clip, p->t);
        age = 0;
    }
    s_prev_mode = p->mode;
    s_prev_call = p->in_call;
    s_prev_ending = p->ending;

    int frame = min_int((int)(age * RED_FPS), red_clips[s_clip].count - 1);
    if (p->mode == MUSE_MODE_OFF && s_clip == RED_BYE_QUICK) {
        frame = min_int((int)(age * 3.1f * RED_FPS), red_clips[s_clip].count - 1);
    }
    if (frame != s_frame) {
        if (s_frame >= 0 && frame > s_frame + 1) s_dropped += frame - s_frame - 1;
        if (!decode(frame)) {
            s_failed = true;
            s_dirty = true;
            return;
        }
        s_frame = frame;
        ++s_total_frames;
        ++s_clip_frames;
        s_dirty = true;
    }

    uint8_t blend = s_blending ? min_int((int)(16 * age / BLEND_S), 16) : 16;
    uint8_t lips = p->mode != MUSE_MODE_SPEAKING || !s_lip_x ? 0 :
        p->level >= 0.40f ? 3 : p->level >= 0.18f ? 2 : p->level >= 0.055f ? 1 : 0;
    s_dirty |= blend != s_blend_weight || lips != s_lips;
    s_blend_weight = blend;
    s_blending = blend < 16;
    s_lips = lips;
}

bool muse_pixel_whole_frame(bool *changed)
{
    *changed = s_dirty;
    s_dirty = false;
    return true;
}

static uint16_t blend565(uint16_t before, uint16_t after, unsigned k)
{
    unsigned prev = 16 - k;
    unsigned red = ((before >> 11) * prev + (after >> 11) * k) >> 4;
    unsigned green = (((before >> 5 & 63) * prev) + ((after >> 5 & 63) * k)) >> 4;
    unsigned blue = (((before & 31) * prev) + ((after & 31) * k)) >> 4;
    return (uint16_t)(red << 11 | green << 5 | blue);
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    size_t row_bytes = (x1 - x0 + 1) * sizeof(uint16_t);
    for (int y = y0; y <= y1; ++y, dst += stride_px) {
        int sy = y < MAX_CANVAS ? s_ymap[y] : -1;
        if (sy < 0 || s_failed || s_frame < 0) {
            memset(dst, 0, row_bytes);
            continue;
        }
        const uint8_t *row = &s_decoder->shown[sy * RED_W];
        const uint8_t *old = &s_decoder->old[sy * RED_W];
        int dy = sy - s_lip_y;
        bool move_mouth = s_lips && dy >= -4 && dy <= 4;
        for (int x = x0; x <= x1; ++x) {
            int sx = x < MAX_CANVAS ? s_xmap[x] : -1;
            uint16_t color = 0;
            if (sx >= 0) {
                color = red_palette[row[sx]];
                if (move_mouth) {
                    int dx = sx - s_lip_x;
                    int width = 4 + s_lips;
                    if (dx * dx <= width * width && dy * dy <= s_lips * s_lips) {
                        int sample = dy < 0 ? sy + 1 : sy - 1;
                        color = dx * dx * s_lips * s_lips + dy * dy * width * width < width * width * s_lips * s_lips
                            ? 0x3002 : red_palette[s_decoder->shown[sample * RED_W + sx]];
                    }
                }
                if (s_blending) color = blend565(red_palette[old[sx]], color, s_blend_weight);
            }
            dst[x - x0] = color;
        }
    }
}
