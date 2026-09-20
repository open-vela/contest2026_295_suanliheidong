/****************************************************************************
 * board/contest_board/src/contest_i2s.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Contest-local I2S0 receive and I2S1 transmit lower-halves.  They own only
 * the microphone RX and speaker TX paths required by this board.
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_CONTEST_BOARD_I2S0_RX) || \
    defined(CONFIG_CONTEST_BOARD_I2S1_TX)

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/cache.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>
#include <arch/irq.h>

#include "esp32s3_dma.h"
#include "esp32s3_gpio.h"
#include "esp32s3_irq.h"

#include "xtensa.h"

#include "hardware/esp32s3_dma.h"
#include "hardware/esp32s3_gpio_sigmap.h"
#include "hardware/esp32s3_i2s.h"
#include "hardware/esp32s3_system.h"

#include "contest_i2s.h"

#define CONTEST_I2S_PORT                0
#define CONTEST_I2S_RATE                16000
#define CONTEST_I2S_TX_PORT             1
#define CONTEST_I2S_TX_RATE             24000
/* RX stays in its established format.  The speaker follows the original
 * board driver exactly: 32-bit mono data in the left physical slot, with two
 * 32-bit slots per LRCK frame. */
#define CONTEST_I2S_WIDTH               16
#define CONTEST_I2S_SLOTS               2
#define CONTEST_I2S_TX_WIDTH            32
#define CONTEST_I2S_TX_SLOTS            2
#define CONTEST_I2S_RX_RAW_MAX_BYTES    4096
#define CONTEST_I2S_RX_DESC_BYTES       2048
#define CONTEST_I2S_RX_XFER_COUNT        2
#define CONTEST_I2S_RX_PCM_SHIFT        16
#define CONTEST_I2S_RX_DIAG_WORDS \
  (CONTEST_I2S_RX_RAW_MAX_BYTES / sizeof(uint32_t))
#define CONTEST_I2S_RX_DIAG_SIGNAL_PEAK 1024
#define CONTEST_I2S_RX_DIAG_INDICES      32
#define CONTEST_I2S_RX_DMA_SENTINEL      UINT32_C(0xa5a5a5a5)
#define CONTEST_I2S_RX_TDM_CHAN_MASK    0x0000ffff
#define CONTEST_I2S_RAW_RMS_SCALE_SHIFT 8
/* The ESP32-S3 reference I2S lower-half selects CLK160 for RX. */

#define CONTEST_I2S_SOURCE_CLOCK        160000000UL
#define CONTEST_I2S_MCLK_MULTIPLE       256
#define CONTEST_I2S_DMA_DESC_COUNT      CONFIG_I2S_DMADESC_NUM
#define CONTEST_I2S_TX_RING_SLOTS        16
#define CONTEST_I2S_TX_SLOT_BYTES        2048
#define CONTEST_I2S_TX_DMA_ALIGN         32
/* Each immutable descriptor reads a private DMA block allocated only during
 * TTS playback.  Upper APBs can therefore complete at OUT_DONE without
 * being revisited by the circular DMA ring.  Keep the completed private
 * block unchanged for a short FIFO-drain interval before its APB is reused. */
#define CONTEST_I2S_TX_FIFO_DRAIN_MS     8

#ifndef DMA_OUT_DONE_CH0_INT_ENA
#  define DMA_OUT_DONE_CH0_INT_ENA       (1U << 0)
#endif

#ifndef DMA_OUT_DONE_CH0_INT_ST
#  define DMA_OUT_DONE_CH0_INT_ST        (1U << 0)
#endif

#ifndef DMA_OUT_EOF_CH0_INT_ENA
#  define DMA_OUT_EOF_CH0_INT_ENA        (1U << 1)
#endif

#ifndef DMA_OUT_EOF_CH0_INT_ST
#  define DMA_OUT_EOF_CH0_INT_ST         (1U << 1)
#endif

struct contest_i2s_xfer_s
{
  FAR struct contest_i2s_xfer_s *next;
  FAR struct ap_buffer_s *apb;
  FAR uint8_t *dma_data;
  i2s_callback_t callback;
  FAR void *arg;
  size_t nbytes;
  int result;
  bool in_use;
  bool done_queued;
  bool dma_bound;
  bool terminal;
  uint8_t desc_last;
  struct esp32s3_dmadesc_s desc[CONTEST_I2S_DMA_DESC_COUNT];
};

/* RX transactions own both their descriptor chain and the physical raw I2S
 * storage.  A completed raw buffer remains intact while HPWORK converts it,
 * even if the EOF ISR has already started the next pending transaction.
 */

struct contest_i2s_rx_xfer_s
{
  FAR struct contest_i2s_rx_xfer_s *next;
  FAR struct ap_buffer_s *apb;
  i2s_callback_t callback;
  FAR void *arg;
  size_t logical_bytes;
  size_t raw_bytes;
  uint32_t queued_bytes;
  int result;
  bool in_use;
  bool dma_diag;
  struct esp32s3_dmadesc_s desc[CONTEST_I2S_DMA_DESC_COUNT];
  uint32_t raw[CONTEST_I2S_RX_RAW_MAX_BYTES / sizeof(uint32_t)];
};

struct contest_i2s_rx_dma_diag_s
{
  bool valid;
  size_t expected_bytes;
  uint32_t queued_bytes;
  size_t first_modified;
  size_t last_modified;
  size_t modified_words;
  size_t untouched_words;
  size_t longest_untouched_run;
  struct esp32s3_dmadesc_s desc[CONTEST_I2S_DMA_DESC_COUNT];
};

struct contest_i2s_s
{
  struct i2s_dev_s dev;
  spinlock_t lock;
  uint32_t rate;
  uint8_t data_width;
  uint8_t channels;
  int dma_channel;
  int rx_irq;
  int cpu;
  bool initialized;
  bool streaming;
  bool reserved;
  FAR struct contest_i2s_rx_xfer_s *active;
  FAR struct contest_i2s_rx_xfer_s *pending;
  FAR struct contest_i2s_rx_xfer_s *done_head;
  FAR struct contest_i2s_rx_xfer_s *done_tail;
  uint32_t irq_status;
  uint32_t eof_count;
  uint32_t chained_count;
  uint32_t underrun_count;
  bool started_once;
  bool idle;
  bool raw_logged;
  bool diagnostics_enabled;
  size_t raw_diag_words;
  uint32_t raw_diag[CONTEST_I2S_RX_DIAG_WORDS];
  bool dma_diag_armed;
  struct contest_i2s_rx_dma_diag_s dma_diag;
  struct work_s rx_work;
  struct contest_i2s_rx_xfer_s xfer[CONTEST_I2S_RX_XFER_COUNT];
};

struct contest_i2s_tx_s
{
  struct i2s_dev_s dev;
  spinlock_t lock;
  uint32_t rate;
  uint8_t logical_width;
  uint8_t data_width;
  uint8_t channels;
  int dma_channel;
  int tx_irq;
  int cpu;
  bool initialized;
  bool streaming;
  bool reserved;
  bool hw_running;
  bool terminal_stop_pending;
  uint8_t terminal_stop_slot;
  FAR struct contest_i2s_xfer_s *done_head;
  FAR struct contest_i2s_xfer_s *done_tail;
  uint8_t fill_index;
  uint8_t done_index;
  uint8_t queued_slots;
  uint8_t max_queued_slots;
  uint8_t bound_slots;
  uint32_t irq_status;
  uint32_t done_count;
  uint32_t chained_count;
  uint32_t underrun_count;
  uint32_t silence_done_count;
  uint32_t late_refill_count;
  uint32_t ring_wrap_count;
  uint32_t submit_count;
  uint32_t dma_start_count;
  uint32_t done_irq_count;
  uint32_t timeout_ena;
  uint32_t timeout_raw;
  uint32_t timeout_status;
  uint32_t timeout_link;
  uint32_t timeout_conf;
  uint32_t timeout_desc_ctrl;
  FAR const void *timeout_desc_addr;
  struct work_s tx_work;
  struct work_s tx_stop_work;
  struct contest_i2s_xfer_s xfer[CONTEST_I2S_TX_RING_SLOTS];
};

struct contest_i2s_pcm_stats_s
{
  int16_t minimum;
  int16_t maximum;
  int64_t sum;
  uint64_t squares;
  uint32_t clip_count;
  uint64_t max_delta;
  int16_t previous;
  bool have_previous;
};

static int contest_i2s_rxchannels(FAR struct i2s_dev_s *dev,
                                  uint8_t channels);
static uint32_t contest_i2s_rxsamplerate(FAR struct i2s_dev_s *dev,
                                         uint32_t rate);
static uint32_t contest_i2s_rxdatawidth(FAR struct i2s_dev_s *dev,
                                        int bits);
static int contest_i2s_receive(FAR struct i2s_dev_s *dev,
                               FAR struct ap_buffer_s *apb,
                               i2s_callback_t callback, FAR void *arg,
                               uint32_t timeout);
static int contest_i2s_ioctl(FAR struct i2s_dev_s *dev, int cmd,
                             unsigned long arg);
static int contest_i2s_txchannels(FAR struct i2s_dev_s *dev,
                                  uint8_t channels);
static uint32_t contest_i2s_txsamplerate(FAR struct i2s_dev_s *dev,
                                         uint32_t rate);
static uint32_t contest_i2s_txdatawidth(FAR struct i2s_dev_s *dev,
                                        int bits);
static int contest_i2s_send(FAR struct i2s_dev_s *dev,
                            FAR struct ap_buffer_s *apb,
                            i2s_callback_t callback, FAR void *arg,
                            uint32_t timeout);
static int contest_i2s_tx_ioctl(FAR struct i2s_dev_s *dev, int cmd,
                                unsigned long arg);

static const struct i2s_ops_s g_contest_i2s_ops =
{
  .i2s_rxchannels = contest_i2s_rxchannels,
  .i2s_rxsamplerate = contest_i2s_rxsamplerate,
  .i2s_rxdatawidth = contest_i2s_rxdatawidth,
  .i2s_receive = contest_i2s_receive,
  .i2s_ioctl = contest_i2s_ioctl,
};

static const struct i2s_ops_s g_contest_i2s_tx_ops =
{
  .i2s_txchannels = contest_i2s_txchannels,
  .i2s_txsamplerate = contest_i2s_txsamplerate,
  .i2s_txdatawidth = contest_i2s_txdatawidth,
  .i2s_send = contest_i2s_send,
  .i2s_ioctl = contest_i2s_tx_ioctl,
};

static struct contest_i2s_s g_contest_i2s0 =
{
  .dev =
  {
    .ops = &g_contest_i2s_ops,
  },
  .lock = SP_UNLOCKED,
  .dma_channel = -1,
  .rx_irq = -1,
};

static struct contest_i2s_tx_s g_contest_i2s1 =
{
  .dev =
  {
    .ops = &g_contest_i2s_tx_ops,
  },
  .lock = SP_UNLOCKED,
  .dma_channel = -1,
  .tx_irq = -1,
};

static uint32_t contest_i2s_isqrt(uint64_t value)
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

static uint32_t contest_i2s_bswap32(uint32_t value)
{
  return ((value & UINT32_C(0x000000ff)) << 24) |
         ((value & UINT32_C(0x0000ff00)) << 8) |
         ((value & UINT32_C(0x00ff0000)) >> 8) |
         ((value & UINT32_C(0xff000000)) >> 24);
}

static int16_t contest_i2s_pcm_candidate_a(uint32_t raw)
{
  return (int16_t)((int32_t)raw >> CONTEST_I2S_RX_PCM_SHIFT);
}

static int16_t contest_i2s_pcm_candidate_b(uint32_t raw)
{
  int32_t signed24 = (int32_t)(raw & UINT32_C(0x00ffffff));

  if ((signed24 & INT32_C(0x00800000)) != 0)
    {
      signed24 |= ~INT32_C(0x00ffffff);
    }

  return (int16_t)(signed24 >> 8);
}

static void contest_i2s_pcm_stats_init(FAR struct contest_i2s_pcm_stats_s *s)
{
  s->minimum = INT16_MAX;
  s->maximum = INT16_MIN;
  s->sum = 0;
  s->squares = 0;
  s->clip_count = 0;
  s->max_delta = 0;
  s->have_previous = false;
}

static void contest_i2s_pcm_stats_add(FAR struct contest_i2s_pcm_stats_s *s,
                                       int16_t value)
{
  int64_t delta;

  if (value < s->minimum)
    {
      s->minimum = value;
    }

  if (value > s->maximum)
    {
      s->maximum = value;
    }

  s->sum += value;
  s->squares += (int64_t)value * value;
  if (value == INT16_MIN || value == INT16_MAX)
    {
      s->clip_count++;
    }

  if (s->have_previous)
    {
      delta = (int64_t)value - s->previous;
      if (delta < 0)
        {
          delta = -delta;
        }

      if ((uint64_t)delta > s->max_delta)
        {
          s->max_delta = delta;
        }
    }

  s->previous = value;
  s->have_previous = true;
}

static void contest_i2s_log_pcm_stats(const char *name,
                                       FAR const struct contest_i2s_pcm_stats_s *s,
                                       size_t count)
{
  uint32_t rms = count == 0 ? 0 :
                 contest_i2s_isqrt(s->squares / count);
  int64_t mean = count == 0 ? 0 : s->sum / (int64_t)count;

  syslog(LOG_INFO,
         "[C-I2S] raw candidate %s min=%d max=%d mean=%ld rms=%lu "
         "clip=%lu max_delta=%lu\n",
         name, s->minimum, s->maximum, (long)mean, (unsigned long)rms,
         (unsigned long)s->clip_count, (unsigned long)s->max_delta);
}

