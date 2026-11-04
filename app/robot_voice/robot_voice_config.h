#ifndef __APPS_ROBOT_VOICE_CONFIG_H
#define __APPS_ROBOT_VOICE_CONFIG_H

#ifndef ROBOT_VOICE_HOST
#define ROBOT_VOICE_HOST             "token-plan-cn.xiaomimimo.com"
#endif
#ifndef ROBOT_VOICE_PORT
#define ROBOT_VOICE_PORT             "443"
#endif
#define ROBOT_VOICE_PATH             "/v1/chat/completions"
#define ROBOT_VOICE_ASR_MODEL        "mimo-v2.5-asr"
#define ROBOT_VOICE_LLM_MODEL        "mimo-v2.6-flash"
#define ROBOT_VOICE_TTS_MODEL        "mimo-v2.5-tts"
#define ROBOT_VOICE_TTS_RATE         24000
#define ROBOT_VOICE_CAPTURE_RATE     16000
#define ROBOT_VOICE_CAPTURE_SECONDS  3
#define ROBOT_VOICE_CHANNELS         1
#define ROBOT_VOICE_BITS             16

/* Continuous voice_start: local TFLite acoustic wake + conversation ASR.
 * All durations are milliseconds. Tune VAD against real microphone logs.
 * Dormant audio is never sent to ASR. After acoustic activation, complete
 * unmodified PCM is uploaded only after the segment finishes.
 * Final segments are joined in capture order and
 * dispatched only when the complete user turn has become quiet.
 */
/* All HTTP work is serialized by the shared transport gate. A second ASR
 * worker only waits there while retaining another 24 KiB task stack, so keep
 * one request worker and let the existing bounded segment queue preserve
 * ordering without reserving another stack. Capture/KWS remains independent. */
#define ROBOT_VOICE_ASR_WORKERS       1
#define ROBOT_VOICE_ASR_QUEUE         4
#define ROBOT_VOICE_ASR_INTERVAL_MS   800
#define ROBOT_VOICE_ASR_BACKOFF_MS    3000
#define ROBOT_VOICE_PRE_ROLL_MS       300
/* Acoustic wake handoff retains PCM while TFLite runs asynchronously.
 * The existing segment allocation is reused as a ring and linearization area. */
#ifndef ROBOT_VOICE_WAKE_CONTEXT_MS
#  define ROBOT_VOICE_WAKE_CONTEXT_MS 6000
#endif
/* Compatibility switch retained for old board flags; v4 always requires
 * a valid final ASR and refuses preemption after Flash starts replying. */
#ifndef ROBOT_VOICE_INTERRUPT_ON_SPEECH
#  define ROBOT_VOICE_INTERRUPT_ON_SPEECH 0
#endif
#ifndef ROBOT_VOICE_INTERRUPT_MS
#  define ROBOT_VOICE_INTERRUPT_MS 160
#endif
/* Integer DC/high-pass + fixed-point WebRTC VAD. ASR always uses raw PCM.
 * Modes: 0..3 are the WebRTC aggressiveness levels; the default is the
 * strongest noise rejection while keeping the same 20 ms frame cost. */
#ifndef ROBOT_VOICE_AFE_MODE
#  define ROBOT_VOICE_AFE_MODE        2
#endif
#define ROBOT_VOICE_AFE_FRAME_MS     20
#ifndef ROBOT_VOICE_VAD_MODE
#  define ROBOT_VOICE_VAD_MODE        3
#endif
#ifndef ROBOT_VOICE_VAD_MIN_LEVEL
#  define ROBOT_VOICE_VAD_MIN_LEVEL   120
#endif
#define ROBOT_VOICE_VAD_WINDOW_FRAMES 6
#define ROBOT_VOICE_VAD_START_FRAMES  4
#define ROBOT_VOICE_VAD_START_MS \
  (ROBOT_VOICE_AFE_FRAME_MS * ROBOT_VOICE_VAD_START_FRAMES)
