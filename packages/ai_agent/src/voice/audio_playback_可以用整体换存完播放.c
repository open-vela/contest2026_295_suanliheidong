/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* audio_playback.c - PCM playback via board I2S or media_player. */

#include "voice/audio_playback.h"
#include "agent_config.h"

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
#include <arch/board/board.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/clock.h>
#include <nuttx/semaphore.h>
#endif

#include <errno.h>
#include <media_player.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

#define PB_OPTIONS_LEN 128

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO

/*
 * MiMo TTS output verified externally:
 *   pcm_s16le / 24000 Hz / mono / 16 bit.
 */
#define BOARD_I2S_TTS_SAMPLE_RATE          24000
#define BOARD_I2S_TTS_CHANNELS             1
#define BOARD_I2S_TTS_BITS_PER_SAMPLE      16

/*
 * One 2048-byte block = 1024 mono PCM16 samples ~= 42.7 ms at 24 kHz.
 *
 * The producer is the HTTP/TTS decoder and can arrive in bursts.  The
 * playback worker is the only thread that submits APBs to I2S.  This cleanly
 * decouples network jitter from the real-time DMA cadence.
 */
/*
 * TTS source is mono PCM16.  Do NOT rely on ESP32-S3 TX_MONO/CHAN_EQUAL for
 * speaker playback: explicitly duplicate each mono sample to L/R and run the
 * wire in ordinary stereo mode.  This removes ambiguity in mono slot packing
 * and prevents the audible delayed/ghost copy seen with the hardware mono
 * path.
 *
 * 1024 source bytes = 512 mono frames ~= 21.3 ms at 24 kHz.
 * After L/R duplication each DMA APB is exactly 2048 bytes.
 */
#define BOARD_I2S_PLAYBACK_BLOCK_BYTES     1024
#define BOARD_I2S_WIRE_CHANNELS            2
#define BOARD_I2S_WIRE_BLOCK_BYTES             (BOARD_I2S_PLAYBACK_BLOCK_BYTES * BOARD_I2S_WIRE_CHANNELS)
#define BOARD_I2S_PLAYBACK_MAX_INFLIGHT    2
#define BOARD_I2S_PLAYBACK_TIMEOUT_MS      1000

/*
 * Network-isolated playback mode.
 *
 * The current board is clean when the HTTPS/TLS/WLAN transfer has already
 * finished, but produces audible drag/ghosting while TTS network traffic is
 * still active.  Keep decoding TTS incrementally, but defer I2S playback until
 * EOS whenever the utterance fits in this ring.
 *
 * 1 MiB stores about 21.8 seconds of pcm_s16le / 24 kHz / mono audio:
 *
 *   1048576 / (24000 * 2) ~= 21.8 s
 *
 * BOARD_I2S_START_PREBUFFER_BYTES intentionally equals the whole ring.  The
 * wait helper also returns on EOS, so normal utterances start immediately
 * after the TTS HTTP stream finishes.  If an unusually long utterance fills
 * the ring before EOS, playback starts at the full-ring threshold instead of
 * deadlocking the network producer.
 *
 * After any real playback underrun, wait for 64 KiB (~1.36 s) before resuming
 * so the I2S side is not repeatedly restarted on short producer bursts.
 */
#define BOARD_I2S_PCM_RING_BYTES           (1024 * 1024)
#define BOARD_I2S_START_PREBUFFER_BYTES    BOARD_I2S_PCM_RING_BYTES
#define BOARD_I2S_REBUFFER_BYTES           (64 * 1024)
#define BOARD_I2S_NETWORK_ISOLATED         1

#define BOARD_I2S_WORKER_STACK             4096
#define BOARD_I2S_WORKER_PRIORITY          120

enum board_i2s_play_state {
    BOARD_I2S_STATE_PREBUFFER = 0,
    BOARD_I2S_STATE_PLAYING,
    BOARD_I2S_STATE_REBUFFER,
    BOARD_I2S_STATE_DRAINING,
    BOARD_I2S_STATE_STOPPED,
};

struct board_i2s_ring {
    unsigned char data[BOARD_I2S_PCM_RING_BYTES];
    size_t head;
    size_t tail;
    size_t count;
};

struct audio_playback;

struct board_i2s_slot {
    struct audio_playback* pb;
    sem_t done;
    int result;
    int inflight;
    unsigned int index;
    uint32_t submit_count;
    uint32_t done_count;
};

