#include "robot_voice_config.h"
#include "robot_voice_diag.h"
#include <agent_config.h>
#include <infra/config_store.h>
#include <infra/network_manager.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* All Wi-Fi mutations remain owned by the Agent network manager. This file
 * only schedules serialized recovery attempts from the voice controller. */

static pthread_mutex_t g_wifi_recovery_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_wifi_transaction_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int g_wifi_connect_failures;
static unsigned long long g_wifi_next_poll_ms;
static unsigned int g_wifi_recovery_backoff_ms = ROBOT_VOICE_WIFI_RECOVER_MIN_MS;
static unsigned long long g_wifi_next_recovery_ms;

extern bool robot_voice_http_network_recovery_begin(void)
  __attribute__((weak));
extern void robot_voice_http_network_recovery_end(void)
  __attribute__((weak));

static int wifi_recovery_saved_credentials(char *ssid, size_t ssid_len,
                                           char *pass, size_t pass_len)
{
  if (claw_config_get(AGENT_CFG_KEY_WIFI_SSID, ssid, ssid_len) != 0 ||
      ssid[0] == '\0')
    {
      return -ENOENT;
    }

  /* config_store treats an empty/open-network password as absent. */
  if (claw_config_get(AGENT_CFG_KEY_WIFI_PASS, pass, pass_len) != 0)
    {
      pass[0] = '\0';
    }

  return 0;
}

/* A DHCP address alone can survive a hotspot/DNS failure.  Treat the
 * configured Agent host as the lightweight transport probe so a command does
 * not enter the LLM path with a stale link. */
static bool robot_voice_wifi_transport_ready(void)
{
  int ret;

  if (!network_is_connected())
    {
      return false;
    }

  ret = network_dns_probe(ROBOT_VOICE_HOST);
  if (ret == 0 || ret == -ENOSYS)
    {
      return true;
    }

  RV_EVENT("[网络确认] DNS probe failed host=%s rc=%d\n",
           ROBOT_VOICE_HOST, ret);
  return false;
}

static int robot_voice_wifi_configure_saved(void)
{
  char ssid[64] = {0};
  char pass[65] = {0};
  int ret;

  ret = wifi_recovery_saved_credentials(ssid, sizeof(ssid),
                                        pass, sizeof(pass));
  if (ret == 0)
    {
      pthread_mutex_lock(&g_wifi_transaction_lock);
      ret = network_wifi_configure(NULL, ssid, pass);
      pthread_mutex_unlock(&g_wifi_transaction_lock);
    }

  memset(pass, 0, sizeof(pass));
  return ret;
}

int __wrap_network_wifi_connect(const char *dev, const char *ssid, const char *pass)
{
  int ret = network_acquire_http_transport(30000);
  if (ret < 0) return ret;
  ret = network_wifi_configure(dev, ssid, pass);
  network_release_http_transport();
  return ret;
}

int __wrap_network_wifi_reconnect(void)
{
  int ret = network_acquire_http_transport(30000);
  if (ret < 0) return ret;
  ret = network_wifi_restore();
  network_release_http_transport();
  return ret;
}

static int robot_voice_wifi_recover_once(void)
{
  int ret;

  if (robot_voice_wifi_transport_ready())
    {
      return 0;
    }

  /* network_wifi_restore() intentionally preserves a stale IPv4 link.  Force
   * a full saved-credential transaction when DNS has already failed. */
  if (network_is_connected())
    {
      ret = robot_voice_wifi_configure_saved();
    }
  else
    {
      pthread_mutex_lock(&g_wifi_transaction_lock);
      ret = network_wifi_restore();
      pthread_mutex_unlock(&g_wifi_transaction_lock);
    }
  if (ret == 0 && network_wait_connected(5000) != OK)
    {
      ret = -ENETUNREACH;
    }
  if (ret == 0 && !robot_voice_wifi_transport_ready())
    {
      ret = -EHOSTUNREACH;
    }
  return ret;
}

