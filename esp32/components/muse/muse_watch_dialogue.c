#include "muse_watch_dialogue.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#ifdef ESP_PLATFORM
#include "freertos/semphr.h"
#endif

enum {
    CUE_COUNT = 48,
    MIC_RATE = 16000,
    AUDIO_QUEUE_LAG_US = 65000,
    SUBTITLE_ROLL_US = 750000,
    SOFT_BREAK_BYTES = 54,
};

#ifdef ESP_PLATFORM
/* 使用静态初始化的 FreeRTOS 互斥锁保护 PSRAM 大数组，避免关中断期间遍历 PSRAM */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void dialogue_lock(void)
{
    if (!s_lock) {
        portENTER_CRITICAL(&s_init_mux);
        if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        portEXIT_CRITICAL(&s_init_mux);
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void dialogue_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}
#else
static void dialogue_lock(void) {}
static void dialogue_unlock(void) {}
#endif

static EXT_RAM_BSS_ATTR muse_watch_turn_text_t s_history[MUSE_WATCH_HISTORY_MAX];
static EXT_RAM_BSS_ATTR muse_watch_caption_t s_cues[CUE_COUNT];
static muse_watch_dialogue_view_t s_view;
static uint32_t s_press, s_next_serial, s_revision, s_received, s_played;
static size_t s_cues_count, s_show, s_history_next, s_history_count;
static bool s_closed, s_reply_final, s_new_sentence, s_have_turn;
static int64_t s_ready_at;
/* 流式增量回复末尾若落在多字节 UTF-8 字符中间，暂存残字节等待下一段拼接 */
static unsigned char s_utf8_carry[4];
static size_t s_utf8_carry_len;

static size_t letter_bytes(const char *text)
{
    unsigned char first = (unsigned char)*text;
    size_t bytes = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    for (size_t i = 1; i < bytes; i++) {
        if (((unsigned char)text[i] & 0xc0) != 0x80) return 1;
    }
    return bytes;
}

static void append(char *out, size_t capacity, const char *text)
{
    size_t at = strlen(out);
    while (*text && at + letter_bytes(text) < capacity) {
        size_t n = letter_bytes(text);
        memcpy(out + at, text, n);
        at += n;
        text += n;
    }
    out[at] = '\0';
}

/* 解码一个完整 UTF-8 码点：返回消费字节数；若末尾残缺返回 0（需缓存续拼）；非法字节返回 -1 */
static int decode_utf8(const unsigned char *s, size_t len, uint32_t *cp)
{
    if (!len) return 0;
    unsigned char b0 = s[0];
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }
    if ((b0 & 0xe0) == 0xc0) {
        if (len < 2) return 0;
        if ((s[1] & 0xc0) != 0x80) return -1;
        uint32_t v = ((uint32_t)(b0 & 0x1f) << 6) | (s[1] & 0x3f);
        if (v < 0x80) return -1;
        *cp = v;
        return 2;
    }
    if ((b0 & 0xf0) == 0xe0) {
        if (len < 3) {
            if (len == 2 && (s[1] & 0xc0) != 0x80) return -1;
            return 0;
        }
        if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80) return -1;
        uint32_t v = ((uint32_t)(b0 & 0x0f) << 12) | ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        if (v < 0x800) return -1;
        *cp = v;
        return 3;
    }
    if ((b0 & 0xf8) == 0xf0) {
        if (len < 4) {
            for (size_t i = 1; i < len; i++) {
                if ((s[i] & 0xc0) != 0x80) return -1;
            }
            return 0;
        }
        if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80) return -1;
        uint32_t v = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(s[1] & 0x3f) << 12) |
                     ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        if (v < 0x10000 || v > 0x10ffff) return -1;
        *cp = v;
        return 4;
    }
    return -1;
}