#endif

enum audio_playback_backend {
    AUDIO_PLAYBACK_BACKEND_NONE = 0,
    AUDIO_PLAYBACK_BACKEND_MEDIA_PLAYER,
#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
    AUDIO_PLAYBACK_BACKEND_BOARD_I2S,
#endif
};

struct audio_playback {
    enum audio_playback_backend backend;
    void* player;
    size_t total_written;
    int bytes_per_frame;
    volatile int stopped;

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
    struct i2s_dev_s* i2s;

    sem_t i2s_done;
    sem_t ring_lock;
    sem_t ring_data;
    sem_t ring_space;

    pthread_t worker;
    int worker_started;
    int worker_exited;

    volatile int i2s_inflight;
    int i2s_result;
    int i2s_started;

    int eos;
    enum board_i2s_play_state state;
    uint32_t underrun_count;

    struct board_i2s_ring ring;
    struct board_i2s_slot slots[BOARD_I2S_PLAYBACK_MAX_INFLIGHT];
#endif
};

static audio_playback_t* s_active_playback;

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO

static int board_i2s_lock(audio_playback_t* pb)
{
    return nxsem_wait_uninterruptible(&pb->ring_lock);
}

static void board_i2s_unlock(audio_playback_t* pb)
{
    nxsem_post(&pb->ring_lock);
}

static size_t board_i2s_ring_count(audio_playback_t* pb)
{
    size_t count;

    if (board_i2s_lock(pb) < 0) {
        return pb->ring.count;
    }

    count = pb->ring.count;
    board_i2s_unlock(pb);
    return count;
}

static size_t board_i2s_ring_space(audio_playback_t* pb)
{
    size_t space;

    if (board_i2s_lock(pb) < 0) {
        return BOARD_I2S_PCM_RING_BYTES - pb->ring.count;
    }

    space = BOARD_I2S_PCM_RING_BYTES - pb->ring.count;
    board_i2s_unlock(pb);
    return space;
}

static int board_i2s_get_inflight(audio_playback_t* pb)
{
    int value;

    if (board_i2s_lock(pb) < 0) {
        return pb->i2s_inflight;
    }

    value = pb->i2s_inflight;
    board_i2s_unlock(pb);
    return value;
}

static int board_i2s_get_result(audio_playback_t* pb)
{
    int value;

    if (board_i2s_lock(pb) < 0) {
        return pb->i2s_result;
    }

    value = pb->i2s_result;
    board_i2s_unlock(pb);
    return value;
}

static int board_i2s_ring_write(audio_playback_t* pb,
    const unsigned char* src, size_t len)
{
    size_t done = 0;

    while (done < len) {
        size_t n;
        size_t first;
        int ret;

        if (pb->stopped) {
            return -ECANCELED;
        }

        ret = board_i2s_lock(pb);
        if (ret < 0) {
            return ret;
        }

        if (pb->ring.count == BOARD_I2S_PCM_RING_BYTES) {
            board_i2s_unlock(pb);
            ret = nxsem_wait_uninterruptible(&pb->ring_space);
            if (ret < 0) {
                return ret;
            }
            continue;
        }

        n = BOARD_I2S_PCM_RING_BYTES - pb->ring.count;
        if (n > len - done) {
            n = len - done;
        }

        first = BOARD_I2S_PCM_RING_BYTES - pb->ring.head;
        if (first > n) {
            first = n;
        }

        memcpy(pb->ring.data + pb->ring.head, src + done, first);
        pb->ring.head = (pb->ring.head + first) % BOARD_I2S_PCM_RING_BYTES;
        pb->ring.count += first;
        done += first;

        if (first < n) {
            size_t second = n - first;

            memcpy(pb->ring.data + pb->ring.head, src + done, second);
            pb->ring.head = (pb->ring.head + second) % BOARD_I2S_PCM_RING_BYTES;
            pb->ring.count += second;
            done += second;
        }

        board_i2s_unlock(pb);
        nxsem_post(&pb->ring_data);
    }

    return OK;
}

