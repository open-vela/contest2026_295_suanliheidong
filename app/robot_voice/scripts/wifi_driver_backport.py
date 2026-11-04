#!/usr/bin/env python3
"""Compile a checked ESP32-S3 Wi-Fi backport without rewriting the SDK tree.

The input must match the OpenVela adapter API. Changed driver functions are
kept in a content-addressed source copy; the compiler retains the original
quoted-header directory and output/dependency options.
"""
from pathlib import Path
import hashlib
import os
import re
import tempfile

MARKER = 'RV295_WIFI_DRIVER_V3'
LAYOUT_MARKER = 'RV295_WIFI_NATIVE_LAYOUT_V3'

MEMORY_DIAG = r'''static void rv_wifi_memory_diag(const char *stage)
{
  struct mallinfo info = kmm_mallinfo();
#ifdef CONFIG_MM_KERNEL_HEAP
  const char *pool = "kernel-dram";
#elif defined(CONFIG_XTENSA_IMEM_USE_SEPARATE_HEAP)
  const char *pool = "legacy-xtensa-separate";
#else
  const char *pool = "mixed-legacy";
#endif
  /* With the native layout kmm is internal DRAM. In legacy configurations
   * these numbers describe kmm, not a separate Xtensa heap. Never call this
   * logger from an allocator callback. */
  syslog(LOG_INFO, "[RV-WIFI-MEM] stage=%s pool=%s kmm_free=%zu kmm_largest=%zu\n",
         stage, pool, (size_t)info.fordblks, (size_t)info.mxordblk);
}'''


def function_span(source, name):
    pattern = r'(?m)^[A-Za-z_][\w\s*]*\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{'
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1:
        raise ValueError('Wi-Fi driver: expected one definition of ' + name)
    m = matches[0]
    pos = m.end() - 1
    depth = 0
    state = 'code'
    while pos < len(source):
        c = source[pos]
        pair = source[pos:pos + 2]
        if state == 'code':
            if pair in ('/*', '//'):
                state = 'block' if pair == '/*' else 'line'; pos += 2; continue
            if c in ('"', "'"):
                state = c
            elif c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return m.start(), pos + 1
        elif state == 'block' and pair == '*/':
            state = 'code'; pos += 2; continue
        elif state == 'line' and c == '\n':
            state = 'code'
        elif state in ('"', "'"):
            if c == '\\':
                pos += 2; continue
            if c == state:
                state = 'code'
        pos += 1
    raise ValueError('Wi-Fi driver: unterminated function ' + name)


def replace_once(text, before, after, label):
    if text.count(before) != 1:
        raise ValueError('Wi-Fi driver differs at ' + label + '; refusing an unchecked backport')
    return text.replace(before, after, 1)


def edit_function(source, name, edit):
    start, end = function_span(source, name)
    return source[:start] + edit(source[start:end]) + source[end:]


