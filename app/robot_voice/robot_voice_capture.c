#include <nuttx/config.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include <arch/board/board.h>

#include "robot_voice_capture.h"
#include "robot_voice_config.h"

/*
 * Keep the exact transaction shape that was stable in voice_echo:
 *   - 1024 logical PCM bytes per RX APB
 *   - two in-flight RX slots
 *   - two warm-up chunks
 *   - heap-owned callback context
 *
 * The important part is callback lifetime.  I2S completion is asynchronous,
 * so a callback context must never live on the caller's stack.  When capture
 * is stopped while an RX is in flight, the transfer is marked abandoned and
 * the eventual callback releases the application APB reference and its own
 * heap context.  This prevents late HPWORK callbacks from touching a returned
 * stack frame or a destroyed semaphore/mutex.
 */

#define ROBOT_VOICE_RX_DMA_BYTES        1024
#define ROBOT_VOICE_RX_SLOT_COUNT       2
#define ROBOT_VOICE_RX_WARMUP_CHUNKS    2
#define ROBOT_VOICE_RX_POLL_MS          50
#define ROBOT_VOICE_RX_TIMEOUT_MS       1000
#define ROBOT_VOICE_RECORD_GUARD_MS     2000

struct robot_voice_transfer_s
{
  sem_t done;
  mutex_t lock;
  int result;
  FAR struct ap_buffer_s *apb;
  bool retain_apb;
  bool callback_done;
  bool abandoned;
  unsigned int chunk;
};

struct robot_voice_rx_slot_s
{
  FAR struct ap_buffer_s *apb;
  FAR struct robot_voice_transfer_s *transfer;
  size_t request_bytes;
  unsigned int chunk;
  bool in_flight;
};

static bool robot_voice_capture_stop_requested(
  FAR volatile bool *stop_requested)
{
  return stop_requested != NULL && *stop_requested;
}

static void robot_voice_transfer_init(
  FAR struct robot_voice_transfer_s *transfer, bool retain_apb,
  unsigned int chunk)
{
  nxsem_init(&transfer->done, 0, 0);
  nxmutex_init(&transfer->lock);
  transfer->result = -EINPROGRESS;
  transfer->apb = NULL;
  transfer->retain_apb = retain_apb;
  transfer->callback_done = false;
  transfer->abandoned = false;
  transfer->chunk = chunk;
}

static void robot_voice_transfer_destroy(
  FAR struct robot_voice_transfer_s *transfer)
{
  nxmutex_lock(&transfer->lock);
  nxmutex_unlock(&transfer->lock);
  nxmutex_destroy(&transfer->lock);
  nxsem_destroy(&transfer->done);
}

static void robot_voice_rx_callback(FAR struct i2s_dev_s *dev,
                                    FAR struct ap_buffer_s *apb,
                                    FAR void *arg, int result)
{
  FAR struct robot_voice_transfer_s *transfer = arg;
  size_t nbytes = apb == NULL ? 0 : apb->nbytes;
  bool abandoned;

  (void)dev;

  if (transfer == NULL)
    {
      return;
    }

  if (transfer->chunk < 2 || result < 0)
    {
      printf("[RV-CAP] callback chunk=%u rc=%d bytes=%zu\n",
             transfer->chunk, result, nbytes);
    }

  nxmutex_lock(&transfer->lock);
  abandoned = transfer->abandoned;

  if (abandoned)
    {
      /* The lower half still owns its own APB reference until this callback
       * returns.  Release only the application's original reference here. */

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
      /* Keep a callback-owned reference until the waiting thread has copied
       * the completed PCM and releases the slot. */

      apb_reference(apb);
      transfer->apb = apb;
    }

  transfer->result = result;
  transfer->callback_done = true;
  nxsem_post(&transfer->done);
  nxmutex_unlock(&transfer->lock);
}

static bool robot_voice_abandon_transfer(
  FAR struct robot_voice_transfer_s *transfer)
{
  bool abandoned = false;

  if (transfer == NULL)
    {
      return false;
    }

  nxmutex_lock(&transfer->lock);
  if (!transfer->callback_done)
    {
      transfer->abandoned = true;
      abandoned = true;
    }
  nxmutex_unlock(&transfer->lock);
  return abandoned;
}