static size_t board_i2s_ring_read(audio_playback_t* pb,
    unsigned char* dst, size_t wanted)
{
    size_t n;
    size_t first;

    if (board_i2s_lock(pb) < 0) {
        return 0;
    }

    n = pb->ring.count;
    if (n > wanted) {
        n = wanted;
    }

    first = BOARD_I2S_PCM_RING_BYTES - pb->ring.tail;
    if (first > n) {
        first = n;
    }

    if (first > 0) {
        memcpy(dst, pb->ring.data + pb->ring.tail, first);
        pb->ring.tail = (pb->ring.tail + first) % BOARD_I2S_PCM_RING_BYTES;
        pb->ring.count -= first;
    }

    if (first < n) {
        size_t second = n - first;

        memcpy(dst + first, pb->ring.data + pb->ring.tail, second);
        pb->ring.tail = (pb->ring.tail + second) % BOARD_I2S_PCM_RING_BYTES;
        pb->ring.count -= second;
    }

    board_i2s_unlock(pb);

    if (n > 0) {
        nxsem_post(&pb->ring_space);
    }

    return n;
}

static void board_i2s_playback_done(struct i2s_dev_s* dev,
    struct ap_buffer_s* apb, void* arg, int result)
{
    struct board_i2s_slot* slot = arg;
    audio_playback_t* pb;

    (void)dev;
    (void)apb;

    if (!slot || !slot->pb) {
        return;
    }

    pb = slot->pb;

    if (board_i2s_lock(pb) == 0) {
        slot->result = result;
        slot->inflight = 0;
        slot->done_count++;

        if (result < 0 && result != -ECANCELED && pb->i2s_result >= 0) {
            pb->i2s_result = result;
        }

        if (pb->i2s_inflight > 0) {
            pb->i2s_inflight--;
        }

        board_i2s_unlock(pb);
    } else {
        slot->result = result;
        slot->inflight = 0;
        slot->done_count++;

        if (result < 0 && result != -ECANCELED && pb->i2s_result >= 0) {
            pb->i2s_result = result;
        }

        if (pb->i2s_inflight > 0) {
            pb->i2s_inflight--;
        }
    }

    /*
     * Each logical TX slot has its own completion semaphore.  This mirrors the
     * known-good voice_echo playback path and prevents stale global completion
     * tokens from changing the refill order.
     */
    nxsem_post(&slot->done);
    nxsem_post(&pb->i2s_done);
}

static int board_i2s_wait_slot_done(audio_playback_t* pb,
    struct board_i2s_slot* slot)
{
    int ret;

    if (!pb || !slot) {
        return -EINVAL;
    }

    for (;;) {
        int inflight;
        int result;

        ret = board_i2s_lock(pb);
        if (ret < 0) {
            return ret;
        }

        inflight = slot->inflight;
        result = slot->result;
        board_i2s_unlock(pb);

        if (!inflight) {
            if (result < 0 && result != -ECANCELED) {
                return result;
            }
            return OK;
        }

        if (pb->stopped) {
            return -ECANCELED;
        }

        ret = nxsem_wait_uninterruptible(&slot->done);
        if (ret < 0) {
            return ret;
        }
    }
}

