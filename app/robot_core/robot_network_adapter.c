#include <nuttx/config.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "agent_config.h"
#include "infra/config_store.h"
#include "robot_network_adapter.h"

#define RV_NET_IFACE                    "wlan0"
#define RV_NET_GUARD_STACK              6144
#define RV_NET_INITIAL_DELAY_MS          3000
#define RV_NET_POLL_MS                   2000
#define RV_NET_REPAIR_GRACE_MS          10000
#define RV_NET_ASSOC_POLL_MS              500
#define RV_NET_ASSOC_TIMEOUT_MS          8000
#define RV_NET_RETRY_BACKOFF_MS          5000
#define RV_NET_DHCP_RETRIES                 3

struct rv_net_status_s
{
  char essid[65];
  char bssid[24];
  char country[16];
  int frequency;
  int bitrate;
};

static pthread_mutex_t g_rv_net_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_rv_net_started;

/****************************************************************************
 * Private helpers
 ****************************************************************************/

static bool rv_net_bssid_valid(const char *bssid)
{
  if (bssid == NULL || bssid[0] == '\0')
    {
      return false;
    }

  if (strcasecmp(bssid, "ff:ff:ff:ff:ff:ff") == 0 ||
      strcasecmp(bssid, "00:00:00:00:00:00") == 0)
    {
      return false;
    }

  return true;
}

static int rv_net_shell_quote(const char *src, char *buf, size_t buf_size)
{
  size_t out = 0;
  const char *p;

  if (src == NULL || buf == NULL || buf_size < 3)
    {
      return -EINVAL;
    }

  buf[out++] = '\'';

  for (p = src; *p != '\0'; p++)
    {
      if (*p == '\'')
        {
          if (out + 4 >= buf_size)
            {
              return -ENOSPC;
            }

          buf[out++] = '\'';
          buf[out++] = '\\';
          buf[out++] = '\'';
          buf[out++] = '\'';
        }
      else
        {
          if (out + 1 >= buf_size)
            {
              return -ENOSPC;
            }

          buf[out++] = *p;
        }
    }

  if (out + 2 > buf_size)
    {
      return -ENOSPC;
    }

  buf[out++] = '\'';
  buf[out] = '\0';
  return 0;
}

static void rv_net_copy_value(char *dst, size_t dst_size,
                              const char *src)
{
  const char *begin;
  const char *finish;
  size_t len;

  if (dst == NULL || dst_size == 0 || src == NULL)
    {
      return;
    }

  begin = src;
  while (*begin != '\0' && isspace((unsigned char)*begin))
    {
      begin++;
    }

  finish = begin + strlen(begin);
  while (finish > begin && isspace((unsigned char)finish[-1]))
    {
      finish--;
    }

  len = (size_t)(finish - begin);
  if (len >= dst_size)
    {
      len = dst_size - 1;
    }

  memcpy(dst, begin, len);
  dst[len] = '\0';
}

static int rv_net_read_show(struct rv_net_status_s *status)
{
  FILE *fp;
  char line[256];

  if (status == NULL)
    {
      return -EINVAL;
    }

  memset(status, 0, sizeof(*status));

  fp = popen("wapi show " RV_NET_IFACE, "r");
  if (fp == NULL)
    {
      return -errno;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      char *p = line;
      char *colon;

      while (*p != '\0' && isspace((unsigned char)*p))
        {
          p++;
        }

      colon = strchr(p, ':');
      if (colon == NULL)
        {
          continue;
        }

      *colon++ = '\0';

      if (strcmp(p, "ESSID") == 0)
        {
          rv_net_copy_value(status->essid, sizeof(status->essid), colon);
        }
      else if (strcmp(p, "AP") == 0)
        {
          rv_net_copy_value(status->bssid, sizeof(status->bssid), colon);
        }
      else if (strcmp(p, "Country") == 0)
        {
          rv_net_copy_value(status->country, sizeof(status->country), colon);
        }
      else if (strcmp(p, "Frequency") == 0 && status->frequency == 0)
        {
          status->frequency = atoi(colon);
        }
      else if (strcmp(p, "BitRate") == 0)
        {
          status->bitrate = atoi(colon);
        }
    }

  (void)pclose(fp);
  return 0;
}

static bool rv_net_has_ipv4(char *ip, size_t ip_size)
{
  struct ifaddrs *list = NULL;
  struct ifaddrs *ifa;
  bool found = false;

  if (ip != NULL && ip_size > 0)
    {
      ip[0] = '\0';
    }

  if (getifaddrs(&list) != 0)
    {
      return false;
    }

  for (ifa = list; ifa != NULL; ifa = ifa->ifa_next)
    {
      struct sockaddr_in *sin;
      uint32_t addr;

      if (ifa->ifa_addr == NULL || ifa->ifa_name == NULL ||
          strcmp(ifa->ifa_name, RV_NET_IFACE) != 0 ||
          ifa->ifa_addr->sa_family != AF_INET)
        {
          continue;
        }

      sin = (struct sockaddr_in *)ifa->ifa_addr;
      addr = ntohl(sin->sin_addr.s_addr);
      if (addr == 0 || (addr >> 24) == 127)
        {
          continue;
        }

      if (ip != NULL && ip_size > 0)
        {
          (void)inet_ntop(AF_INET, &sin->sin_addr, ip, ip_size);
        }

      found = true;
      break;
    }

  freeifaddrs(list);
  return found;
}

