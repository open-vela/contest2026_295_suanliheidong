#!/usr/bin/env python3
"""Stage the contest TX driver's bounded, negotiated internal DMA ring.

Keep the SDK/board source immutable. Adapt only the verified contest driver
contract; an unknown source shape fails explicitly before invoking the compiler.
"""
from pathlib import Path
import hashlib
import os
import tempfile
from wifi_driver_backport import function_span, edit_function, replace_once

MARKER = 'RV295_AUDIO_DMA_V1'
RUNTIME_FUNCTIONS = (
    'contest_i2s_tx_link_slot', 'contest_i2s_tx_link_immutable_ring',
    'contest_i2s_tx_start_ring', 'contest_i2s_tx_terminal_stop_worker',
    'contest_i2s_tx_interrupt', 'contest_i2s_send', 'contest_i2s_tx_ioctl',
)


def is_current_dma_source(source):
    """Recognize the newer contest DMA implementation already in the source.

    That implementation folds slot linking into bind/link_immutable_ring and
    therefore intentionally has no separate contest_i2s_tx_link_slot().
    """
    return (
        'static void contest_i2s_tx_link_slot' not in source and
        'size_t wire_bytes, bool terminal' in source and
        'FAR struct contest_i2s_tx_s *priv, bool finite' in source and
        'terminal = terminal_short || (apb->flags & AUDIO_APB_FINAL) != 0;' in source
    )

ACTIVE_SLOTS = r'''static unsigned int contest_i2s_tx_active_slots(
  FAR const struct contest_i2s_tx_s *priv)
{
  return priv->dma_slot_count ? priv->dma_slot_count :
         CONTEST_I2S_TX_RING_SLOTS;
}'''

ALLOCATOR = r'''static int contest_i2s_tx_alloc_dma_slots(
  FAR struct contest_i2s_tx_s *priv, unsigned int limit)
{
  static const unsigned int choices[] = {16, 8, 4};
  unsigned int attempt;
  unsigned int i;
  unsigned int selected = 0;
  struct mallinfo info;

  if (limit != 0 && limit != 4 && limit != 8 && limit != 16)
    {
      return -EINVAL;
    }
  if (limit == 0) limit = CONTEST_I2S_TX_RING_SLOTS;

  /* START is called only while stopped. Clear every possible old slot,
   * including partial allocations left by an interrupted earlier START. */
  for (i = 0; i < CONTEST_I2S_TX_RING_SLOTS; i++)
    {
      if (priv->xfer[i].dma_data) kmm_free(priv->xfer[i].dma_data);
      priv->xfer[i].dma_data = NULL;
      priv->xfer[i].dma_bound = false;
    }
  priv->dma_slot_count = 0;

  for (attempt = 0; attempt < sizeof(choices) / sizeof(choices[0]); attempt++)
    {
      selected = choices[attempt];
      if (selected > limit) continue;
      for (i = 0; i < selected; i++)
        {
          priv->xfer[i].dma_data = kmm_memalign(CONTEST_I2S_TX_DMA_ALIGN,
                                               CONTEST_I2S_TX_SLOT_BYTES);
          if (!priv->xfer[i].dma_data) break;
          memset(priv->xfer[i].dma_data, 0, CONTEST_I2S_TX_SLOT_BYTES);
          up_clean_dcache((uintptr_t)priv->xfer[i].dma_data,
                          (uintptr_t)priv->xfer[i].dma_data +
                          CONTEST_I2S_TX_SLOT_BYTES);
        }
      if (i == selected)
        {
          priv->dma_slot_count = selected;
          info = kmm_mallinfo();
          syslog(LOG_INFO, "[RV-DMA] selected slots=%u slot_bytes=%u kmm_free=%zu largest=%zu\n",
                 selected, CONTEST_I2S_TX_SLOT_BYTES,
                 (size_t)info.fordblks, (size_t)info.mxordblk);
          return OK;
        }

      info = kmm_mallinfo();
      syslog(LOG_WARNING, "[RV-DMA] allocation failed slots=%u allocated=%u kmm_free=%zu largest=%zu\n",
             selected, i, (size_t)info.fordblks, (size_t)info.mxordblk);
      /* Release ALL partial blocks before trying a smaller ring. */
      for (i = 0; i < CONTEST_I2S_TX_RING_SLOTS; i++)
        {
          if (priv->xfer[i].dma_data) kmm_free(priv->xfer[i].dma_data);
          priv->xfer[i].dma_data = NULL;
        }
    }
  return -ENOMEM;
}'''