static void contest_i2s_log_raw_diagnostics(FAR const uint32_t *raw_words,
                                             size_t words)
{
  struct contest_i2s_pcm_stats_s stats_a;
  struct contest_i2s_pcm_stats_s stats_b;
  struct contest_i2s_pcm_stats_s stats_ca;
  struct contest_i2s_pcm_stats_s stats_cb;
  int32_t raw_min = INT32_MAX;
  int32_t raw_max = INT32_MIN;
  int64_t raw_sum = 0;
  uint64_t raw_squares = 0;
  uint64_t raw_max_delta = 0;
  unsigned int zero_count = 0;
  unsigned int positive_count = 0;
  unsigned int negative_count = 0;
  unsigned int low8_nonzero = 0;
  unsigned int low16_nonzero = 0;
  unsigned int raw_mod[4] = {0, 0, 0, 0};
  unsigned int pcm_mod[4] = {0, 0, 0, 0};
  unsigned int pcm_zero_count = 0;
  unsigned int pcm_nonzero_count = 0;
  unsigned int pcm_zero_run = 0;
  unsigned int pcm_longest_zero_run = 0;
  unsigned int pcm_indices[CONTEST_I2S_RX_DIAG_INDICES];
  unsigned int pcm_index_count = 0;
  int32_t previous = 0;
  bool have_previous = false;
  unsigned int i;

  contest_i2s_pcm_stats_init(&stats_a);
  contest_i2s_pcm_stats_init(&stats_b);
  contest_i2s_pcm_stats_init(&stats_ca);
  contest_i2s_pcm_stats_init(&stats_cb);

  for (i = 0; i < words; i++)
    {
      uint32_t raw = raw_words[i];
      uint32_t swapped = contest_i2s_bswap32(raw);
      int16_t pcm = contest_i2s_pcm_candidate_a(raw);
      int32_t value = (int32_t)raw;
      int32_t scaled = value >> CONTEST_I2S_RAW_RMS_SCALE_SHIFT;

      if (value == 0)
        {
          zero_count++;
        }
      else if (value > 0)
        {
          positive_count++;
        }
      else
        {
          negative_count++;
        }

      if (raw != 0)
        {
          raw_mod[i % 4]++;
        }

      if (pcm == 0)
        {
          pcm_zero_count++;
          pcm_zero_run++;
          if (pcm_zero_run > pcm_longest_zero_run)
            {
              pcm_longest_zero_run = pcm_zero_run;
            }
        }
      else
        {
          pcm_mod[i % 4]++;
          pcm_nonzero_count++;
          pcm_zero_run = 0;
          if (pcm_index_count < CONTEST_I2S_RX_DIAG_INDICES)
            {
              pcm_indices[pcm_index_count++] = i;
            }
        }

      if ((raw & UINT32_C(0x000000ff)) != 0)
        {
          low8_nonzero++;
        }

      if ((raw & UINT32_C(0x0000ffff)) != 0)
        {
          low16_nonzero++;
        }

      if (value < raw_min)
        {
          raw_min = value;
        }

      if (value > raw_max)
        {
          raw_max = value;
        }

      raw_sum += value;
      raw_squares += (int64_t)scaled * scaled;
      if (have_previous)
        {
          int64_t delta = (int64_t)value - previous;

          if (delta < 0)
            {
              delta = -delta;
            }

          if ((uint64_t)delta > raw_max_delta)
            {
              raw_max_delta = delta;
            }
        }

      previous = value;
      have_previous = true;
      contest_i2s_pcm_stats_add(&stats_a,
                                 contest_i2s_pcm_candidate_a(raw));
      contest_i2s_pcm_stats_add(&stats_b,
                                 contest_i2s_pcm_candidate_b(raw));
      contest_i2s_pcm_stats_add(&stats_ca,
                                 contest_i2s_pcm_candidate_a(swapped));
      contest_i2s_pcm_stats_add(&stats_cb,
                                 contest_i2s_pcm_candidate_b(swapped));
    }

  for (i = 0; i < words && i < 64; i++)
    {
      syslog(LOG_INFO, "[C-I2S] raw[%u]=%08lx\n", i,
             (unsigned long)raw_words[i]);
    }

  syslog(LOG_INFO,
         "[C-I2S] raw words=%u min=%ld max=%ld mean=%ld rms=%lu "
         "zero=%u positive=%u negative=%u low8_nonzero=%u "
         "low16_nonzero=%u max_delta=%lu\n",
         (unsigned int)words, (long)raw_min, (long)raw_max,
         (long)(words == 0 ? 0 : raw_sum / (int64_t)words),
         (unsigned long)(contest_i2s_isqrt(raw_squares / words) <<
                         CONTEST_I2S_RAW_RMS_SCALE_SHIFT),
         zero_count, positive_count, negative_count, low8_nonzero,
         low16_nonzero, (unsigned long)raw_max_delta);
  contest_i2s_log_pcm_stats("A", &stats_a, words);
  contest_i2s_log_pcm_stats("B", &stats_b, words);
  contest_i2s_log_pcm_stats("C-A", &stats_ca, words);
  contest_i2s_log_pcm_stats("C-B", &stats_cb, words);
  syslog(LOG_INFO,
         "[C-I2S] stride raw nonzero mod0=%u mod1=%u mod2=%u mod3=%u\n",
         raw_mod[0], raw_mod[1], raw_mod[2], raw_mod[3]);
  syslog(LOG_INFO,
         "[C-I2S] stride pcm nonzero mod0=%u mod1=%u mod2=%u mod3=%u\n",
         pcm_mod[0], pcm_mod[1], pcm_mod[2], pcm_mod[3]);
  syslog(LOG_INFO,
         "[C-I2S] stride pcm zero=%u nonzero=%u longest_zero_run=%u\n",
         pcm_zero_count, pcm_nonzero_count, pcm_longest_zero_run);
  for (i = 0; i < pcm_index_count; i++)
    {
      syslog(LOG_INFO, "[C-I2S] stride pcm nonzero_index[%u]=%u\n", i,
             pcm_indices[i]);
    }
}

static bool contest_i2s_rx_has_signal(FAR const int16_t *pcm, size_t samples)
{
  unsigned int i;

  for (i = 0; i < samples; i++)
    {
      int32_t value = pcm[i];

      if (value < 0)
        {
          value = -value;
        }

      if (value >= CONTEST_I2S_RX_DIAG_SIGNAL_PEAK)
        {
          return true;
        }
    }

  return false;
}

static void contest_i2s_capture_rx_dma_diag(
  FAR struct contest_i2s_s *priv, FAR struct contest_i2s_rx_xfer_s *xfer)
{
  FAR struct contest_i2s_rx_dma_diag_s *diag = &priv->dma_diag;
  size_t words = xfer->raw_bytes / sizeof(uint32_t);
  size_t untouched_run = 0;
  size_t i;

  memset(diag, 0, sizeof(*diag));
  diag->valid = true;
  diag->expected_bytes = xfer->raw_bytes;
  diag->queued_bytes = xfer->queued_bytes;
  diag->first_modified = SIZE_MAX;
  diag->last_modified = SIZE_MAX;
  memcpy(diag->desc, xfer->desc, sizeof(diag->desc));

  for (i = 0; i < words; i++)
    {
      if (xfer->raw[i] == CONTEST_I2S_RX_DMA_SENTINEL)
        {
          diag->untouched_words++;
          untouched_run++;
          if (untouched_run > diag->longest_untouched_run)
            {
              diag->longest_untouched_run = untouched_run;
            }
        }
      else
        {
          if (diag->first_modified == SIZE_MAX)
            {
              diag->first_modified = i;
            }

          diag->last_modified = i;
          diag->modified_words++;
          untouched_run = 0;
        }
    }
}

static void contest_i2s_log_rx_dma_diag(
  FAR const struct contest_i2s_rx_dma_diag_s *diag)
{
  unsigned int i;

  if (!diag->valid)
    {
      syslog(LOG_INFO, "[C-I2S] dma diagnostic unavailable\n");
      return;
    }

  syslog(LOG_INFO,
         "[C-I2S] dma expected=%zu queued=%lu modified_words=%zu "
         "untouched_words=%zu first_modified=%zu last_modified=%zu "
         "longest_untouched_run=%zu\n",
         diag->expected_bytes, (unsigned long)diag->queued_bytes,
         diag->modified_words, diag->untouched_words, diag->first_modified,
         diag->last_modified, diag->longest_untouched_run);

  for (i = 0; i < CONTEST_I2S_DMA_DESC_COUNT; i++)
    {
      uint32_t ctrl = diag->desc[i].ctrl;
      uint32_t size = (ctrl >> ESP32S3_DMA_CTRL_BUFLEN_S) &
                      ESP32S3_DMA_CTRL_BUFLEN_V;
      uint32_t length = (ctrl >> ESP32S3_DMA_CTRL_DATALEN_S) &
                        ESP32S3_DMA_CTRL_DATALEN_V;

      syslog(LOG_INFO,
             "[C-I2S] dma desc=%u buf=%p size=%lu length=%lu owner=%u "
             "eof=%u next=%p\n",
             i, diag->desc[i].pbuf, (unsigned long)size,
             (unsigned long)length,
             (ctrl & ESP32S3_DMA_CTRL_OWN) != 0,
             (ctrl & ESP32S3_DMA_CTRL_EOF) != 0, diag->desc[i].next);
    }
}

static void contest_i2s_stop_hw(FAR struct contest_i2s_s *priv)
{
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), I2S_RX_START, 0);
  esp32s3_dma_disable(priv->dma_channel, false);
  CLR_GDMA_CH_BITS(DMA_IN_INT_ENA_CH0_REG, priv->dma_channel,
                   DMA_IN_SUC_EOF_CH0_INT_ENA | DMA_IN_ERR_EOF_CH0_INT_ENA);
  SET_GDMA_CH_REG(DMA_IN_INT_CLR_CH0_REG, priv->dma_channel, UINT32_MAX);
}

/* The generic ESP32-S3 helper splits a 4096-byte RX request into 4095+1.
 * With the I2S RX EOF unit used by this board, that layout can terminate
 * after the first short descriptor.  Keep RX descriptors 4-byte aligned and
 * use two equal buffers for the normal 4096-byte transaction.
 */

static uint32_t contest_i2s_dma_setup_rx(
  FAR struct esp32s3_dmadesc_s *desc, uint32_t count,
  FAR uint8_t *buffer, uint32_t bytes)
{
  uint32_t offset = 0;
  uint32_t i;

  memset(desc, 0, count * sizeof(*desc));

  for (i = 0; i < count && offset < bytes; i++)
    {
      uint32_t length = bytes - offset;

      if (length > CONTEST_I2S_RX_DESC_BYTES)
        {
          length = CONTEST_I2S_RX_DESC_BYTES;
        }

      desc[i].ctrl = ESP32S3_DMA_CTRL_OWN |
                     (length << ESP32S3_DMA_CTRL_BUFLEN_S);
      desc[i].pbuf = buffer + offset;
      offset += length;
      desc[i].next = offset < bytes ? &desc[i + 1] : NULL;
    }

  return offset;
}

/* The local ESP32-S3 reference lower-half writes its eof_nbytes directly to
 * I2S_RX_EOF_NUM.  Keep this board-local experiment in the same byte-count
 * unit; do not derive the field from the 32-bit container width.
 */

static uint32_t contest_i2s_rx_eof_value(size_t raw_bytes)
{
  DEBUGASSERT(raw_bytes <= I2S_RX_EOF_NUM_V);
  return raw_bytes;
}

static size_t contest_i2s_rx_completed_bytes(
  FAR const struct contest_i2s_rx_xfer_s *xfer)
{
  size_t bytes = 0;
  unsigned int i;

  for (i = 0; i < CONTEST_I2S_DMA_DESC_COUNT; i++)
    {
      uint32_t length = (xfer->desc[i].ctrl >> ESP32S3_DMA_CTRL_DATALEN_S) &
                        ESP32S3_DMA_CTRL_DATALEN_V;

      bytes += length;
      if (xfer->desc[i].next == NULL)
        {
          break;
        }
    }

  return bytes > xfer->raw_bytes ? xfer->raw_bytes : bytes;
}

static void contest_i2s_set_format(FAR struct contest_i2s_s *priv)
{
  modifyreg32(I2S_RX_CONF1_REG(CONTEST_I2S_PORT), I2S_RX_BITS_MOD_M,
              FIELD_TO_VALUE(I2S_RX_BITS_MOD,
                             CONTEST_INMP441_SLOT_BITS - 1));
  modifyreg32(I2S_RX_CONF1_REG(CONTEST_I2S_PORT), I2S_RX_TDM_CHAN_BITS_M,
              FIELD_TO_VALUE(I2S_RX_TDM_CHAN_BITS,
                             CONTEST_INMP441_SLOT_BITS - 1));
  modifyreg32(I2S_RX_CONF1_REG(CONTEST_I2S_PORT),
              I2S_RX_HALF_SAMPLE_BITS_M,
              FIELD_TO_VALUE(I2S_RX_HALF_SAMPLE_BITS,
                             CONTEST_INMP441_SLOT_BITS - 1));
  modifyreg32(I2S_RX_CONF1_REG(CONTEST_I2S_PORT), I2S_RX_TDM_WS_WIDTH_M,
              FIELD_TO_VALUE(I2S_RX_TDM_WS_WIDTH,
                             CONTEST_INMP441_SLOT_BITS - 1));
  modifyreg32(I2S_RX_TDM_CTRL_REG(CONTEST_I2S_PORT),
              I2S_RX_TDM_TOT_CHAN_NUM_M | CONTEST_I2S_RX_TDM_CHAN_MASK,
              FIELD_TO_VALUE(I2S_RX_TDM_TOT_CHAN_NUM,
                             CONTEST_I2S_SLOTS - 1) |
              CONTEST_INMP441_RX_SLOT_MASK);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), 0, I2S_RX_UPDATE);
}