#if ROBOT_VOICE_AFE_MODE < 0 || ROBOT_VOICE_AFE_MODE > 2 || \
    ROBOT_VOICE_VAD_MODE < 0 || ROBOT_VOICE_VAD_MODE > 3 || \
    ROBOT_VOICE_VAD_WINDOW_FRAMES < 1 || ROBOT_VOICE_VAD_WINDOW_FRAMES > 16 || \
    ROBOT_VOICE_VAD_START_FRAMES < 1 || \
    ROBOT_VOICE_VAD_START_FRAMES > ROBOT_VOICE_VAD_WINDOW_FRAMES || \
    ROBOT_VOICE_PRE_ROLL_MS < ROBOT_VOICE_VAD_WINDOW_FRAMES * ROBOT_VOICE_AFE_FRAME_MS
#  error Invalid AFE / VAD configuration
#endif
#define ROBOT_VOICE_VAD_END_MS        640
#ifndef ROBOT_VOICE_VAD_MIN_MS
#  define ROBOT_VOICE_VAD_MIN_MS      240
#endif
#define ROBOT_VOICE_EARLY_QUIET_MS    160
#define ROBOT_VOICE_SEGMENT_MAX_MS    30000
#if ROBOT_VOICE_AFE_FRAME_MS != 20 || ROBOT_VOICE_VAD_MIN_LEVEL < 0 || \
    ROBOT_VOICE_VAD_MIN_LEVEL > 32767 || \
    ROBOT_VOICE_PRE_ROLL_MS + ROBOT_VOICE_AFE_FRAME_MS > ROBOT_VOICE_SEGMENT_MAX_MS
#  error Invalid AFE frame / pre-roll / level configuration
#endif
#if ROBOT_VOICE_WAKE_CONTEXT_MS % ROBOT_VOICE_AFE_FRAME_MS != 0 || \
    ROBOT_VOICE_WAKE_CONTEXT_MS < ROBOT_VOICE_PRE_ROLL_MS || \
    2 * ROBOT_VOICE_WAKE_CONTEXT_MS > ROBOT_VOICE_SEGMENT_MAX_MS || \
    ROBOT_VOICE_INTERRUPT_MS < ROBOT_VOICE_VAD_START_MS || \
    ROBOT_VOICE_INTERRUPT_MS % ROBOT_VOICE_AFE_FRAME_MS != 0
#  error Invalid wake handoff / speech interrupt settings
#endif
#define ROBOT_VOICE_INTERRUPT_FRAMES \
  (ROBOT_VOICE_INTERRUPT_MS / ROBOT_VOICE_AFE_FRAME_MS)
#define ROBOT_VOICE_INTERRUPT_WINDOW_FRAMES (ROBOT_VOICE_INTERRUPT_FRAMES + 2)
#if ROBOT_VOICE_INTERRUPT_WINDOW_FRAMES > 30
#  error Speech interrupt vote window must fit in a 32-bit history
#endif
#ifndef ROBOT_VOICE_CHAT_IDLE_MS
#  define ROBOT_VOICE_CHAT_IDLE_MS    20000
#endif
#define ROBOT_VOICE_REPLY_TIMEOUT_MS  120000
#define ROBOT_VOICE_ECHO_GUARD_MS     0
#ifndef ROBOT_VOICE_START_GREETING
#  define ROBOT_VOICE_START_GREETING "你好，我准备好啦。"
#endif
#define ROBOT_VOICE_READY_TIMEOUT_MS 10000
/* Absolute RT priority changes remain opt-in. The fair profile below uses
 * the existing caller priority and only selects RR when the board has it. */