int robot_voice_wifi_ensure_connected(uint32_t timeout_ms)
{
  unsigned long long deadline;
  unsigned long long now;
  unsigned int attempt = 0;
  int last = -ENETDOWN;
  bool recovery_started = false;

  if (timeout_ms == 0)
    {
      timeout_ms = ROBOT_VOICE_ACTION_NET_TIMEOUT_MS;
    }

  now = rv_diag_now_ms();
  deadline = now + timeout_ms;
  if (robot_voice_wifi_transport_ready())
    {
      return 0;
    }

  RV_EVENT("[网络确认] action gate waiting timeout=%ums\n", timeout_ms);
  while (rv_diag_now_ms() < deadline)
    {
      unsigned long long remaining;
      unsigned long long before = rv_diag_now_ms();
      uint32_t gate_wait = (uint32_t)(deadline > before ? deadline - before : 0);
      if (gate_wait > 5000) gate_wait = 5000;
      int gate_rc = network_acquire_http_transport(gate_wait);
      if (gate_rc < 0)
        {
          last = gate_rc;
          usleep(100 * 1000);
          continue;
        }

      if (robot_voice_http_network_recovery_begin != NULL &&
          robot_voice_http_network_recovery_end != NULL)
        {
          if (!robot_voice_http_network_recovery_begin())
            {
              network_release_http_transport();
              usleep(100 * 1000);
              continue;
            }
          recovery_started = true;
        }

      attempt++;
      RV_EVENT("[网络确认] reconnect attempt=%u\n", attempt);
      last = robot_voice_wifi_recover_once();
      if (recovery_started)
        {
          robot_voice_http_network_recovery_end();
          recovery_started = false;
        }
      network_release_http_transport();

      if (last == 0 && robot_voice_wifi_transport_ready())
        {
          RV_EVENT("[网络确认] connected attempt=%u\n", attempt);
          return 0;
        }

      now = rv_diag_now_ms();
      if (now >= deadline)
        {
          break;
        }

      remaining = deadline - now;
      if (remaining > ROBOT_VOICE_ACTION_NET_RETRY_MS)
        {
          remaining = ROBOT_VOICE_ACTION_NET_RETRY_MS;
        }
      usleep((useconds_t)remaining * 1000);
    }

  if (recovery_started)
    {
      robot_voice_http_network_recovery_end();
    }
  RV_EVENT("[网络确认] failed rc=%d attempts=%u\n", last, attempt);
  return last < 0 ? last : -ETIMEDOUT;
}

int robot_voice_wifi_reconnect_and_ensure(uint32_t timeout_ms)
{
  unsigned long long deadline;
  unsigned long long now;
  unsigned int attempt = 0;
  int last = -ENETDOWN;
  bool recovery_started = false;

  if (timeout_ms == 0)
    {
      timeout_ms = ROBOT_VOICE_ACTION_NET_TIMEOUT_MS;
    }

  now = rv_diag_now_ms();
  deadline = now + timeout_ms;
  RV_EVENT("[网络确认] forced reconnect timeout=%ums\n", timeout_ms);

  while (rv_diag_now_ms() < deadline)
    {
      unsigned long long remaining;
      unsigned long long before = rv_diag_now_ms();
      uint32_t gate_wait = (uint32_t)(deadline > before ? deadline - before : 0);
      if (gate_wait > 5000) gate_wait = 5000;
      int gate_rc = network_acquire_http_transport(gate_wait);
      if (gate_rc < 0)
        {
          last = gate_rc;
          usleep(100 * 1000);
          continue;
        }

      if (robot_voice_http_network_recovery_begin != NULL &&
          robot_voice_http_network_recovery_end != NULL)
        {
          if (!robot_voice_http_network_recovery_begin())
            {
              network_release_http_transport();
              usleep(100 * 1000);
              continue;
            }
          recovery_started = true;
        }

      attempt++;
      last = robot_voice_wifi_configure_saved();

      if (recovery_started)
        {
          robot_voice_http_network_recovery_end();
          recovery_started = false;
        }

      if (last == 0 && network_wait_connected(5000) == OK &&
          robot_voice_wifi_transport_ready())
        {
          network_release_http_transport();
          RV_EVENT("[网络确认] forced reconnect connected attempt=%u\n",
                   attempt);
          return 0;
        }

      network_release_http_transport();

      now = rv_diag_now_ms();
      if (now >= deadline)
        {
          break;
        }
      remaining = deadline - now;
      if (remaining > ROBOT_VOICE_ACTION_NET_RETRY_MS)
        {
          remaining = ROBOT_VOICE_ACTION_NET_RETRY_MS;
        }
      usleep((useconds_t)remaining * 1000);
    }

  if (recovery_started)
    {
      robot_voice_http_network_recovery_end();
    }
  RV_EVENT("[网络确认] forced reconnect failed rc=%d attempts=%u\n",
           last, attempt);
  return last < 0 ? last : -ETIMEDOUT;
}

/* Record transport evidence without treating HTTP/application errors as a
 * radio failure. A successful HTTP response proves the link is usable. */
void robot_voice_http_network_result(int result, bool connect_failure)
{
  pthread_mutex_lock(&g_wifi_recovery_lock);
  if (result >= 100)
    {
      g_wifi_connect_failures = 0;
      g_wifi_recovery_backoff_ms = ROBOT_VOICE_WIFI_RECOVER_MIN_MS;
      g_wifi_next_recovery_ms = 0;
    }
  else if (connect_failure && result != -ENOMEM && result != -ENOBUFS &&
           result != -EMFILE && result != -ENFILE && result != -EAGAIN &&
           g_wifi_connect_failures < 3)
    {
      g_wifi_connect_failures++;
    }
  pthread_mutex_unlock(&g_wifi_recovery_lock);
}