static bool rv_net_associated_with(const struct rv_net_status_s *status,
                                   const char *ssid)
{
  (void)ssid;

  /*
   * A valid unicast BSSID is the reliable association signal on this board.
   * The textual ESSID query may lag behind the actual link state, so keep it
   * for diagnostics but do not use it to tear a working connection down.
   */
  return status != NULL && rv_net_bssid_valid(status->bssid);
}

static int rv_net_wait_associated(const char *ssid,
                                  struct rv_net_status_s *last)
{
  unsigned int elapsed = 0;
  struct rv_net_status_s status;

  while (elapsed < RV_NET_ASSOC_TIMEOUT_MS)
    {
      if (rv_net_read_show(&status) == 0 &&
          rv_net_associated_with(&status, ssid))
        {
          if (last != NULL)
            {
              *last = status;
            }

          printf("[RV-NET] associated ssid=%s bssid=%s freq=%d bitrate=%d\n",
                 status.essid, status.bssid,
                 status.frequency, status.bitrate);
          return 0;
        }

      if (last != NULL)
        {
          *last = status;
        }

      usleep(RV_NET_ASSOC_POLL_MS * 1000);
      elapsed += RV_NET_ASSOC_POLL_MS;
    }

  return -ETIMEDOUT;
}

static int rv_net_run_connect(const char *ssid, const char *pass)
{
  struct rv_net_status_s status;
  char q_ssid[192];
  char q_pass[384];
  char cmd[640];
  char ip[INET_ADDRSTRLEN];
  int ret;
  int i;

  ret = rv_net_shell_quote(ssid, q_ssid, sizeof(q_ssid));
  if (ret < 0)
    {
      return ret;
    }

  if (pass != NULL && pass[0] != '\0')
    {
      ret = rv_net_shell_quote(pass, q_pass, sizeof(q_pass));
      if (ret < 0)
        {
          return ret;
        }
    }

  /*
   * Start from a known link state.  ifdown/ifup alone may leave the previous
   * IPv4 address visible, therefore explicitly clear stale IPv4 before DHCP.
   */
  (void)system("ifdown " RV_NET_IFACE);
  usleep(200 * 1000);
  (void)system("ifconfig " RV_NET_IFACE " 0.0.0.0");
  (void)system("ifup " RV_NET_IFACE);
  usleep(500 * 1000);

  (void)system("wapi mode " RV_NET_IFACE " 2");

  if (pass != NULL && pass[0] != '\0')
    {
      snprintf(cmd, sizeof(cmd), "wapi psk " RV_NET_IFACE " %s 3 2", q_pass);
      ret = system(cmd);
      if (ret != 0)
        {
          printf("[RV-NET] wapi psk failed rc=%d\n", ret);
          return -EIO;
        }
    }

  snprintf(cmd, sizeof(cmd), "wapi essid " RV_NET_IFACE " %s 1", q_ssid);
  ret = system(cmd);
  if (ret != 0)
    {
      printf("[RV-NET] wapi essid failed rc=%d\n", ret);
      return -EIO;
    }

  memset(&status, 0, sizeof(status));
  ret = rv_net_wait_associated(ssid, &status);
  if (ret < 0)
    {
      printf("[RV-NET] association timeout ssid=%s essid=%s bssid=%s "
             "freq=%d country=%s\n",
             ssid,
             status.essid[0] ? status.essid : "<none>",
             status.bssid[0] ? status.bssid : "<none>",
             status.frequency,
             status.country[0] ? status.country : "<unknown>");

      if (status.frequency >= 2467 &&
          strcmp(status.country, "01") == 0)
        {
          printf("[RV-NET] hint: AP is on upper 2.4GHz channel (freq=%d) "
                 "while country=01; verify regulatory domain or move AP "
                 "to channel 1-11\n",
                 status.frequency);
        }

      return ret;
    }

  /*
   * Association is real only now.  DHCP is deliberately after the BSSID
   * check so a leftover 10.x/192.168.x address cannot masquerade as Wi-Fi.
   */
  for (i = 0; i < RV_NET_DHCP_RETRIES; i++)
    {
      ret = system("renew " RV_NET_IFACE);
      if (ret == 0)
        {
          unsigned int waited = 0;

          while (waited < 5000)
            {
              if (rv_net_has_ipv4(ip, sizeof(ip)))
                {
                  printf("[RV-NET] DHCP ready ip=%s\n", ip);
                  return 0;
                }

              usleep(250 * 1000);
              waited += 250;
            }
        }

      printf("[RV-NET] DHCP attempt=%d failed rc=%d\n", i + 1, ret);
      usleep(500 * 1000);
    }

  return -ENETUNREACH;
}