static void contest_i2s_set_rate(FAR struct contest_i2s_s *priv)
{
  uint32_t mclk = priv->rate * CONTEST_I2S_MCLK_MULTIPLE;
  uint32_t mclk_div = CONTEST_I2S_SOURCE_CLOCK / mclk;
  uint32_t remainder = CONTEST_I2S_SOURCE_CLOCK % mclk;
  uint32_t bclk = priv->rate * CONTEST_I2S_SLOTS *
                  CONTEST_INMP441_SLOT_BITS;
  uint32_t bclk_div = mclk / bclk;
  uint32_t regval;
  uint32_t numerator = 0;
  uint32_t denominator = 1;
  uint32_t a;

  /* Match the ESP32-S3 reference driver's CLK160 fractional divider. */

  if (remainder != 0)
    {
      for (a = 2; a <= 63; a++)
        {
          uint32_t b = (remainder * a + mclk / 2) / mclk;

          if (b != 0 && remainder * a == mclk * b)
            {
              numerator = b;
              denominator = a;
              break;
            }
        }
    }

  if (numerator == 0)
    {
      denominator = 1;
    }

  regval = getreg32(I2S_RX_CLKM_CONF_REG(CONTEST_I2S_PORT));
  regval &= ~I2S_RX_CLKM_DIV_NUM_M;
  regval |= FIELD_TO_VALUE(I2S_RX_CLKM_DIV_NUM, mclk_div);
  putreg32(regval, I2S_RX_CLKM_CONF_REG(CONTEST_I2S_PORT));

  regval = getreg32(I2S_RX_CLKM_DIV_CONF_REG(CONTEST_I2S_PORT));
  regval &= ~(I2S_RX_CLKM_DIV_Z_M | I2S_RX_CLKM_DIV_Y_M |
              I2S_RX_CLKM_DIV_X_M | I2S_RX_CLKM_DIV_YN1_M);
  if (numerator != 0)
    {
      regval |= FIELD_TO_VALUE(I2S_RX_CLKM_DIV_Z, numerator);
      regval |= FIELD_TO_VALUE(I2S_RX_CLKM_DIV_Y, denominator % numerator);
      regval |= FIELD_TO_VALUE(I2S_RX_CLKM_DIV_X,
                               denominator / numerator - 1);
    }

  putreg32(regval, I2S_RX_CLKM_DIV_CONF_REG(CONTEST_I2S_PORT));

  modifyreg32(I2S_RX_CONF1_REG(CONTEST_I2S_PORT), I2S_RX_BCK_DIV_NUM_M,
              FIELD_TO_VALUE(I2S_RX_BCK_DIV_NUM, bclk_div - 1));
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), 0, I2S_RX_UPDATE);
}

static void contest_i2s_rx_done_push(FAR struct contest_i2s_s *priv,
                                     FAR struct contest_i2s_rx_xfer_s *xfer)
{
  xfer->next = NULL;
  if (priv->done_tail == NULL)
    {
      priv->done_head = xfer;
    }
  else
    {
      priv->done_tail->next = xfer;
    }

  priv->done_tail = xfer;
}

/* Called with priv->lock held.  Unlike the old single-buffer path, this is
 * used for the next pending buffer directly from EOF ISR context.  It keeps
 * RX_START asserted across normal chunk boundaries.
 */

static void contest_i2s_rx_start_active(FAR struct contest_i2s_s *priv)
{
  FAR struct contest_i2s_rx_xfer_s *xfer = priv->active;
  uint32_t eof_value;

  DEBUGASSERT(xfer != NULL);

  eof_value = contest_i2s_rx_eof_value(xfer->raw_bytes);
  DEBUGASSERT(xfer->raw_bytes != 0 && eof_value <= I2S_RX_EOF_NUM_V);

  esp32s3_dma_load(xfer->desc, priv->dma_channel, false);
  SET_GDMA_CH_REG(DMA_IN_INT_CLR_CH0_REG, priv->dma_channel, UINT32_MAX);
  SET_GDMA_CH_BITS(DMA_IN_INT_ENA_CH0_REG, priv->dma_channel,
                   DMA_IN_SUC_EOF_CH0_INT_ENA | DMA_IN_ERR_EOF_CH0_INT_ENA);
  modifyreg32(I2S_RXEOF_NUM_REG(CONTEST_I2S_PORT), I2S_RX_EOF_NUM_M,
              FIELD_TO_VALUE(I2S_RX_EOF_NUM, eof_value));
  esp32s3_dma_enable(priv->dma_channel, false);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), 0, I2S_RX_START);
  priv->started_once = true;
  priv->idle = false;
}

static void contest_i2s_rx_worker(FAR void *arg)
{
  FAR struct contest_i2s_s *priv = arg;
  FAR struct contest_i2s_rx_xfer_s *xfer;
  FAR struct ap_buffer_s *apb;
  i2s_callback_t callback;
  FAR void *callback_arg;
  int result;
  uint32_t irq_status;
  irqstate_t flags;
  size_t logical_bytes;
  size_t raw_bytes;
  size_t logical_samples;
  size_t dma_bytes;
  size_t dma_samples;
  unsigned int i;

  for (; ;)
    {
      flags = spin_lock_irqsave(&priv->lock);
      xfer = priv->done_head;
      if (xfer != NULL)
        {
          priv->done_head = xfer->next;
          if (priv->done_head == NULL)
            {
              priv->done_tail = NULL;
            }

          xfer->next = NULL;
        }

      irq_status = priv->irq_status;
      spin_unlock_irqrestore(&priv->lock, flags);

      if (xfer == NULL)
        {
          return;
        }

      result = xfer->result;
      logical_bytes = xfer->logical_bytes;
      raw_bytes = xfer->raw_bytes;
      logical_samples = logical_bytes / sizeof(int16_t);
      dma_bytes = contest_i2s_rx_completed_bytes(xfer);
      dma_samples = dma_bytes / sizeof(uint32_t);

      if (result >= 0 &&
          (logical_bytes == 0 ||
           raw_bytes != logical_samples * sizeof(uint32_t) ||
           dma_bytes != raw_bytes ||
           dma_bytes % sizeof(uint32_t) != 0))
        {
          result = -EIO;
        }

      up_invalidate_dcache((uintptr_t)xfer->raw,
                           (uintptr_t)xfer->raw + raw_bytes);

      if (priv->diagnostics_enabled && xfer->dma_diag)
        {
          contest_i2s_capture_rx_dma_diag(priv, xfer);
          xfer->dma_diag = false;
        }

      if (result >= 0)
        {
          FAR int16_t *pcm = (FAR int16_t *)(xfer->apb->samp +
                                             xfer->apb->curbyte);

          /* Only convert words that hardware reports as received.  A short
           * transaction is rejected above, so unwritten sentinel/old data
           * can never be exposed as a successful PCM frame. */

          for (i = 0; i < dma_samples; i++)
            {
              pcm[i] = (int16_t)((int32_t)xfer->raw[i] >>
                                 CONTEST_I2S_RX_PCM_SHIFT);
            }

          /* Preserve the first clearly non-silent frame for post-stop
           * diagnostics.  Raw words are never printed while recording.
           */

          if (priv->diagnostics_enabled && !priv->raw_logged &&
              contest_i2s_rx_has_signal(pcm, dma_samples))
            {
              memcpy(priv->raw_diag, xfer->raw, raw_bytes);
              priv->raw_diag_words = dma_samples;
              priv->raw_logged = true;
            }

          xfer->apb->nbytes = logical_bytes;
          xfer->apb->nsamples = logical_samples;
        }
      else
        {
          xfer->apb->nbytes = 0;
          xfer->apb->nsamples = 0;
        }

      apb = xfer->apb;
      callback = xfer->callback;
      callback_arg = xfer->arg;

      /* Conversion is complete.  Admit a replacement before invoking the
       * upper callback, exactly as the upstream done queue permits.
       */

      flags = spin_lock_irqsave(&priv->lock);
      xfer->in_use = false;
      spin_unlock_irqrestore(&priv->lock, flags);

      if (priv->eof_count <= 2 || result < 0)
        {
          syslog(LOG_INFO, "[C-I2S] rx eof status=%08lx\n",
                 (unsigned long)irq_status);
          syslog(LOG_INFO, "[C-I2S] rx worker bytes=%zu\n", logical_bytes);
          syslog(LOG_INFO, "[C-I2S] rx callback\n");
        }

      callback(&priv->dev, apb, callback_arg, result);
      apb_free(apb);
    }
}

static int contest_i2s_rx_interrupt(int irq, FAR void *context,
                                    FAR void *arg)
{
  FAR struct contest_i2s_s *priv = arg;
  uint32_t status;
  irqstate_t flags;

  (void)irq;
  (void)context;

  status = GET_GDMA_CH_REG(DMA_IN_INT_ST_CH0_REG, priv->dma_channel);
  SET_GDMA_CH_REG(DMA_IN_INT_CLR_CH0_REG, priv->dma_channel, UINT32_MAX);

  if ((status & (DMA_IN_SUC_EOF_CH0_INT_ST |
                 DMA_IN_ERR_EOF_CH0_INT_ST)) == 0)
    {
      return OK;
    }

  flags = spin_lock_irqsave(&priv->lock);
  if (priv->active != NULL)
    {
      priv->irq_status = status;
      if ((status & DMA_IN_SUC_EOF_CH0_INT_ST) != 0)
        {
          priv->active->result = OK;
          contest_i2s_rx_done_push(priv, priv->active);
          priv->active = priv->pending;
          priv->pending = NULL;
          priv->eof_count++;

          if (priv->active != NULL && priv->streaming)
            {
              contest_i2s_rx_start_active(priv);
              priv->chained_count++;
              if (priv->eof_count <= 2)
                {
                  syslog(LOG_INFO, "[C-I2S] rx eof=%lu chained\n",
                         (unsigned long)priv->eof_count);
                }
            }
          else
            {
              /* No pending transaction is either the intentional final
               * drain or a potential underrun.  Count it only if a later
               * submission resumes this idle stream.
               */

              priv->idle = true;
            }
        }
      else
        {
          priv->active->result = -EIO;
          contest_i2s_rx_done_push(priv, priv->active);
          priv->active = NULL;
          if (priv->pending != NULL)
            {
              priv->pending->result = -ECANCELED;
              contest_i2s_rx_done_push(priv, priv->pending);
              priv->pending = NULL;
            }

          priv->streaming = false;
          contest_i2s_stop_hw(priv);
        }

      if (work_available(&priv->rx_work))
        {
          work_queue(HPWORK, &priv->rx_work, contest_i2s_rx_worker, priv,
                     0);
        }
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}

static void contest_i2s_configure(FAR struct contest_i2s_s *priv)
{
  modifyreg32(SYSTEM_PERIP_CLK_EN0_REG, 0, SYSTEM_I2S0_CLK_EN);
  modifyreg32(SYSTEM_PERIP_RST_EN0_REG, 0, SYSTEM_I2S0_RST);
  modifyreg32(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_I2S0_RST, 0);

  modifyreg32(I2S_TX_CLKM_CONF_REG(CONTEST_I2S_PORT), 0, I2S_CLK_EN);
  modifyreg32(I2S_RX_CLKM_CONF_REG(CONTEST_I2S_PORT), I2S_RX_CLK_SEL_M,
              FIELD_TO_VALUE(I2S_RX_CLK_SEL, 2));
  modifyreg32(I2S_RX_CLKM_CONF_REG(CONTEST_I2S_PORT), 0,
              I2S_RX_CLK_ACTIVE);

  esp32s3_configgpio(AUDIO_I2S_MIC_GPIO_DIN, INPUT_FUNCTION_2);
  esp32s3_gpio_matrix_in(AUDIO_I2S_MIC_GPIO_DIN, I2S0I_SD_IN_IDX, 0);
  esp32s3_gpiowrite(AUDIO_I2S_MIC_GPIO_WS, 1);
  esp32s3_configgpio(AUDIO_I2S_MIC_GPIO_WS, OUTPUT_FUNCTION_2);
  esp32s3_gpio_matrix_out(AUDIO_I2S_MIC_GPIO_WS, I2S0I_WS_OUT_IDX, 0, 0);
  esp32s3_gpiowrite(AUDIO_I2S_MIC_GPIO_SCK, 1);
  esp32s3_configgpio(AUDIO_I2S_MIC_GPIO_SCK, OUTPUT_FUNCTION_2);
  esp32s3_gpio_matrix_out(AUDIO_I2S_MIC_GPIO_SCK, I2S0I_BCK_OUT_IDX, 0, 0);

  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), 0, I2S_RX_RESET);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), I2S_RX_RESET, 0);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), 0, I2S_RX_FIFO_RESET);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), I2S_RX_FIFO_RESET, 0);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), I2S_RX_SLAVE_MOD, 0);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), I2S_RX_TDM_EN_M,
              I2S_RX_TDM_EN);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), I2S_RX_WS_IDLE_POL, 0);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), I2S_RX_24_FILL_EN,
              I2S_RX_LEFT_ALIGN);
  modifyreg32(I2S_RX_CONF1_REG(CONTEST_I2S_PORT), I2S_RX_MSB_SHIFT_M,
              I2S_RX_MSB_SHIFT);
  modifyreg32(I2S_RX_CONF_REG(CONTEST_I2S_PORT), I2S_RX_BIG_ENDIAN, 0);
  contest_i2s_set_format(priv);
  contest_i2s_set_rate(priv);

  syslog(LOG_INFO, "[C-I2S] gpio configured\n");
  syslog(LOG_INFO,
         "[C-I2S] RXCHUNK1024 build=2 eof_mode=direct_raw_bytes\n");
  syslog(LOG_INFO,
         "[C-I2S] physical rx rate=%lu slot=%u valid=%u left-mask=%#x\n",
         (unsigned long)priv->rate, CONTEST_INMP441_SLOT_BITS,
         CONTEST_INMP441_VALID_BITS, CONTEST_INMP441_RX_SLOT_MASK);
  syslog(LOG_INFO, "[C-I2S] bclk=%lu\n",
         (unsigned long)(priv->rate * CONTEST_I2S_SLOTS *
                         CONTEST_INMP441_SLOT_BITS));
}