static int robot_voice_buffer_alloc(FAR struct ap_buffer_s **apb,
                                    size_t nbytes)
{
  struct audio_buf_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.numbytes = nbytes;
  desc.u.pbuffer = apb;
  return apb_alloc(&desc) < 0 ? -ENOMEM : OK;
}

static void robot_voice_rx_slot_release(
  FAR struct robot_voice_rx_slot_s *slot)
{
  if (slot == NULL)
    {
      return;
    }

  if (slot->transfer != NULL)
    {
      if (slot->transfer->apb != NULL)
        {
          apb_free(slot->transfer->apb);
          slot->transfer->apb = NULL;
        }

      robot_voice_transfer_destroy(slot->transfer);
      free(slot->transfer);
      slot->transfer = NULL;
    }

  if (slot->apb != NULL)
    {
      apb_free(slot->apb);
      slot->apb = NULL;
    }

  memset(slot, 0, sizeof(*slot));
}

static int robot_voice_rx_slot_submit(FAR struct i2s_dev_s *mic,
                                      FAR struct robot_voice_rx_slot_s *slot,
                                      size_t request_bytes,
                                      unsigned int chunk)
{
  int ret;

  if (mic == NULL || slot == NULL || request_bytes == 0 ||
      request_bytes > ROBOT_VOICE_RX_DMA_BYTES)
    {
      return -EINVAL;
    }

  ret = robot_voice_buffer_alloc(&slot->apb, request_bytes);
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
  robot_voice_transfer_init(slot->transfer, true, chunk);

  if (chunk < 2)
    {
      printf("[RV-CAP] receive submit chunk=%u logical=%zu\n",
             chunk, request_bytes);
    }

  ret = I2S_RECEIVE(mic, slot->apb, robot_voice_rx_callback,
                    slot->transfer,
                    MSEC2TICK(ROBOT_VOICE_RX_TIMEOUT_MS));
  if (chunk < 2 || ret < 0)
    {
      printf("[RV-CAP] I2S_RECEIVE chunk=%u rc=%d\n", chunk, ret);
    }

  if (ret < 0)
    {
      robot_voice_rx_slot_release(slot);
      return ret;
    }

  slot->in_flight = true;
  return OK;
}

static int robot_voice_rx_slot_wait_interruptible(
  FAR struct robot_voice_rx_slot_s *slot, clock_t deadline,
  FAR volatile bool *stop_requested)
{
  FAR struct robot_voice_transfer_s *transfer;
  int ret;

  if (slot == NULL || slot->transfer == NULL)
    {
      return -EINVAL;
    }

  transfer = slot->transfer;

  for (;;)
    {
      ret = nxsem_tickwait(&transfer->done,
                           MSEC2TICK(ROBOT_VOICE_RX_POLL_MS));
      if (ret >= 0)
        {
          nxmutex_lock(&transfer->lock);
          ret = transfer->result;
          nxmutex_unlock(&transfer->lock);
          return ret;
        }

      if (ret == -EINTR)
        {
          continue;
        }

      if (ret != -ETIMEDOUT)
        {
          return ret;
        }

      if (robot_voice_capture_stop_requested(stop_requested))
        {
          return -ECANCELED;
        }

      if ((int32_t)(deadline - clock_systime_ticks()) <= 0)
        {
          return -ETIMEDOUT;
        }
    }
}

static int robot_voice_rx_abort(FAR struct i2s_dev_s *mic,
                                FAR struct robot_voice_rx_slot_s *slots)
{
  unsigned int i;
  int stop_ret;

  /* Mark every live callback context abandoned BEFORE stopping hardware.
   * AUDIOIOC_STOP can schedule completion immediately on HPWORK. */

  for (i = 0; i < ROBOT_VOICE_RX_SLOT_COUNT; i++)
    {
      FAR struct robot_voice_rx_slot_s *slot = &slots[i];

      if (!slot->in_flight)
        {
          robot_voice_rx_slot_release(slot);
          continue;
        }

      if (robot_voice_abandon_transfer(slot->transfer))
        {
          /* Callback owns cleanup from this point.  Do not touch either
           * pointer after AUDIOIOC_STOP. */

          slot->transfer = NULL;
          slot->apb = NULL;
          slot->in_flight = false;
        }
      else
        {
          robot_voice_rx_slot_release(slot);
        }
    }

  stop_ret = I2S_IOCTL(mic, AUDIOIOC_STOP, 0);
  printf("[RV-CAP] abort stop rc=%d\n", stop_ret);
  return stop_ret;
}