#ifndef ROBOT_VOICE_EXPLICIT_SCHED
#  define ROBOT_VOICE_EXPLICIT_SCHED 0
#endif
#ifndef ROBOT_VOICE_WIFI_CHECKED_CONNECT
#  define ROBOT_VOICE_WIFI_CHECKED_CONNECT 1
#endif
#define ROBOT_VOICE_WIFI_DRIVER_READY_MS 500
#define ROBOT_VOICE_WIFI_ASSOC_MS 15000
#define ROBOT_VOICE_WIFI_POLL_MS  200
#ifndef ROBOT_VOICE_WIFI_CHECK_MS
#  define ROBOT_VOICE_WIFI_CHECK_MS 1000
#endif
#ifndef ROBOT_VOICE_WIFI_RECOVER_MIN_MS
#  define ROBOT_VOICE_WIFI_RECOVER_MIN_MS 3000
#endif
#ifndef ROBOT_VOICE_WIFI_RECOVER_MAX_MS
#  define ROBOT_VOICE_WIFI_RECOVER_MAX_MS 30000
#endif
#ifndef ROBOT_VOICE_WIFI_CONNECT_FAILURES
#  define ROBOT_VOICE_WIFI_CONNECT_FAILURES 3
#endif
#ifndef ROBOT_VOICE_ACTION_NET_TIMEOUT_MS
#  define ROBOT_VOICE_ACTION_NET_TIMEOUT_MS 30000
#endif
#ifndef ROBOT_VOICE_ACTION_NET_RETRY_MS
#  define ROBOT_VOICE_ACTION_NET_RETRY_MS 250
#endif
#if ROBOT_VOICE_WIFI_CHECK_MS < 1 || ROBOT_VOICE_WIFI_RECOVER_MIN_MS < 1 || \
    ROBOT_VOICE_WIFI_RECOVER_MAX_MS < ROBOT_VOICE_WIFI_RECOVER_MIN_MS || \
    ROBOT_VOICE_WIFI_CONNECT_FAILURES < 1 || \
    ROBOT_VOICE_ACTION_NET_TIMEOUT_MS < 1000 || \
    ROBOT_VOICE_ACTION_NET_RETRY_MS < 1
#  error Invalid Wi-Fi recovery settings
#endif
/* Optional explicit profile: capture > DMA > consumer > cloud producer. */
#define ROBOT_VOICE_CAPTURE_PRIORITY 125
#define ROBOT_VOICE_PLAY_PRIORITY    120
#define ROBOT_VOICE_CONSUMER_PRIORITY 110
#define ROBOT_VOICE_NETWORK_PRIORITY 100
#define ROBOT_VOICE_TTS_WORKER_STACK (32 * 1024)
#ifndef ROBOT_VOICE_FAIR_SCHED
#  define ROBOT_VOICE_FAIR_SCHED 1
#endif

/* A final turn is committed only after this much quiet, all earlier ASR
 * finals have completed, and no new speech is being captured. */
#define ROBOT_VOICE_TURN_END_MS       800
#define ROBOT_VOICE_TEXT_CAP          4096
#define ROBOT_VOICE_QUEUED_PCM_MAX    (1536 * 1024)
#define ROBOT_VOICE_LLM_MAX_TOKENS    768
#define ROBOT_VOICE_HTTP_CONNECT_MS  4000
/* An unbuffered NuttX TCP sender may require a blocking send to schedule
 * transmission. Keep reads nonblocking, and bound each send by a short
 * SO_SNDTIMEO and the request deadline. Buffered TCP/host sockets keep the
 * normal nonblocking BIO. No global network configuration is changed. */
#ifndef ROBOT_VOICE_TCP_TIMED_SEND
#  if defined(__NuttX__) && !defined(CONFIG_NET_TCP_WRITE_BUFFERS)
#    define ROBOT_VOICE_TCP_TIMED_SEND 1
#  else
#    define ROBOT_VOICE_TCP_TIMED_SEND 0
#  endif
#endif
#ifndef ROBOT_VOICE_TCP_SEND_CHUNK
#  define ROBOT_VOICE_TCP_SEND_CHUNK 1024
#endif
#ifndef ROBOT_VOICE_HTTP_BACKOFF_MS
#  define ROBOT_VOICE_HTTP_BACKOFF_MS 10
#endif
#if ROBOT_VOICE_TCP_SEND_CHUNK < 1 || ROBOT_VOICE_HTTP_BACKOFF_MS < 1
#  error Invalid HTTP send/backoff settings
#endif
/* Agent music is OpenVela Media only. There is no local HTTP/MP3 fallback. */
#ifndef ROBOT_VOICE_MEDIA_STARTUP_MS
#  define ROBOT_VOICE_MEDIA_STARTUP_MS 3000
#endif
/* ESP32/OpenVela uses bounded blocking connect. Only established TLS I/O
 * uses O_NONBLOCK/poll. Other hosts retain the POSIX readiness path. */
