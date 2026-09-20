/****************************************************************************
 * contest2026_295_suanliheidong/app/voice_echo/voice_echo_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/param.h>

#include <errno.h>
#include <limits.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/clock.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include <arch/board/board.h>

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
#include <voice/voice_channel.h>
#endif

#define VOICE_INPUT_RATE       16000
#define VOICE_OUTPUT_RATE      24000
#define VOICE_SAMPLE_BITS      16
#define VOICE_SLOT_BITS        16
#define VOICE_CHANNELS         1
#define VOICE_ECHO_RECORD_SECONDS 5
#define VOICE_RECORDMEM_SECONDS 3
#define VOICE_DMA_BYTES        2048
#define VOICE_RX_DMA_BYTES     1024
#define VOICE_RX_WARMUP_CHUNKS 2
#define VOICE_TRANSFER_TIMEOUT_MS 1000
#define VOICE_RECORD_TIMEOUT_MS 7000
#define VOICE_DIAGNOSTIC_SECONDS 5
#define VOICE_DIAGNOSTIC_OUTPUT_SAMPLES \
  (VOICE_OUTPUT_RATE * VOICE_DIAGNOSTIC_SECONDS)
#define VOICE_TONE_SAMPLES 32
#define VOICE_TX_SLOT_COUNT 2
#define VOICE_RX_SLOT_COUNT 2
#define VOICE_RX_BOUNDARY_TOP_COUNT 10

enum voice_mode_e
{
  VOICE_MODE_ECHO = 0,
  VOICE_MODE_RECORDONLY,
  VOICE_MODE_RECORDMEM,
  VOICE_MODE_SILENCE,
  VOICE_MODE_TONE,
  VOICE_MODE_RESTONE,
};

struct voice_transfer_s
{
  sem_t done;
  mutex_t lock;
  int result;
  FAR struct ap_buffer_s *apb;
  bool retain_apb;
  bool log_rx;
  bool log_tx;
  bool callback_done;
  bool abandoned;
  unsigned int chunk;
};

struct voice_oled_s
{
  struct lcd_dev_s *dev;
  struct lcd_planeinfo_s plane;
  bool available;
};

struct voice_rx_slot_s
{
  FAR struct ap_buffer_s *apb;
  FAR struct voice_transfer_s *transfer;
  size_t request_bytes;
  unsigned int chunk;
  bool in_flight;
};

struct voice_rx_boundary_s
{
  unsigned int chunk;
  uint32_t delta;
};

struct voice_rx_stats_s
{
  bool have_previous;
  int16_t previous_last;
  uint32_t boundary_min;
  uint32_t boundary_max;
  uint64_t boundary_sum;
  unsigned int boundary_count;
  struct voice_rx_boundary_s top[VOICE_RX_BOUNDARY_TOP_COUNT];
};

struct voice_wav_view_s
{
  uint8_t header[44];
  FAR const uint8_t *pcm;
  size_t pcm_bytes;
  size_t total_bytes;
};

/* 750 Hz at 24 kHz has 32 samples per cycle.  The table is deliberately
 * low-amplitude and the sample index persists across DMA chunks.
 */

static const int16_t g_tone_750hz[VOICE_TONE_SAMPLES] =
{
  0, 780, 1531, 2222, 2828, 3326, 3696, 3923,
  4000, 3923, 3696, 3326, 2828, 2222, 1531, 780,
  0, -780, -1531, -2222, -2828, -3326, -3696, -3923,
  -4000, -3923, -3696, -3326, -2828, -2222, -1531, -780
};

static uint32_t voice_isqrt(uint64_t value);

static uint32_t voice_elapsed_ms(FAR const struct timespec *start,
                                 FAR const struct timespec *end)
{
  int64_t elapsed_ns;

  elapsed_ns = ((int64_t)end->tv_sec - (int64_t)start->tv_sec) *
               NSEC_PER_SEC;
  elapsed_ns += (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
  return elapsed_ns > 0 ? (uint32_t)(elapsed_ns / NSEC_PER_MSEC) : 0;
}

static const uint8_t g_font[][5] =
{
  [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
  ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
  [':'] = {0x00, 0x36, 0x36, 0x00, 0x00},
  ['0'] = {0x3e, 0x51, 0x49, 0x45, 0x3e},
  ['1'] = {0x00, 0x42, 0x7f, 0x40, 0x00},
  ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
  ['3'] = {0x21, 0x41, 0x45, 0x4b, 0x31},
  ['4'] = {0x18, 0x14, 0x12, 0x7f, 0x10},
  ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
  ['6'] = {0x3c, 0x4a, 0x49, 0x49, 0x30},
  ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
  ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
  ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1e},
  ['A'] = {0x7e, 0x11, 0x11, 0x11, 0x7e},
  ['C'] = {0x3e, 0x41, 0x41, 0x41, 0x22},
  ['D'] = {0x7f, 0x41, 0x41, 0x22, 0x1c},
  ['E'] = {0x7f, 0x49, 0x49, 0x49, 0x41},
  ['G'] = {0x3e, 0x41, 0x49, 0x49, 0x7a},
  ['I'] = {0x00, 0x41, 0x7f, 0x41, 0x00},
  ['L'] = {0x7f, 0x40, 0x40, 0x40, 0x40},
  ['N'] = {0x7f, 0x02, 0x04, 0x08, 0x7f},
  ['O'] = {0x3e, 0x41, 0x41, 0x41, 0x3e},
  ['P'] = {0x7f, 0x09, 0x09, 0x09, 0x06},
  ['R'] = {0x7f, 0x09, 0x19, 0x29, 0x46},
  ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},
  ['V'] = {0x1f, 0x20, 0x40, 0x20, 0x1f},
  ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07}
};

static void voice_complete(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                           void *arg, int result)
{
  struct voice_transfer_s *transfer = arg;
  size_t nbytes = apb == NULL ? 0 : apb->nbytes;
  bool abandoned;

  (void)dev;

  if (transfer->log_rx)
    {
      if (transfer->chunk < 2 || result < 0)
        {
          printf("voice_echo: RX callback chunk=%u result=%d bytes=%zu\n",
                 transfer->chunk, result, nbytes);
        }
    }

  if (transfer->log_tx)
    {
      if (transfer->chunk < 2 || result < 0)
        {
          printf("voice_echo: TX callback chunk=%u result=%d bytes=%zu\n",
                 transfer->chunk, result, nbytes);
        }
    }

  nxmutex_lock(&transfer->lock);
  abandoned = transfer->abandoned;

  if (abandoned)
    {
      /* The lower-half still owns one reference and will release it after
       * this callback.  Release the application's original reference here.
       */

      nxmutex_unlock(&transfer->lock);
      if (apb != NULL)
        {
          apb_free(apb);
        }
      nxmutex_destroy(&transfer->lock);
      nxsem_destroy(&transfer->done);
      free(transfer);
      return;
    }

  if (transfer->retain_apb && apb != NULL && result >= 0)
    {
      apb_reference(apb);
      transfer->apb = apb;
    }

  transfer->result = result;
  transfer->callback_done = true;
  nxsem_post(&transfer->done);
  nxmutex_unlock(&transfer->lock);
}