static int contest_i2s_rxchannels(FAR struct i2s_dev_s *dev,
                                  uint8_t channels)
{
  FAR struct contest_i2s_s *priv = (FAR struct contest_i2s_s *)dev;

  if (channels != 1 && channels != 2)
    {
      return -EINVAL;
  }

  priv->channels = channels;
  return OK;
}

static uint32_t contest_i2s_rxsamplerate(FAR struct i2s_dev_s *dev,
                                         uint32_t rate)
{
  FAR struct contest_i2s_s *priv = (FAR struct contest_i2s_s *)dev;

  if (rate != CONTEST_I2S_RATE)
    {
      return 0;
    }

  priv->rate = rate;
  contest_i2s_set_rate(priv);
  return rate;
}

static uint32_t contest_i2s_rxdatawidth(FAR struct i2s_dev_s *dev,
                                        int bits)
{
  FAR struct contest_i2s_s *priv = (FAR struct contest_i2s_s *)dev;

  if (bits != CONTEST_I2S_WIDTH)
    {
      return 0;
    }

  priv->data_width = bits;
  contest_i2s_set_format(priv);
  contest_i2s_set_rate(priv);
  return bits;
}

static int contest_i2s_receive(FAR struct i2s_dev_s *dev,
                               FAR struct ap_buffer_s *apb,
                               i2s_callback_t callback, FAR void *arg,
                               uint32_t timeout)
{
  FAR struct contest_i2s_s *priv = (FAR struct contest_i2s_s *)dev;
  FAR struct contest_i2s_rx_xfer_s *xfer = NULL;
  irqstate_t flags;
  size_t logical_bytes;
  size_t logical_samples;
  size_t raw_bytes;
  uint32_t queued;
  unsigned int i;

  (void)timeout;

  if (apb == NULL || callback == NULL || apb->samp == NULL)
    {
      return -EINVAL;
    }

  logical_bytes = apb->nmaxbytes - apb->curbyte;
  logical_bytes -= logical_bytes % sizeof(int16_t);
  logical_samples = logical_bytes / sizeof(int16_t);
  raw_bytes = logical_samples * sizeof(uint32_t);
  if (logical_bytes == 0 || raw_bytes > CONTEST_I2S_RX_RAW_MAX_BYTES)
    {
      return -EINVAL;
    }

  apb_reference(apb);

  /* Reserve a free transaction while the descriptor is built outside the
   * spinlock.  active and pending each retain independent raw DMA storage.
   */

  flags = spin_lock_irqsave(&priv->lock);
  if (!priv->streaming || priv->reserved || priv->pending != NULL)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      apb_free(apb);
      return -EBUSY;
    }

  for (i = 0; i < CONTEST_I2S_RX_XFER_COUNT; i++)
    {
      if (!priv->xfer[i].in_use)
        {
          xfer = &priv->xfer[i];
          xfer->in_use = true;
          xfer->dma_diag = !priv->dma_diag_armed;
          priv->dma_diag_armed = true;
          break;
        }
    }

  if (xfer == NULL)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      apb_free(apb);
      return -EBUSY;
    }

  priv->reserved = true;
  spin_unlock_irqrestore(&priv->lock, flags);

  xfer->next = NULL;
  xfer->apb = apb;
  xfer->callback = callback;
  xfer->arg = arg;
  xfer->logical_bytes = logical_bytes;
  xfer->raw_bytes = raw_bytes;
  xfer->queued_bytes = 0;
  xfer->result = -EINPROGRESS;
  if (xfer->dma_diag)
    {
      for (i = 0; i < logical_samples; i++)
        {
          xfer->raw[i] = CONTEST_I2S_RX_DMA_SENTINEL;
        }

      up_clean_dcache((uintptr_t)xfer->raw,
                      (uintptr_t)xfer->raw + raw_bytes);
    }

  queued = contest_i2s_dma_setup_rx(xfer->desc, CONTEST_I2S_DMA_DESC_COUNT,
                                     (FAR uint8_t *)xfer->raw, raw_bytes);
  xfer->queued_bytes = queued;
  if (queued != raw_bytes)
    {
      flags = spin_lock_irqsave(&priv->lock);
      priv->reserved = false;
      xfer->in_use = false;
      spin_unlock_irqrestore(&priv->lock, flags);
      apb_free(apb);
      return -ENOMEM;
    }

  /* Publish before hardware can complete.  The first transaction becomes
   * active; the second becomes pending and is started by the EOF ISR.
   */

  flags = spin_lock_irqsave(&priv->lock);
  if (!priv->streaming)
    {
      priv->reserved = false;
      xfer->in_use = false;
      spin_unlock_irqrestore(&priv->lock, flags);
      apb_free(apb);
      return -ECANCELED;
    }

  priv->reserved = false;
  if (priv->active == NULL)
    {
      if (priv->started_once && priv->idle)
        {
          priv->underrun_count++;
        }

      priv->active = xfer;
      contest_i2s_rx_start_active(priv);
    }
  else
    {
      priv->pending = xfer;
    }

  spin_unlock_irqrestore(&priv->lock, flags);

  if (priv->eof_count < 2)
    {
      uint32_t eof_reg = getreg32(I2S_RXEOF_NUM_REG(CONTEST_I2S_PORT));

      syslog(LOG_INFO, "[C-I2S] rx submit bytes=%zu\n", logical_bytes);
      syslog(LOG_INFO, "[C-I2S] raw bytes=%zu logical bytes=%zu\n",
             raw_bytes, logical_bytes);
      syslog(LOG_INFO,
             "[C-I2S] rx dma start raw=%zu eof_arg=%lu eof_reg=%08lx\n",
             raw_bytes,
             (unsigned long)contest_i2s_rx_eof_value(raw_bytes),
             (unsigned long)eof_reg);
    }

  return OK;
}

static int contest_i2s_ioctl(FAR struct i2s_dev_s *dev, int cmd,
                             unsigned long arg)
{
  FAR struct contest_i2s_s *priv = (FAR struct contest_i2s_s *)dev;
  FAR struct contest_i2s_rx_xfer_s *xfer;
  irqstate_t flags;
  bool schedule = false;

  (void)arg;

  switch (cmd)
    {
      case AUDIOIOC_START:
        flags = spin_lock_irqsave(&priv->lock);
        priv->streaming = true;
        priv->eof_count = 0;
        priv->chained_count = 0;
        priv->underrun_count = 0;
        priv->started_once = false;
        priv->idle = false;
        priv->raw_logged = false;
        priv->raw_diag_words = 0;
        priv->dma_diag_armed = !priv->diagnostics_enabled;
        memset(&priv->dma_diag, 0, sizeof(priv->dma_diag));
        spin_unlock_irqrestore(&priv->lock, flags);
        return OK;

      case AUDIOIOC_STOP:
        flags = spin_lock_irqsave(&priv->lock);
        priv->streaming = false;
        contest_i2s_stop_hw(priv);
        if (priv->active != NULL)
          {
            xfer = priv->active;
            xfer->result = -ECANCELED;
            contest_i2s_rx_done_push(priv, xfer);
            priv->active = NULL;
            schedule = true;
          }

        if (priv->pending != NULL)
          {
            xfer = priv->pending;
            xfer->result = -ECANCELED;
            contest_i2s_rx_done_push(priv, xfer);
            priv->pending = NULL;
            schedule = true;
          }

        priv->idle = false;
        spin_unlock_irqrestore(&priv->lock, flags);
        syslog(LOG_INFO, "[C-I2S] rx eof=%lu chained=%lu underrun=%lu\n",
               (unsigned long)priv->eof_count,
               (unsigned long)priv->chained_count,
               (unsigned long)priv->underrun_count);
        if (priv->diagnostics_enabled)
          {
            if (priv->raw_logged)
              {
                contest_i2s_log_raw_diagnostics(priv->raw_diag,
                                                 priv->raw_diag_words);
              }
            else
              {
                syslog(LOG_INFO,
                       "[C-I2S] raw stride diagnostics: no signal chunk\n");
              }

            contest_i2s_log_rx_dma_diag(&priv->dma_diag);
          }
        if (schedule && work_available(&priv->rx_work))
          {
            work_queue(HPWORK, &priv->rx_work, contest_i2s_rx_worker,
                       priv, 0);
          }

        return OK;

      default:
        return -ENOTTY;
    }
}


static void contest_i2s_tx_stop_hw(FAR struct contest_i2s_tx_s *priv)
{
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), I2S_TX_START, 0);
  esp32s3_dma_disable(priv->dma_channel, true);
  CLR_GDMA_CH_BITS(DMA_OUT_INT_ENA_CH0_REG, priv->dma_channel,
                   DMA_OUT_DONE_CH0_INT_ENA |
                   DMA_OUT_EOF_CH0_INT_ENA |
                   DMA_OUT_TOTAL_EOF_CH0_INT_ENA |
                   DMA_OUT_DSCR_ERR_CH0_INT_ENA);
  SET_GDMA_CH_REG(DMA_OUT_INT_CLR_CH0_REG, priv->dma_channel, UINT32_MAX);
  priv->hw_running = false;
}

static void contest_i2s_tx_done_push(FAR struct contest_i2s_tx_s *priv,
                                     FAR struct contest_i2s_xfer_s *xfer)
{
  xfer->next = NULL;
  if (priv->done_tail == NULL)
    {
      priv->done_head = xfer;
    }
  else
    {
      priv->done_tail->next = xfer;
    }

  priv->done_tail = xfer;
}

/* Forward declaration: tx_start_active() calls this helper before its
 * definition later in this file.
 */

static void contest_i2s_tx_update_wait(void);


static int contest_i2s_tx_desc_last(FAR struct contest_i2s_xfer_s *xfer,
                                    FAR unsigned int *last)
{
  unsigned int i = 0;

  while (i + 1 < CONTEST_I2S_DMA_DESC_COUNT &&
         xfer->desc[i].next != NULL)
    {
      i++;
    }

  /* 1024 bytes fits in one ESP32-S3 GDMA descriptor.  Keep exactly one
   * descriptor per audio slot so OUT_DONE maps 1:1 to one 21.33 ms slot.
   */
  if (i != 0 || (xfer->desc[i].ctrl & ESP32S3_DMA_CTRL_EOF) == 0)
    {
      return -EIO;
    }

  xfer->desc_last = (uint8_t)i;
  if (last != NULL)
    {
      *last = i;
    }

  return OK;
}

static void contest_i2s_tx_link_slot(FAR struct contest_i2s_tx_s *priv,
                                     unsigned int slot)
{
  FAR struct contest_i2s_xfer_s *xfer = &priv->xfer[slot];
  FAR struct contest_i2s_xfer_s *next =
    &priv->xfer[(slot + 1) % CONTEST_I2S_TX_RING_SLOTS];

  xfer->desc[xfer->desc_last].next = &next->desc[0];
  up_clean_dcache((uintptr_t)xfer->desc,
                  (uintptr_t)xfer->desc + sizeof(xfer->desc));
}


static int contest_i2s_tx_bind_slot(FAR struct contest_i2s_tx_s *priv,
                                    unsigned int slot,
                                    FAR struct ap_buffer_s *apb)
{
  FAR struct contest_i2s_xfer_s *xfer = &priv->xfer[slot];
  uint32_t queued;
  int ret;

  DEBUGASSERT(apb != NULL);
  DEBUGASSERT(apb->samp != NULL);
  DEBUGASSERT(!priv->hw_running);

  if (xfer->dma_bound)
    {
      return OK;
    }

  if (xfer->dma_data == NULL)
    {
      return -ENOMEM;
    }

  if (priv->logical_width == 16)
    {
      FAR const int16_t *src =
        (FAR const int16_t *)(apb->samp + apb->curbyte);
      FAR uint32_t *dst = (FAR uint32_t *)xfer->dma_data;
      unsigned int i;

      for (i = 0; i < CONTEST_I2S_TX_SLOT_BYTES / sizeof(uint32_t); i++)
        {
          /* Sign-extend PCM16 into the high half of the 32-bit Philips
           * slot.  Do not reinterpret the input buffer as 32-bit data. */
          dst[i] = ((uint32_t)(int32_t)src[i]) << 16;
        }
    }
  else
    {
      memcpy(xfer->dma_data, apb->samp + apb->curbyte,
             CONTEST_I2S_TX_SLOT_BYTES);
    }
  up_clean_dcache((uintptr_t)xfer->dma_data,
                  (uintptr_t)xfer->dma_data + CONTEST_I2S_TX_SLOT_BYTES);

  memset(xfer->desc, 0, sizeof(xfer->desc));
  queued = esp32s3_dma_setup(xfer->desc, CONTEST_I2S_DMA_DESC_COUNT,
                              xfer->dma_data, CONTEST_I2S_TX_SLOT_BYTES,
                              true, priv->dma_channel);
  if (queued != CONTEST_I2S_TX_SLOT_BYTES)
    {
      memset(xfer->desc, 0, sizeof(xfer->desc));
      return -ENOMEM;
    }

  ret = contest_i2s_tx_desc_last(xfer, NULL);
  if (ret < 0)
    {
      memset(xfer->desc, 0, sizeof(xfer->desc));
      return ret;
    }

  xfer->dma_bound = true;
  priv->bound_slots++;

  return OK;
}