static int board_i2s_queue_slot(audio_playback_t* pb,
    struct board_i2s_slot* slot, const void* buf, size_t len)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_s* apb = NULL;
    const unsigned char* src = buf;
    unsigned char* dst;
    size_t frames;
    size_t tx_len;
    size_t i;
    int ret;

    if (!pb || !slot || !buf ||
        len == 0 || len > BOARD_I2S_PLAYBACK_BLOCK_BYTES) {
        return -EINVAL;
    }

    /*
     * The input ring contains MiMo's verified mono PCM16 stream.
     * Keep source frame alignment before expanding it for the physical I2S
     * wire.
     */
    len -= len % (size_t)pb->bytes_per_frame;
    if (len == 0 || pb->bytes_per_frame != 2) {
        return -EINVAL;
    }

    frames = len / 2;
    tx_len = frames * 2 * BOARD_I2S_WIRE_CHANNELS;
    if (tx_len == 0 || tx_len > BOARD_I2S_WIRE_BLOCK_BYTES) {
        return -EINVAL;
    }

    /*
     * A logical slot is never reused until its own callback has completed.
     */
    ret = board_i2s_wait_slot_done(pb, slot);
    if (ret < 0) {
        return ret;
    }

    memset(&desc, 0, sizeof(desc));
    desc.numbytes = tx_len;
    desc.u.pbuffer = &apb;

    ret = apb_alloc(&desc);
    if (ret < 0) {
        return ret;
    }

    /*
     * Explicit mono -> stereo duplication in little-endian byte form:
     *
     *   input : [S0lo S0hi] [S1lo S1hi] ...
     *   output: [S0lo S0hi][S0lo S0hi] [S1lo S1hi][S1lo S1hi] ...
     *
     * Avoid int16_t casts here because 'buf' is not guaranteed to be
     * naturally aligned.
     */
    dst = apb->samp;
    for (i = 0; i < frames; i++) {
        unsigned char lo = src[i * 2];
        unsigned char hi = src[i * 2 + 1];

        dst[i * 4] = lo;
        dst[i * 4 + 1] = hi;
        dst[i * 4 + 2] = lo;
        dst[i * 4 + 3] = hi;
    }

    apb->curbyte = 0;
    apb->nbytes = tx_len;
    apb->nsamples = frames;

    ret = board_i2s_lock(pb);
    if (ret < 0) {
        apb_free(apb);
        return ret;
    }

    slot->result = -EINPROGRESS;
    slot->inflight = 1;
    slot->submit_count++;
    pb->i2s_inflight++;
    board_i2s_unlock(pb);

    ret = I2S_SEND(pb->i2s, apb, board_i2s_playback_done, slot,
        MSEC2TICK(BOARD_I2S_PLAYBACK_TIMEOUT_MS));
    if (ret < 0) {
        if (board_i2s_lock(pb) == 0) {
            slot->result = ret;
            slot->inflight = 0;

            if (pb->i2s_inflight > 0) {
                pb->i2s_inflight--;
            }

            if (ret != -EBUSY && pb->i2s_result >= 0) {
                pb->i2s_result = ret;
            }

            board_i2s_unlock(pb);
        }

        nxsem_post(&slot->done);
        apb_free(apb);
        return ret;
    }

    /*
     * contest_i2s_send() takes its own APB reference before returning.
     */
    apb_free(apb);
    return OK;
}

static int board_i2s_drain(audio_playback_t* pb)
{
    int result = OK;
    int ret;

    for (;;) {
        ret = board_i2s_get_result(pb);
        if (ret < 0 && result >= 0) {
            result = ret;
        }

        if (board_i2s_get_inflight(pb) == 0) {
            return result;
        }

        ret = nxsem_wait_uninterruptible(&pb->i2s_done);
        if (ret < 0) {
            return ret;
        }
    }
}

static int board_i2s_wait_buffer(audio_playback_t* pb, size_t threshold)
{
    for (;;) {
        size_t available = board_i2s_ring_count(pb);

        if (available >= threshold) {
            return OK;
        }

        if (pb->eos || pb->stopped) {
            return OK;
        }

        if (board_i2s_get_result(pb) < 0) {
            return board_i2s_get_result(pb);
        }

        if (nxsem_wait_uninterruptible(&pb->ring_data) < 0) {
            return -EINTR;
        }
    }
}