/* 将字库未收录的常见中文标点转为字库 100% 收录的等价字符，滤除 Emoji 与不可显字符 */
static const char *map_unsupported_codepoint(uint32_t cp)
{
    switch (cp) {
    case 0x201c: return "\xe3\x80\x8c"; /* “ -> 「 (U+300C) */
    case 0x201d: return "\xe3\x80\x8d"; /* ” -> 」 (U+300D) */
    case 0x2018:
    case 0x2019: return "'";            /* ‘ ’ -> ' */
    case 0x2013:
    case 0x2014:
    case 0x2015: return "-";            /* – — ― -> - */
    case 0x2026: return "...";          /* … -> ... */
    case 0x00b7:
    case 0x2022:
    case 0x2027: return ".";            /* · • ‧ -> . */
    case 0x2103: return "C";            /* ℃ -> C */
    case 0x2109: return "F";            /* ℉ -> F */
    case 0x00b0: return "\xe5\xba\xa6"; /* ° -> 度 (U+5EA6) */
    case 0x00d7: return "x";            /* × -> x */
    case 0x00f7: return "/";            /* ÷ -> / */
    case 0x00a0:
    case 0x3000: return " ";            /* 不换行/全角空格 -> 空格 */
    default: return NULL;
    }
}

static bool glyph_supported(uint32_t cp)
{
    if ((cp >= 0x20 && cp <= 0x7e) || cp == '\n' || cp == '\r' || cp == '\t') return true;
    if (cp >= 0x3000 && cp <= 0x30ff) return true; /* CJK 标点与假名 */
    if (cp >= 0x4e00 && cp <= 0x9fa5) return true; /* CJK 常用汉字 */
    if (cp >= 0xff01 && cp <= 0xffee) return true; /* 全角标点与字母数字 */
    return false;
}

static void emit_sanitized_cp(char *out, size_t cap, size_t *at, const unsigned char *raw, size_t raw_len, uint32_t cp)
{
    const char *mapped = map_unsupported_codepoint(cp);
    if (mapped) {
        size_t mlen = strlen(mapped);
        if (*at + mlen < cap) {
            memcpy(out + *at, mapped, mlen);
            *at += mlen;
        }
        return;
    }
    if (glyph_supported(cp) && *at + raw_len < cap) {
        memcpy(out + *at, raw, raw_len);
        *at += raw_len;
    }
}

/* 净化文本并可选支持流式 UTF-8 跨块拼接（streaming=true 时末尾半截字符存入 carry） */
static void sanitize_text(const char *src, char *out, size_t cap, bool streaming)
{
    size_t at = 0;
    const unsigned char *p = (const unsigned char *)src;
    size_t rem = strlen(src);

    if (streaming && s_utf8_carry_len) {
        unsigned char comb[8];
        size_t take = rem < sizeof(comb) - s_utf8_carry_len ? rem : sizeof(comb) - s_utf8_carry_len;
        memcpy(comb, s_utf8_carry, s_utf8_carry_len);
        memcpy(comb + s_utf8_carry_len, p, take);
        size_t clen = s_utf8_carry_len + take;
        size_t pos = 0;
        while (pos < s_utf8_carry_len) {
            uint32_t cp = 0;
            int n = decode_utf8(comb + pos, clen - pos, &cp);
            if (n == 0) {
                /* 加上本次全部字节仍不完整，继续暂存 */
                size_t left = clen - pos;
                if (left <= sizeof(s_utf8_carry)) {
                    memmove(s_utf8_carry, comb + pos, left);
                    s_utf8_carry_len = left;
                } else {
                    s_utf8_carry_len = 0;
                }
                out[at] = '\0';
                return;
            }
            if (n < 0) {
                pos++;
                continue;
            }
            emit_sanitized_cp(out, cap, &at, comb + pos, (size_t)n, cp);
            pos += (size_t)n;
        }
        size_t consumed_from_src = pos - s_utf8_carry_len;
        s_utf8_carry_len = 0;
        p += consumed_from_src;
        rem -= consumed_from_src;
    }

    while (rem) {
        uint32_t cp = 0;
        int n = decode_utf8(p, rem, &cp);
        if (n == 0) {
            if (streaming && rem <= sizeof(s_utf8_carry)) {
                memcpy(s_utf8_carry, p, rem);
                s_utf8_carry_len = rem;
            }
            break;
        }
        if (n < 0) {
            p++;
            rem--;
            continue;
        }
        emit_sanitized_cp(out, cap, &at, p, (size_t)n, cp);
        p += (size_t)n;
        rem -= (size_t)n;
    }
    out[at] = '\0';
}