static int contest_i2s_tx_link_immutable_ring(
  FAR struct contest_i2s_tx_s *priv)
{
  FAR struct contest_i2s_xfer_s *xfer;
  FAR struct contest_i2s_xfer_s *next;
  unsigned int i;

  if (priv->bound_slots != CONTEST_I2S_TX_RING_SLOTS)
    {
      return -EAGAIN;
    }

  for (i = 0; i < CONTEST_I2S_TX_RING_SLOTS; i++)
    {
      xfer = &priv->xfer[i];
      next = &priv->xfer[(i + 1) % CONTEST_I2S_TX_RING_SLOTS];

      if (!xfer->dma_bound || !next->dma_bound)
        {
          return -EINVAL;
        }

      xfer->desc[xfer->desc_last].next = &next->desc[0];
      up_clean_dcache((uintptr_t)xfer->desc,
                      (uintptr_t)xfer->desc + sizeof(xfer->desc));
    }

  return OK;
}


static bool contest_i2s_tx_complete_slot(
  FAR struct contest_i2s_tx_s *priv, unsigned int slot)
{
  FAR struct contest_i2s_xfer_s *xfer = &priv->xfer[slot];
  bool schedule = false;

  /*
   * The descriptor owns xfer->dma_data, never the upper APB.
   *
   * For a normal block, OUT_DONE means GDMA has already handed the payload to
   * the I2S peripheral, so publish its completion immediately.  Delaying every
   * normal callback makes refill lag behind the immutable cyclic ring and can
   * let hardware lap the producer at EOF.
   *
   * Only a terminal-short descriptor gets the separate 8 ms FIFO/shifter
   * drain in contest_i2s_tx_terminal_stop_worker().
   */

  if (xfer->in_use && !xfer->done_queued)
    {
      xfer->result = OK;
      xfer->done_queued = true;
      contest_i2s_tx_done_push(priv, xfer);
      schedule = true;

      if (priv->queued_slots > 0)
        {
          priv->queued_slots--;
        }
    }
  else
    {
      priv->silence_done_count++;
      priv->late_refill_count++;
      priv->underrun_count++;
    }

  priv->done_count++;
  priv->chained_count++;
  return schedule;
}


static void contest_i2s_tx_start_ring(FAR struct contest_i2s_tx_s *priv)
{
  DEBUGASSERT(!priv->hw_running);

  CLR_GDMA_CH_BITS(DMA_OUT_CONF1_CH0_REG, priv->dma_channel,
                   DMA_OUT_CHECK_OWNER_CH0);

  esp32s3_dma_load(priv->xfer[0].desc, priv->dma_channel, true);
  SET_GDMA_CH_REG(DMA_OUT_INT_CLR_CH0_REG, priv->dma_channel, UINT32_MAX);

  /* OUT_DONE is the completion event used by the fixed sequential ring. */
  SET_GDMA_CH_BITS(DMA_OUT_INT_ENA_CH0_REG, priv->dma_channel,
                   DMA_OUT_DONE_CH0_INT_ENA |
                   DMA_OUT_DSCR_ERR_CH0_INT_ENA);
  CLR_GDMA_CH_BITS(DMA_OUT_INT_ENA_CH0_REG, priv->dma_channel,
                   DMA_OUT_TOTAL_EOF_CH0_INT_ENA);
  esp32s3_dma_enable(priv->dma_channel, true);

  contest_i2s_tx_update_wait();
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), 0, I2S_TX_START);
  priv->hw_running = true;
  priv->dma_start_count++;

  syslog(LOG_INFO,
         "[C-I2S] tx GDMA-IMMUTABLE start=%lu slots=%u slot_bytes=%u "
         "irq=DONE(sequential) descriptors=frozen\n",
         (unsigned long)priv->dma_start_count,
         CONTEST_I2S_TX_RING_SLOTS, CONTEST_I2S_TX_SLOT_BYTES);
}


static void contest_i2s_tx_capture_state(FAR struct contest_i2s_tx_s *priv,
                                         FAR struct contest_i2s_xfer_s *xfer)
{
  priv->timeout_ena = GET_GDMA_CH_REG(DMA_OUT_INT_ENA_CH0_REG,
                                       priv->dma_channel);
  priv->timeout_raw = GET_GDMA_CH_REG(DMA_OUT_INT_RAW_CH0_REG,
                                       priv->dma_channel);
  priv->timeout_status = GET_GDMA_CH_REG(DMA_OUT_INT_ST_CH0_REG,
                                          priv->dma_channel);
  priv->timeout_link = GET_GDMA_CH_REG(DMA_OUT_LINK_CH0_REG,
                                        priv->dma_channel);
  priv->timeout_conf = getreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT));

  if (xfer != NULL)
    {
      priv->timeout_desc_ctrl = xfer->desc[xfer->desc_last].ctrl;
      priv->timeout_desc_addr = xfer->desc[xfer->desc_last].pbuf;
    }
}

static void contest_i2s_tx_dump_state(FAR struct contest_i2s_tx_s *priv)
{
  syslog(LOG_INFO,
         "[C-I2S] tx timeout ena=%08lx raw=%08lx st=%08lx link=%08lx conf=%08lx\n",
         (unsigned long)priv->timeout_ena,
         (unsigned long)priv->timeout_raw,
         (unsigned long)priv->timeout_status,
         (unsigned long)priv->timeout_link,
         (unsigned long)priv->timeout_conf);
  syslog(LOG_INFO, "[C-I2S] tx desc ctrl=%08lx addr=%p\n",
         (unsigned long)priv->timeout_desc_ctrl, priv->timeout_desc_addr);
}

static void contest_i2s_tx_update_wait(void)
{
  /*
   * ESP32-S3 TX configuration crosses from the APB clock domain into the
   * I2S TX clock domain.  Reading the APB register back is not sufficient:
   * TX_UPDATE must complete before another format/clock change or TX_START.
   */
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), 0, I2S_TX_UPDATE);
  while ((getreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT)) &
          I2S_TX_UPDATE) != 0)
    {
    }
}

/* Keep the extra 16 KiB out of static BSS: ASR runs before TTS and must not
 * lose heap/layout headroom merely because the speaker lower-half exists.
 * These buffers are DMA payloads, not descriptor memory. */

static int contest_i2s_tx_alloc_dma_slots(FAR struct contest_i2s_tx_s *priv)
{
  FAR struct contest_i2s_xfer_s *xfer;
  unsigned int i;

  for (i = 0; i < CONTEST_I2S_TX_RING_SLOTS; i++)
    {
      xfer = &priv->xfer[i];
      if (xfer->dma_data != NULL)
        {
          continue;
        }

      xfer->dma_data = kmm_memalign(CONTEST_I2S_TX_DMA_ALIGN,
                                     CONTEST_I2S_TX_SLOT_BYTES);
      if (xfer->dma_data == NULL)
        {
          while (i > 0)
            {
              i--;
              xfer = &priv->xfer[i];
              kmm_free(xfer->dma_data);
              xfer->dma_data = NULL;
            }

          return -ENOMEM;
        }

      memset(xfer->dma_data, 0, CONTEST_I2S_TX_SLOT_BYTES);
      up_clean_dcache((uintptr_t)xfer->dma_data,
                      (uintptr_t)xfer->dma_data +
                      CONTEST_I2S_TX_SLOT_BYTES);
    }

  return OK;
}

static void contest_i2s_tx_free_dma_slots(FAR struct contest_i2s_tx_s *priv)
{
  unsigned int i;

  for (i = 0; i < CONTEST_I2S_TX_RING_SLOTS; i++)
    {
      if (priv->xfer[i].dma_data != NULL)
        {
          kmm_free(priv->xfer[i].dma_data);
        }

      priv->xfer[i].dma_data = NULL;
      priv->xfer[i].dma_bound = false;
    }
}

static void contest_i2s_tx_set_format(FAR struct contest_i2s_tx_s *priv)
{
  uint32_t value = priv->data_width - 1;

  /* ESP32-S3 standard Philips-I2S timing used by the original board driver:
   *
   *   - two physical 32-bit slots per WS frame
   *   - WS width = one slot = 32 BCLKs (50% LRCK duty cycle)
   *   - one-BCLK MSB shift (Philips, not left-justified)
   *   - only the left slot carries the mono PCM word
   *   - MSB first, normal byte order, with data left-aligned in that word
   *
   * Keep all of these explicit.  Relying on reset defaults makes this board
   * path fragile when another I2S user has touched the peripheral before us.
   */

  modifyreg32(I2S_TX_CONF1_REG(CONTEST_I2S_TX_PORT), I2S_TX_BITS_MOD_M,
              FIELD_TO_VALUE(I2S_TX_BITS_MOD, value));
  modifyreg32(I2S_TX_CONF1_REG(CONTEST_I2S_TX_PORT),
              I2S_TX_TDM_CHAN_BITS_M,
              FIELD_TO_VALUE(I2S_TX_TDM_CHAN_BITS, value));
  modifyreg32(I2S_TX_CONF1_REG(CONTEST_I2S_TX_PORT),
              I2S_TX_HALF_SAMPLE_BITS_M,
              FIELD_TO_VALUE(I2S_TX_HALF_SAMPLE_BITS, value));
  modifyreg32(I2S_TX_CONF1_REG(CONTEST_I2S_TX_PORT),
              I2S_TX_TDM_WS_WIDTH_M,
              FIELD_TO_VALUE(I2S_TX_TDM_WS_WIDTH, value));
  modifyreg32(I2S_TX_CONF1_REG(CONTEST_I2S_TX_PORT),
              I2S_TX_MSB_SHIFT_M, I2S_TX_MSB_SHIFT);

#ifdef I2S_TX_WS_IDLE_POL
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT),
              I2S_TX_WS_IDLE_POL, 0);
#endif

#ifdef I2S_TX_LEFT_ALIGN
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT),
              0, I2S_TX_LEFT_ALIGN);
#endif


#ifdef I2S_TX_BIG_ENDIAN
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT),
              I2S_TX_BIG_ENDIAN, 0);
#endif

#ifdef I2S_TX_BIT_ORDER
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT),
              I2S_TX_BIT_ORDER, 0);
#endif

#ifdef I2S_TX_PCM_BYPASS
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT),
              0, I2S_TX_PCM_BYPASS);
#endif

  contest_i2s_tx_update_wait();
}