#ifndef ROBOT_VOICE_TCP_BLOCKING_CONNECT
#  ifdef __NuttX__
#    define ROBOT_VOICE_TCP_BLOCKING_CONNECT 1
#  else
#    define ROBOT_VOICE_TCP_BLOCKING_CONNECT 0
#  endif
#endif
#ifndef ROBOT_VOICE_TCP_BUSY_RETRIES
#  define ROBOT_VOICE_TCP_BUSY_RETRIES 2
#endif
#ifndef ROBOT_VOICE_TCP_BUSY_DELAY_MS
#  define ROBOT_VOICE_TCP_BUSY_DELAY_MS 100
#endif
#if ROBOT_VOICE_TCP_BUSY_RETRIES < 0 || ROBOT_VOICE_TCP_BUSY_RETRIES > 4 || \
    ROBOT_VOICE_TCP_BUSY_DELAY_MS < 1
#  error Invalid bounded TCP busy recovery settings
#endif

/* A DHCP lease can remain valid while the hotspot DNS server briefly drops
 * queries. Retry resolver failures inside the HTTP connect budget before
 * considering Wi-Fi recovery. */
#ifndef ROBOT_VOICE_DNS_RETRIES
#  define ROBOT_VOICE_DNS_RETRIES 3
#endif
#ifndef ROBOT_VOICE_DNS_RETRY_DELAY_MS
#  define ROBOT_VOICE_DNS_RETRY_DELAY_MS 150
#endif
#if ROBOT_VOICE_DNS_RETRIES < 1 || ROBOT_VOICE_DNS_RETRY_DELAY_MS < 1
#  error Invalid bounded DNS recovery settings
#endif

/* TLS has its own budget; DNS/TCP must not consume the handshake window. */
#ifndef ROBOT_VOICE_HTTP_TLS_MS
#  define ROBOT_VOICE_HTTP_TLS_MS   12000
#endif
/* Modest per-socket tuning; set to 0 for a controlled board A/B comparison.
 * Larger windows require checking the board's IOB/out-of-order capacity. */
#ifndef ROBOT_VOICE_TCP_RCVBUF
#  define ROBOT_VOICE_TCP_RCVBUF     4096
#endif
/* One TTS receiver can borrow more of the existing shared IOB pool. Keep
 * ASR/Flash small; do not enlarge every socket or the internal DRAM heap.
 * A second TTS worker shares this same aggregate receive budget.
 * Set to 0 for an on-board A/B run with the platform's default window. */
#ifndef ROBOT_VOICE_TTS_TCP_RCVBUF
#  define ROBOT_VOICE_TTS_TCP_RCVBUF (16 * 1024)
#endif
#ifndef ROBOT_VOICE_HTTP_IDLE_MS
#  define ROBOT_VOICE_HTTP_IDLE_MS   8000
#endif
#define ROBOT_VOICE_HTTP_LLM_MS      60000
#define ROBOT_VOICE_LLM_SSE_MAX      (64 * 1024)
#ifndef ROBOT_VOICE_HTTP_LLM_REUSE
#  define ROBOT_VOICE_HTTP_LLM_REUSE  0
#endif
#define ROBOT_VOICE_HTTP_POLL_MS     250
/* Set a board PEM/DER CA file to require authenticated server certificates.
 * Empty preserves the existing board verification policy. A configured CA
 * that cannot load/validate fails closed; there is no OPTIONAL fallback. */
