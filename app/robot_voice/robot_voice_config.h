#ifndef __APPS_ROBOT_VOICE_CONFIG_H
#define __APPS_ROBOT_VOICE_CONFIG_H

#define ROBOT_VOICE_HOST             "token-plan-cn.xiaomimimo.com"
#define ROBOT_VOICE_PORT             "443"
#define ROBOT_VOICE_PATH             "/v1/chat/completions"
#define ROBOT_VOICE_ASR_MODEL        "mimo-v2.5-asr"
#define ROBOT_VOICE_LLM_MODEL        "mimo-v2.5"
#define ROBOT_VOICE_TTS_MODEL        "mimo-v2.5-tts"
#define ROBOT_VOICE_TTS_RATE         24000
#define ROBOT_VOICE_CAPTURE_RATE     16000
#define ROBOT_VOICE_CAPTURE_SECONDS  3
#define ROBOT_VOICE_CHANNELS         1
#define ROBOT_VOICE_BITS             16

/* ASR/normal JSON responses remain small. */
#define ROBOT_VOICE_HTTP_RESPONSE    (256 * 1024)
#define ROBOT_VOICE_MAX_BODY         (512 * 1024)

/* TTS embeds WAV PCM as base64 inside JSON.  A 20-second, 24-kHz mono s16 WAV
 * is roughly 960 KiB PCM / 1.28 MiB base64.  Keep TTS capacity separate from
 * the normal response buffer so ASR does not permanently pay this cost.
 *
 * This is a compatibility fallback for the current synchronous
 * robot_mimo_post()/vela_https_post_json() transport.  The preferred future
 * optimization is HTTP response streaming, which can remove this large
 * allocation and reduce first-audio latency.
 */
#define ROBOT_VOICE_TTS_RESPONSE     (2 * 1024 * 1024)

#endif