static void contest_i2s_tx_set_rate(FAR struct contest_i2s_tx_s *priv)
{
  uint32_t mclk = priv->rate * CONTEST_I2S_MCLK_MULTIPLE;
  uint32_t mclk_div = CONTEST_I2S_SOURCE_CLOCK / mclk;
  uint32_t freq_diff = CONTEST_I2S_SOURCE_CLOCK % mclk;
  uint32_t bclk = priv->rate * CONTEST_I2S_TX_SLOTS * priv->data_width;
  uint32_t bclk_div = mclk / bclk;
  uint32_t numerator = 0;   /* b */
  uint32_t denominator = 1; /* a */
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t z = 0;
  uint32_t yn1 = 0;
  uint32_t regval;
  uint32_t a;

  /*
   * Match Espressif ESP32-S3 i2s_ll_tx_set_clk() semantics exactly.
   *
   *   Fmclk = Fsrc / (N + b/a)
   *
   * The previous local implementation had a subtle but critical error:
   * for b <= a/2 it programmed Y = a % b, while the ESP32-S3 LL requires
   * Y = a % b + 1.
   *
   * At 24 kHz:
   *   mclk = 6.144 MHz
   *   160 MHz / 6.144 MHz = 26 + 1/24
   *   N=26, a=24, b=1 -> X=23, Y=1, Z=1, YN1=0.
   */

  if (mclk == 0 || bclk == 0 || bclk_div == 0)
    {
      return;
    }

  if (freq_diff != 0)
    {
      for (a = 2; a <= 63; a++)
        {
          uint32_t b = (freq_diff * a + mclk / 2) / mclk;

          if (b != 0 && freq_diff * a == mclk * b)
            {
              numerator = b;
              denominator = a;
              break;
            }
        }
    }

  if (numerator != 0)
    {
      if (numerator > denominator / 2)
        {
          uint32_t inv = denominator - numerator;

          x = denominator / inv - 1;
          y = denominator % inv;
          z = inv;
          yn1 = 1;
        }
      else
        {
          x = denominator / numerator - 1;
          y = denominator % numerator + 1;
          z = numerator;
          yn1 = 0;
        }
    }

  regval = getreg32(I2S_TX_CLKM_DIV_CONF_REG(CONTEST_I2S_TX_PORT));
  regval &= ~(I2S_TX_CLKM_DIV_Z_M | I2S_TX_CLKM_DIV_Y_M |
              I2S_TX_CLKM_DIV_X_M | I2S_TX_CLKM_DIV_YN1_M);
  regval |= FIELD_TO_VALUE(I2S_TX_CLKM_DIV_Z, z);
  regval |= FIELD_TO_VALUE(I2S_TX_CLKM_DIV_Y, y);
  regval |= FIELD_TO_VALUE(I2S_TX_CLKM_DIV_X, x);
  if (yn1 != 0)
    {
      regval |= I2S_TX_CLKM_DIV_YN1_M;
    }

  putreg32(regval, I2S_TX_CLKM_DIV_CONF_REG(CONTEST_I2S_TX_PORT));

  regval = getreg32(I2S_TX_CLKM_CONF_REG(CONTEST_I2S_TX_PORT));
  regval &= ~I2S_TX_CLKM_DIV_NUM_M;
  regval |= FIELD_TO_VALUE(I2S_TX_CLKM_DIV_NUM, mclk_div);
  putreg32(regval, I2S_TX_CLKM_CONF_REG(CONTEST_I2S_TX_PORT));

  modifyreg32(I2S_TX_CONF1_REG(CONTEST_I2S_TX_PORT), I2S_TX_BCK_DIV_NUM_M,
              FIELD_TO_VALUE(I2S_TX_BCK_DIV_NUM, bclk_div - 1));

  contest_i2s_tx_update_wait();

  syslog(LOG_INFO,
         "[C-I2S] tx fracclk rate=%lu N=%lu a=%lu b=%lu "
         "X=%lu Y=%lu Z=%lu YN1=%lu bdiv=%lu\n",
         (unsigned long)priv->rate,
         (unsigned long)mclk_div,
         (unsigned long)denominator,
         (unsigned long)numerator,
         (unsigned long)x,
         (unsigned long)y,
         (unsigned long)z,
         (unsigned long)yn1,
         (unsigned long)bclk_div);
}

static void contest_i2s_tx_set_channels(
  FAR struct contest_i2s_tx_s *priv)
{
  uint32_t channels_mask;

  /* The original ESP-IDF simplex codec uses slot_mode=MONO with LEFT mask.
   * On ESP32-S3 HW v2 TX_MONO means *copy to both slots* and is therefore
   * deliberately clear here; the LEFT channel mask provides the one active
   * DMA word per frame.  Two physical slots (TOT_CHAN_NUM=1) still preserve
   * the 64-BCLK LRCK frame. */
  modifyreg32(I2S_TX_TDM_CTRL_REG(CONTEST_I2S_TX_PORT),
              I2S_TX_TDM_TOT_CHAN_NUM_M,
              FIELD_TO_VALUE(I2S_TX_TDM_TOT_CHAN_NUM, 1));

  DEBUGASSERT(priv->channels == 1);
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT),
              I2S_TX_MONO_M | I2S_TX_CHAN_EQUAL_M, 0);

  channels_mask = getreg32(I2S_TX_TDM_CTRL_REG(CONTEST_I2S_TX_PORT));
  channels_mask &= 0xffff0000;
#ifdef I2S_TX_TDM_SKIP_MSK_EN
  channels_mask &= ~I2S_TX_TDM_SKIP_MSK_EN;
#endif
  channels_mask |= I2S_TX_TDM_CHAN0_EN;

  putreg32(channels_mask, I2S_TX_TDM_CTRL_REG(CONTEST_I2S_TX_PORT));
  contest_i2s_tx_update_wait();
}

static void contest_i2s_tx_configure(FAR struct contest_i2s_tx_s *priv)
{
  modifyreg32(SYSTEM_PERIP_CLK_EN0_REG, 0, SYSTEM_I2S1_CLK_EN);
  modifyreg32(SYSTEM_PERIP_RST_EN0_REG, 0, SYSTEM_I2S1_RST);
  modifyreg32(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_I2S1_RST, 0);
  modifyreg32(I2S_TX_CLKM_CONF_REG(CONTEST_I2S_TX_PORT), 0, I2S_CLK_EN);
  modifyreg32(I2S_TX_CLKM_CONF_REG(CONTEST_I2S_TX_PORT), I2S_TX_CLK_SEL_M,
              FIELD_TO_VALUE(I2S_TX_CLK_SEL, 2));
  modifyreg32(I2S_TX_CLKM_CONF_REG(CONTEST_I2S_TX_PORT), 0,
              I2S_TX_CLK_ACTIVE);

  esp32s3_gpiowrite(AUDIO_I2S_SPK_GPIO_LRCK, 1);
  esp32s3_configgpio(AUDIO_I2S_SPK_GPIO_LRCK, OUTPUT_FUNCTION_2);
  esp32s3_gpio_matrix_out(AUDIO_I2S_SPK_GPIO_LRCK, I2S1O_WS_OUT_IDX, 0, 0);
  esp32s3_gpiowrite(AUDIO_I2S_SPK_GPIO_BCLK, 1);
  esp32s3_configgpio(AUDIO_I2S_SPK_GPIO_BCLK, OUTPUT_FUNCTION_2);
  esp32s3_gpio_matrix_out(AUDIO_I2S_SPK_GPIO_BCLK, I2S1O_BCK_OUT_IDX, 0, 0);
  esp32s3_gpiowrite(AUDIO_I2S_SPK_GPIO_DOUT, 1);
  esp32s3_configgpio(AUDIO_I2S_SPK_GPIO_DOUT, OUTPUT_FUNCTION_2);
  esp32s3_gpio_matrix_out(AUDIO_I2S_SPK_GPIO_DOUT, I2S1O_SD_OUT_IDX, 0, 0);

  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), 0, I2S_TX_RESET);
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), I2S_TX_RESET, 0);
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), 0,
              I2S_TX_FIFO_RESET);
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), I2S_TX_FIFO_RESET, 0);
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), I2S_TX_SLAVE_MOD, 0);

#ifdef I2S_TX_STOP_EN
  /* Keep BCLK/WS running across descriptor boundaries.  The circular DMA
   * ring supplies silence only at the final explicit stop, never between
   * normal 2048-byte PCM blocks. */
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), I2S_TX_STOP_EN, 0);
#endif

  /* ESP32-S3 HW v2 uses the TDM engine for this standard mono-left path. */
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), I2S_TX_TDM_EN_M,
              I2S_TX_TDM_EN);
#ifdef I2S_TX_PDM_EN_M
  modifyreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT), I2S_TX_PDM_EN_M, 0);
#endif

  contest_i2s_tx_set_format(priv);
  contest_i2s_tx_set_rate(priv);
  contest_i2s_tx_set_channels(priv);

  syslog(LOG_INFO, "[C-I2S] tx gpio configured\n");
  syslog(LOG_INFO,
         "[C-I2S] tx PHILIPS24-GUIDE rate=%lu width=%u channels=%u "
         "bclk=%lu mclk=%lu\n",
         (unsigned long)priv->rate, priv->data_width, priv->channels,
         (unsigned long)(priv->rate * CONTEST_I2S_TX_SLOTS * priv->data_width),
         (unsigned long)(priv->rate * CONTEST_I2S_MCLK_MULTIPLE));
  syslog(LOG_INFO,
         "[C-I2S] tx continuity private-dma=dynamic completed-slot-clear=on "
         "normal_cb=immediate terminal_drain_ms=%u\n",
         CONTEST_I2S_TX_FIFO_DRAIN_MS);
  syslog(LOG_INFO,
         "[C-I2S] tx regs conf=%08lx conf1=%08lx tdm=%08lx "
         "clkm=%08lx clkm_div=%08lx\n",
         (unsigned long)getreg32(I2S_TX_CONF_REG(CONTEST_I2S_TX_PORT)),
         (unsigned long)getreg32(I2S_TX_CONF1_REG(CONTEST_I2S_TX_PORT)),
         (unsigned long)getreg32(I2S_TX_TDM_CTRL_REG(CONTEST_I2S_TX_PORT)),
         (unsigned long)getreg32(I2S_TX_CLKM_CONF_REG(CONTEST_I2S_TX_PORT)),
         (unsigned long)getreg32(I2S_TX_CLKM_DIV_CONF_REG(CONTEST_I2S_TX_PORT)));
}


static void contest_i2s_tx_worker(FAR void *arg)
{
  FAR struct contest_i2s_tx_s *priv = arg;
  FAR struct contest_i2s_xfer_s *xfer;
  FAR struct ap_buffer_s *apb;
  i2s_callback_t callback;
  FAR void *callback_arg;
  uint32_t irq_status;
  size_t nbytes;
  int result;
  irqstate_t flags;

  for (;;)
    {
      flags = spin_lock_irqsave(&priv->lock);
      xfer = priv->done_head;
      if (xfer == NULL)
        {
          spin_unlock_irqrestore(&priv->lock, flags);
          return;
        }

      priv->done_head = xfer->next;
      if (priv->done_head == NULL)
        {
          priv->done_tail = NULL;
        }

      xfer->next = NULL;
      apb = xfer->apb;
      callback = xfer->callback;
      callback_arg = xfer->arg;
      nbytes = xfer->nbytes;
      result = xfer->result;
      irq_status = priv->irq_status;

      /* The APB binding and descriptor links stay fixed for this run. */
      xfer->apb = NULL;
      xfer->callback = NULL;
      xfer->arg = NULL;
      xfer->nbytes = 0;
      xfer->result = OK;
      xfer->done_queued = false;
      xfer->in_use = false;
      xfer->terminal = false;

      spin_unlock_irqrestore(&priv->lock, flags);

      /*
       * Mirror the clean ESP-IDF reference driver's
       * auto_clear_after_cb=true behavior.
       *
       * This lower-half uses an immutable cyclic DMA descriptor ring whose
       * private payload buffer stays attached to the descriptor.  Once a slot
       * has completed, clear that private payload BEFORE publishing the upper
       * callback.  If the producer is late refilling the slot, the next ring
       * wrap therefore emits digital zero instead of replaying stale PCM.
       *
       * Normal completions reach this worker immediately after OUT_DONE; GDMA
       * has already consumed this private payload.  The final terminal-short
       * path still waits for FIFO/shifter drain before publishing completion.
       * AUDIOIOC_STOP may free DMA buffers before a canceled callback runs,
       * hence the NULL guard.
       */
      if (xfer->dma_data != NULL)
        {
          memset(xfer->dma_data, 0, CONTEST_I2S_TX_SLOT_BYTES);
          up_clean_dcache((uintptr_t)xfer->dma_data,
                          (uintptr_t)xfer->dma_data +
                          CONTEST_I2S_TX_SLOT_BYTES);
        }

      if (result < 0)
        {
          syslog(LOG_ERR,
                 "[C-I2S] tx completion failed irq=%08lx bytes=%zu rc=%d\n",
                 (unsigned long)irq_status, nbytes, result);
        }

      if (callback != NULL && apb != NULL)
        {
          callback(&priv->dev, apb, callback_arg, result);
          apb_free(apb);
        }
    }
}


static void contest_i2s_tx_terminal_stop_worker(FAR void *arg)
{
  FAR struct contest_i2s_tx_s *priv = arg;
  unsigned int slot;
  bool schedule = false;
  bool stopped = false;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);

  if (!priv->terminal_stop_pending)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      return;
    }

  slot = priv->terminal_stop_slot;
  priv->terminal_stop_pending = false;

  /*
   * The terminal descriptor already has next=NULL, so GDMA cannot wrap back
   * into stale PCM. Keep I2S alive until this delayed worker runs so the
   * peripheral FIFO / shifter can emit the final real samples cleanly.
   */
  if (priv->hw_running)
    {
      contest_i2s_tx_stop_hw(priv);
      stopped = true;
    }

  /*
   * Publish terminal completion only after the hardware drain. The upper
   * playback layer may issue AUDIOIOC_STOP immediately after this callback.
   */
  if (slot < CONTEST_I2S_TX_RING_SLOTS &&
      priv->xfer[slot].in_use &&
      !priv->xfer[slot].done_queued)
    {
      schedule = contest_i2s_tx_complete_slot(priv, slot);
    }

  spin_unlock_irqrestore(&priv->lock, flags);

  if (stopped)
    {
      syslog(LOG_INFO,
             "[C-I2S] tx terminal drain complete slot=%u delay_ms=%u\n",
             slot, CONTEST_I2S_TX_FIFO_DRAIN_MS);
    }

  if (schedule && work_available(&priv->tx_work))
    {
      work_queue(HPWORK, &priv->tx_work, contest_i2s_tx_worker, priv, 0);
    }
}


