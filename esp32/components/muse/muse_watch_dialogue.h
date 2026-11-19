#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MUSE_WATCH_CAPTION_BYTES = 148, MUSE_WATCH_HISTORY_MAX = 8, MUSE_WATCH_HISTORY_BYTES = 310 };

typedef struct {
    uint32_t serial;
    uint32_t audio_at;
    uint32_t played;
    bool reply;
    char text[MUSE_WATCH_CAPTION_BYTES];
} muse_watch_caption_t;

typedef struct {
    muse_watch_caption_t previous;
    muse_watch_caption_t current;
    int64_t started_us;
} muse_watch_dialogue_view_t;

typedef struct {
    char question[MUSE_WATCH_HISTORY_BYTES];
    char reply[MUSE_WATCH_HISTORY_BYTES];
} muse_watch_turn_text_t;

/* A press identifies every packet and transcript until the next PTT press. */
void muse_watch_dialogue_begin(uint32_t press, bool new_call);
void muse_watch_dialogue_stop(uint32_t press);
void muse_watch_dialogue_heard(uint32_t press, const char *text, int64_t now_us);
void muse_watch_dialogue_reply(uint32_t press, const char *text, bool final);
void muse_watch_dialogue_received(uint32_t press, size_t samples);
void muse_watch_dialogue_played(uint32_t press, size_t samples, int64_t now_us);
void muse_watch_dialogue_view(int64_t now_us, muse_watch_dialogue_view_t *view);
uint32_t muse_watch_dialogue_revision(void);
size_t muse_watch_dialogue_history(muse_watch_turn_text_t *turns, size_t capacity);

#ifdef __cplusplus
}
#endif