static int voice_wait_transfer(struct voice_transfer_s *transfer,
                               clock_t timeout)
{
  int ret;

  do
    {
      ret = nxsem_tickwait(&transfer->done, timeout);
    }
  while (ret == -EINTR);

  if (ret >= 0)
    {
      nxmutex_lock(&transfer->lock);
      ret = transfer->result;
      nxmutex_unlock(&transfer->lock);
    }

  return ret;
}

static bool voice_abandon_transfer(struct voice_transfer_s *transfer)
{
  nxmutex_lock(&transfer->lock);
  if (transfer->callback_done)
    {
      nxmutex_unlock(&transfer->lock);
      return false;
    }

  transfer->abandoned = true;
  nxmutex_unlock(&transfer->lock);
  return true;
}

static void voice_transfer_init(struct voice_transfer_s *transfer,
                                bool retain_apb, bool log_rx, bool log_tx,
                                unsigned int chunk)
{
  nxsem_init(&transfer->done, 0, 0);
  nxmutex_init(&transfer->lock);
  transfer->result = -EINPROGRESS;
  transfer->apb = NULL;
  transfer->retain_apb = retain_apb;
  transfer->log_rx = log_rx;
  transfer->log_tx = log_tx;
  transfer->callback_done = false;
  transfer->abandoned = false;
  transfer->chunk = chunk;
}

static void voice_transfer_destroy(struct voice_transfer_s *transfer)
{
  nxmutex_lock(&transfer->lock);
  nxmutex_unlock(&transfer->lock);
  nxmutex_destroy(&transfer->lock);
  nxsem_destroy(&transfer->done);
}

static int voice_buffer_alloc(struct ap_buffer_s **apb, size_t nbytes)
{
  struct audio_buf_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.numbytes = nbytes;
  desc.u.pbuffer = apb;
  return apb_alloc(&desc) < 0 ? -ENOMEM : 0;
}