#ifndef ROBOT_VOICE_TLS_CA_FILE
#  define ROBOT_VOICE_TLS_CA_FILE    ""
#endif
#define ROBOT_VOICE_ERROR_NOTICE_MS  15000
/* Local time is UTC plus this offset; never depend on the shell's TZ.
 * A clock must be explicitly confirmed after board NTP/RTC synchronization. */
#ifndef ROBOT_VOICE_TIME_UTC_OFFSET_MIN
#  define ROBOT_VOICE_TIME_UTC_OFFSET_MIN 480
#endif
#define ROBOT_VOICE_CLOCK_VALID_MS   (6ULL * 60 * 60 * 1000)
#ifndef ROBOT_VOICE_LOG_TEXT
#  define ROBOT_VOICE_LOG_TEXT       0
#endif
/* User-visible final recognition/model text; independent of debug traces. */
#ifndef ROBOT_VOICE_LOG_DIALOGUE
#  define ROBOT_VOICE_LOG_DIALOGUE   1
#endif
#ifndef ROBOT_VOICE_ASR_STREAM
#  define ROBOT_VOICE_ASR_STREAM    1
#endif
#ifndef ROBOT_VOICE_ASR_LANGUAGE
#  define ROBOT_VOICE_ASR_LANGUAGE  "auto"
#endif
#define ROBOT_VOICE_HTTP_ASR_MS      20000
/* A failed ASR/Flash/TTS request may retry up to five times. All retries
 * share this additional deadline; every retry HTTP call is bounded by it. */
#ifndef ROBOT_VOICE_RETRY_MAX
#  define ROBOT_VOICE_RETRY_MAX         5
#endif
#ifndef ROBOT_VOICE_RETRY_BUDGET_MS
#  define ROBOT_VOICE_RETRY_BUDGET_MS   5000
#endif
#ifndef ROBOT_VOICE_RETRY_DELAY_MS
#  define ROBOT_VOICE_RETRY_DELAY_MS    100
#endif
/* Pre-HTTP socket recovery also backs off, inside the original deadline. */
#ifndef ROBOT_VOICE_HTTP_CONNECT_RETRY_DELAY_MS
#  define ROBOT_VOICE_HTTP_CONNECT_RETRY_DELAY_MS 250
#endif
#ifndef ROBOT_VOICE_TTS_LIVE
#  define ROBOT_VOICE_TTS_LIVE       0
#endif
/* Sentence pipeline: Flash deltas feed one TTS producer. As soon as a
 * sentence is completely validated, the consumer plays that finite PCM
 * sentence while the producer synthesizes the next one. Set continuity to
 * 1 only to opt back into waiting for the complete reply/cache window. */
#ifndef ROBOT_VOICE_TTS_CONTINUITY
#  define ROBOT_VOICE_TTS_CONTINUITY 0
#endif
#if ROBOT_VOICE_TTS_CONTINUITY && ROBOT_VOICE_TTS_LIVE
#  error Choose complete-cache continuity or live PCM, not both
#endif

/* One TTS HTTP producer plus ordered playback. It still prefetches the next
 * phrase during playback, without a second simultaneous receive queue
 * competing with LLM/Wi-Fi and the speaker's internal DMA allocations. */
#ifndef ROBOT_VOICE_TTS_WORKERS
#  define ROBOT_VOICE_TTS_WORKERS   1
#endif
#if ROBOT_VOICE_TTS_WORKERS < 1 || ROBOT_VOICE_TTS_WORKERS > 2
#  error TTS workers must be 1 or 2
#endif
#ifndef ROBOT_VOICE_TTS_SENTENCE_BYTES
#  define ROBOT_VOICE_TTS_SENTENCE_BYTES 240
#endif
/* Dispatch each completed sentence immediately in the default profile,
 * including a short first sentence. Reply-cache mode may coalesce clauses.
 * An explicit larger target retains the optional timed merge policy. */
#ifndef ROBOT_VOICE_TTS_CHUNK_TARGET_BYTES
#  define ROBOT_VOICE_TTS_CHUNK_TARGET_BYTES \
     (ROBOT_VOICE_TTS_CONTINUITY ? 180 : 1)