static muse_watch_turn_text_t *turn(void)
{
    if (!s_have_turn) {
        memset(&s_history[s_history_next], 0, sizeof(s_history[0]));
        s_history_next = (s_history_next + 1) % MUSE_WATCH_HISTORY_MAX;
        if (s_history_count < MUSE_WATCH_HISTORY_MAX) s_history_count++;
        s_have_turn = true;
    }
    return &s_history[(s_history_next + MUSE_WATCH_HISTORY_MAX - 1) % MUSE_WATCH_HISTORY_MAX];
}

/* 将已收到但尚未滚上屏的句子全部追加到历史记录（已上屏句在滚出及增量时已追加，绝不重复） */
static void flush_unshown_to_history(void)
{
    if (s_show >= s_cues_count) return;
    muse_watch_turn_text_t *saved = turn();
    for (; s_show < s_cues_count; s_show++) {
        append(saved->reply, sizeof(saved->reply), s_cues[s_show].text);
    }
    s_revision++;
    s_ready_at = 0;
}

void muse_watch_dialogue_begin(uint32_t press, bool new_call)
{
    dialogue_lock();
    s_press = press;
    s_received = s_played = 0;
    s_cues_count = s_show = 0;
    s_ready_at = 0;
    s_have_turn = s_closed = s_reply_final = false;
    s_new_sentence = true;
    s_utf8_carry_len = 0;
    if (new_call) memset(&s_view, 0, sizeof(s_view));
    dialogue_unlock();
}

void muse_watch_dialogue_stop(uint32_t press)
{
    dialogue_lock();
    if (press == s_press) {
        flush_unshown_to_history();
        s_closed = true;
        s_ready_at = 0;
        s_utf8_carry_len = 0;
    }
    dialogue_unlock();
}

static void display(const muse_watch_caption_t *caption, int64_t now_us)
{
    s_view.previous = s_view.current;
    s_view.current = *caption;
    s_view.current.played = s_played;
    s_view.started_us = now_us;
}

void muse_watch_dialogue_heard(uint32_t press, const char *text, int64_t now_us)
{
    if (!text || !*text) return;
    char clean[sizeof(s_history[0].question)];
    sanitize_text(text, clean, sizeof(clean), false);
    if (!clean[0]) return;
    dialogue_lock();
    if (press == s_press && !s_closed) {
        muse_watch_turn_text_t *saved = turn();
        saved->question[0] = '\0';
        append(saved->question, sizeof(saved->question), clean);
        s_revision++;
        /* 只要首句回答尚未滚出，就正常在慢滚字幕区展示用户提问 */
        if (!s_show) {
            muse_watch_caption_t caption = { .serial = ++s_next_serial };
            append(caption.text, sizeof(caption.text), clean);
            display(&caption, now_us);
        }
    }
    dialogue_unlock();
}

static bool sentence_end(const char *ch, size_t n, const char *next)
{
    if (n == 1) {
        if (*ch == '?' || *ch == '!' || *ch == '\n') return true;
        if (*ch == '.') {
            /* 避免把省略号 ...、小数点或人名间隔号误判为英文句号断句 */
            return *next == ' ' || *next == '\n' || *next == '\r' || *next == '\t';
        }
        return false;
    }
    return n == 3 && (!memcmp(ch, "。", 3) || !memcmp(ch, "？", 3) || !memcmp(ch, "！", 3));
}

static bool soft_sentence_end(const char *text, size_t n)
{
    if (n == 1) return *text == ',' || *text == ';' || *text == ':';
    return n == 3 && (!memcmp(text, "，", 3) || !memcmp(text, "；", 3) || !memcmp(text, "：", 3));
}