void robot_voice_wifi_poll(void)
{
  unsigned long long now = rv_diag_now_ms();
  unsigned int failures;
  bool connected;
  bool force;
  bool recovery_started = false;
  int ret;

  pthread_mutex_lock(&g_wifi_recovery_lock);
  if (now < g_wifi_next_poll_ms)
    {
      pthread_mutex_unlock(&g_wifi_recovery_lock);
      return;
    }

  g_wifi_next_poll_ms = now + ROBOT_VOICE_WIFI_CHECK_MS;
  if (now < g_wifi_next_recovery_ms)
    {
      pthread_mutex_unlock(&g_wifi_recovery_lock);
      return;
    }
  failures = g_wifi_connect_failures;
  force = failures >= ROBOT_VOICE_WIFI_CONNECT_FAILURES;
  pthread_mutex_unlock(&g_wifi_recovery_lock);

  connected = network_is_connected();
  if (connected && !force)
    {
      return;
    }

  /* Reserve the same gate used by Agent, ASR, Flash and TTS before touching
   * Wi-Fi. Checking only the contest pool misses vela_tls requests, so the
   * old recovery path could disconnect an unrelated active HTTPS request. */
  ret = network_acquire_http_transport(1);
  if (ret < 0)
    {
      return;
    }

  if (robot_voice_http_network_recovery_begin != NULL &&
      robot_voice_http_network_recovery_end != NULL)
    {
      if (!robot_voice_http_network_recovery_begin())
        {
          network_release_http_transport();
          return;
        }
      recovery_started = true;
    }

  /* Recheck after quiescing HTTP: a request may have restored connectivity. */
  connected = network_is_connected();
  pthread_mutex_lock(&g_wifi_recovery_lock);
  force = g_wifi_connect_failures >= ROBOT_VOICE_WIFI_CONNECT_FAILURES;
  pthread_mutex_unlock(&g_wifi_recovery_lock);
  if (connected && !force)
    {
      if (recovery_started)
        {
          robot_voice_http_network_recovery_end();
        }
      network_release_http_transport();
      return;
    }

  RV_EVENT("[网络恢复] attempt reason=%s\n",
           connected ? "persistent-tcp-failure" : "link-or-ip-down");
  if (!connected)
    {
      pthread_mutex_lock(&g_wifi_transaction_lock);
      ret = network_wifi_restore();
      pthread_mutex_unlock(&g_wifi_transaction_lock);
    }
  else
    {
      char ssid[64] = {0};
      char pass[65] = {0};

      ret = wifi_recovery_saved_credentials(ssid, sizeof(ssid),
                                            pass, sizeof(pass));
      if (ret == 0)
        {
          /* Force the serialized Agent transaction to issue ifdown/ifup. */
          pthread_mutex_lock(&g_wifi_transaction_lock);
          ret = network_wifi_configure(NULL, ssid, pass);
          pthread_mutex_unlock(&g_wifi_transaction_lock);
        }
      memset(pass, 0, sizeof(pass));
    }

  if (ret == 0)
    {
      pthread_mutex_lock(&g_wifi_recovery_lock);
      g_wifi_connect_failures = 0;
      g_wifi_recovery_backoff_ms = ROBOT_VOICE_WIFI_RECOVER_MIN_MS;
      g_wifi_next_recovery_ms = 0;
      pthread_mutex_unlock(&g_wifi_recovery_lock);
      RV_EVENT("[网络恢复] connected\n");
    }
  else
    {
      unsigned int next_backoff;
      pthread_mutex_lock(&g_wifi_recovery_lock);
      next_backoff = g_wifi_recovery_backoff_ms <
          ROBOT_VOICE_WIFI_RECOVER_MAX_MS / 2 ?
          g_wifi_recovery_backoff_ms * 2 : ROBOT_VOICE_WIFI_RECOVER_MAX_MS;
      if (next_backoff < ROBOT_VOICE_WIFI_RECOVER_MIN_MS)
        {
          next_backoff = ROBOT_VOICE_WIFI_RECOVER_MIN_MS;
        }
      if (next_backoff > ROBOT_VOICE_WIFI_RECOVER_MAX_MS)
        {
          next_backoff = ROBOT_VOICE_WIFI_RECOVER_MAX_MS;
        }
      g_wifi_recovery_backoff_ms = next_backoff;
      g_wifi_next_recovery_ms = rv_diag_now_ms() + next_backoff;
      pthread_mutex_unlock(&g_wifi_recovery_lock);
      RV_EVENT("[网络恢复] failed rc=%d next_attempt=%ums\n",
               ret, next_backoff);
    }

  if (recovery_started)
    {
      robot_voice_http_network_recovery_end();
    }
  network_release_http_transport();
}

void robot_voice_http_network_diag(void) { network_wifi_diag(); }
void robot_voice_wifi_link_anchor(void) {}
