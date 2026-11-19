#include "muse_watch_dialogue.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

enum {
    CUE_COUNT = 24,
    MIC_RATE = 16000,
    AUDIO_QUEUE_LAG_US = 65000,
    SUBTITLE_ROLL_US = 750000,
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR muse_watch_turn_text_t s_history[MUSE_WATCH_HISTORY_MAX];
static EXT_RAM_BSS_ATTR muse_watch_caption_t s_cues[CUE_COUNT];
static muse_watch_dialogue_view_t s_view;
static uint32_t s_press, s_next_serial, s_revision, s_received, s_played;
static size_t s_cues_count, s_show, s_history_next, s_history_count;
static bool s_closed, s_reply_final, s_new_sentence, s_have_turn;
static int64_t s_ready_at;

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

void muse_watch_dialogue_begin(uint32_t press, bool new_call)
{
    portENTER_CRITICAL(&s_lock);
    s_press = press;
    s_received = s_played = 0;
    s_cues_count = s_show = 0;
    s_ready_at = 0;
    s_have_turn = s_closed = s_reply_final = false;
    s_new_sentence = true;
    if (new_call) memset(&s_view, 0, sizeof(s_view));
    portEXIT_CRITICAL(&s_lock);
}

void muse_watch_dialogue_stop(uint32_t press)
{
    portENTER_CRITICAL(&s_lock);
    if (press == s_press) {
        s_closed = true;
        s_show = s_cues_count;
        s_ready_at = 0;
    }
    portEXIT_CRITICAL(&s_lock);
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
    if (!*text) return;
    portENTER_CRITICAL(&s_lock);
    if (press == s_press && !s_closed) {
        muse_watch_turn_text_t *saved = turn();
        saved->question[0] = '\0';
        append(saved->question, sizeof(saved->question), text);
        s_revision++;
        if (!s_show && !s_played) {
            muse_watch_caption_t caption = { .serial = ++s_next_serial };
            append(caption.text, sizeof(caption.text), text);
            display(&caption, now_us);
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool sentence_end(const char *text, size_t n)
{
    if (n == 1) return *text == '.' || *text == '?' || *text == '!' || *text == '\n';
    return n == 3 && (!memcmp(text, "。", 3) || !memcmp(text, "？", 3) || !memcmp(text, "！", 3));
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
            // Transcript pieces and audio arrive interleaved. A small lookback
            // covers a text piece arriving just after the audio it describes.
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
        s_new_sentence = sentence_end(text - n, n);
    }
}

void muse_watch_dialogue_reply(uint32_t press, const char *text, bool final)
{
    portENTER_CRITICAL(&s_lock);
    if (press == s_press && !s_closed) {
        // The final event repeats the transcript. It is a fallback when the
        // service sent audio plus only the final text.
        if (!final || !s_cues_count) reply_text(text);
        s_reply_final = final;
    }
    portEXIT_CRITICAL(&s_lock);
}

void muse_watch_dialogue_received(uint32_t press, size_t samples)
{
    portENTER_CRITICAL(&s_lock);
    if (press == s_press && !s_closed) s_received += (uint32_t)samples;
    portEXIT_CRITICAL(&s_lock);
}

static void ready(int64_t now_us)
{
    if (s_ready_at || s_show >= s_cues_count || !s_played) return;
    if (s_played >= s_cues[s_show].audio_at) s_ready_at = now_us + AUDIO_QUEUE_LAG_US;
}

void muse_watch_dialogue_played(uint32_t press, size_t samples, int64_t now_us)
{
    portENTER_CRITICAL(&s_lock);
    if (press == s_press && !s_closed) {
        s_played += (uint32_t)samples;
        ready(now_us);
    }
    portEXIT_CRITICAL(&s_lock);
}

void muse_watch_dialogue_view(int64_t now_us, muse_watch_dialogue_view_t *view)
{
    portENTER_CRITICAL(&s_lock);
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
        // A late final transcript is still useful in history after the
        // audio drained, even when there was too little time to roll it all.
        if (s_reply_final && s_played && s_played >= s_received && s_show < s_cues_count &&
            s_cues[s_show].audio_at > s_played) {
            muse_watch_turn_text_t *saved = turn();
            for (; s_show < s_cues_count; s_show++) append(saved->reply, sizeof(saved->reply), s_cues[s_show].text);
            s_revision++;
            s_ready_at = 0;
        }
    }
    *view = s_view;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t muse_watch_dialogue_revision(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t version = s_revision;
    portEXIT_CRITICAL(&s_lock);
    return version;
}

size_t muse_watch_dialogue_history(muse_watch_turn_text_t *turns, size_t capacity)
{
    portENTER_CRITICAL(&s_lock);
    size_t n = s_history_count < capacity ? s_history_count : capacity;
    for (size_t i = 0; i < n; i++) {
        turns[i] = s_history[(s_history_next + MUSE_WATCH_HISTORY_MAX - n + i) % MUSE_WATCH_HISTORY_MAX];
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}