static int robot_voice_rx_copy(FAR const struct robot_voice_rx_slot_s *slot,
                               uint8_t *dst, size_t cap, size_t *used)
{
  FAR const uint8_t *src_bytes;
  size_t nbytes;
  size_t samples;
  size_t i;

  if (slot == NULL || slot->apb == NULL || dst == NULL || used == NULL)
    {
      return -EINVAL;
    }

  nbytes = slot->apb->nbytes;
  if (nbytes != slot->request_bytes || nbytes > cap - *used ||
      (nbytes % sizeof(int16_t)) != 0)
    {
      return -EIO;
    }

  src_bytes = slot->apb->samp;
  memcpy(dst + *used, src_bytes, nbytes);

  /* Apply the same 2x capture gain that produced the known-good ASR input,
   * but use memcpy per sample so alignment of the destination never matters. */

  samples = nbytes / sizeof(int16_t);
  for (i = 0; i < samples; i++)
    {
      int16_t sample;
      int32_t gained;

      memcpy(&sample, dst + *used + i * sizeof(int16_t), sizeof(sample));
      gained = (int32_t)sample * 2;
      if (gained > INT16_MAX)
        {
          gained = INT16_MAX;
        }
      else if (gained < INT16_MIN)
        {
          gained = INT16_MIN;
        }

      sample = (int16_t)gained;
      memcpy(dst + *used + i * sizeof(int16_t), &sample, sizeof(sample));
    }

  *used += nbytes;
  return OK;
}

