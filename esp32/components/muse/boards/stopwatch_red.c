// The StopWatch Live avatar. Audio and input stay owned by the voice task;
// this renderer just follows the pose sampled by the existing LVGL timer.
#include "muse_pixel.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "muse_vdec.h"
#include "muse_vstream.h"
#include "muse_yuv.h"
#include "stopwatch_red_data.h"

#define PANEL 466
#define BLEND_S 0.16f
#define IDLE_GAP_S 16.0f

typedef struct {
    uint16_t frame[2][(PANEL * PANEL + 7) & ~7];
    uint8_t packet[RED_PACKET_MAX + 4];
} decoder_t;

extern const uint8_t red_asset[] asm("_binary_stopwatch_red_h264_start");
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
static muse_vdec_t *s_codec;
static uint16_t *s_shown, *s_old;
static bool s_init, s_failed, s_blending, s_dirty, s_prev_call, s_prev_ending;
static uint8_t s_clip = RED_REST, s_choices[RED_COUNT], s_blend_weight = 16;
static unsigned s_lips;
static int s_lip_x, s_lip_y;
static muse_mode_t s_prev_mode = MUSE_MODE_COUNT;
static int s_frame = -1;
static float s_started, s_next_idle;
static uint16_t s_expand_map[PANEL];

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

void muse_pixel_set_size(int px)
{
    (void)px;   /* The portrait and its footer always share one fixed 466 px camera. */
    for (int i = 0; i < PANEL; ++i) s_expand_map[i] = i * RED_SIZE / PANEL;
    s_dirty = true;
}

static bool decode(int target)
{
    /* Catch up from the last independently decodable picture. Bound the work
     * of one UI tick so a late display frame cannot monopolise the controls. */
    int first = target / RED_GROUP * RED_GROUP;
    if (first <= s_frame) first = s_frame + 1;
    int last = min_int(target, first + 1);
    int64_t start = esp_timer_get_time();
    muse_pic_t pic;
    for (int i = first; i <= last; ++i) {
        unsigned n = red_clips[s_clip].frame + i;
        size_t len = red_offsets[n+1] - red_offsets[n];
        if (len > RED_PACKET_MAX) return false;
        /* tinyh264 removes emulation bytes in place. The embedded stream is read-only. */
        memcpy(s_decoder->packet, red_asset + red_offsets[n], len);
        if (muse_vdec_decode(s_codec, s_decoder->packet, len, &pic) != MUSE_VDEC_PICTURE ||
            pic.w != RED_SIZE || pic.h != RED_SIZE) return false;
    }
    muse_yuv_1x(pic.y, pic.u, pic.v, pic.y_stride, pic.c_stride, RED_SIZE,
                RED_SIZE, (uint8_t *)s_shown, RED_SIZE * 2);
    /* Expand in place from the last pixel to the first. LVGL receives native
     * RGB565 and repeated source rows only need a copy. */
    int previous = -1;
    for (int y = PANEL - 1; y >= 0; --y) {
        uint16_t *out = s_shown + y * PANEL;
        int sy = s_expand_map[y];
        if (sy == previous) {
            memcpy(out, out + PANEL, PANEL * sizeof(*out));
        } else {
            const uint16_t *in = s_shown + sy * RED_SIZE;
            for (int x = PANEL - 1; x >= 0; --x) out[x] = __builtin_bswap16(in[s_expand_map[x]]);
        }
        previous = sy;
    }
    unsigned elapsed = (unsigned)(esp_timer_get_time() - start);
    if (elapsed > s_decode_max_us) s_decode_max_us = elapsed;
    uint16_t mouth = red_mouth[red_clips[s_clip].frame + last];
    s_lip_x = (mouth & 255) * PANEL / RED_SIZE;
    s_lip_y = (mouth >> 8) * PANEL / RED_SIZE;
    if (s_frame >= 0) s_dropped += last - s_frame - 1;
    s_frame = last;
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
        uint16_t *previous = s_old;
        s_old = s_shown;
        s_shown = previous;
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

bool muse_stopwatch_red_leaving(void)
{
    return s_clip == RED_BYE_SLOW || s_clip == RED_BYE_QUICK;
}

/* Snapshot and rendering run in the same LVGL task. Between fades, the
 * previous frame buffer is spare and the next transition swaps it out. */
void *muse_stopwatch_red_snapshot_buf(size_t bytes)
{
    return s_codec && s_frame >= 0 && !s_blending && bytes <= sizeof(s_decoder->frame[0]) ? s_old : NULL;
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
        s_decoder = heap_caps_aligned_calloc(16, 1, sizeof(*s_decoder), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_decoder) {
            s_shown = s_decoder->frame[0];
            s_old = s_decoder->frame[1];
            s_codec = muse_vdec_open(MUSE_VIDEO_CODEC_H264, RED_SIZE, RED_SIZE);
        }
        if (!s_codec) {
            heap_caps_free(s_decoder);
            ESP_LOGE(TAG, "cannot allocate offline portrait decoder");
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
        if (!p->in_call && s_prev_call && !muse_stopwatch_red_leaving()) {
            clip = alternate(RED_BYE_SLOW);
        } else if (p->woke || s_prev_mode == MUSE_MODE_COUNT) {
            clip = alternate(RED_WAKE_EYES);
        } else if (p->touched && p->mode == MUSE_MODE_IDLE) {
            clip = (++s_choices[RED_TOUCH_SMILE] % 5 == 0) ? RED_TOUCH_KISS : RED_TOUCH_SMILE;
        } else if (entered && s_prev_mode != MUSE_MODE_BOOT && !muse_stopwatch_red_leaving()) {
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
        if (!decode(frame)) {
            ESP_LOGE(TAG, "could not decode %s frame %d", CLIP_NAME[s_clip], frame);
            s_failed = true;
            s_dirty = true;
            return;
        }
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
    size_t bytes = (x1 - x0 + 1) * sizeof(*dst);
    for (int y = y0; y <= y1; ++y, dst += stride_px) {
        if (s_failed || s_frame < 0) {
            memset(dst, 0, bytes);
            continue;
        }
        const uint16_t *row = s_shown + y * PANEL;
        memcpy(dst, row + x0, bytes);
        if (s_blending) {
            const uint16_t *old = s_old + y * PANEL;
            for (int x = x0; x <= x1; ++x) dst[x-x0] = blend565(old[x], row[x], s_blend_weight);
        }
        int dy = y - s_lip_y;
        int height = (int)s_lips + 1, width = 7 + (int)s_lips * 3;
        if (!s_lips || dy < -height-2 || dy > height+2) continue;
        int start = x0 > s_lip_x-width ? x0 : s_lip_x-width;
        int stop = min_int(x1, s_lip_x+width);
        for (int x = start; x <= stop; ++x) {
            int dx = x - s_lip_x;
            int opening = height * (width*width - dx*dx) / (width*width);
            int distance = dy < 0 ? -dy : dy;
            if (distance > opening+1) continue;
            uint16_t color = distance < opening ? 0x3823 :
                s_shown[(y + (dy < 0 ? 1 : -1)) * PANEL + x];
            dst[x-x0] = blend565(dst[x-x0], color, distance < opening ? 14 : 8);
        }
    }
}