#endif
#ifndef ROBOT_VOICE_TTS_CHUNK_MERGE_WAIT_MS
#  define ROBOT_VOICE_TTS_CHUNK_MERGE_WAIT_MS 120
#endif
#if ROBOT_VOICE_TTS_CHUNK_TARGET_BYTES < 1 || \
    ROBOT_VOICE_TTS_CHUNK_TARGET_BYTES > ROBOT_VOICE_TTS_SENTENCE_BYTES
#  error TTS chunk target must fit within the sentence byte limit
#endif
#define ROBOT_VOICE_TTS_SENTENCE_PCM (1024 * 1024)
#define ROBOT_VOICE_TTS_READY_SLOTS  2
#define ROBOT_VOICE_TTS_QUEUE_BYTES (3 * 1024 * 1024)
#if (ROBOT_VOICE_TTS_READY_SLOTS + 1) * ROBOT_VOICE_TTS_SENTENCE_PCM > ROBOT_VOICE_TTS_QUEUE_BYTES
#  error TTS sentence caches exceed PCM budget
#endif

#define ROBOT_VOICE_HTTP_TTS_MS      60000
#define ROBOT_VOICE_WEATHER_MS       5000
#define ROBOT_VOICE_TTS_STREAM       1
#define ROBOT_VOICE_TTS_PREBUFFER    (24 * 1024)
#ifndef ROBOT_VOICE_TTS_LISTEN_GAP_MS
#  define ROBOT_VOICE_TTS_LISTEN_GAP_MS 400
#endif
#ifndef ROBOT_VOICE_TTS_LISTEN_BUFFER_BYTES
#  define ROBOT_VOICE_TTS_LISTEN_BUFFER_BYTES 0
#endif
/* Default: finish each sentence's SSE synthesis before finite playback;
 * prefetch the following sentence on the independent TTS worker. The live
 * PCM ring below is used only by the opt-in same-sentence streaming mode. */
#define ROBOT_VOICE_TTS_STREAM_RING_BYTES (256 * 1024)
#if (ROBOT_VOICE_TTS_READY_SLOTS + 1) * ROBOT_VOICE_TTS_STREAM_RING_BYTES > ROBOT_VOICE_TTS_QUEUE_BYTES
#  error TTS stream rings exceed the configured PCM budget
#endif
/* Live mode bounds each sentence's total PCM by the same limit and feeds
 * fixed rings as chunks arrive; a failed partial stream may have been heard.
 * 2 MiB is about 43.7 seconds at 24 kHz mono PCM16; an oversized sentence
 * fails on overflow. Pipeline segments additionally have the smaller
 * TTS_SENTENCE_PCM limit. Music retains the streaming 512 KiB ring. */
#ifndef ROBOT_VOICE_TTS_CACHE_BYTES
#  define ROBOT_VOICE_TTS_CACHE_BYTES (2 * 1024 * 1024)
#endif
#ifndef ROBOT_VOICE_LOG_VERBOSE
#  define ROBOT_VOICE_LOG_VERBOSE    0
#endif
#define ROBOT_VOICE_TTS_SSE_MAX      (256 * 1024)

/* ASR/normal JSON responses remain small. */
#define ROBOT_VOICE_HTTP_RESPONSE    (256 * 1024)
#define ROBOT_VOICE_MAX_BODY         (2 * 1024 * 1024)

/* TTS embeds WAV PCM as base64 inside JSON.  A 20-second, 24-kHz mono s16 WAV
 * is roughly 960 KiB PCM / 1.28 MiB base64.  Keep TTS capacity separate from
 * the normal response buffer so ASR does not permanently pay this cost.
 *
 * Kept for the opt-in legacy WAV fallback (TTS_STREAM=0). The default SSE
 * PCM16 path never allocates this whole-response buffer.
 */
#define ROBOT_VOICE_TTS_RESPONSE     (2 * 1024 * 1024)

#endif