CONNECT = r'''int esp_wifi_sta_connect(void)
{
  int ret;
  uint32_t ticks;
  unsigned int nonzero = 0;
  unsigned int nonbroadcast = 0;
  unsigned int alloc_start = g_rv_wifi_internal_failures;
  unsigned int null_start = g_rv_wifi_malloc_null;
  unsigned int reject_start = g_rv_wifi_psram_rejected;

  esp_wifi_lock(true);
  if (g_sta_connected)
    {
      esp_wifi_lock(false);
      return OK;
    }
  if (!g_sta_started)
    {
      esp_wifi_lock(false);
      return -ENETDOWN;
    }

  /* A wildcard is not a concrete AP selection. Preserve valid explicit
   * BSSID pins while discarding an impossible zero/broadcast filter. */
  for (unsigned int i = 0; i < 6; i++)
    {
      nonzero |= g_sta_wifi_cfg.sta.bssid[i];
      nonbroadcast |= g_sta_wifi_cfg.sta.bssid[i] ^ 0xff;
    }
  if (g_sta_wifi_cfg.sta.bssid_set &&
      (!nonzero || !nonbroadcast || (g_sta_wifi_cfg.sta.bssid[0] & 1)))
    {
      g_sta_wifi_cfg.sta.bssid_set = false;
      memset(g_sta_wifi_cfg.sta.bssid, 0, sizeof(g_sta_wifi_cfg.sta.bssid));
      syslog(LOG_WARNING, "[RV-WIFI-DRV] cleared invalid BSSID filter\n");
    }

  g_rv_wifi_last_reason = 0;
  g_sta_reconnect = true;
  rv_wifi_memory_diag("join");
  syslog(LOG_INFO, "[RV-WIFI-DRV] join ssid_len=%u psk_len=%u auth=%u channel=%u pinned=%u\n",
         (unsigned int)strnlen((const char *)g_sta_wifi_cfg.sta.ssid, SSID_MAX_LEN),
         (unsigned int)strnlen((const char *)g_sta_wifi_cfg.sta.password, PWD_MAX_LEN),
         (unsigned int)g_sta_wifi_cfg.sta.threshold.authmode,
         (unsigned int)g_sta_wifi_cfg.sta.channel,
         (unsigned int)g_sta_wifi_cfg.sta.bssid_set);

  ret = esp_wifi_set_config(WIFI_IF_STA, &g_sta_wifi_cfg);
  if (ret)
    {
      syslog(LOG_ERR, "[RV-WIFI-DRV] set_config raw=0x%08x\n", (unsigned int)ret);
      ret = wifi_errno_trans(ret);
      goto errout;
    }
  ret = esp_wifi_connect();
  if (ret)
    {
      syslog(LOG_ERR, "[RV-WIFI-DRV] connect raw=0x%08x internal_alloc_failures_delta=%u last_bytes=%zu malloc_null_delta=%u psram_reject_delta=%u\n",
             (unsigned int)ret, g_rv_wifi_internal_failures - alloc_start,
             g_rv_wifi_internal_last_failure, g_rv_wifi_malloc_null - null_start,
             g_rv_wifi_psram_rejected - reject_start);
      rv_wifi_memory_diag("connect-failed");
      ret = wifi_errno_trans(ret);
      goto errout;
    }
  esp_wifi_lock(false);

  ticks = SEC2TICK(WIFI_CONNECT_TIMEOUT);
  /* Keep the adapter's original lock-free event wait: the event worker
   * may own the Wi-Fi mutex while publishing link status to the net stack. */
  do
    {
      if (g_sta_connected)
        {
          rv_wifi_memory_diag("connected");
          return OK;
        }
      if (!g_sta_started)
        {
          g_sta_reconnect = false;
          return -ENETDOWN;
        }
      esp_task_delay(1);
    }
  while (ticks--);

  /* A CONNECTED event may arrive at the deadline. Do not tear that link down. */
  if (g_sta_connected)
    {
      rv_wifi_memory_diag("connected");
      return OK;
    }
  g_sta_reconnect = false;
  (void)esp_wifi_disconnect();
  syslog(LOG_ERR, "[RV-WIFI-DRV] association_timeout reason=%u internal_alloc_failures_delta=%u last_bytes=%zu malloc_null_delta=%u psram_reject_delta=%u\n",
         g_rv_wifi_last_reason, g_rv_wifi_internal_failures - alloc_start,
         g_rv_wifi_internal_last_failure, g_rv_wifi_malloc_null - null_start,
         g_rv_wifi_psram_rejected - reject_start);
  rv_wifi_memory_diag("timeout");
  return -ETIMEDOUT;
errout:
  g_sta_reconnect = false;
  esp_wifi_lock(false);
  return ret;
}'''

ALLOCATOR = r'''static void *esp_malloc_internal(size_t size)
{
  void *ptr;
  bool rejected = false;
#ifdef CONFIG_MM_KERNEL_HEAP
  ptr = kmm_malloc(size);
#elif defined(CONFIG_XTENSA_IMEM_USE_SEPARATE_HEAP)
  ptr = xtensa_imm_malloc(size);
#else
  ptr = kmm_malloc(size);
#endif
  if (ptr != NULL && esp32s3_ptr_extram(ptr))
    {
      kmm_free(ptr);
      ptr = NULL;
      rejected = true;
      g_rv_wifi_psram_rejected++;
    }
  if (ptr == NULL && size != 0)
    {
      /* No logging/allocation here: record evidence for the join result.
       * Counters are diagnostic, never used to make connection decisions. */
      g_rv_wifi_internal_last_failure = size;
      g_rv_wifi_internal_failures++;
      if (!rejected) g_rv_wifi_malloc_null++;
    }
  return ptr;
}'''