static int rv_net_load_credentials(char *ssid, size_t ssid_size,
                                   char *pass, size_t pass_size)
{
  if (ssid == NULL || ssid_size == 0 || pass == NULL || pass_size == 0)
    {
      return -EINVAL;
    }

  ssid[0] = '\0';
  pass[0] = '\0';

  if (claw_config_get(AGENT_CFG_KEY_WIFI_SSID, ssid, ssid_size) != OK ||
      ssid[0] == '\0')
    {
      return -ENOENT;
    }

  (void)claw_config_get(AGENT_CFG_KEY_WIFI_PASS, pass, pass_size);
  return 0;
}

static void *rv_net_guard_worker(void *arg)
{
  char ssid[64];
  char pass[128];
  unsigned int offline_ms = 0;

  (void)arg;

  usleep(RV_NET_INITIAL_DELAY_MS * 1000);
  printf("[RV-NET] association guard started non_destructive=1 grace_ms=%u\n",
         (unsigned int)RV_NET_REPAIR_GRACE_MS);

  for (;;)
    {
      struct rv_net_status_s status;
      char ip[INET_ADDRSTRLEN];
      bool has_ip;
      bool associated;
      int ret;

      ret = rv_net_load_credentials(ssid, sizeof(ssid),
                                    pass, sizeof(pass));
      if (ret < 0)
        {
          offline_ms = 0;
          usleep(RV_NET_RETRY_BACKOFF_MS * 1000);
          continue;
        }

      memset(&status, 0, sizeof(status));
      (void)rv_net_read_show(&status);
      associated = rv_net_associated_with(&status, ssid);
      has_ip = rv_net_has_ipv4(ip, sizeof(ip));

      /*
       * F8C safety rule:
       * once wlan0 owns an IPv4 address, this guard is observation-only.
       * The official set_wifi path can complete DHCP before `wapi show`
       * exposes a stable BSSID.  Resetting wlan0 here races active sockets
       * and was the reason the F8B LLM TLS connection was dropped.
       */
      if (has_ip)
        {
          /*
           * A usable IPv4 lease takes precedence over the WAPI AP/BSSID query.
           * On this ESP32-S3 target `wapi show` may keep reporting a broadcast
           * BSSID even while TCP/TLS traffic is working normally.
           */
          offline_ms = 0;
          usleep(RV_NET_POLL_MS * 1000);
          continue;
        }

      if (associated)
        {
          /*
           * A real Wi-Fi link exists but DHCP is missing.  Give the official
           * manager a full grace window before trying our own renew.
           */
          offline_ms += RV_NET_POLL_MS;
          if (offline_ms < RV_NET_REPAIR_GRACE_MS)
            {
              usleep(RV_NET_POLL_MS * 1000);
              continue;
            }

          printf("[RV-NET] associated bssid=%s but no IPv4 for %u ms -> DHCP\n",
                 status.bssid, offline_ms);
          ret = system("renew " RV_NET_IFACE);
          offline_ms = 0;
          if (ret != 0)
            {
              printf("[RV-NET] DHCP repair failed rc=%d\n", ret);
            }

          usleep(RV_NET_POLL_MS * 1000);
          continue;
        }

      /*
       * Neither link nor IPv4 is present.  Do not race a just-issued
       * `set_wifi`: wait for a continuous outage before intervening.
       */
      offline_ms += RV_NET_POLL_MS;
      if (offline_ms < RV_NET_REPAIR_GRACE_MS)
        {
          usleep(RV_NET_POLL_MS * 1000);
          continue;
        }

      printf("[RV-NET] no association/IP for %u ms -> repair\n", offline_ms);
      ret = rv_net_run_connect(ssid, pass);
      offline_ms = 0;

      if (ret < 0)
        {
          printf("[RV-NET] repair failed rc=%d; retrying\n", ret);
          usleep(RV_NET_RETRY_BACKOFF_MS * 1000);
        }
      else
        {
          printf("[RV-NET] repair complete\n");
          usleep(RV_NET_POLL_MS * 1000);
        }
    }

  return NULL;
}

/****************************************************************************
 * Public API
 ****************************************************************************/

int robot_network_adapter_start(void)
{
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  pthread_mutex_lock(&g_rv_net_lock);
  if (g_rv_net_started)
    {
      pthread_mutex_unlock(&g_rv_net_lock);
      return 0;
    }

  /*
   * Do not clear an existing IPv4 address here.  The official set_wifi path
   * may be completing a valid association/DHCP sequence concurrently.
   */
  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = pthread_attr_setstacksize(&attr, RV_NET_GUARD_STACK);
      if (ret == 0)
        {
          ret = pthread_create(&thread, &attr, rv_net_guard_worker, NULL);
        }

      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      pthread_mutex_unlock(&g_rv_net_lock);
      printf("[RV-NET] failed to start guard rc=%d\n", ret);
      return -ret;
    }

  (void)pthread_detach(thread);
  g_rv_net_started = true;
  pthread_mutex_unlock(&g_rv_net_lock);

  printf("[RV-NET] guard scheduled delay_ms=%u\n",
         (unsigned int)RV_NET_INITIAL_DELAY_MS);
  return 0;
}