static void* board_i2s_worker(void* arg)
{
    audio_playback_t* pb = arg;
    unsigned char block[BOARD_I2S_PLAYBACK_BLOCK_BYTES];
    unsigned int slot_index = 0;
    int primed_slots = 0;
    int ret = OK;

    syslog(LOG_INFO,
        "[%s] playback worker started ring=%u start_prebuffer=%u "
        "rebuffer=%u source_block=%u wire=stereo-dup cadence=strict-dual-slot "
        "network_isolated=%u\n",
        TAG, BOARD_I2S_PCM_RING_BYTES, BOARD_I2S_START_PREBUFFER_BYTES,
        BOARD_I2S_REBUFFER_BYTES, BOARD_I2S_PLAYBACK_BLOCK_BYTES,
        BOARD_I2S_NETWORK_ISOLATED);

    pb->state = BOARD_I2S_STATE_PREBUFFER;

    while (!pb->stopped) {
        struct board_i2s_slot* slot;
        size_t available;
        size_t want;
        size_t got;

        ret = board_i2s_get_result(pb);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] I2S worker sees error: %d\n", TAG, ret);
            break;
        }

        if (pb->state == BOARD_I2S_STATE_PREBUFFER ||
            pb->state == BOARD_I2S_STATE_REBUFFER) {
            size_t threshold = pb->state == BOARD_I2S_STATE_PREBUFFER ?
                BOARD_I2S_START_PREBUFFER_BYTES :
                BOARD_I2S_REBUFFER_BYTES;
            const char* from = pb->state == BOARD_I2S_STATE_PREBUFFER ?
                "PREBUFFER" : "REBUFFER";

            ret = board_i2s_wait_buffer(pb, threshold);
            if (ret < 0) {
                break;
            }

            available = board_i2s_ring_count(pb);
            if (available >= (size_t)pb->bytes_per_frame) {
                pb->state = BOARD_I2S_STATE_PLAYING;
                primed_slots = 0;
                slot_index = 0;
                syslog(LOG_INFO,
                    "[%s] playback %s -> PLAYING buffered=%zu inflight=%d\n",
                    TAG, from, available, board_i2s_get_inflight(pb));
            } else if (pb->eos) {
                pb->state = BOARD_I2S_STATE_DRAINING;
            } else {
                continue;
            }
        }

        if (pb->state == BOARD_I2S_STATE_DRAINING) {
            break;
        }

        slot = &pb->slots[slot_index];

        /*
         * After initial priming, wait for exactly the logical slot that is
         * about to be refilled.  This keeps the submit order:
         *   slot0, slot1, wait0, refill0, wait1, refill1, ...
         * identical to the known-good voice_echo playback implementation.
         */
        if (primed_slots >= BOARD_I2S_PLAYBACK_MAX_INFLIGHT) {
            ret = board_i2s_wait_slot_done(pb, slot);
            if (ret < 0) {
                break;
            }
        }

        available = board_i2s_ring_count(pb);

        if (available >= BOARD_I2S_PLAYBACK_BLOCK_BYTES) {
            want = BOARD_I2S_PLAYBACK_BLOCK_BYTES;
        } else if (pb->eos && available > 0) {
            want = available - (available % (size_t)pb->bytes_per_frame);
            if (want == 0) {
                syslog(LOG_WARNING,
                    "[%s] dropping %zu trailing PCM byte(s) at EOS\n",
                    TAG, available);
                (void)board_i2s_ring_read(pb, block, available);
                continue;
            }
        } else if (pb->eos && available == 0) {
            pb->state = BOARD_I2S_STATE_DRAINING;
            break;
        } else {
            /*
             * During normal playback, do not consume a partial block.  If the
             * producer is briefly behind but another slot is still active, wait
             * for more ring data.  If both slots have drained, enter REBUFFER.
             */
            if (board_i2s_get_inflight(pb) > 0) {
                ret = nxsem_wait_uninterruptible(&pb->ring_data);
                if (ret < 0) {
                    break;
                }
                continue;
            }

            pb->underrun_count++;
            pb->state = BOARD_I2S_STATE_REBUFFER;
            syslog(LOG_WARNING,
                "[%s] I2S underrun #%lu: buffered=%zu; enter REBUFFER "
                "until %u bytes\n",
                TAG, (unsigned long)pb->underrun_count, available,
                BOARD_I2S_REBUFFER_BYTES);
            continue;
        }

        got = board_i2s_ring_read(pb, block, want);
        if (got == 0) {
            continue;
        }

        ret = board_i2s_queue_slot(pb, slot, block, got);
        if (ret < 0) {
            if (ret != -ECANCELED) {
                syslog(LOG_ERR,
                    "[%s] I2S queue slot=%u failed: %d\n",
                    TAG, slot_index, ret);
            }
            break;
        }

        if (primed_slots < BOARD_I2S_PLAYBACK_MAX_INFLIGHT) {
            primed_slots++;
        }

        slot_index = (slot_index + 1) % BOARD_I2S_PLAYBACK_MAX_INFLIGHT;
    }

    /*
     * Wait for each logical slot rather than consuming a global semaphore.
     */
    {
        unsigned int i;

        for (i = 0; i < BOARD_I2S_PLAYBACK_MAX_INFLIGHT; i++) {
            int slot_ret = board_i2s_wait_slot_done(pb, &pb->slots[i]);

            if (slot_ret < 0 && slot_ret != -ECANCELED && ret >= 0) {
                ret = slot_ret;
            }
        }
    }

    if (!pb->stopped) {
        int drain_ret = board_i2s_drain(pb);
        if (drain_ret < 0 && ret >= 0) {
            ret = drain_ret;
            syslog(LOG_ERR, "[%s] I2S drain failed: %d\n", TAG, ret);
        }
    }

    if (board_i2s_lock(pb) == 0) {
        if (ret < 0 && ret != -ECANCELED && pb->i2s_result >= 0) {
            pb->i2s_result = ret;
        }
        pb->worker_exited = 1;
        pb->state = BOARD_I2S_STATE_STOPPED;
        board_i2s_unlock(pb);
    } else {
        pb->worker_exited = 1;
        pb->state = BOARD_I2S_STATE_STOPPED;
    }

    syslog(LOG_INFO,
        "[%s] playback worker exit buffered=%zu inflight=%d underruns=%lu "
        "result=%d eos=%d stopped=%d slots=%lu/%lu,%lu/%lu\n",
        TAG, board_i2s_ring_count(pb), board_i2s_get_inflight(pb),
        (unsigned long)pb->underrun_count, board_i2s_get_result(pb),
        pb->eos, pb->stopped,
        (unsigned long)pb->slots[0].done_count,
        (unsigned long)pb->slots[0].submit_count,
        (unsigned long)pb->slots[1].done_count,
        (unsigned long)pb->slots[1].submit_count);

    nxsem_post(&pb->i2s_done);
    return NULL;
}