def transform(source):
    if MARKER in source:
        return source
    if is_current_dma_source(source):
        return source
    for name in (*RUNTIME_FUNCTIONS, 'contest_i2s_tx_alloc_dma_slots',
                 'contest_i2s_tx_free_dma_slots'):
        function_span(source, name)
    a, b = function_span(source, 'contest_i2s_tx_free_dma_slots')
    if 'kmm_free(priv->xfer[i].dma_data);' not in source[a:b]:
        raise ValueError('contest DMA free ownership differs; refusing allocation backport')
    source = replace_once(source,
        '#define CONTEST_I2S_TX_RING_SLOTS        16',
        '#define CONTEST_I2S_TX_RING_SLOTS        16\n/* ' + MARKER + ' */', 'maximum slots')
    if '#define CONTEST_I2S_TX_SLOT_BYTES        2048' not in source:
        raise ValueError('contest I2S slot width differs; refusing DMA adaptation')
    source = replace_once(source, '#include <nuttx/kmalloc.h>',
                          '#include <nuttx/kmalloc.h>\n#include <malloc.h>', 'heap diagnostics include')
    source = replace_once(source,
        'struct contest_i2s_tx_s\n{\n  struct i2s_dev_s dev;',
        'struct contest_i2s_tx_s\n{\n  struct i2s_dev_s dev;\n'
        '  uint8_t dma_slot_count;', 'TX ring state (device must stay first)')
    source = replace_once(source, 'struct contest_i2s_pcm_stats_s\n{',
                          ACTIVE_SLOTS + '\n\nstruct contest_i2s_pcm_stats_s\n{', 'active ring helper')
    for name in RUNTIME_FUNCTIONS:
        def replace_slots(text):
            if 'CONTEST_I2S_TX_RING_SLOTS' not in text:
                raise ValueError('contest I2S runtime ring anchor missing in ' + name)
            return text.replace('CONTEST_I2S_TX_RING_SLOTS', 'contest_i2s_tx_active_slots(priv)')
        source = edit_function(source, name, replace_slots)
    source = edit_function(source, 'contest_i2s_tx_alloc_dma_slots', lambda _: ALLOCATOR)
    source = edit_function(source, 'contest_i2s_tx_ioctl', lambda t: replace_once(t,
        'contest_i2s_tx_alloc_dma_slots(priv);',
        'contest_i2s_tx_alloc_dma_slots(priv, (unsigned int)arg);', 'START limit'))
    # Free the complete maximum-capacity array even after a failed partial
    # START. Runtime loop bounds elsewhere use the negotiated active count.
    return source


def stage_source(path, cache):
    path = Path(path).resolve()
    before = path.read_bytes()
    patched = transform(before.decode('utf-8')).encode('utf-8')
    name = str(path).replace('\\', '\\\\').replace('"', '\\"')
    patched = ('#line 1 "' + name + '"\n').encode() + patched
    digest = hashlib.sha256(patched).hexdigest()[:24]
    directory = Path(cache) / digest
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / path.name
    if not target.exists():
        fd, temp = tempfile.mkstemp(dir=directory)
        try:
            with os.fdopen(fd, 'wb') as stream:
                stream.write(patched)
            os.replace(temp, target)
        finally:
            Path(temp).unlink(missing_ok=True)
    elif target.read_bytes() != patched:
        raise ValueError('audio DMA source cache mismatch')
    if path.read_bytes() != before:
        raise ValueError('contest I2S source changed during staging')
    return target


def invalidate_objects(root):
    removed = []
    for base in (Path(root) / 'nuttx/boards',
                 Path(root) / 'vendor/openvela/boards/contest2026_295_board',
                 Path(root) / 'contest2026_295_suanliheidong/board/contest_board'):
        if base.is_dir():
            for obj in base.rglob('*contest_i2s*.o'):
                if obj.is_file() and not obj.is_symlink():
                    obj.unlink()
                    removed.append(obj)
    return removed
