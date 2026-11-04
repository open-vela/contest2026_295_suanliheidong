#include "robot_voice_prompts.h"
#include "robot_audio_playback.h"
#include "robot_voice_diag.h"
#include <errno.h>
#include <string.h>

static const char *const tokens[] = {
  ROBOT_PROMPT_STARTUP, ROBOT_PROMPT_NETWORK_ERROR,
  ROBOT_PROMPT_CHAT_EXIT, ROBOT_PROMPT_NOT_HEARD
};
static const char *const names[] = {
  "startup", "network_error", "chat_exit", "not_heard"
};
static const char *const texts[] = {
  "你好，我准备好啦。", "暂时连不上，请稍后再试。",
  "我先休息啦，需要时再叫我。", "没听清，请再说一次。"
};
int robot_voice_prompt_id(const char *token)
{
  for (int i = 0; token && i < RV_PROMPT_COUNT; i++)
    if (!strcmp(token, tokens[i])) return i;
  return -1;
}
const char *robot_voice_prompt_name(enum robot_voice_prompt_e id)
{ return (unsigned int)id < RV_PROMPT_COUNT ? names[id] : "invalid"; }
const char *robot_voice_prompt_text(enum robot_voice_prompt_e id)
{ return (unsigned int)id < RV_PROMPT_COUNT ? texts[id] : ""; }

int robot_voice_prompt_play(enum robot_voice_prompt_e id,
    int (*sink)(const uint8_t *, size_t, void *), void *arg,
    bool (*cancelled)(void *), void *cancel_arg)
{
  struct robot_voice_prompt_asset_s asset;
  int ret;
  if ((unsigned int)id >= RV_PROMPT_COUNT || !sink) return -EINVAL;
  if (cancelled && cancelled(cancel_arg)) return -ECANCELED;
  asset = robot_voice_prompt_asset(id);
  /* Missing recordings are deliberately NOT replaced by an online request.
   * Until WAVs are supplied, one short local tone makes this visible/audible. */
  RV_EVENT("[本地提示] %s.wav %s%s\n", names[id], texts[id],
           asset.size ? "" : "（尚未烧录，播放提示音）");
  ret = robot_audio_playback_open_voice();
  if (ret < 0) return ret;
  if (asset.pcm && asset.size) {
    for (size_t offset = 0; offset < asset.size && ret == 0; ) {
      size_t n = asset.size - offset;
      if (n > 1024) n = 1024;
      if (cancelled && cancelled(cancel_arg)) { ret = -ECANCELED; break; }
      ret = sink(asset.pcm + offset, n, arg);
      offset += n;
    }
  } else {
    /* 64 ms, 600 Hz triangle with a short attack/release, 24 kHz PCM16. */
    uint8_t pcm[512];
    const size_t samples = ROBOT_VOICE_TTS_RATE * 64 / 1000;
    for (size_t offset = 0; offset < samples && ret == 0; ) {
      size_t n = samples - offset;
      if (n > sizeof(pcm) / 2) n = sizeof(pcm) / 2;
      if (cancelled && cancelled(cancel_arg)) { ret = -ECANCELED; break; }
      for (size_t i = 0; i < n; i++) {
        size_t at = offset + i;
        int phase = (int)(at % 40);
        int value = (phase < 20 ? phase - 10 : 30 - phase) * 180;
        size_t edge = at < samples - 1 - at ? at : samples - 1 - at;
        if (edge < 96) value = value * (int)edge / 96;
        uint16_t raw = (uint16_t)(int16_t)value;
        pcm[i * 2] = raw & 255; pcm[i * 2 + 1] = raw >> 8;
      }
      ret = sink(pcm, n * 2, arg); offset += n;
    }
  }
  if (ret == 0 && cancelled && cancelled(cancel_arg)) ret = -ECANCELED;
  if (ret == 0) ret = robot_audio_playback_finish();
  robot_audio_playback_close();
  return ret;
}