static int open_board_i2s_playback(audio_playback_t* pb,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    pthread_attr_t attr;
    struct sched_param param;
    int actual;
    int ret;

    if (sample_rate != BOARD_I2S_TTS_SAMPLE_RATE ||
        channels != BOARD_I2S_TTS_CHANNELS ||
        bits_per_sample != BOARD_I2S_TTS_BITS_PER_SAMPLE) {
        syslog(LOG_ERR,
            "[%s] unsupported contest PCM format: %uHz %uch %ubit; "
            "expected pcm_s16le %uHz %uch %ubit\n",
            TAG, sample_rate, channels, bits_per_sample,
            BOARD_I2S_TTS_SAMPLE_RATE, BOARD_I2S_TTS_CHANNELS,
            BOARD_I2S_TTS_BITS_PER_SAMPLE);
        return -ENOTSUP;
    }

    pb->i2s = board_voice_speaker_i2s();
    if (!pb->i2s) {
        return -ENODEV;
    }

    /*
     * Source remains mono, but use a conventional two-slot I2S wire.  Samples
     * are duplicated explicitly in board_i2s_queue_slot().
     */
    ret = I2S_TXCHANNELS(pb->i2s, BOARD_I2S_WIRE_CHANNELS);
    if (ret < 0) {
        return ret;
    }

    actual = (int)I2S_TXSAMPLERATE(pb->i2s, sample_rate);
    if (actual != (int)sample_rate) {
        return actual < 0 ? actual : -ENOTSUP;
    }

    actual = (int)I2S_TXDATAWIDTH(pb->i2s, bits_per_sample);
    if (actual != (int)bits_per_sample) {
        return actual < 0 ? actual : -ENOTSUP;
    }

    ret = I2S_IOCTL(pb->i2s, AUDIOIOC_START, 0);
    if (ret < 0) {
        return ret;
    }

    pb->i2s_started = 1;

    ret = nxsem_init(&pb->i2s_done, 0, 0);
    if (ret < 0) {
        goto fail_stop;
    }

    ret = nxsem_init(&pb->ring_lock, 0, 1);
    if (ret < 0) {
        goto fail_done;
    }

    ret = nxsem_init(&pb->ring_data, 0, 0);
    if (ret < 0) {
        goto fail_lock;
    }

    ret = nxsem_init(&pb->ring_space, 0, 0);
    if (ret < 0) {
        goto fail_data;
    }

    {
        unsigned int i;

        for (i = 0; i < BOARD_I2S_PLAYBACK_MAX_INFLIGHT; i++) {
            ret = nxsem_init(&pb->slots[i].done, 0, 0);
            if (ret < 0) {
                while (i > 0) {
                    i--;
                    nxsem_destroy(&pb->slots[i].done);
                }
                goto fail_space;
            }

            pb->slots[i].pb = pb;
            pb->slots[i].result = OK;
            pb->slots[i].inflight = 0;
            pb->slots[i].index = i;
            pb->slots[i].submit_count = 0;
            pb->slots[i].done_count = 0;
        }
    }

    pb->backend = AUDIO_PLAYBACK_BACKEND_BOARD_I2S;
    pb->i2s_result = OK;
    pb->i2s_inflight = 0;
    pb->eos = 0;
    pb->worker_started = 0;
    pb->worker_exited = 0;
    pb->underrun_count = 0;
    pb->state = BOARD_I2S_STATE_PREBUFFER;
    memset(&pb->ring, 0, sizeof(pb->ring));

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, BOARD_I2S_WORKER_STACK);

    memset(&param, 0, sizeof(param));
    param.sched_priority = BOARD_I2S_WORKER_PRIORITY;
    pthread_attr_setschedparam(&attr, &param);

    ret = pthread_create(&pb->worker, &attr, board_i2s_worker, pb);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        ret = -ret;
        goto fail_space;
    }

    pb->worker_started = 1;

    syslog(LOG_INFO,
        "[%s] opened contest I2S playback "
        "(source=pcm_s16le %uHz %uch %ubit, wire=%uch duplicated, "
        "ring=%u, start_prebuffer=%u, rebuffer=%u, source_block=%u, "
        "wire_block=%u, network_isolated=%u)\n",
        TAG, sample_rate, channels, bits_per_sample,
        BOARD_I2S_WIRE_CHANNELS, BOARD_I2S_PCM_RING_BYTES,
        BOARD_I2S_START_PREBUFFER_BYTES, BOARD_I2S_REBUFFER_BYTES,
        BOARD_I2S_PLAYBACK_BLOCK_BYTES, BOARD_I2S_WIRE_BLOCK_BYTES,
        BOARD_I2S_NETWORK_ISOLATED);
    return OK;