def transform(source):
    if MARKER in source:
        return source
    if 'RV295_WIFI_DRIVER_V2' in source:
        raise ValueError('Wi-Fi SDK source contains an old in-place V2 patch; use the original SDK source')
    # Validate every required function before changing any source.
    for name in ('wifi_errno_trans', 'esp_event_post', 'esp_evt_work_cb',
                 'esp_wifi_sta_stop', 'esp_wifi_sta_connect',
                 'esp_wifi_sta_start', 'esp_wifi_sta_bssid', 'esp_malloc_internal'):
        function_span(source, name)
    source = replace_once(source, '#include <stdio.h>',
        '#include <stdio.h>\n#include <syslog.h>\n#include <malloc.h>\n'
        '/* ' + MARKER + ' */\n'
        'static volatile unsigned int g_rv_wifi_internal_failures;\n'
        'static volatile size_t g_rv_wifi_internal_last_failure;\n'
        'static volatile unsigned int g_rv_wifi_malloc_null;\n'
        'static volatile unsigned int g_rv_wifi_psram_rejected;', 'includes')
    source = replace_once(source, 'static wifi_config_t g_sta_wifi_cfg;',
        'static wifi_config_t g_sta_wifi_cfg;\n'
        'static unsigned int g_rv_wifi_last_reason;\n' + MEMORY_DIAG, 'STA state')
    source = replace_once(source, 'uint8_t buf[0];',
        'size_t data_size;\n  uint8_t buf[0];', 'event payload length')
    source = edit_function(source, 'esp_event_post', lambda t: replace_once(t,
        'evt_adpt->id = id;', 'evt_adpt->id = id;\n'
        '  evt_adpt->data_size = event_data_size;', 'event post'))
    def events(t):
        t = replace_once(t,
            '            ret = esp_wifi_get_config(WIFI_IF_STA, &g_sta_wifi_cfg);\n'
            '            if (ret)\n'
            '              {\n'
            '                wlerr("Failed to get Wi-Fi config data ret=%d\\n", ret);\n'
            '              }',
            '            /* Cached config is loaded synchronously by sta_start.\n'
            '             * A delayed START event must not erase a new PSK/SSID. */',
            'delayed START configuration overwrite')
        t = replace_once(t, 'g_sta_connected = false;\n            ret = esp32s3_wlan_sta_set_linkstatus(false);',
            'if (evt_adpt->data_size >= sizeof(wifi_event_sta_disconnected_t))\n'
            '              {\n'
            '                const wifi_event_sta_disconnected_t *info =\n'
            '                  (const void *)evt_adpt->buf;\n'
            '                if (g_rv_wifi_last_reason != info->reason)\n'
            '                  syslog(LOG_WARNING, "[RV-WIFI-DRV] disconnected reason=%u\\n",\n'
            '                         (unsigned int)info->reason);\n'
            '                g_rv_wifi_last_reason = info->reason;\n'
            '              }\n'
            '            g_sta_connected = false;\n'
            '            ret = esp32s3_wlan_sta_set_linkstatus(false);', 'disconnect reason')
        return replace_once(t, 'if (g_sta_reconnect)',
            'if (g_sta_reconnect && g_sta_started)', 'stop/reconnect race')
    source = edit_function(source, 'esp_evt_work_cb', events)
    source = edit_function(source, 'esp_wifi_sta_start', lambda t: replace_once(t,
        '  g_sta_started = true;',
        '  /* Snapshot before releasing the mutex; setters may run immediately\n'
        '   * after ifup while the START event is still queued. */\n'
        '  ret = esp_wifi_get_config(WIFI_IF_STA, &g_sta_wifi_cfg);\n'
        '  if (ret)\n'
        '    {\n'
        '      (void)esp_wifi_stop();\n'
        '      ret = wifi_errno_trans(ret);\n'
        '      goto errout;\n'
        '    }\n'
        '  g_sta_started = true;', 'synchronous station configuration'))
    source = edit_function(source, 'esp_wifi_sta_stop', lambda t: replace_once(t,
        '  ret = esp_wifi_stop();',
        '  /* Cancel reconnect before stop can queue DISCONNECTED events. */\n'
        '  g_sta_reconnect = false;\n  g_sta_started = false;\n'
        '  g_sta_connected = false;\n  ret = esp_wifi_stop();', 'STA stop'))
    source = edit_function(source, 'esp_wifi_sta_connect', lambda _: CONNECT)
    source = edit_function(source, 'esp_malloc_internal', lambda _: ALLOCATOR)
    source = edit_function(source, 'wifi_errno_trans', lambda t: replace_once(t,
        '      return ERROR;',
        '      syslog(LOG_ERR, "[RV-WIFI-DRV] unmapped_sdk_error raw=0x%08x\\n",\n'
        '             (unsigned int)ret);\n      return -EIO;', 'SDK error mapping'))
    def bssid(t):
        return replace_once(t,
            '      memcpy(pdata, wifi_cfg.sta.bssid, MAC_LEN);',
            '      if (g_sta_connected)\n'
            '        {\n'
            '          wifi_ap_record_t ap_info;\n'
            '          ret = esp_wifi_sta_get_ap_info(&ap_info);\n'
            '          if (ret) return wifi_errno_trans(ret);\n'
            '          memcpy(pdata, ap_info.bssid, MAC_LEN);\n'
            '        }\n'
            '      else\n'
            '        {\n'
            '          memcpy(pdata, wifi_cfg.sta.bssid, MAC_LEN);\n'
            '        }', 'live AP getter')
    return edit_function(source, 'esp_wifi_sta_bssid', bssid)