static int voice_configure_mic(struct i2s_dev_s *mic)
{
  int actual;
  int ret;

  ret = I2S_RXCHANNELS(mic, VOICE_CHANNELS);
  printf("voice_echo: MIC set channels ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  actual = (int)I2S_RXSAMPLERATE(mic, VOICE_INPUT_RATE);
  printf("voice_echo: MIC set rate ret=%d\n", actual);
  if (actual != VOICE_INPUT_RATE)
    {
      return actual < 0 ? actual : -ENOTSUP;
    }

  actual = (int)I2S_RXDATAWIDTH(mic, VOICE_SAMPLE_BITS);
  printf("voice_echo: MIC set width ret=%d\n", actual);
  if (actual != VOICE_SAMPLE_BITS)
    {
      return actual < 0 ? actual : -ENOTSUP;
    }

  return 0;
}

static int voice_configure_speaker(struct i2s_dev_s *speaker)
{
  int actual;
  int ret;

  ret = I2S_TXCHANNELS(speaker, VOICE_CHANNELS);
  printf("voice_echo: SPK set channels ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  actual = (int)I2S_TXSAMPLERATE(speaker, VOICE_OUTPUT_RATE);
  printf("voice_echo: SPK set rate ret=%d\n", actual);
  if (actual != VOICE_OUTPUT_RATE)
    {
      return actual < 0 ? actual : -ENOTSUP;
    }

  actual = (int)I2S_TXDATAWIDTH(speaker, VOICE_SAMPLE_BITS);
  printf("voice_echo: SPK set width ret=%d\n", actual);
  if (actual != VOICE_SAMPLE_BITS)
    {
      return actual < 0 ? actual : -ENOTSUP;
    }

  return 0;
}

static void voice_oled_pixel(uint8_t *row, unsigned int x, bool on)
{
  uint8_t mask;

#ifdef CONFIG_LCD_PACKEDMSFIRST
  mask = 0x80 >> (x & 7);
#else
  mask = 1 << (x & 7);
#endif

  if (on)
    {
      row[x >> 3] |= mask;
    }
}

static int voice_oled_show(struct voice_oled_s *oled, const char *line1,
                           const char *line2)
{
  const char *lines[2] = {line1, line2};
  uint8_t row[128 / 8];
  unsigned int y;
  int ret = 0;

  if (!oled->available)
    {
      return 0;
    }

  for (y = 0; y < 64; y++)
    {
      const char *text;
      unsigned int x;
      unsigned int charrow;

      memset(row, 0, sizeof(row));
      text = lines[y / 32];
      charrow = (y % 32) / 3;

      if (charrow < 7)
        {
          for (x = 0; text[x] != '\0' && x < 20; x++)
            {
              uint8_t glyph = (uint8_t)text[x];
              unsigned int col;

              if (glyph >= sizeof(g_font) || glyph < ' ')
                {
                  glyph = ' ';
                }

              for (col = 0; col < 5; col++)
                {
                  if ((g_font[glyph][col] & (1u << charrow)) != 0)
                    {
                      voice_oled_pixel(row, x * 6 + col, true);
                    }
                }
            }
        }

      ret = oled->plane.putrun(oled->dev, y, 0, row, 128);
      if (ret < 0)
        {
          oled->available = false;
          printf("OLED unavailable\n");
          return ret;
        }
    }

  return 0;
}

static void voice_oled_init(struct voice_oled_s *oled)
{
  memset(oled, 0, sizeof(*oled));

  if (board_oled_initialize() < 0)
    {
      printf("OLED unavailable\n");
      return;
    }

  oled->dev = board_oled_getdev();
  if (oled->dev == NULL || oled->dev->getplaneinfo(oled->dev, 0,
                                                    &oled->plane) < 0)
    {
      printf("OLED unavailable\n");
      return;
    }

  oled->available = true;
}

static void voice_rx_slot_release(FAR struct voice_rx_slot_s *slot)
{
  if (slot->transfer != NULL)
    {
      if (slot->transfer->apb != NULL)
        {
          apb_free(slot->transfer->apb);
        }

      voice_transfer_destroy(slot->transfer);
      free(slot->transfer);
    }

  if (slot->apb != NULL)
    {
      apb_free(slot->apb);
    }

  memset(slot, 0, sizeof(*slot));
}

static int voice_rx_slot_submit(struct i2s_dev_s *mic,
                                FAR struct voice_rx_slot_s *slot,
                                size_t request_bytes, unsigned int chunk)
{
  int ret;

  ret = voice_buffer_alloc(&slot->apb, request_bytes);
  if (ret < 0)
    {
      return ret;
    }

  slot->transfer = calloc(1, sizeof(*slot->transfer));
  if (slot->transfer == NULL)
    {
      apb_free(slot->apb);
      slot->apb = NULL;
      return -ENOMEM;
    }

  slot->request_bytes = request_bytes;
  slot->chunk = chunk;
  voice_transfer_init(slot->transfer, true, true, false, chunk);
  if (chunk < 2)
    {
      printf("voice_echo: RX submit chunk=%u bytes=%zu\n", chunk,
             request_bytes);
    }
  ret = I2S_RECEIVE(mic, slot->apb, voice_complete, slot->transfer,
                    MSEC2TICK(VOICE_TRANSFER_TIMEOUT_MS));
  if (chunk < 2 || ret < 0)
    {
      printf("voice_echo: RX submit chunk=%u ret=%d\n", chunk, ret);
    }
  if (ret < 0)
    {
      voice_rx_slot_release(slot);
      return ret;
    }

  slot->in_flight = true;
  return OK;
}

static int voice_rx_slot_wait(FAR struct voice_rx_slot_s *slot,
                              clock_t timeout)
{
  FAR struct voice_transfer_s *transfer = slot->transfer;
  int ret;

  ret = voice_wait_transfer(transfer, timeout);
  if (ret == -ETIMEDOUT && !voice_abandon_transfer(transfer))
    {
      nxmutex_lock(&transfer->lock);
      ret = transfer->result;
      nxmutex_unlock(&transfer->lock);
    }

  if (ret == -ETIMEDOUT)
    {
      /* A later callback owns both APB references and the transfer. */

      slot->transfer = NULL;
      slot->apb = NULL;
      slot->in_flight = false;
    }

  return ret;
}

static int voice_rx_abort(struct i2s_dev_s *mic,
                          FAR struct voice_rx_slot_s *slots)
{
  unsigned int i;
  int stop_ret;

  for (i = 0; i < VOICE_RX_SLOT_COUNT; i++)
    {
      FAR struct voice_rx_slot_s *slot = &slots[i];

      if (!slot->in_flight)
        {
          continue;
        }

      if (voice_abandon_transfer(slot->transfer))
        {
          /* The callback will release both APB references and the context. */

          slot->transfer = NULL;
          slot->apb = NULL;
          slot->in_flight = false;
        }
      else
        {
          voice_rx_slot_release(slot);
        }
    }

  stop_ret = I2S_IOCTL(mic, AUDIOIOC_STOP, 0);
  printf("voice_echo: MIC stop ret=%d\n", stop_ret);
  return stop_ret;
}

static void voice_rx_boundary_add(FAR struct voice_rx_stats_s *stats,
                                  unsigned int chunk, uint32_t delta)
{
  unsigned int i;

  if (stats->boundary_count == 0 || delta < stats->boundary_min)
    {
      stats->boundary_min = delta;
    }

  if (delta > stats->boundary_max)
    {
      stats->boundary_max = delta;
    }

  stats->boundary_sum += delta;
  stats->boundary_count++;
  for (i = 0; i < VOICE_RX_BOUNDARY_TOP_COUNT; i++)
    {
      if (delta > stats->top[i].delta)
        {
          unsigned int j;

          for (j = VOICE_RX_BOUNDARY_TOP_COUNT - 1; j > i; j--)
            {
              stats->top[j] = stats->top[j - 1];
            }

          stats->top[i].chunk = chunk;
          stats->top[i].delta = delta;
          break;
        }
    }
}

static int voice_rx_account(FAR struct voice_rx_slot_s *slot,
                            FAR int16_t *samples, size_t capacity,
                            size_t *count, int16_t *minimum,
                            int16_t *maximum, int16_t *peak, int64_t *sum,
                            uint64_t *squares, size_t *clip_count,
                            uint32_t *max_adjacent_delta,
                            FAR struct voice_rx_stats_s *boundaries,
                            int16_t *pre_gain_peak,
                            int16_t *post_gain_peak,
                            size_t *gain_clip_count)
{
  FAR struct voice_transfer_s *transfer = slot->transfer;
  size_t nbytes;
  size_t nsamples;
  int16_t chunk_min = INT16_MAX;
  int16_t chunk_max = INT16_MIN;
  int16_t chunk_first = 0;
  int16_t chunk_last = 0;
  int64_t chunk_sum = 0;
  uint64_t chunk_squares = 0;
  uint32_t chunk_delta = 0;
  size_t zero_count = 0;
  size_t first_nonzero = SIZE_MAX;
  size_t i;

  if (transfer == NULL || transfer->apb == NULL)
    {
      return -EIO;
    }

  nbytes = transfer->apb->nbytes;
  nbytes -= nbytes % sizeof(int16_t);
  if (nbytes != slot->request_bytes || nbytes >
      (capacity - *count) * sizeof(int16_t))
    {
      return -EIO;
    }

  nsamples = nbytes / sizeof(int16_t);
  if (nsamples == 0)
    {
      return -EIO;
    }

  memcpy(&samples[*count], transfer->apb->samp, nbytes);
  for (i = 0; i < nsamples; i++)
    {
      int16_t pre_value = samples[*count + i];
      int16_t pre_absolute = pre_value == INT16_MIN ? INT16_MAX :
                             (pre_value < 0 ? -pre_value : pre_value);
      int32_t gained = (int32_t)pre_value * 2;
      int16_t value;
      int16_t absolute;

      if (pre_absolute > *pre_gain_peak)
        {
          *pre_gain_peak = pre_absolute;
        }

      if (gained > INT16_MAX)
        {
          gained = INT16_MAX;
          (*gain_clip_count)++;
        }
      else if (gained < INT16_MIN)
        {
          gained = INT16_MIN;
          (*gain_clip_count)++;
        }

      value = (int16_t)gained;
      samples[*count + i] = value;
      absolute = value == INT16_MIN ? INT16_MAX :
                 (value < 0 ? -value : value);

      if (absolute > *post_gain_peak)
        {
          *post_gain_peak = absolute;
        }

      if (i == 0)
        {
          chunk_first = value;
        }

      chunk_last = value;
      if (value == 0)
        {
          zero_count++;
        }
      else if (first_nonzero == SIZE_MAX)
        {
          first_nonzero = i;
        }

      if (value < chunk_min)
        {
          chunk_min = value;
        }

      if (value > chunk_max)
        {
          chunk_max = value;
        }

      if (i != 0)
        {
          int32_t delta = (int32_t)value - samples[*count + i - 1];

          if (delta < 0)
            {
              delta = -delta;
            }

          if ((uint32_t)delta > chunk_delta)
            {
              chunk_delta = delta;
            }
        }

      if (*count + i != 0)
        {
          int32_t delta = (int32_t)value - samples[*count + i - 1];

          if (delta < 0)
            {
              delta = -delta;
            }

          if ((uint32_t)delta > *max_adjacent_delta)
            {
              *max_adjacent_delta = delta;
            }
        }

      if (value < *minimum)
        {
          *minimum = value;
        }

      if (value > *maximum)
        {
          *maximum = value;
        }

      if (absolute > *peak)
        {
          *peak = absolute;
        }

      chunk_sum += value;
      chunk_squares += (int64_t)value * value;
      *sum += value;
      *squares += (int64_t)value * value;
      if (value == INT16_MIN || value == INT16_MAX)
        {
          (*clip_count)++;
        }
    }

  if (boundaries->have_previous)
    {
      int32_t delta = (int32_t)chunk_first - boundaries->previous_last;

      if (delta < 0)
        {
          delta = -delta;
        }

      voice_rx_boundary_add(boundaries, slot->chunk, (uint32_t)delta);
    }

  boundaries->previous_last = chunk_last;
  boundaries->have_previous = true;
  if (slot->chunk < 2)
    {
      printf("voice_echo: RX chunk=%u first=%d last=%d min=%d max=%d "
             "mean=%ld rms=%lu max_delta=%lu zero=%zu first_nonzero=%zu\n",
             slot->chunk, chunk_first, chunk_last, chunk_min, chunk_max,
             (long)(chunk_sum / (int64_t)nsamples),
             (unsigned long)voice_isqrt(chunk_squares / nsamples),
             (unsigned long)chunk_delta, zero_count, first_nonzero);
    }

  *count += nsamples;
  return OK;
}

static void voice_rx_report_boundaries(
  FAR const struct voice_rx_stats_s *boundaries)
{
  unsigned int i;

  printf("voice_echo: RX boundary count=%u min=%lu mean=%lu max=%lu\n",
         boundaries->boundary_count, (unsigned long)boundaries->boundary_min,
         (unsigned long)(boundaries->boundary_count == 0 ? 0 :
         boundaries->boundary_sum / boundaries->boundary_count),
         (unsigned long)boundaries->boundary_max);
  for (i = 0; i < VOICE_RX_BOUNDARY_TOP_COUNT &&
       i < boundaries->boundary_count; i++)
    {
      printf("voice_echo: RX boundary top[%u] chunk=%u delta=%lu\n", i,
             boundaries->top[i].chunk,
             (unsigned long)boundaries->top[i].delta);
    }
}

static int voice_record(struct i2s_dev_s *mic, int16_t *samples,
                        size_t capacity, size_t *count,
                        int16_t *minimum, int16_t *maximum, int16_t *peak,
                        int64_t *sum, uint64_t *squares, size_t *clip_count,
                        uint32_t *max_adjacent_delta)
{
  struct voice_rx_slot_s slots[VOICE_RX_SLOT_COUNT];
  struct voice_rx_stats_s boundaries;
  size_t target_bytes = capacity * sizeof(int16_t);
  size_t capture_target_bytes = target_bytes +
                                VOICE_RX_WARMUP_CHUNKS * VOICE_RX_DMA_BYTES;
  size_t submitted_bytes = 0;
  size_t warmup_samples = 0;
  unsigned int submit_chunk = 0;
  unsigned int wait_slot = 0;
  unsigned int submitted = 0;
  unsigned int completed = 0;
  unsigned int in_flight = 0;
  unsigned int warmup_chunks = 0;
  int16_t pre_gain_peak = 0;
  int16_t post_gain_peak = 0;
  size_t gain_clip_count = 0;
  clock_t deadline;
  int ret;

  memset(slots, 0, sizeof(slots));
  memset(&boundaries, 0, sizeof(boundaries));
  *count = 0;
  *minimum = INT16_MAX;
  *maximum = INT16_MIN;
  *peak = 0;
  *sum = 0;
  *squares = 0;
  *clip_count = 0;
  *max_adjacent_delta = 0;

  ret = I2S_IOCTL(mic, AUDIOIOC_START, 0);
  printf("voice_echo: MIC start ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  deadline = clock_systime_ticks() + MSEC2TICK(VOICE_RECORD_TIMEOUT_MS);
  while (submitted_bytes < capture_target_bytes &&
         in_flight < VOICE_RX_SLOT_COUNT)
  {
      size_t request_bytes = MIN(VOICE_RX_DMA_BYTES,
                                 capture_target_bytes - submitted_bytes);

      ret = voice_rx_slot_submit(mic, &slots[in_flight], request_bytes,
                                 submit_chunk++);
      if (ret < 0)
        {
          goto errout;
        }

      submitted_bytes += request_bytes;
      submitted++;
      in_flight++;
    }

  while (in_flight != 0)
    {
      struct voice_rx_slot_s *slot = &slots[wait_slot];
      clock_t remaining = deadline - clock_systime_ticks();
      clock_t wait_ticks;

      if ((int32_t)remaining <= 0)
        {
          ret = -ETIMEDOUT;
          goto errout;
        }

      wait_ticks = MIN(MSEC2TICK(VOICE_TRANSFER_TIMEOUT_MS), remaining);
      ret = voice_rx_slot_wait(slot, wait_ticks);
      if (ret < 0)
        {
          printf("voice_echo: RX wait chunk=%u failed: %d\n", slot->chunk,
                 ret);
          goto errout;
        }

      if (warmup_chunks < VOICE_RX_WARMUP_CHUNKS)
        {
          size_t discarded_samples = slot->request_bytes /
                                      sizeof(int16_t);

          warmup_chunks++;
          warmup_samples += discarded_samples;
          voice_rx_slot_release(slot);
          completed++;
          in_flight--;

          if (warmup_chunks == VOICE_RX_WARMUP_CHUNKS)
            {
              printf("voice_echo: RX warmup discarded=%zu samples\n",
                     warmup_samples);
            }

          if (submitted_bytes < capture_target_bytes)
            {
              size_t request_bytes = MIN(VOICE_RX_DMA_BYTES,
                                         capture_target_bytes -
                                         submitted_bytes);

              ret = voice_rx_slot_submit(mic, slot, request_bytes,
                                         submit_chunk++);
              if (ret < 0)
                {
                  goto errout;
                }

              submitted_bytes += request_bytes;
              submitted++;
              in_flight++;
            }

          wait_slot = (wait_slot + 1) % VOICE_RX_SLOT_COUNT;
          continue;
        }

      ret = voice_rx_account(slot, samples, capacity, count, minimum,
                             maximum, peak, sum, squares, clip_count,
                             max_adjacent_delta, &boundaries,
                             &pre_gain_peak, &post_gain_peak,
                             &gain_clip_count);
      if (ret < 0)
        {
          printf("voice_echo: RX chunk=%u invalid logical byte count\n",
                 slot->chunk);
          goto errout;
        }

      voice_rx_slot_release(slot);
      completed++;
      in_flight--;
      if (submitted_bytes < capture_target_bytes)
        {
          size_t request_bytes = MIN(VOICE_RX_DMA_BYTES,
                                     capture_target_bytes - submitted_bytes);

          ret = voice_rx_slot_submit(mic, slot, request_bytes,
                                     submit_chunk++);
          if (ret < 0)
            {
              goto errout;
            }

          submitted_bytes += request_bytes;
          submitted++;
          in_flight++;
        }

      wait_slot = (wait_slot + 1) % VOICE_RX_SLOT_COUNT;
    }

  if (*count != capacity || submitted_bytes != capture_target_bytes)
    {
      ret = -EIO;
      goto errout;
    }

  ret = I2S_IOCTL(mic, AUDIOIOC_STOP, 0);
  printf("voice_echo: MIC stop ret=%d\n", ret);
  printf("voice_echo: RX submitted=%u completed=%u\n", submitted,
         completed);
  voice_rx_report_boundaries(&boundaries);
  printf("voice_echo: gain=2.0x pre_gain_peak=%d post_gain_peak=%d "
         "gain_clip_count=%zu\n", pre_gain_peak, post_gain_peak,
         gain_clip_count);
  return ret;

errout:
  (void)voice_rx_abort(mic, slots);
  printf("voice_echo: RX submitted=%u completed=%u\n", submitted,
         completed);
  return ret;
}

static void voice_put_le16(uint8_t *dst, uint16_t value)
{
  dst[0] = (uint8_t)(value & 0xff);
  dst[1] = (uint8_t)(value >> 8);
}

static void voice_put_le32(uint8_t *dst, uint32_t value)
{
  dst[0] = (uint8_t)(value & 0xff);
  dst[1] = (uint8_t)((value >> 8) & 0xff);
  dst[2] = (uint8_t)((value >> 16) & 0xff);
  dst[3] = (uint8_t)(value >> 24);
}

static int voice_prepare_wav(struct voice_wav_view_s *wav,
                             const int16_t *samples, size_t count)
{
  uint32_t data_bytes;

  if (wav == NULL || samples == NULL ||
      count > (UINT32_MAX - 36) / sizeof(int16_t))
    {
      return -EFBIG;
    }

  data_bytes = (uint32_t)(count * sizeof(int16_t));
  memset(wav->header, 0, sizeof(wav->header));
  memcpy(&wav->header[0], "RIFF", 4);
  voice_put_le32(&wav->header[4], 36 + data_bytes);
  memcpy(&wav->header[8], "WAVEfmt ", 8);
  voice_put_le32(&wav->header[16], 16);
  voice_put_le16(&wav->header[20], 1);
  voice_put_le16(&wav->header[22], VOICE_CHANNELS);
  voice_put_le32(&wav->header[24], VOICE_INPUT_RATE);
  voice_put_le32(&wav->header[28], VOICE_INPUT_RATE * sizeof(int16_t));
  voice_put_le16(&wav->header[32], sizeof(int16_t));
  voice_put_le16(&wav->header[34], VOICE_SAMPLE_BITS);
  memcpy(&wav->header[36], "data", 4);
  voice_put_le32(&wav->header[40], data_bytes);
  wav->pcm = (FAR const uint8_t *)samples;
  wav->pcm_bytes = data_bytes;
  wav->total_bytes = sizeof(wav->header) + data_bytes;

  return 0;
}

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
static int voice_request_asr(FAR const uint8_t *pcm, size_t pcm_bytes)
{
  char text[512];
  int ret;

  ret = voice_channel_process_pcm(pcm, pcm_bytes, text, sizeof(text));
  if (ret < 0)
    {
      printf("[VOICE] ASR request failed: %d\n", ret);
      return ret;
    }

  printf("[VOICE] ASR result: %s\n", text[0] == '\0' ? "<empty>" : text);
  return 0;
}
#else
static int voice_request_asr(FAR const uint8_t *pcm, size_t pcm_bytes)
{
  (void)pcm;
  (void)pcm_bytes;
  printf("[VOICE] ASR unavailable: ai_agent is disabled\n");
  return -ENOSYS;
}
#endif

static uint32_t voice_isqrt(uint64_t value)
{
  uint64_t bit = UINT64_C(1) << 62;
  uint64_t root = 0;

  while (bit > value)
    {
      bit >>= 2;
    }

  while (bit != 0)
    {
      if (value >= root + bit)
        {
          value -= root + bit;
          root = (root >> 1) + bit;
        }
      else
        {
          root >>= 1;
        }

      bit >>= 2;
    }

  return (uint32_t)root;
}

static void voice_report_record_stats(size_t count, int16_t minimum,
                                      int16_t maximum, int16_t peak,
                                      int64_t sum, uint64_t squares,
                                      size_t clip_count,
                                      uint32_t max_adjacent_delta)
{
  printf("voice_echo: captured %zu bytes\n", count * sizeof(int16_t));
  printf("voice_echo: min=%d max=%d peak=%d\n", minimum, maximum, peak);
  printf("voice_echo: mean=%ld rms=%lu clip_count=%zu max_delta=%lu\n",
         (long)(count == 0 ? 0 : sum / (int64_t)count),
         (unsigned long)(count == 0 ? 0 : voice_isqrt(squares / count)),
         clip_count, (unsigned long)max_adjacent_delta);
}

#if 0 /* Replaced by the two-slot gapless TX implementation below. */
static int voice_play(struct i2s_dev_s *speaker, const int16_t *samples,
                      size_t count)
{
  size_t output_count = (count * VOICE_OUTPUT_RATE + VOICE_INPUT_RATE - 1) /
                        VOICE_INPUT_RATE;
  size_t output_index = 0;
  unsigned int chunk_index = 0;
  int ret;

  ret = I2S_IOCTL(speaker, AUDIOIOC_START, 0);
  printf("voice_echo: SPK start ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  while (output_index < output_count)
    {
      struct ap_buffer_s *apb;
      struct voice_transfer_s *transfer;
      size_t chunk = output_count - output_index;
      size_t i;
      bool submitted;

      if (chunk > VOICE_DMA_BYTES / sizeof(int16_t))
        {
          chunk = VOICE_DMA_BYTES / sizeof(int16_t);
        }

      ret = voice_buffer_alloc(&apb, VOICE_DMA_BYTES);
      if (ret < 0)
        {
          goto errout;
        }

      transfer = calloc(1, sizeof(*transfer));
      if (transfer == NULL)
        {
          apb_free(apb);
          ret = -ENOMEM;
          goto errout;
        }

      for (i = 0; i < chunk; i++)
        {
          size_t source = ((output_index + i) * VOICE_INPUT_RATE) /
                          VOICE_OUTPUT_RATE;

          ((int16_t *)apb->samp)[i] = samples[source];
        }

      apb->nbytes = chunk * sizeof(int16_t);
      apb->nsamples = chunk;
      voice_transfer_init(transfer, false, false, true, chunk_index);
      printf("voice_echo: TX submit chunk=%u bytes=%zu\n", chunk_index,
             apb->nbytes);
      ret = I2S_SEND(speaker, apb, voice_complete, transfer,
                     MSEC2TICK(VOICE_TRANSFER_TIMEOUT_MS));
      submitted = ret >= 0;
      printf("voice_echo: TX submit chunk=%u ret=%d\n", chunk_index, ret);
      if (submitted)
        {
          ret = voice_wait_transfer(transfer,
                                    MSEC2TICK(VOICE_TRANSFER_TIMEOUT_MS));
        }

      if (ret < 0)
        {
          printf("voice_echo: TX wait chunk=%u failed: %d\n", chunk_index,
                 ret);
          if (submitted && voice_abandon_transfer(transfer))
            {
              int stop_ret = I2S_IOCTL(speaker, AUDIOIOC_STOP, 0);

              printf("voice_echo: SPK stop ret=%d\n", stop_ret);
              /* The callback owns transfer and the application's APB
               * reference after abandonment.  It releases both safely.
               */

              return ret;
            }

          voice_transfer_destroy(transfer);
          free(transfer);
          apb_free(apb);
          goto errout;
        }

      voice_transfer_destroy(transfer);
      free(transfer);
      apb_free(apb);
      output_index += chunk;
      chunk_index++;
    }

errout:
  {
    int stop_ret = I2S_IOCTL(speaker, AUDIOIOC_STOP, 0);

    printf("voice_echo: SPK stop ret=%d\n", stop_ret);
    if (ret >= 0 && stop_ret < 0)
      {
        ret = stop_ret;
      }
  }

  return ret;
}

static int voice_play_diagnostic(struct i2s_dev_s *speaker, bool tone)
{
  size_t output_index = 0;
  unsigned int chunk_index = 0;
  int ret;

  ret = I2S_IOCTL(speaker, AUDIOIOC_START, 0);
  printf("voice_echo: SPK start ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  while (output_index < VOICE_DIAGNOSTIC_OUTPUT_SAMPLES)
    {
      struct ap_buffer_s *apb;
      struct voice_transfer_s *transfer;
      size_t chunk = MIN(VOICE_DMA_BYTES / sizeof(int16_t),
                         VOICE_DIAGNOSTIC_OUTPUT_SAMPLES - output_index);
      size_t i;
      bool submitted;

      ret = voice_buffer_alloc(&apb, VOICE_DMA_BYTES);
      if (ret < 0)
        {
          goto errout;
        }

      transfer = calloc(1, sizeof(*transfer));
      if (transfer == NULL)
        {
          apb_free(apb);
          ret = -ENOMEM;
          goto errout;
        }

      for (i = 0; i < chunk; i++)
        {
          ((int16_t *)apb->samp)[i] = tone ?
            g_tone_750hz[(output_index + i) % VOICE_TONE_SAMPLES] : 0;
        }

      apb->nbytes = chunk * sizeof(int16_t);
      apb->nsamples = chunk;
      voice_transfer_init(transfer, false, false, true, chunk_index);
      printf("voice_echo: TX submit chunk=%u bytes=%zu\n", chunk_index,
             apb->nbytes);
      ret = I2S_SEND(speaker, apb, voice_complete, transfer,
                     MSEC2TICK(VOICE_TRANSFER_TIMEOUT_MS));
      submitted = ret >= 0;
      printf("voice_echo: TX submit chunk=%u ret=%d\n", chunk_index, ret);
      if (submitted)
        {
          ret = voice_wait_transfer(transfer,
                                    MSEC2TICK(VOICE_TRANSFER_TIMEOUT_MS));
        }

      if (ret < 0)
        {
          printf("voice_echo: TX wait chunk=%u failed: %d\n", chunk_index,
                 ret);
          if (submitted && voice_abandon_transfer(transfer))
            {
              int stop_ret = I2S_IOCTL(speaker, AUDIOIOC_STOP, 0);

              printf("voice_echo: SPK stop ret=%d\n", stop_ret);
              return ret;
            }

          voice_transfer_destroy(transfer);
          free(transfer);
          apb_free(apb);
          goto errout;
        }

      voice_transfer_destroy(transfer);
      free(transfer);
      apb_free(apb);
      output_index += chunk;
      chunk_index++;
    }

errout:
  {
    int stop_ret = I2S_IOCTL(speaker, AUDIOIOC_STOP, 0);

    printf("voice_echo: SPK stop ret=%d\n", stop_ret);
    if (ret >= 0 && stop_ret < 0)
      {
        ret = stop_ret;
      }
  }

  return ret;
}

#endif

struct voice_tx_slot_s
{
  FAR struct ap_buffer_s *apb;
  FAR struct voice_transfer_s *transfer;
  bool in_flight;
};

static void voice_tx_slot_release(FAR struct voice_tx_slot_s *slot)
{
  if (slot->transfer != NULL)
    {
      voice_transfer_destroy(slot->transfer);
      free(slot->transfer);
    }

  if (slot->apb != NULL)
    {
      apb_free(slot->apb);
    }

  slot->apb = NULL;
  slot->transfer = NULL;
  slot->in_flight = false;
}

static void voice_tx_fill(enum voice_mode_e mode, FAR int16_t *output,
                          size_t output_index, size_t count,
                          FAR const int16_t *samples)
{
  size_t i;

  for (i = 0; i < count; i++)
    {
      if (mode == VOICE_MODE_ECHO)
        {
          size_t source = ((output_index + i) * VOICE_INPUT_RATE) /
                          VOICE_OUTPUT_RATE;

          output[i] = samples[source];
        }
      else if (mode == VOICE_MODE_TONE)
        {
          output[i] = g_tone_750hz[(output_index + i) %
                                    VOICE_TONE_SAMPLES];
        }
      else
        {
          output[i] = 0;
        }
    }
}

static int voice_tx_slot_submit(struct i2s_dev_s *speaker,
                                FAR struct voice_tx_slot_s *slot,
                                enum voice_mode_e mode,
                                FAR const int16_t *samples,
                                size_t output_index, size_t count,
                                unsigned int chunk)
{
  int ret;

  ret = voice_buffer_alloc(&slot->apb, VOICE_DMA_BYTES);
  if (ret < 0)
    {
      return ret;
    }

  slot->transfer = calloc(1, sizeof(*slot->transfer));
  if (slot->transfer == NULL)
    {
      apb_free(slot->apb);
      slot->apb = NULL;
      return -ENOMEM;
    }

  voice_tx_fill(mode, (FAR int16_t *)slot->apb->samp, output_index, count,
                samples);
  slot->apb->nbytes = count * sizeof(int16_t);
  slot->apb->nsamples = count;
  voice_transfer_init(slot->transfer, false, false, true, chunk);
  if (chunk < 2)
    {
      printf("voice_echo: TX submit chunk=%u bytes=%zu\n", chunk,
             slot->apb->nbytes);
    }
  ret = I2S_SEND(speaker, slot->apb, voice_complete, slot->transfer,
                 MSEC2TICK(VOICE_TRANSFER_TIMEOUT_MS));
  if (chunk < 2 || ret < 0)
    {
      printf("voice_echo: TX submit chunk=%u ret=%d\n", chunk, ret);
    }
  if (ret < 0)
    {
      voice_tx_slot_release(slot);
      return ret;
    }

  slot->in_flight = true;
  return OK;
}

static int voice_tx_slot_wait(FAR struct voice_tx_slot_s *slot)
{
  FAR struct voice_transfer_s *transfer = slot->transfer;
  int ret;

  ret = voice_wait_transfer(transfer, MSEC2TICK(VOICE_TRANSFER_TIMEOUT_MS));
  if (ret == -ETIMEDOUT && !voice_abandon_transfer(transfer))
    {
      nxmutex_lock(&transfer->lock);
      ret = transfer->result;
      nxmutex_unlock(&transfer->lock);
    }

  if (ret == -ETIMEDOUT)
    {
      /* The late callback owns the transfer and the original APB. */

      slot->transfer = NULL;
      slot->apb = NULL;
      slot->in_flight = false;
      return ret;
    }

  voice_tx_slot_release(slot);
  return ret;
}

static int voice_tx_abort(struct i2s_dev_s *speaker,
                          FAR struct voice_tx_slot_s *slots)
{
  unsigned int i;
  int stop_ret;

  for (i = 0; i < VOICE_TX_SLOT_COUNT; i++)
    {
      FAR struct voice_tx_slot_s *slot = &slots[i];

      if (!slot->in_flight)
        {
          continue;
        }

      if (voice_abandon_transfer(slot->transfer))
        {
          /* The callback will release both application-owned objects. */

          slot->transfer = NULL;
          slot->apb = NULL;
          slot->in_flight = false;
        }
      else
        {
          voice_tx_slot_release(slot);
        }
    }

  stop_ret = I2S_IOCTL(speaker, AUDIOIOC_STOP, 0);
  printf("voice_echo: SPK stop ret=%d\n", stop_ret);
  return stop_ret;
}

static int voice_play_stream(struct i2s_dev_s *speaker,
                             enum voice_mode_e mode,
                             FAR const int16_t *samples, size_t count)
{
  struct voice_tx_slot_s slots[VOICE_TX_SLOT_COUNT];
  size_t output_count;
  size_t output_index = 0;
  unsigned int submit_chunk = 0;
  unsigned int wait_slot = 0;
  unsigned int submitted = 0;
  unsigned int completed = 0;
  unsigned int in_flight = 0;
  int ret;

  memset(slots, 0, sizeof(slots));
  output_count = mode == VOICE_MODE_ECHO ?
    (count * VOICE_OUTPUT_RATE + VOICE_INPUT_RATE - 1) / VOICE_INPUT_RATE :
    VOICE_DIAGNOSTIC_OUTPUT_SAMPLES;

  ret = I2S_IOCTL(speaker, AUDIOIOC_START, 0);
  printf("voice_echo: SPK start ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  while (output_index < output_count && in_flight < VOICE_TX_SLOT_COUNT)
    {
      size_t chunk = MIN(VOICE_DMA_BYTES / sizeof(int16_t),
                         output_count - output_index);

      ret = voice_tx_slot_submit(speaker, &slots[in_flight], mode, samples,
                                 output_index, chunk, submit_chunk++);
      if (ret < 0)
        {
          goto errout;
        }

      output_index += chunk;
      in_flight++;
      submitted++;
    }

  while (in_flight != 0)
    {
      ret = voice_tx_slot_wait(&slots[wait_slot]);
      if (ret < 0)
        {
          printf("voice_echo: TX wait slot=%u failed: %d\n", wait_slot,
                 ret);
          goto errout;
        }

      completed++;
      in_flight--;
      if (output_index < output_count)
        {
          size_t chunk = MIN(VOICE_DMA_BYTES / sizeof(int16_t),
                             output_count - output_index);

          ret = voice_tx_slot_submit(speaker, &slots[wait_slot], mode,
                                     samples, output_index, chunk,
                                     submit_chunk++);
          if (ret < 0)
            {
              goto errout;
            }

          output_index += chunk;
          in_flight++;
          submitted++;
        }

      wait_slot = (wait_slot + 1) % VOICE_TX_SLOT_COUNT;
    }

  ret = I2S_IOCTL(speaker, AUDIOIOC_STOP, 0);
  printf("voice_echo: SPK stop ret=%d\n", ret);
  printf("voice_echo: TX submitted=%u completed=%u\n", submitted,
         completed);
  return ret;

errout:
  (void)voice_tx_abort(speaker, slots);
  printf("voice_echo: TX submitted=%u completed=%u\n", submitted,
         completed);
  return ret;
}

static int voice_play(struct i2s_dev_s *speaker, const int16_t *samples,
                      size_t count)
{
  return voice_play_stream(speaker, VOICE_MODE_ECHO, samples, count);
}

static int voice_play_diagnostic(struct i2s_dev_s *speaker, bool tone)
{
  return voice_play_stream(speaker, tone ? VOICE_MODE_TONE :
                           VOICE_MODE_SILENCE, NULL, 0);
}

/* The 32-sample table is one 500 Hz period at 16 kHz.  The production
 * voice_play() path subsequently converts it to 24 kHz for I2S1 TX.
 */

static void voice_fill_restone(FAR int16_t *samples, size_t count)
{
  size_t i;

  for (i = 0; i < count; i++)
    {
      samples[i] = g_tone_750hz[i % VOICE_TONE_SAMPLES];
    }
}

int main(int argc, char *argv[])
{
  struct i2s_dev_s *mic = NULL;
  struct i2s_dev_s *speaker = NULL;
  struct voice_oled_s oled;
  int16_t *samples = NULL;
  size_t capacity = VOICE_INPUT_RATE * VOICE_ECHO_RECORD_SECONDS;
  enum voice_mode_e mode = VOICE_MODE_ECHO;
  bool need_mic;
  bool need_speaker;
  int ret;

  if (argc == 2)
    {
      if (strcmp(argv[1], "recordonly") == 0)
        {
          mode = VOICE_MODE_RECORDONLY;
        }
      else if (strcmp(argv[1], "recordmem") == 0)
        {
          mode = VOICE_MODE_RECORDMEM;
        }
      else if (strcmp(argv[1], "silence") == 0)
        {
          mode = VOICE_MODE_SILENCE;
        }
      else if (strcmp(argv[1], "tone") == 0)
        {
          mode = VOICE_MODE_TONE;
        }
      else if (strcmp(argv[1], "restone") == 0)
        {
          mode = VOICE_MODE_RESTONE;
        }
      else if (strcmp(argv[1], "--once") != 0)
        {
          printf("Usage: voice_echo [--once|recordonly|recordmem|silence|tone|"
                 "restone]\n");
          return EXIT_FAILURE;
        }
    }
  else if (argc > 2)
    {
      printf("Usage: voice_echo [--once|recordonly|recordmem|silence|tone|"
             "restone]\n");
      return EXIT_FAILURE;
    }

  need_mic = mode == VOICE_MODE_ECHO || mode == VOICE_MODE_RECORDONLY ||
             mode == VOICE_MODE_RECORDMEM;
  need_speaker = mode == VOICE_MODE_ECHO || mode == VOICE_MODE_SILENCE ||
                 mode == VOICE_MODE_TONE || mode == VOICE_MODE_RESTONE;

  if (need_mic)
    {
      printf("voice_echo: MIC acquire start\n");
      mic = board_voice_mic_i2s();
      if (mic == NULL)
        {
          printf("I2S MIC unavailable\n");
          return EXIT_FAILURE;
        }

      printf("voice_echo: MIC acquire OK\n");
      ret = voice_configure_mic(mic);
      if (ret < 0)
        {
          printf("MIC format configuration failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      board_voice_mic_set_diagnostics(mode == VOICE_MODE_RECORDONLY);
    }

  if (need_speaker)
    {
      speaker = board_voice_speaker_i2s();
      if (speaker == NULL)
        {
          printf("I2S SPK unavailable\n");
          return EXIT_FAILURE;
        }

      ret = voice_configure_speaker(speaker);
      if (ret < 0)
        {
          printf("SPK format configuration failed: %d\n", ret);
          return EXIT_FAILURE;
        }
    }

  voice_oled_init(&oled);
  printf("MIC: I2S0 RX, SPK: I2S1 TX\n");
  printf("Rate: %d Hz -> %d Hz, slot: %d bit, sample: %d bit, mono\n",
         VOICE_INPUT_RATE, VOICE_OUTPUT_RATE, VOICE_SLOT_BITS,
         VOICE_SAMPLE_BITS);

  if (mode == VOICE_MODE_SILENCE || mode == VOICE_MODE_TONE)
    {
      voice_oled_show(&oled, "PLAY", "5 SEC");
      ret = voice_play_diagnostic(speaker, mode == VOICE_MODE_TONE);
      if (ret < 0)
        {
          printf("voice_echo: SPK TX failed: %d\n", ret);
          voice_oled_show(&oled, "SPK ERROR", "");
        }
      else
        {
          printf("voice_echo: diagnostic mode=%s complete\n",
                 mode == VOICE_MODE_TONE ? "tone" : "silence");
          voice_oled_show(&oled, "DONE", "");
        }

      return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (mode == VOICE_MODE_RESTONE)
    {
      samples = malloc(capacity * sizeof(*samples));
      if (samples == NULL)
        {
          printf("Record buffer allocation failed\n");
          return EXIT_FAILURE;
        }

      voice_fill_restone(samples, capacity);
      voice_oled_show(&oled, "PLAY", "RESTONE");
      ret = voice_play(speaker, samples, capacity);
      free(samples);
      if (ret < 0)
        {
          printf("voice_echo: restone failed: %d\n", ret);
          voice_oled_show(&oled, "SPK ERROR", "");
        }
      else
        {
          printf("voice_echo: diagnostic mode=restone complete\n");
          voice_oled_show(&oled, "DONE", "");
        }

      return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (mode == VOICE_MODE_RECORDMEM)
    {
      capacity = VOICE_INPUT_RATE * VOICE_RECORDMEM_SECONDS;
    }

  samples = malloc(capacity * sizeof(*samples));
  if (samples == NULL)
    {
      printf("Record buffer allocation failed\n");
      return EXIT_FAILURE;
    }

  printf("Record duration: %d s\n",
         mode == VOICE_MODE_RECORDMEM ? VOICE_RECORDMEM_SECONDS :
         VOICE_ECHO_RECORD_SECONDS);

  {
    int16_t minimum;
    int16_t maximum;
    int16_t peak;
    int64_t sum;
    uint64_t squares;
    size_t clip_count;
    uint32_t max_adjacent_delta;
    size_t count;
    struct timespec record_start;
    struct timespec record_end;
    uint32_t record_elapsed_ms;
    uint32_t effective_rate;

    voice_oled_show(&oled, "REC", mode == VOICE_MODE_RECORDMEM ? "3 SEC" :
                    "5 SEC");
    printf("voice_echo: recording %d seconds...\n",
           mode == VOICE_MODE_RECORDMEM ? VOICE_RECORDMEM_SECONDS :
           VOICE_ECHO_RECORD_SECONDS);
    clock_gettime(CLOCK_MONOTONIC, &record_start);
    ret = voice_record(mic, samples, capacity, &count, &minimum, &maximum,
                       &peak, &sum, &squares, &clip_count,
                       &max_adjacent_delta);
    clock_gettime(CLOCK_MONOTONIC, &record_end);
    record_elapsed_ms = voice_elapsed_ms(&record_start, &record_end);
    effective_rate = record_elapsed_ms == 0 ? 0 :
      (uint32_t)((count * 1000) / record_elapsed_ms);
    printf("voice_echo: record elapsed=%lu ms samples=%zu "
           "effective_rate=%lu Hz\n",
           (unsigned long)record_elapsed_ms, count,
           (unsigned long)effective_rate);
    if (ret < 0)
      {
        printf("voice_echo: MIC RX failed: %d\n", ret);
        voice_oled_show(&oled, "MIC ERROR", "");
      }
    else
      {
        voice_report_record_stats(count, minimum, maximum, peak, sum,
                                  squares, clip_count, max_adjacent_delta);
        if (mode == VOICE_MODE_RECORDONLY)
          {
            printf("voice_echo: diagnostic mode=recordonly complete\n");
            voice_oled_show(&oled, "DONE", "");
          }
        else if (mode == VOICE_MODE_RECORDMEM)
          {
            struct voice_wav_view_s wav;

            ret = voice_prepare_wav(&wav, samples, count);
            if (ret < 0)
              {
                printf("[VOICE] error stage=wav_memory code=%d\n", ret);
                voice_oled_show(&oled, "WAV ERROR", "");
              }
            else
              {
                printf("[VOICE] wav_memory_ready total=%zu pcm=%zu\n",
                       wav.total_bytes, wav.pcm_bytes);
                voice_oled_show(&oled, "ASR", "REQ");
                ret = voice_request_asr(wav.pcm, wav.pcm_bytes);
                if (ret < 0)
                  {
                    voice_oled_show(&oled, "ASR ERROR", "");
                  }
                else
                  {
                    voice_oled_show(&oled, "ASR", "DONE");
                  }
              }
          }
        else
          {
            struct timespec playback_start;
            struct timespec playback_end;
            uint32_t playback_elapsed_ms;

            voice_oled_show(&oled, "PLAY", "");
            printf("voice_echo: playing...\n");
            clock_gettime(CLOCK_MONOTONIC, &playback_start);
            ret = voice_play(speaker, samples, count);
            clock_gettime(CLOCK_MONOTONIC, &playback_end);
            playback_elapsed_ms = voice_elapsed_ms(&playback_start,
                                                    &playback_end);
            printf("voice_echo: playback elapsed=%lu ms\n",
                   (unsigned long)playback_elapsed_ms);
            if (ret < 0)
              {
                printf("voice_echo: SPK TX failed: %d\n", ret);
                voice_oled_show(&oled, "SPK ERROR", "");
              }
            else
              {
                printf("voice_echo: playback complete\n");
                voice_oled_show(&oled, "DONE", "");
              }
          }
      }
  }

  free(samples);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