static int contest_i2s_tx_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct contest_i2s_tx_s *priv = arg;
  FAR struct contest_i2s_xfer_s *xfer;
  uint32_t status;
  unsigned int slot;
  unsigned int i;
  bool schedule = false;
  bool terminal_stop_schedule = false;
  bool fatal = false;
  irqstate_t flags;

  (void)irq;
  (void)context;

  /* OUT_DONE is sufficient for the fixed sequential ring. */
  status = GET_GDMA_CH_REG(DMA_OUT_INT_ST_CH0_REG, priv->dma_channel);
  SET_GDMA_CH_REG(DMA_OUT_INT_CLR_CH0_REG, priv->dma_channel, UINT32_MAX);

  if ((status & (DMA_OUT_DONE_CH0_INT_ST |
                 DMA_OUT_DSCR_ERR_CH0_INT_ST)) == 0)
    {
      return OK;
    }

  flags = spin_lock_irqsave(&priv->lock);
  priv->irq_status = status;

  if (!priv->streaming || !priv->hw_running)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      return OK;
    }

  if ((status & DMA_OUT_DSCR_ERR_CH0_INT_ST) != 0)
    {
      fatal = true;
    }
  else
    {
      /* The descriptor ring is immutable and advances in slot order.  Its
       * payload remains stable until the deferred callback below runs; do not
       * wait one extra ring block and replay stale PCM. */
      if ((status & DMA_OUT_DONE_CH0_INT_ST) != 0)
        {
          priv->done_irq_count++;

          bool terminal_done;

          slot = priv->done_index;
          terminal_done = priv->xfer[slot].terminal;

          if (terminal_done)
            {
              /*
               * OUT_DONE means GDMA finished the descriptor, but the I2S FIFO
               * / shifter may still contain the final samples. Do not stop HW
               * or publish the terminal callback until the drain delay passes.
               */
              priv->streaming = false;
              priv->terminal_stop_pending = true;
              priv->terminal_stop_slot = (uint8_t)slot;
              terminal_stop_schedule = true;
            }
          else if (contest_i2s_tx_complete_slot(priv, slot))
            {
              schedule = true;
            }

          priv->done_index = (slot + 1) % CONTEST_I2S_TX_RING_SLOTS;
          if (slot == CONTEST_I2S_TX_RING_SLOTS - 1)
            {
              priv->ring_wrap_count++;
            }

        }

    }

  if (fatal)
    {
      for (i = 0; i < CONTEST_I2S_TX_RING_SLOTS; i++)
        {
          xfer = &priv->xfer[i];
          if (xfer->in_use && !xfer->done_queued)
            {
              xfer->result = -EIO;
              xfer->done_queued = true;
              contest_i2s_tx_done_push(priv, xfer);
              schedule = true;
            }
        }

      priv->queued_slots = 0;
      priv->streaming = false;
      priv->terminal_stop_pending = false;
      contest_i2s_tx_stop_hw(priv);
    }

  if (terminal_stop_schedule)
    {
      if (work_available(&priv->tx_stop_work))
        {
          work_queue(HPWORK, &priv->tx_stop_work,
                     contest_i2s_tx_terminal_stop_worker, priv,
                     MSEC2TICK(CONTEST_I2S_TX_FIFO_DRAIN_MS));
        }
      else
        {
          /* Defensive fallback: never leave I2S running forever. */
          priv->terminal_stop_pending = false;
          contest_i2s_tx_stop_hw(priv);
          if (contest_i2s_tx_complete_slot(priv, priv->terminal_stop_slot))
            {
              schedule = true;
            }
          syslog(LOG_ERR,
                 "[C-I2S] terminal drain worker busy; immediate fallback\n");
        }
    }

  if (schedule && work_available(&priv->tx_work))
    {
      /*
       * Normal OUT_DONE callbacks are immediate.  Keeping the old 8 ms delay
       * here increased refill latency and let the cyclic DMA ring run ahead of
       * the logical producer near EOF.  Terminal FIFO drain is handled only by
       * tx_stop_work above.
       */
      work_queue(HPWORK, &priv->tx_work, contest_i2s_tx_worker, priv, 0);
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}


static int contest_i2s_txchannels(FAR struct i2s_dev_s *dev,
                                  uint8_t channels)
{
  FAR struct contest_i2s_tx_s *priv = (FAR struct contest_i2s_tx_s *)dev;

  syslog(LOG_INFO, "[C-I2S-DIAG] txchannels requested=%u\n", channels);

  /* One logical PCM channel is emitted in the physical left 32-bit slot. */
  if (channels != 1)
    {
      syslog(LOG_INFO, "[C-I2S-DIAG] txchannels rejected=%u expected=1\n",
             channels);
      return -ENOTSUP;
    }

  priv->channels = channels;
  contest_i2s_tx_set_channels(priv);
  syslog(LOG_INFO,
         "[C-I2S] tx format channels=1 width=32 dma_bytes=%u "
         "mode=mono-left frame=4B\n",
         CONTEST_I2S_TX_SLOT_BYTES);
  return OK;
}

static uint32_t contest_i2s_txsamplerate(FAR struct i2s_dev_s *dev,
                                         uint32_t rate)
{
  FAR struct contest_i2s_tx_s *priv = (FAR struct contest_i2s_tx_s *)dev;

  syslog(LOG_INFO, "[C-I2S-DIAG] txsamplerate requested=%lu expected=%u\n",
         (unsigned long)rate, CONTEST_I2S_TX_RATE);

  if (rate != CONTEST_I2S_TX_RATE)
    {
      syslog(LOG_ERR,
             "[C-I2S] reject tx rate=%lu expected=%u\n",
             (unsigned long)rate, CONTEST_I2S_TX_RATE);
      syslog(LOG_INFO, "[C-I2S-DIAG] txsamplerate rejected=%lu\n",
             (unsigned long)rate);
      return 0;
    }

  priv->rate = rate;
  contest_i2s_tx_set_rate(priv);

  syslog(LOG_INFO,
         "[C-I2S] tx rate applied GUIDE24=%lu bclk=%lu mclk=%lu "
         "conf1=%08lx clkm=%08lx div=%08lx\n",
         (unsigned long)priv->rate,
         (unsigned long)(priv->rate * CONTEST_I2S_TX_SLOTS * priv->data_width),
         (unsigned long)(priv->rate * CONTEST_I2S_MCLK_MULTIPLE),
         (unsigned long)getreg32(I2S_TX_CONF1_REG(CONTEST_I2S_TX_PORT)),
         (unsigned long)getreg32(I2S_TX_CLKM_CONF_REG(CONTEST_I2S_TX_PORT)),
         (unsigned long)getreg32(I2S_TX_CLKM_DIV_CONF_REG(CONTEST_I2S_TX_PORT)));

  syslog(LOG_INFO, "[C-I2S-DIAG] txsamplerate applied=%lu\n",
         (unsigned long)rate);

  return rate;
}

static uint32_t contest_i2s_txdatawidth(FAR struct i2s_dev_s *dev, int bits)
{
  FAR struct contest_i2s_tx_s *priv = (FAR struct contest_i2s_tx_s *)dev;

  syslog(LOG_INFO, "[C-I2S-DIAG] txdatawidth requested=%d wire_width=%u\n",
         bits, CONTEST_I2S_TX_WIDTH);

  if (bits != 16 && bits != CONTEST_I2S_TX_WIDTH)
    {
      syslog(LOG_INFO, "[C-I2S-DIAG] txdatawidth rejected=%d\n", bits);
      return 0;
    }

  priv->logical_width = bits;
  priv->data_width = CONTEST_I2S_TX_WIDTH;
  contest_i2s_tx_set_format(priv);
  contest_i2s_tx_set_rate(priv);
  syslog(LOG_INFO,
         "[C-I2S-DIAG] txdatawidth applied logical=%d wire_width=%u\n",
         bits, CONTEST_I2S_TX_WIDTH);
  return bits;
}



static int contest_i2s_send(FAR struct i2s_dev_s *dev,
                            FAR struct ap_buffer_s *apb,
                            i2s_callback_t callback, FAR void *arg,
                            uint32_t timeout)
{
  FAR struct contest_i2s_tx_s *priv = (FAR struct contest_i2s_tx_s *)dev;
  FAR struct contest_i2s_xfer_s *xfer;
  unsigned int slot;
  size_t nbytes;
  size_t wire_bytes;
  bool need_bind;
  bool terminal_short;
  bool was_streaming;
  bool was_reserved;
  bool slot_in_use;
  bool slot_done_queued;
  irqstate_t flags;
  int ret = OK;

  (void)timeout;

  if (apb == NULL || callback == NULL || apb->samp == NULL)
    {
      syslog(LOG_INFO, "[C-I2S-DIAG] send rejected invalid argument\n");
      return -EINVAL;
    }

  if (apb->nbytes < apb->curbyte)
    {
      syslog(LOG_INFO, "[C-I2S-DIAG] send rejected curbyte beyond nbytes\n");
      return -EINVAL;
    }

  nbytes = apb->nbytes - apb->curbyte;
  {
    size_t expected = CONTEST_I2S_TX_SLOT_BYTES;
    size_t align = sizeof(uint32_t);

    if (priv->logical_width == 16)
      {
        expected /= 2;
        align = sizeof(int16_t);
      }

    if (nbytes == 0 || nbytes > expected || (nbytes % align) != 0)
      {
        syslog(LOG_ERR,
               "[C-I2S] reject bytes=%zu max=%zu align=%zu\n",
               nbytes, expected, align);
        return -EINVAL;
      }

    terminal_short = nbytes < expected;
    wire_bytes = priv->logical_width == 16 ? nbytes * 2 : nbytes;

    if (wire_bytes == 0 || wire_bytes > CONTEST_I2S_TX_SLOT_BYTES ||
        (wire_bytes & 3) != 0)
      {
        syslog(LOG_ERR,
               "[C-I2S] reject wire bytes=%zu max=%u\n",
               wire_bytes, CONTEST_I2S_TX_SLOT_BYTES);
        return -EINVAL;
      }
  }

  apb_reference(apb);

  flags = spin_lock_irqsave(&priv->lock);
  if (!priv->streaming || priv->reserved)
    {
      was_streaming = priv->streaming;
      was_reserved = priv->reserved;
      spin_unlock_irqrestore(&priv->lock, flags);
      apb_free(apb);
      syslog(LOG_INFO, "[C-I2S-DIAG] send rejected busy streaming=%d reserved=%d\n",
             was_streaming ? 1 : 0, was_reserved ? 1 : 0);
      return -EBUSY;
    }

  slot = priv->fill_index;
  xfer = &priv->xfer[slot];

  if (xfer->in_use || xfer->done_queued)
    {
      slot_in_use = xfer->in_use;
      slot_done_queued = xfer->done_queued;
      spin_unlock_irqrestore(&priv->lock, flags);
      apb_free(apb);
      syslog(LOG_INFO,
             "[C-I2S-DIAG] send rejected slot=%u in_use=%d done_queued=%d\n",
             slot, slot_in_use ? 1 : 0, slot_done_queued ? 1 : 0);
      return -EBUSY;
    }

  need_bind = !xfer->dma_bound;

  if (terminal_short && need_bind)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      apb_free(apb);
      syslog(LOG_ERR,
             "[C-I2S] terminal-short rejected before ring bind slot=%u\n",
             slot);
      return -EINVAL;
    }

  if (need_bind && priv->hw_running)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      apb_free(apb);
      syslog(LOG_INFO, "[C-I2S-DIAG] send rejected bind while hw_running\n");
      return -EIO;
    }

  priv->reserved = true;
  spin_unlock_irqrestore(&priv->lock, flags);

  if (need_bind)
    {
      ret = contest_i2s_tx_bind_slot(priv, slot, apb);
      if (ret < 0)
        {
          goto fail_reserved;
        }
    }
  else
    {
      uint32_t queued;

      /* Normal requests retain the existing 2048-byte cyclic descriptor.
       * A short request is terminal: copy only real samples, rebuild this
       * already-completed descriptor at the real wire length, and set
       * next=NULL so hardware cannot loop into an old audio slot. */
      if (priv->logical_width == 16)
        {
          FAR const int16_t *src =
            (FAR const int16_t *)(apb->samp + apb->curbyte);
          FAR uint32_t *dst = (FAR uint32_t *)xfer->dma_data;
          unsigned int i;

          for (i = 0; i < wire_bytes / sizeof(uint32_t); i++)
            {
              dst[i] = ((uint32_t)(int32_t)src[i]) << 16;
            }
        }
      else
        {
          memcpy(xfer->dma_data, apb->samp + apb->curbyte, wire_bytes);
        }

      up_clean_dcache((uintptr_t)xfer->dma_data,
                      (uintptr_t)xfer->dma_data + wire_bytes);

      if (terminal_short)
        {
          memset(xfer->desc, 0, sizeof(xfer->desc));
          queued = esp32s3_dma_setup(xfer->desc,
                                     CONTEST_I2S_DMA_DESC_COUNT,
                                     xfer->dma_data, wire_bytes,
                                     true, priv->dma_channel);
          if (queued != wire_bytes)
            {
              ret = -ENOMEM;
              goto fail_reserved;
            }

          ret = contest_i2s_tx_desc_last(xfer, NULL);
          if (ret < 0)
            {
              goto fail_reserved;
            }

          xfer->desc[xfer->desc_last].next = NULL;
          up_clean_dcache((uintptr_t)xfer->desc,
                          (uintptr_t)xfer->desc + sizeof(xfer->desc));

          syslog(LOG_INFO,
                 "[C-I2S] tx terminal-short slot=%u wire_bytes=%zu "
                 "next=NULL\n",
                 slot, wire_bytes);
        }
    }

  flags = spin_lock_irqsave(&priv->lock);
  if (!priv->streaming)
    {
      priv->reserved = false;
      spin_unlock_irqrestore(&priv->lock, flags);
      apb_free(apb);
      syslog(LOG_INFO, "[C-I2S-DIAG] send canceled after bind\n");
      return -ECANCELED;
    }

  xfer->next = NULL;
  xfer->apb = apb;
  xfer->callback = callback;
  xfer->arg = arg;
  xfer->nbytes = wire_bytes;
  xfer->result = -EINPROGRESS;
  xfer->done_queued = false;
  xfer->terminal = terminal_short;
  xfer->in_use = true;

  priv->fill_index++;
  if (priv->fill_index >= CONTEST_I2S_TX_RING_SLOTS)
    {
      priv->fill_index = 0;
    }

  priv->queued_slots++;
  if (priv->queued_slots > priv->max_queued_slots)
    {
      priv->max_queued_slots = priv->queued_slots;
    }

  priv->submit_count++;

  /* Freeze and start only after all private DMA slots are initialised. */
  if (!priv->hw_running &&
      priv->bound_slots == CONTEST_I2S_TX_RING_SLOTS)
    {
      ret = contest_i2s_tx_link_immutable_ring(priv);
      if (ret < 0)
        {
          xfer->in_use = false;
          xfer->apb = NULL;
          xfer->callback = NULL;
          xfer->arg = NULL;
          xfer->nbytes = 0;
          if (priv->queued_slots > 0)
            {
              priv->queued_slots--;
            }

          priv->reserved = false;
          spin_unlock_irqrestore(&priv->lock, flags);
          apb_free(apb);
          syslog(LOG_INFO, "[C-I2S-DIAG] send rejected immutable link rc=%d\n",
                 ret);
          return ret;
        }

      contest_i2s_tx_start_ring(priv);
    }

  priv->reserved = false;
  spin_unlock_irqrestore(&priv->lock, flags);

  return OK;