def transform_heap_layout(source):
    """Flat PSRAM uses no protected board memory-map constants.

    Some contest board includes lack board_memorymap.h. The native flat
    allocator does not use that header; retain it for protected builds.
    """
    if LAYOUT_MARKER in source:
        return source
    for name in ('up_allocate_heap', 'up_allocate_kheap'):
        function_span(source, name)
    source = replace_once(source,
        '#ifdef CONFIG_MM_KERNEL_HEAP\n#include <arch/board/board_memorymap.h>\n#endif',
        '/* ' + LAYOUT_MARKER + ' */\n'
        '#if defined(CONFIG_MM_KERNEL_HEAP) && !defined(CONFIG_BUILD_FLAT)\n'
        '#include <arch/board/board_memorymap.h>\n#endif', 'protected board memory-map include')
    return source


def stage_source(path, cache):
    path = Path(path).resolve()
    original = path.read_bytes()
    edit = transform_heap_layout if path.name == 'esp32s3_allocateheap.c' else transform
    patched = edit(original.decode('utf-8')).encode('utf-8')
    # Keep __FILE__ and diagnostic line locations grounded in the SDK source.
    original_name = str(path).replace('\\', '\\\\').replace('"', '\\"')
    patched = ('#line 1 "' + original_name + '"\n').encode() + patched
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
        raise ValueError('Wi-Fi driver cache content mismatch: ' + str(target))
    if path.read_bytes() != original:
        raise ValueError('Wi-Fi driver source changed while preparing the build')
    return target


def invalidate_objects(root):
    """A changed compiler wrapper is not a C prerequisite in older NuttX Make."""
    removed = []
    base = Path(root) / 'nuttx/arch/xtensa/src'
    if base.is_dir():
        for pattern in ('*esp32s3_wifi_adapter*.o', '*esp32s3_allocateheap*.o'):
            for path in base.rglob(pattern):
                if path.is_file() and not path.is_symlink():
                    path.unlink()
                    removed.append(path)
    return removed