static void reply_text(const char *text)
{
    while (*text) {
        size_t n = letter_bytes(text);
        if (s_new_sentence) {
            if (n == 1 && (*text == ' ' || *text == '\r' || *text == '\n' || *text == '\t')) {
                text++;
                continue;
            }
            if (s_cues_count == CUE_COUNT) return;
            /* 文本与音频交错到达，略微回看锚点以覆盖紧随音频之后到达的字幕片段 */
            uint32_t anchor = s_received > MIC_RATE / 10 ? s_received - MIC_RATE / 10 : 0;
            if (s_cues_count) {
                uint32_t earliest = s_cues[s_cues_count - 1].audio_at + MIC_RATE * 3 / 4;
                if (anchor < earliest) anchor = earliest;
            } else {
                anchor = 0;
            }
            s_cues[s_cues_count++] = (muse_watch_caption_t){
                .serial = ++s_next_serial, .audio_at = anchor, .reply = true,
            };
            s_new_sentence = false;
        }
        muse_watch_caption_t *cue = &s_cues[s_cues_count - 1];
        size_t len = strlen(cue->text);
        if (len + n < sizeof(cue->text)) {
            memcpy(cue->text + len, text, n);
            cue->text[len + n] = '\0';
            if (s_view.current.serial == cue->serial) {
                append(s_view.current.text, sizeof(s_view.current.text), cue->text + len);
                muse_watch_turn_text_t *saved = turn();
                append(saved->reply, sizeof(saved->reply), cue->text + len);
                s_revision++;
            }
        } else {
            s_new_sentence = true;
            continue;
        }
        text += n;
        size_t new_len = len + n;
        s_new_sentence = sentence_end(text - n, n, text) ||
                         (new_len >= SOFT_BREAK_BYTES && soft_sentence_end(text - n, n));
    }
}

void muse_watch_dialogue_reply(uint32_t press, const char *text, bool final)
{
    if (!text) return;
    dialogue_lock();
    if (press == s_press && !s_closed) {
        /* 终态事件会重复全文，仅在流式未收到任何分段时作为兜底；流式增量启用 UTF-8 跨块拼接 */
        if (!final || !s_cues_count) {
            char clean[704];
            if (final) s_utf8_carry_len = 0;
            sanitize_text(text, clean, sizeof(clean), !final);
            if (clean[0]) reply_text(clean);
        }
        if (final) s_utf8_carry_len = 0;
        s_reply_final = final;
    }
    dialogue_unlock();
}

void muse_watch_dialogue_received(uint32_t press, size_t samples)
{
    dialogue_lock();
    if (press == s_press && !s_closed) s_received += (uint32_t)samples;
    dialogue_unlock();
}

static void ready(int64_t now_us)
{
    if (s_ready_at || s_show >= s_cues_count || !s_played) return;
    if (s_played >= s_cues[s_show].audio_at) s_ready_at = now_us + AUDIO_QUEUE_LAG_US;
}

void muse_watch_dialogue_played(uint32_t press, size_t samples, int64_t now_us)
{
    dialogue_lock();
    if (press == s_press && !s_closed) {
        s_played += (uint32_t)samples;
        ready(now_us);
    }
    dialogue_unlock();
}

void muse_watch_dialogue_view(int64_t now_us, muse_watch_dialogue_view_t *view)
{
    dialogue_lock();
    if (!s_closed) {
        ready(now_us);
        if (s_ready_at && now_us >= s_ready_at &&
            now_us - s_view.started_us >= SUBTITLE_ROLL_US) {
            const muse_watch_caption_t *cue = &s_cues[s_show++];
            display(cue, now_us);
            muse_watch_turn_text_t *saved = turn();
            append(saved->reply, sizeof(saved->reply), cue->text);
            s_revision++;
            s_ready_at = 0;
        }
        /* 音频已全部播完时，将锚点超出播音时长或滞留未滚出的尾句补录进历史记录，避免丢尾句 */
        if (s_reply_final && s_played && s_played >= s_received && s_show < s_cues_count &&
            (s_cues[s_show].audio_at > s_played || now_us - s_view.started_us >= SUBTITLE_ROLL_US * 2)) {
            flush_unshown_to_history();
        }
    }
    *view = s_view;
    dialogue_unlock();
}

uint32_t muse_watch_dialogue_revision(void)
{
    dialogue_lock();
    uint32_t version = s_revision;
    dialogue_unlock();
    return version;
}

size_t muse_watch_dialogue_history(muse_watch_turn_text_t *turns, size_t capacity)
{
    dialogue_lock();
    size_t n = s_history_count < capacity ? s_history_count : capacity;
    for (size_t i = 0; i < n; i++) {
        turns[i] = s_history[(s_history_next + MUSE_WATCH_HISTORY_MAX - n + i) % MUSE_WATCH_HISTORY_MAX];
    }
    dialogue_unlock();
    return n;
}
