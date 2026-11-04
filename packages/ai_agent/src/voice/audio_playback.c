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

/* Reuse the contest board's I2S1 speaker path when the standalone voice
 * echo demo is not selected. */
#if defined(CONFIG_CONTEST_BOARD_I2S1_TX) && \
    !defined(CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO)
#define CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO 1
#endif

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
#include <arch/board/board.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/clock.h>
#include <nuttx/semaphore.h>
#endif

#include <errno.h>
#include <media_player.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

#define PB_OPTIONS_LEN 128

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
#define BOARD_I2S_PLAYBACK_BLOCK_BYTES 2048
#define BOARD_I2S_PLAYBACK_TIMEOUT_MS 1000
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
    int i2s_result;
    int i2s_pending;
    int i2s_started;
#endif
};

static audio_playback_t* s_active_playback;

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO

static void board_i2s_playback_done(struct i2s_dev_s* dev,
    struct ap_buffer_s* apb, void* arg, int result)
{
    audio_playback_t* pb = arg;

    (void)dev;
    (void)apb;
    pb->i2s_result = result;
    pb->i2s_pending = 0;
    nxsem_post(&pb->i2s_done);
}

static int open_board_i2s_playback(audio_playback_t* pb,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    int actual;
    int ret;

    if (channels != 1 || bits_per_sample != 16) {
        return -ENOTSUP;
    }

    pb->i2s = board_voice_speaker_i2s();
    if (!pb->i2s) {
        return -ENODEV;
    }

    ret = I2S_TXCHANNELS(pb->i2s, channels);
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

    nxsem_init(&pb->i2s_done, 0, 0);
    pb->backend = AUDIO_PLAYBACK_BACKEND_BOARD_I2S;
    pb->i2s_started = 1;
    syslog(LOG_INFO, "[%s] opened contest I2S playback (%uHz %uch %ubit)\n",
        TAG, sample_rate, channels, bits_per_sample);
    return 0;
}

static int board_i2s_write_block(audio_playback_t* pb,
    const void* buf, size_t len)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_s* apb = NULL;
    int ret;

    memset(&desc, 0, sizeof(desc));
    desc.numbytes = len;
    desc.u.pbuffer = &apb;
    ret = apb_alloc(&desc);
    if (ret < 0) {
        return ret;
    }

    memcpy(apb->samp, buf, len);
    apb->nbytes = len;
    apb->nsamples = len / (size_t)pb->bytes_per_frame;

    while (nxsem_trywait(&pb->i2s_done) == 0)
        ;

    pb->i2s_result = -EINPROGRESS;
    pb->i2s_pending = 1;
    ret = I2S_SEND(pb->i2s, apb, board_i2s_playback_done, pb,
        MSEC2TICK(BOARD_I2S_PLAYBACK_TIMEOUT_MS));
    if (ret >= 0) {
        /* STOP from audio_playback_stop() completes this wait safely. */
        ret = nxsem_wait_uninterruptible(&pb->i2s_done);
        if (ret >= 0) {
            ret = pb->i2s_result;
        }
    }

    pb->i2s_pending = 0;
    apb_free(apb);
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

    char opts[PB_OPTIONS_LEN];
    snprintf(opts, sizeof(opts),
        "format=s%ule:sample_rate=%u:ch_layout=%s",
        bits_per_sample, sample_rate,
        (channels == 1) ? "mono" : "stereo");

    int ret = media_player_prepare(pb->player, NULL, opts);
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
        const unsigned char* src = buf;
        size_t total = 0;

        len -= len % (size_t)pb->bytes_per_frame;
        while (total < len) {
            size_t block = len - total;
            int ret;

            if (block > BOARD_I2S_PLAYBACK_BLOCK_BYTES) {
                block = BOARD_I2S_PLAYBACK_BLOCK_BYTES;
            }
            block -= block % (size_t)pb->bytes_per_frame;
            ret = board_i2s_write_block(pb, src + total, block);
            if (ret < 0) {
                return total > 0 ? (int)total : ret;
            }
            total += block;
        }

        pb->total_written += total;
        return (int)total;
    }
#endif

    if (pb->backend != AUDIO_PLAYBACK_BACKEND_MEDIA_PLAYER || !pb->player) {
        return -EINVAL;
    }

    ssize_t n = media_player_write_data(pb->player, buf, len);
    if (n > 0) {
        pb->total_written += (size_t)n;
    }

    return (int)n;
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    pb->stopped = 1;
#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO
    if (pb->backend == AUDIO_PLAYBACK_BACKEND_BOARD_I2S && pb->i2s
        && pb->i2s_started) {
        pb->i2s_started = 0;
        I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
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
        if (pb->i2s && pb->i2s_started) {
            I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
        }
        nxsem_destroy(&pb->i2s_done);
        pb->i2s = NULL;
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
