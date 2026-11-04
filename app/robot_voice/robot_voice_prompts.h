#ifndef ROBOT_VOICE_PROMPTS_H
#define ROBOT_VOICE_PROMPTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Internal queue tokens: never send these to online TTS. */
#define ROBOT_PROMPT_STARTUP       "\037local:startup"
#define ROBOT_PROMPT_NETWORK_ERROR "\037local:network_error"
#define ROBOT_PROMPT_CHAT_EXIT     "\037local:chat_exit"
#define ROBOT_PROMPT_NOT_HEARD     "\037local:not_heard"

enum robot_voice_prompt_e {
  RV_PROMPT_STARTUP, RV_PROMPT_NETWORK_ERROR, RV_PROMPT_CHAT_EXIT,
  RV_PROMPT_NOT_HEARD, RV_PROMPT_COUNT
};
struct robot_voice_prompt_asset_s {
  const uint8_t *pcm;
  size_t size;
};
int robot_voice_prompt_id(const char *token);
const char *robot_voice_prompt_name(enum robot_voice_prompt_e id);
const char *robot_voice_prompt_text(enum robot_voice_prompt_e id);
/* Defined by the build-time WAV -> const PCM generator. */
struct robot_voice_prompt_asset_s robot_voice_prompt_asset(enum robot_voice_prompt_e id);
/* Caller owns the speaker lock and microphone echo guard. No network/key/file IO. */
int robot_voice_prompt_play(enum robot_voice_prompt_e id,
    int (*sink)(const uint8_t *, size_t, void *), void *arg,
    bool (*cancelled)(void *), void *cancel_arg);
#endif