fail_space:
    nxsem_destroy(&pb->ring_space);
fail_data:
    nxsem_destroy(&pb->ring_data);
fail_lock:
    nxsem_destroy(&pb->ring_lock);
fail_done:
    nxsem_destroy(&pb->i2s_done);
fail_stop:
    pb->i2s_started = 0;
    I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
    pb->i2s = NULL;
    pb->backend = AUDIO_PLAYBACK_BACKEND_NONE;
    return ret;
}

static int board_i2s_feed(audio_playback_t* pb, const void* buf, size_t len)
{
    if (!pb || !buf || len == 0) {
        return -EINVAL;
    }

    /*
     * Preserve every byte across HTTP/Base64 callback boundaries.  The ring is
     * byte-oriented; only the worker rounds the true end-of-stream down to a
     * complete PCM frame.
     */
    return board_i2s_ring_write(pb, buf, len);
}

static void board_i2s_signal_all(audio_playback_t* pb)
{
    unsigned int i;

    /*
     * Wake every possible waiter.  Posting more than once is harmless and
     * avoids shutdown deadlocks if producer, worker and drain are sleeping on
     * different events.
     */
    nxsem_post(&pb->ring_data);
    nxsem_post(&pb->ring_space);
    nxsem_post(&pb->i2s_done);

    for (i = 0; i < BOARD_I2S_PLAYBACK_MAX_INFLIGHT; i++) {
        nxsem_post(&pb->slots[i].done);
    }
}

static int board_i2s_finish(audio_playback_t* pb)
{
    int ret = OK;

    pb->eos = 1;
    board_i2s_signal_all(pb);

    if (pb->worker_started) {
        void* value = NULL;
        int join_ret = pthread_join(pb->worker, &value);

        if (join_ret != 0 && ret >= 0) {
            ret = -join_ret;
        }

        pb->worker_started = 0;
    }

    if (board_i2s_get_result(pb) < 0 && ret >= 0) {
        ret = board_i2s_get_result(pb);
    }

    return ret;
}

#endif /* CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO */

audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    audio_playback_t* pb;

    (void)dev_path;

    if (s_active_playback) {
        syslog(LOG_WARNING, "[%s] force closing stale playback\n", TAG);
        audio_playback_close(s_active_playback);
        usleep(100000);
    }

    pb = calloc(1, sizeof(*pb));
    if (!pb) {
        return NULL;
    }

    pb->bytes_per_frame = (bits_per_sample / 8) * channels;
    if (pb->bytes_per_frame == 0) {
        free(pb);
        return NULL;
    }

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
    if (open_board_i2s_playback(pb, sample_rate, channels,
            bits_per_sample) == 0) {
        s_active_playback = pb;
        return pb;
    }