int robot_voice_capture_record_interruptible(
  uint8_t *buf, size_t cap, size_t *out_len, unsigned int duration_ms,
  FAR volatile bool *stop_requested)
{
  FAR struct i2s_dev_s *mic;
  struct robot_voice_rx_slot_s slots[ROBOT_VOICE_RX_SLOT_COUNT];
  size_t target;
  size_t capture_target;
  size_t submitted_bytes = 0;
  size_t used = 0;
  unsigned int submit_chunk = 0;
  unsigned int wait_slot = 0;
  unsigned int submitted = 0;
  unsigned int completed = 0;
  unsigned int in_flight = 0;
  unsigned int warmup_chunks = 0;
  clock_t deadline;
  int ret;

  if (buf == NULL || out_len == NULL || cap < sizeof(int16_t) ||
      duration_ms == 0)
    {
      return -EINVAL;
    }

  memset(slots, 0, sizeof(slots));
  *out_len = 0;

  target = ((size_t)ROBOT_VOICE_CAPTURE_RATE * sizeof(int16_t) *
            duration_ms) / 1000;
  target = target > cap ? cap : target;
  target -= target % sizeof(int16_t);
  if (target == 0)
    {
      return -EINVAL;
    }

  mic = board_voice_mic_i2s();
  if (mic == NULL)
    {
      return -ENODEV;
    }

  ret = I2S_RXCHANNELS(mic, ROBOT_VOICE_CHANNELS);
  printf("[RV-CAP] set channels %d rc=%d\n", ROBOT_VOICE_CHANNELS, ret);
  if (ret < 0)
    {
      return ret;
    }

  ret = (int)I2S_RXSAMPLERATE(mic, ROBOT_VOICE_CAPTURE_RATE);
  printf("[RV-CAP] set rate %d rc=%d\n", ROBOT_VOICE_CAPTURE_RATE, ret);
  if (ret != ROBOT_VOICE_CAPTURE_RATE)
    {
      return ret < 0 ? ret : -ENOTSUP;
    }

  ret = (int)I2S_RXDATAWIDTH(mic, ROBOT_VOICE_BITS);
  printf("[RV-CAP] set width %d rc=%d\n", ROBOT_VOICE_BITS, ret);
  if (ret != ROBOT_VOICE_BITS)
    {
      return ret < 0 ? ret : -ENOTSUP;
    }

  board_voice_mic_set_diagnostics(false);
  ret = I2S_IOCTL(mic, AUDIOIOC_START, 0);
  printf("[RV-CAP] start rc=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  capture_target = target + ROBOT_VOICE_RX_WARMUP_CHUNKS *
                             ROBOT_VOICE_RX_DMA_BYTES;
  /* Allow the requested capture window to grow without being cut off by a
   * stale fixed seven-second deadline.  The guard covers the RX queue,
   * callback and final APB completion overhead. */
  deadline = clock_systime_ticks() +
             MSEC2TICK(duration_ms + ROBOT_VOICE_RECORD_GUARD_MS);

  printf("[RV-CAP] recording %u ms target=%zu chunk=%d slots=%d warmup=%d\n",
         duration_ms, target, ROBOT_VOICE_RX_DMA_BYTES,
         ROBOT_VOICE_RX_SLOT_COUNT, ROBOT_VOICE_RX_WARMUP_CHUNKS);

  while (submitted_bytes < capture_target &&
         in_flight < ROBOT_VOICE_RX_SLOT_COUNT)
    {
      size_t request_bytes = capture_target - submitted_bytes;

      if (robot_voice_capture_stop_requested(stop_requested))
        {
          ret = -ECANCELED;
          goto errout;
        }

      if (request_bytes > ROBOT_VOICE_RX_DMA_BYTES)
        {
          request_bytes = ROBOT_VOICE_RX_DMA_BYTES;
        }

      ret = robot_voice_rx_slot_submit(mic, &slots[in_flight], request_bytes,
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
      FAR struct robot_voice_rx_slot_s *slot = &slots[wait_slot];

      if (robot_voice_capture_stop_requested(stop_requested))
        {
          ret = -ECANCELED;
          goto errout;
        }

      ret = robot_voice_rx_slot_wait_interruptible(slot, deadline,
                                                   stop_requested);
      if (ret < 0)
        {
          printf("[RV-CAP] RX wait chunk=%u failed rc=%d\n",
                 slot->chunk, ret);
          goto errout;
        }

      if (warmup_chunks < ROBOT_VOICE_RX_WARMUP_CHUNKS)
        {
          warmup_chunks++;
          if (warmup_chunks == ROBOT_VOICE_RX_WARMUP_CHUNKS)
            {
              printf("[RV-CAP] warmup discarded=%u chunks\n", warmup_chunks);
            }
        }
      else
        {
          ret = robot_voice_rx_copy(slot, buf, cap, &used);
          if (ret < 0)
            {
              goto errout;
            }
        }

      robot_voice_rx_slot_release(slot);
      completed++;
      in_flight--;

      if (submitted_bytes < capture_target)
        {
          size_t request_bytes = capture_target - submitted_bytes;

          if (request_bytes > ROBOT_VOICE_RX_DMA_BYTES)
            {
              request_bytes = ROBOT_VOICE_RX_DMA_BYTES;
            }

          ret = robot_voice_rx_slot_submit(mic, slot, request_bytes,
                                           submit_chunk++);
          if (ret < 0)
            {
              goto errout;
            }

          submitted_bytes += request_bytes;
          submitted++;
          in_flight++;
        }

      wait_slot = (wait_slot + 1) % ROBOT_VOICE_RX_SLOT_COUNT;
    }

  if (used != target || submitted_bytes != capture_target)
    {
      ret = -EIO;
      goto errout;
    }

  ret = I2S_IOCTL(mic, AUDIOIOC_STOP, 0);
  printf("[RV-CAP] stop complete rc=%d submitted=%u completed=%u\n",
         ret, submitted, completed);
  if (ret < 0)
    {
      return ret;
    }

  *out_len = used;
  printf("[RV-CAP] captured %zu bytes\n", used);
  printf("[RV-CAP] capture complete\n");
  return OK;

errout:
  (void)robot_voice_rx_abort(mic, slots);
  printf("[RV-CAP] failed stage=rx rc=%d submitted=%u completed=%u\n",
         ret, submitted, completed);
  return ret;
}

int robot_voice_capture_record(uint8_t *buf, size_t cap, size_t *out_len,
                               unsigned int duration_ms)
{
  return robot_voice_capture_record_interruptible(buf, cap, out_len,
                                                  duration_ms, NULL);
}