fail_reserved:
  flags = spin_lock_irqsave(&priv->lock);
  priv->reserved = false;
  spin_unlock_irqrestore(&priv->lock, flags);
  apb_free(apb);
  syslog(LOG_INFO, "[C-I2S-DIAG] send failed bind rc=%d\n", ret);
  return ret;
}



static int contest_i2s_tx_ioctl(FAR struct i2s_dev_s *dev, int cmd,
                                unsigned long arg)
{
  FAR struct contest_i2s_tx_s *priv = (FAR struct contest_i2s_tx_s *)dev;
  FAR struct contest_i2s_xfer_s *xfer;
  unsigned int i;
  bool schedule = false;
  irqstate_t flags;
  int ret;

  syslog(LOG_INFO, "[C-I2S-DIAG] ioctl cmd=0x%x arg=0x%lx\n",
         cmd, arg);

  switch (cmd)
    {
      case AUDIOIOC_START:
        flags = spin_lock_irqsave(&priv->lock);
        if (priv->streaming || priv->hw_running || priv->done_head != NULL)
          {
            spin_unlock_irqrestore(&priv->lock, flags);
            syslog(LOG_INFO,
                   "[C-I2S-DIAG] ioctl START rejected busy streaming=%d "
                   "hw_running=%d done_head=%p\n",
                   priv->streaming ? 1 : 0, priv->hw_running ? 1 : 0,
                   priv->done_head);
            return -EBUSY;
          }

        spin_unlock_irqrestore(&priv->lock, flags);
        ret = contest_i2s_tx_alloc_dma_slots(priv);
        if (ret < 0)
          {
            syslog(LOG_ERR,
                   "[C-I2S] tx private DMA allocation failed bytes=%u\n",
                   CONTEST_I2S_TX_RING_SLOTS * CONTEST_I2S_TX_SLOT_BYTES);
            syslog(LOG_INFO, "[C-I2S-DIAG] ioctl START allocation rc=%d\n",
                   ret);
            return ret;
          }

        flags = spin_lock_irqsave(&priv->lock);
        if (priv->streaming || priv->hw_running || priv->done_head != NULL)
          {
            spin_unlock_irqrestore(&priv->lock, flags);
            contest_i2s_tx_free_dma_slots(priv);
            syslog(LOG_INFO,
                   "[C-I2S-DIAG] ioctl START rejected busy after allocation\n");
            return -EBUSY;
          }

        priv->streaming = true;
        priv->reserved = false;
        priv->terminal_stop_pending = false;
        priv->terminal_stop_slot = 0;
        priv->fill_index = 0;
        priv->done_index = 0;
        priv->queued_slots = 0;
        priv->max_queued_slots = 0;
        priv->bound_slots = 0;
        priv->done_count = 0;
        priv->chained_count = 0;
        priv->underrun_count = 0;
        priv->silence_done_count = 0;
        priv->late_refill_count = 0;
        priv->ring_wrap_count = 0;
        priv->submit_count = 0;
        priv->dma_start_count = 0;
        priv->done_irq_count = 0;

        for (i = 0; i < CONTEST_I2S_TX_RING_SLOTS; i++)
          {
            xfer = &priv->xfer[i];
            xfer->next = NULL;
            xfer->apb = NULL;
            xfer->dma_bound = false;
            xfer->callback = NULL;
            xfer->arg = NULL;
            xfer->nbytes = 0;
            xfer->result = OK;
            xfer->in_use = false;
            xfer->done_queued = false;
            xfer->terminal = false;
            xfer->desc_last = 0;
            memset(xfer->desc, 0, sizeof(xfer->desc));
          }

        spin_unlock_irqrestore(&priv->lock, flags);

        up_enable_irq(priv->tx_irq);
        syslog(LOG_INFO,
               "[C-I2S] tx GDMA-IMMUTABLE armed rate=%lu slots=%u "
               "slot_bytes=%u private_dma=dynamic\n",
               (unsigned long)priv->rate,
               CONTEST_I2S_TX_RING_SLOTS,
               CONTEST_I2S_TX_SLOT_BYTES);
        syslog(LOG_INFO, "[C-I2S-DIAG] ioctl START rc=0\n");
        return OK;

      case AUDIOIOC_STOP:
        flags = spin_lock_irqsave(&priv->lock);
        priv->streaming = false;
        priv->terminal_stop_pending = false;

        if (priv->hw_running)
          {
            contest_i2s_tx_capture_state(
              priv, &priv->xfer[priv->done_index]);
          }

        contest_i2s_tx_stop_hw(priv);

        for (i = 0; i < CONTEST_I2S_TX_RING_SLOTS; i++)
          {
            xfer = &priv->xfer[i];

            if (xfer->in_use && !xfer->done_queued)
              {
                xfer->result = -ECANCELED;
                xfer->done_queued = true;
                contest_i2s_tx_done_push(priv, xfer);
                schedule = true;
              }

            memset(xfer->desc, 0, sizeof(xfer->desc));
            xfer->desc_last = 0;
            xfer->dma_bound = false;
            xfer->terminal = false;
            up_clean_dcache((uintptr_t)xfer->desc,
                            (uintptr_t)xfer->desc + sizeof(xfer->desc));
          }

        priv->queued_slots = 0;
        spin_unlock_irqrestore(&priv->lock, flags);

        contest_i2s_tx_free_dma_slots(priv);

        syslog(LOG_INFO,
               "[C-I2S] tx GDMA-IMMUTABLE stop done=%lu chained=%lu "
               "underrun=%lu late_refill=%lu wraps=%lu "
               "submit=%lu max_queued=%u bound=%u dma_start_count=%lu "
               "done_irq=%lu\n",
               (unsigned long)priv->done_count,
               (unsigned long)priv->chained_count,
               (unsigned long)priv->underrun_count,
               (unsigned long)priv->late_refill_count,
               (unsigned long)priv->ring_wrap_count,
               (unsigned long)priv->submit_count,
               priv->max_queued_slots,
               priv->bound_slots,
               (unsigned long)priv->dma_start_count,
               (unsigned long)priv->done_irq_count);

        priv->bound_slots = 0;

        if (schedule || priv->done_head != NULL)
          {
            contest_i2s_tx_dump_state(priv);
            if (work_available(&priv->tx_work))
              {
                work_queue(HPWORK, &priv->tx_work, contest_i2s_tx_worker,
                           priv, 0);
              }
          }

        syslog(LOG_INFO, "[C-I2S-DIAG] ioctl STOP rc=0\n");
        return OK;

      case AUDIOIOC_GETBUFFERINFO:
        {
          FAR struct ap_buffer_info_s *info =
            (FAR struct ap_buffer_info_s *)arg;

          if (info == NULL)
            {
              syslog(LOG_INFO,
                     "[C-I2S-DIAG] GETBUFFERINFO rejected null arg\n");
              return -EINVAL;
            }

          info->nbuffers = CONTEST_I2S_TX_RING_SLOTS;
          info->buffer_size = CONTEST_I2S_TX_SLOT_BYTES;
          if (priv->logical_width == 16)
            {
              info->buffer_size /= 2;
            }

          syslog(LOG_INFO,
                 "[C-I2S-DIAG] GETBUFFERINFO buffer_size=%u nbuffers=%u "
                 "logical_width=%u wire_bytes=%u\n",
                 info->buffer_size, info->nbuffers, priv->logical_width,
                 CONTEST_I2S_TX_SLOT_BYTES);
          return OK;
        }

      default:
        syslog(LOG_INFO, "[C-I2S-DIAG] unsupported ioctl cmd=0x%x\n", cmd);
        return -ENOTTY;
    }
}

static FAR struct i2s_dev_s *contest_i2s1_initialize(void)
{
  FAR struct contest_i2s_tx_s *priv = &g_contest_i2s1;
  int periph;
  int cpuint;
  int ret;

  if (priv->initialized)
    {
      return &priv->dev;
    }

  priv->rate = CONTEST_I2S_TX_RATE;
  /* The contest speaker is normally fed by the Audio PCM16 upper-half. */
  priv->logical_width = 16;
  priv->data_width = CONTEST_I2S_TX_WIDTH;
  priv->channels = 1;
  contest_i2s_tx_configure(priv);

  priv->dma_channel = esp32s3_dma_request(ESP32S3_DMA_PERIPH_I2S1, 1, 1,
                                           true);
  if (priv->dma_channel < 0)
    {
      return NULL;
    }

  priv->cpu = this_cpu();
  periph = ESP32S3_PERIPH_DMA_OUT_CH0 + priv->dma_channel;
  cpuint = esp32s3_setup_irq(priv->cpu, periph, 1, ESP32S3_CPUINT_LEVEL);
  if (cpuint < 0)
    {
      esp32s3_dma_release(priv->dma_channel);
      priv->dma_channel = -1;
      return NULL;
    }

  priv->tx_irq = ESP32S3_PERIPH2IRQ(periph);
  ret = irq_attach(priv->tx_irq, contest_i2s_tx_interrupt, priv);
  if (ret < 0)
    {
      esp32s3_teardown_irq(priv->cpu, periph, cpuint);
      esp32s3_dma_release(priv->dma_channel);
      priv->dma_channel = -1;
      return NULL;
    }

  up_enable_irq(priv->tx_irq);
  priv->initialized = true;
  syslog(LOG_INFO, "[C-I2S] init port=1 tx\n");
  syslog(LOG_INFO, "[C-I2S] tx dma_channel=%d irq=%d\n", priv->dma_channel,
         priv->tx_irq);
  return &priv->dev;
}

static FAR struct i2s_dev_s *contest_i2s0_initialize(void)
{
  FAR struct contest_i2s_s *priv = &g_contest_i2s0;
  int periph;
  int cpuint;
  int ret;

  if (priv->initialized)
    {
      return &priv->dev;
    }

  priv->rate = CONTEST_I2S_RATE;
  priv->data_width = CONTEST_I2S_WIDTH;
  priv->channels = 1;
  contest_i2s_configure(priv);

  priv->dma_channel = esp32s3_dma_request(ESP32S3_DMA_PERIPH_I2S0, 1, 1,
                                           false);
  if (priv->dma_channel < 0)
    {
      return NULL;
    }

  priv->cpu = this_cpu();
  periph = ESP32S3_PERIPH_DMA_IN_CH0 + priv->dma_channel;
  cpuint = esp32s3_setup_irq(priv->cpu, periph, 1, ESP32S3_CPUINT_LEVEL);
  if (cpuint < 0)
    {
      esp32s3_dma_release(priv->dma_channel);
      priv->dma_channel = -1;
      return NULL;
    }

  priv->rx_irq = ESP32S3_PERIPH2IRQ(periph);
  ret = irq_attach(priv->rx_irq, contest_i2s_rx_interrupt, priv);
  if (ret < 0)
    {
      esp32s3_teardown_irq(priv->cpu, periph, cpuint);
      esp32s3_dma_release(priv->dma_channel);
      priv->dma_channel = -1;
      return NULL;
    }

  up_enable_irq(priv->rx_irq);
  priv->initialized = true;
  syslog(LOG_INFO, "[C-I2S] init port=0\n");
  syslog(LOG_INFO, "[C-I2S] dma_channel=%d irq=%d\n", priv->dma_channel,
         priv->rx_irq);
  return &priv->dev;
}

FAR struct i2s_dev_s *contest_i2s_initialize(int port)
{
  if (port == CONTEST_I2S_PORT)
    {
      return contest_i2s0_initialize();
    }

  if (port == CONTEST_I2S_TX_PORT)
    {
      return contest_i2s1_initialize();
    }

  return NULL;
}

void contest_i2s_set_rx_diagnostics(bool enabled)
{
  irqstate_t flags;

  flags = spin_lock_irqsave(&g_contest_i2s0.lock);
  g_contest_i2s0.diagnostics_enabled = enabled;
  spin_unlock_irqrestore(&g_contest_i2s0.lock, flags);
}

#endif /* CONFIG_CONTEST_BOARD_I2S0_RX || CONFIG_CONTEST_BOARD_I2S1_TX */