#endif

    pb->player = media_player_open(MEDIA_STREAM_MUSIC);
    if (!pb->player) {
        syslog(LOG_ERR, "[%s] media_player_open failed\n", TAG);
        free(pb);
        return NULL;
    }

    {
        char opts[PB_OPTIONS_LEN];
        int ret;

        snprintf(opts, sizeof(opts),
            "format=s%ule:sample_rate=%u:ch_layout=%s",
            bits_per_sample, sample_rate,
            (channels == 1) ? "mono" : "stereo");

        ret = media_player_prepare(pb->player, NULL, opts);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] prepare failed: %d\n", TAG, ret);
            media_player_close(pb->player, 0);
            free(pb);
            return NULL;
        }

        ret = media_player_start(pb->player);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] start failed: %d\n", TAG, ret);
            media_player_close(pb->player, 0);
            free(pb);
            return NULL;
        }
    }

    pb->backend = AUDIO_PLAYBACK_BACKEND_MEDIA_PLAYER;
    s_active_playback = pb;
    syslog(LOG_INFO, "[%s] opened media playback (%uHz %uch %ubit)\n",
        TAG, sample_rate, channels, bits_per_sample);
    return pb;
}

int audio_playback_write(audio_playback_t* pb, const void* buf, size_t len)
{
    if (!pb || !buf || len == 0) {
        return -EINVAL;
    }

    if (pb->stopped) {
        return -ECANCELED;
    }

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
    if (pb->backend == AUDIO_PLAYBACK_BACKEND_BOARD_I2S) {
        int ret = board_i2s_feed(pb, buf, len);

        if (ret < 0) {
            return ret;
        }

        pb->total_written += len;
        return (int)len;
    }
#endif

    if (pb->backend != AUDIO_PLAYBACK_BACKEND_MEDIA_PLAYER || !pb->player) {
        return -EINVAL;
    }

    {
        ssize_t n = media_player_write_data(pb->player, buf, len);

        if (n > 0) {
            pb->total_written += (size_t)n;
        }

        return (int)n;
    }
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    pb->stopped = 1;

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
    if (pb->backend == AUDIO_PLAYBACK_BACKEND_BOARD_I2S) {
        board_i2s_signal_all(pb);

        if (pb->i2s && pb->i2s_started) {
            pb->i2s_started = 0;
            I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
            board_i2s_signal_all(pb);
        }

        if (pb->worker_started) {
            void* value = NULL;
            pthread_join(pb->worker, &value);
            pb->worker_started = 0;
        }

        board_i2s_drain(pb);
        return;
    }
#endif

    if (pb->backend == AUDIO_PLAYBACK_BACKEND_MEDIA_PLAYER && pb->player) {
        media_player_stop(pb->player);
    }
}

void audio_playback_close(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    syslog(LOG_INFO, "[%s] closing (%zu bytes written)\n",
        TAG, pb->total_written);

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
    if (pb->backend == AUDIO_PLAYBACK_BACKEND_BOARD_I2S) {
        if (pb->i2s) {
            int ret = OK;

            if (!pb->stopped) {
                ret = board_i2s_finish(pb);
                if (ret < 0) {
                    syslog(LOG_ERR,
                        "[%s] ring playback finish failed: %d\n", TAG, ret);
                }
            } else if (pb->worker_started) {
                void* value = NULL;
                board_i2s_signal_all(pb);
                pthread_join(pb->worker, &value);
                pb->worker_started = 0;
            }

            /*
             * board_i2s_finish() drains normal EOS playback.  For stop/abort,
             * AUDIOIOC_STOP above forces callbacks and drain waits them out.
             */
            if (pb->i2s_started) {
                pb->i2s_started = 0;
                I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
            }

            board_i2s_drain(pb);

            syslog(LOG_INFO,
                "[%s] ring playback summary: underruns=%lu buffered=%zu "
                "inflight=%d result=%d eos=%d stopped=%d\n",
                TAG, (unsigned long)pb->underrun_count,
                board_i2s_ring_count(pb), board_i2s_get_inflight(pb),
                board_i2s_get_result(pb), pb->eos, pb->stopped);

            {
                unsigned int i;

                for (i = 0; i < BOARD_I2S_PLAYBACK_MAX_INFLIGHT; i++) {
                    nxsem_destroy(&pb->slots[i].done);
                }
            }

            nxsem_destroy(&pb->ring_space);
            nxsem_destroy(&pb->ring_data);
            nxsem_destroy(&pb->ring_lock);
            nxsem_destroy(&pb->i2s_done);
            pb->i2s = NULL;
        }
    }
#endif

    if (pb->backend == AUDIO_PLAYBACK_BACKEND_MEDIA_PLAYER && pb->player) {
        media_player_stop(pb->player);
        usleep(50 * 1000);
        media_player_close(pb->player, 0);
        pb->player = NULL;
    }

    if (s_active_playback == pb) {
        s_active_playback = NULL;
    }

    free(pb);
}
