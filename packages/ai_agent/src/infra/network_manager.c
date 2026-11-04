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

#include <nuttx/config.h>
#include "network_manager.h"
#include "agent_compat.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef __NuttX__
#include <nuttx/net/dns.h>
#endif
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "netmgr";

static char s_ip_str[INET_ADDRSTRLEN] = "0.0.0.0";
static pthread_mutex_t s_ip_lock = PTHREAD_MUTEX_INITIALIZER;

#if defined(CONFIG_WIRELESS_WAPI) && defined(CONFIG_NETUTILS_DHCPC) && \
    !defined(CONFIG_ARCH_CHIP_GOLDFISH_ARM64) && !defined(CONFIG_ARCH_CHIP_QEMU_ARM) && \
    !defined(CONFIG_AI_AGENT_NET_RPMSG)
#  define AGENT_CHECKED_WIFI 1
static bool ag_wifi_iface_usable(const char *dev);
static void ag_wifi_diag(void);
#endif

static bool network_valid_ipv4(in_addr_t address)
{
    uint32_t a = ntohl(address);
    return (a >> 24) != 0 && (a >> 24) != 127 && (a >> 24) < 224 &&
           (a >> 16) != 0xa9fe;
}

bool network_is_connected(void)
{
    struct ifaddrs *list = NULL;
    char found[INET_ADDRSTRLEN] = "0.0.0.0";
    bool connected = false;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ||
                !ifa->ifa_name || (ifa->ifa_flags & IFF_LOOPBACK) ||
                !(ifa->ifa_flags & IFF_UP)) continue;
            const struct sockaddr_in *sin = (const void *)ifa->ifa_addr;
            if (!network_valid_ipv4(sin->sin_addr.s_addr)) continue;
#ifdef AGENT_CHECKED_WIFI
            if (!strncmp(ifa->ifa_name, "wlan", 4) &&
                !ag_wifi_iface_usable(ifa->ifa_name)) continue;
#endif
            if (inet_ntop(AF_INET, &sin->sin_addr, found, sizeof(found))) {
                connected = true;
                break;
            }
        }
        freeifaddrs(list);
    }
    pthread_mutex_lock(&s_ip_lock);
    if (strcmp(s_ip_str, found))
        syslog(LOG_INFO, "[%s] interface IPv4 state: %s\n", TAG, found);
    snprintf(s_ip_str, sizeof(s_ip_str), "%s", found);
    pthread_mutex_unlock(&s_ip_lock);
    return connected;
}

int network_wait_connected(uint32_t timeout_ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (network_is_connected()) return OK;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_nsec - start.tv_nsec) / 1000000;
        if (!timeout_ms || (timeout_ms != UINT32_MAX && elapsed >= timeout_ms)) return ERROR;
        usleep(200000);
    }
}

const char *network_get_ip(void)
{
    network_is_connected();
    return s_ip_str;
}

const char *network_wifi_error_name(int rc)
{
    if (rc == 0) return "ok";
    /* Old ESP32 adapters use literal -1 for association timeout and unknown
     * SDK errors. It is not reliable evidence of EPERM. */
    if (rc == -1) return "legacy driver failure; radio reason required";
    if (rc == -ETIMEDOUT) return "association/DHCP timeout";
    if (rc == -ENOMEM) return "memory allocation failed";
    if (rc == -EINVAL) return "invalid configuration";
    if (rc == -EIO) return "driver/SDK failure";
    if (rc == -ENETDOWN) return "station is stopped";
    if (rc == -ENOTCONN) return "station is not associated";
    if (rc == -ENETUNREACH) return "no usable DHCP address";
    return "network operation failed";
}

#ifndef AGENT_CHECKED_WIFI
int network_wifi_configure(const char *iface, const char *ssid, const char *pass)
{
    return network_wifi_connect(iface, ssid, pass);
}
int network_wifi_restore(void) { return network_wifi_reconnect(); }
#endif

void network_wifi_diag(void)
{
#ifdef AGENT_CHECKED_WIFI
    ag_wifi_diag();
#else
    bool connected = network_is_connected();
    syslog(LOG_INFO, "[%s] network IPv4=%u ip=%s\n", TAG, connected, s_ip_str);
#endif
}

int network_dns_probe(const char *host)
{
#ifdef CONFIG_NETDB_DNSCLIENT
    if (!host || !host[0]) return -EINVAL;
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    errno = 0;
    int result = getaddrinfo(host, NULL, &hints, &addresses);
    if (result != 0) {
        syslog(LOG_ERR, "[%s] DNS host=%s gai=%d errno=%d; check DHCP DNS and router reachability\n",
               TAG, host, result, errno);
        return -EHOSTUNREACH;
    }
    int rc = -EHOSTUNREACH;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        if (a->ai_family == AF_INET && a->ai_addr) {
            char ip[INET_ADDRSTRLEN];
            const struct sockaddr_in *sin = (const void *)a->ai_addr;
            if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) {
                printf("DNS resolved: %s -> %s\n", host, ip);
                rc = OK;
            }
        }
    }
    freeaddrinfo(addresses);
    return rc;
#else
    (void)host;
    return -ENOSYS;
#endif
}

/* ── WiFi connect (real hardware only) ───────────────────────── */

#if defined(CONFIG_ARCH_CHIP_GOLDFISH_ARM64) || defined(CONFIG_ARCH_CHIP_QEMU_ARM)
/* On QEMU, virtio-net provides connectivity automatically — but needs
 * ifup/renew */
#include <net/if.h>
#include <nuttx/net/netconfig.h>
#include <sys/ioctl.h>

int network_wifi_connect(const char* iface, const char* ssid,
    const char* pass)
{
    (void)iface;
    (void)ssid;
    (void)pass;
    syslog(LOG_INFO,
        "[%s] QEMU: skipping wifi_connect (virtio-net handles networking)\n",
        TAG);
    return OK;
}

int network_wifi_reconnect(void)
{
    syslog(LOG_INFO, "[%s] QEMU: Initializing eth0...\n", TAG);

    /* Bring up eth0 interface */
    int ret = system("ifup eth0");
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] ifup eth0 failed: %d\n", TAG, ret);
    }

    /* Request DHCP lease */
    ret = system("renew eth0");
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] renew eth0 failed: %d\n", TAG, ret);
    }

    /* Wait a bit for network to come up */
    usleep(500000);

    return network_wait_connected(5000);
}

#elif defined(CONFIG_AI_AGENT_NET_RPMSG)
/* ── RPMSG/TUN network via BLE proxy ─────────────────────────── */

#include "config/config_store.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Static state ─────────────────────────────────────────────── */

typedef struct {
    char proxy_mode[16];    /* "usrsock" or "tun" */
    char rpmsg_cpu[16];     /* RPMSG target CPU name */
    int connect_timeout;    /* TLS connect timeout (seconds) */
    int read_timeout;       /* TLS read timeout (seconds) */
    int retry_max;          /* HTTP max retries */
    int retry_base_sec;     /* Retry backoff base (seconds) */
} net_config_t;

static net_config_t g_net_config;
static net_status_t g_net_status;

typedef struct {
    net_state_cb_t cb;
    void* arg;
} net_listener_t;

static net_listener_t g_listeners[NET_MAX_LISTENERS];
static int g_listener_count;

static pthread_mutex_t g_net_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_net_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_poll_thread;
static volatile bool g_poll_running;
static volatile bool g_force_check;

/* ── Internal helpers ─────────────────────────────────────────── */

static void notify_listeners(net_state_t state)
{
    for (int i = 0; i < g_listener_count; i++) {
        if (g_listeners[i].cb) {
            g_listeners[i].cb(state, g_listeners[i].arg);
        }
    }
}

static void set_net_state(net_state_t new_state)
{
    pthread_mutex_lock(&g_net_mutex);
    if (g_net_status.state != new_state) {
        net_state_t old = g_net_status.state;
        g_net_status.state = new_state;

        if (new_state == NET_STATE_CONNECTED) {
            g_net_status.connected_since = time(NULL);
        }

        syslog(LOG_INFO, "[%s] State: %s -> %s\n", TAG,
            old == NET_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED",
            new_state == NET_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED");

        notify_listeners(new_state);
        pthread_cond_broadcast(&g_net_cond);
    }
    pthread_mutex_unlock(&g_net_mutex);
}

/**
 * Check network interfaces via getifaddrs().
 * Returns true if a valid non-loopback IPv4 interface is found.
 * Accepts rpmsg*, tun*, eth* etc. Skips lo* and 127.x.x.x / 0.0.0.0.
 */
static bool check_interfaces(void)
{
    struct ifaddrs* ifa_list = NULL;
    bool found = false;

    if (getifaddrs(&ifa_list) != 0) {
        syslog(LOG_WARNING, "[%s] getifaddrs failed: %d\n", TAG, errno);
        return false;
    }

    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifa->ifa_name && strncmp(ifa->ifa_name, "lo", 2) == 0) {
            continue;
        }

        struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
        uint32_t addr = ntohl(sin->sin_addr.s_addr);

        /* Skip 127.x.x.x loopback */
        if ((addr >> 24) == 127) {
            continue;
        }
        /* Skip 0.0.0.0 — interface exists but has no IP yet */
        if (addr == 0) {
            continue;
        }

        /* Valid interface found */
        pthread_mutex_lock(&g_net_mutex);
        inet_ntop(AF_INET, &sin->sin_addr,
            g_net_status.ip_addr, sizeof(g_net_status.ip_addr));
        if (ifa->ifa_name) {
            strncpy(g_net_status.iface_name, ifa->ifa_name,
                sizeof(g_net_status.iface_name) - 1);
            g_net_status.iface_name[sizeof(g_net_status.iface_name) - 1] = '\0';
        }
        g_net_status.last_check = time(NULL);
        pthread_mutex_unlock(&g_net_mutex);

        /* Also update the shared s_ip_str for network_get_ip() */
        inet_ntop(AF_INET, &sin->sin_addr, s_ip_str, sizeof(s_ip_str));

        syslog(LOG_DEBUG, "[%s] Found iface %s addr %s\n", TAG,
            ifa->ifa_name ? ifa->ifa_name : "?",
            g_net_status.ip_addr);

        found = true;
        break;
    }

    freeifaddrs(ifa_list);
    return found;
}

/* ── Config loading ────────────────────────────────────────────── */

static void load_net_config(void)
{
    char buf[32] = { 0 };

    /* proxy_mode: default "usrsock" */
    if (claw_config_get("net.proxy_mode", g_net_config.proxy_mode,
            sizeof(g_net_config.proxy_mode))
            != OK
        || g_net_config.proxy_mode[0] == '\0') {
        strncpy(g_net_config.proxy_mode, "usrsock",
            sizeof(g_net_config.proxy_mode) - 1);
        g_net_config.proxy_mode[sizeof(g_net_config.proxy_mode) - 1] = '\0';
    }

    /* rpmsg_cpu: default "ap" */
    if (claw_config_get("net.rpmsg_cpu", g_net_config.rpmsg_cpu,
            sizeof(g_net_config.rpmsg_cpu))
            != OK
        || g_net_config.rpmsg_cpu[0] == '\0') {
        strncpy(g_net_config.rpmsg_cpu, "ap",
            sizeof(g_net_config.rpmsg_cpu) - 1);
        g_net_config.rpmsg_cpu[sizeof(g_net_config.rpmsg_cpu) - 1] = '\0';
    }

    /* connect_timeout: default 15 */
    if (claw_config_get("net.connect_timeout", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.connect_timeout = atoi(buf);
    } else {
        g_net_config.connect_timeout = 15;
    }

    /* read_timeout: default 30 */
    memset(buf, 0, sizeof(buf));
    if (claw_config_get("net.read_timeout", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.read_timeout = atoi(buf);
    } else {
        g_net_config.read_timeout = 30;
    }

    /* retry_max: default 3 */
    memset(buf, 0, sizeof(buf));
    if (claw_config_get("net.retry_max", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.retry_max = atoi(buf);
    } else {
        g_net_config.retry_max = 3;
    }

    /* retry_base_sec: default 2 */
    memset(buf, 0, sizeof(buf));
    if (claw_config_get("net.retry_base_sec", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.retry_base_sec = atoi(buf);
    } else {
        g_net_config.retry_base_sec = 2;
    }

    syslog(LOG_INFO,
        "[%s] Config loaded: proxy=%s cpu=%s timeout=%d/%d retry=%d base=%d\n",
        TAG, g_net_config.proxy_mode, g_net_config.rpmsg_cpu,
        g_net_config.connect_timeout, g_net_config.read_timeout,
        g_net_config.retry_max, g_net_config.retry_base_sec);
}

int network_save_proxy_config(const char* mode, const char* cpu_name)
{
    if (!mode || mode[0] == '\0') {
        syslog(LOG_ERR, "[%s] save_proxy_config: mode required\n", TAG);
        return -EINVAL;
    }

    claw_config_set("net.proxy_mode", mode);
    strncpy(g_net_config.proxy_mode, mode,
        sizeof(g_net_config.proxy_mode) - 1);
    g_net_config.proxy_mode[sizeof(g_net_config.proxy_mode) - 1] = '\0';

    if (cpu_name && cpu_name[0] != '\0') {
        claw_config_set("net.rpmsg_cpu", cpu_name);
        strncpy(g_net_config.rpmsg_cpu, cpu_name,
            sizeof(g_net_config.rpmsg_cpu) - 1);
        g_net_config.rpmsg_cpu[sizeof(g_net_config.rpmsg_cpu) - 1] = '\0';
    }

    syslog(LOG_INFO, "[%s] Proxy config saved: mode=%s cpu=%s\n",
        TAG, g_net_config.proxy_mode, g_net_config.rpmsg_cpu);
    return OK;
}

/* ── Config getters ───────────────────────────────────────────── */

int network_get_connect_timeout(void)
{
    return g_net_config.connect_timeout;
}

int network_get_read_timeout(void)
{
    return g_net_config.read_timeout;
}

int network_get_retry_max(void)
{
    return g_net_config.retry_max;
}

int network_get_retry_base_sec(void)
{
    return g_net_config.retry_base_sec;
}

const char* network_get_proxy_mode(void)
{
    return g_net_config.proxy_mode;
}

const char* network_get_rpmsg_cpu(void)
{
    return g_net_config.rpmsg_cpu;
}

/* ── Interface poll thread ────────────────────────────────────── */

static void* iface_poll_thread(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] O74I interface poll thread started\n", TAG);

    /* Startup timeout: 30 seconds to get an IP */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    bool startup_warned = false;

    while (g_poll_running) {
        bool has_ip = check_interfaces();

        if (has_ip) {
            set_net_state(NET_STATE_CONNECTED);
            startup_warned = false;
        } else {
            set_net_state(NET_STATE_DISCONNECTED);

            /* Check startup timeout */
            if (!startup_warned) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L
                    + (now.tv_nsec - start.tv_nsec) / 1000000L;
                if (elapsed_ms >= NET_STARTUP_TIMEOUT_MS) {
                    syslog(LOG_WARNING,
                        "[%s] No IP after %d ms startup timeout, "
                        "continuing poll\n",
                        TAG, NET_STARTUP_TIMEOUT_MS);
                    startup_warned = true;
                }
            }
        }

        /* Sleep for poll interval, but wake early on force check */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += NET_POLL_INTERVAL_MS / 1000;
        ts.tv_nsec += (NET_POLL_INTERVAL_MS % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&g_net_mutex);
        while (!g_force_check && g_poll_running) {
            int rc = pthread_cond_timedwait(&g_net_cond, &g_net_mutex, &ts);
            if (rc == ETIMEDOUT) {
                break;
            }
        }
        g_force_check = false;
        pthread_mutex_unlock(&g_net_mutex);
    }

    syslog(LOG_INFO, "[%s] O74I interface poll thread exiting\n", TAG);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

int network_register_listener(net_state_cb_t cb, void* arg)
{
    if (!cb) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_net_mutex);
    if (g_listener_count >= NET_MAX_LISTENERS) {
        pthread_mutex_unlock(&g_net_mutex);
        syslog(LOG_ERR, "[%s] Max listeners (%d) reached\n",
            TAG, NET_MAX_LISTENERS);
        return -ENOMEM;
    }

    g_listeners[g_listener_count].cb = cb;
    g_listeners[g_listener_count].arg = arg;
    g_listener_count++;
    pthread_mutex_unlock(&g_net_mutex);

    return OK;
}

net_state_t network_get_state(void)
{
    net_state_t state;
    pthread_mutex_lock(&g_net_mutex);
    state = g_net_status.state;
    pthread_mutex_unlock(&g_net_mutex);
    return state;
}

int network_get_active_conns(void)
{
    int conns;
    pthread_mutex_lock(&g_net_mutex);
    conns = g_net_status.active_conns;
    pthread_mutex_unlock(&g_net_mutex);
    return conns;
}

int network_get_iob_usage(void)
{
    FILE* fp = NULL;
    int usage_pct = 0;
    char line[128];
    int total = 0;
    int free_cnt = 0;

    fp = fopen("/proc/net/iob", "r");
    if (!fp) {
        syslog(LOG_DEBUG, "[%s] Cannot open /proc/net/iob\n", TAG);
        return 0;
    }

    /* Parse IOB stats — look for total and free counts.
     * Format varies by NuttX version; try common patterns. */
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "  total:%d", &total) == 1) {
            /* found total */
        } else if (sscanf(line, "  free:%d", &free_cnt) == 1) {
            /* found free */
        }
    }

    fclose(fp);

    if (total > 0) {
        usage_pct = ((total - free_cnt) * 100) / total;
        if (usage_pct < 0) {
            usage_pct = 0;
        }
        if (usage_pct > 100) {
            usage_pct = 100;
        }
    }

    pthread_mutex_lock(&g_net_mutex);
    g_net_status.iob_usage_pct = usage_pct;
    pthread_mutex_unlock(&g_net_mutex);

    return usage_pct;
}

int network_reconnect(void)
{
    syslog(LOG_INFO, "[%s] Reconnect requested\n", TAG);

    pthread_mutex_lock(&g_net_mutex);
    g_net_status.state = NET_STATE_DISCONNECTED;
    memset(g_net_status.ip_addr, 0, sizeof(g_net_status.ip_addr));
    memset(g_net_status.iface_name, 0, sizeof(g_net_status.iface_name));
    g_force_check = true;
    pthread_cond_signal(&g_net_cond);
    pthread_mutex_unlock(&g_net_mutex);

    return OK;
}

int network_set_dns(const char* primary, const char* secondary)
{
    FILE* fp = NULL;
    int ret = ERROR;

    if (!primary || primary[0] == '\0') {
        syslog(LOG_ERR, "[%s] set_dns: primary DNS required\n", TAG);
        return -EINVAL;
    }

    fp = fopen("/tmp/resolv.conf", "w");
    if (!fp) {
        syslog(LOG_ERR, "[%s] Cannot open /tmp/resolv.conf: %d\n",
            TAG, errno);
        return -errno;
    }

    fprintf(fp, "nameserver %s\n", primary);
    if (secondary && secondary[0] != '\0') {
        fprintf(fp, "nameserver %s\n", secondary);
    }

    fclose(fp);

    /* Persist to config_store */
    claw_config_set("net.dns_primary", primary);
    if (secondary && secondary[0] != '\0') {
        claw_config_set("net.dns_secondary", secondary);
    }

    syslog(LOG_INFO, "[%s] DNS configured: %s %s\n", TAG,
        primary, (secondary && secondary[0]) ? secondary : "");

    ret = OK;
    return ret;
}

/* ── BLE state callback stub ──────────────────────────────────── */

static void ble_state_callback(bool connected)
{
    syslog(LOG_INFO, "[%s] BLE state: %s\n", TAG,
        connected ? "connected" : "disconnected");

    pthread_mutex_lock(&g_net_mutex);
    g_net_status.ble_connected = connected;
    pthread_mutex_unlock(&g_net_mutex);

    if (!connected) {
        set_net_state(NET_STATE_DISCONNECTED);
    } else {
        /* BLE reconnected — trigger immediate interface check */
        pthread_mutex_lock(&g_net_mutex);
        g_force_check = true;
        pthread_cond_signal(&g_net_cond);
        pthread_mutex_unlock(&g_net_mutex);
    }
}

/* ── RPMSG init ───────────────────────────────────────────────── */

int network_rpmsg_init(void)
{
    int ret = ERROR;
    char dns_primary[64] = { 0 };
    char dns_secondary[64] = { 0 };

    syslog(LOG_INFO, "[%s] O74I: Initializing RPMSG/TUN network\n", TAG);

    /* Load network config from config_store */
    load_net_config();

    /* Initialize state */
    memset(&g_net_status, 0, sizeof(g_net_status));
    g_net_status.state = NET_STATE_DISCONNECTED;
    g_net_status.ble_connected = true; /* assume BLE up at boot */
    g_listener_count = 0;
    g_force_check = false;

    /* Load DNS config from config_store, use defaults if absent */
    if (claw_config_get("net.dns_primary", dns_primary,
            sizeof(dns_primary))
            != OK
        || dns_primary[0] == '\0') {
        strncpy(dns_primary, "223.5.5.5", sizeof(dns_primary) - 1);
        dns_primary[sizeof(dns_primary) - 1] = '\0';
    }

    if (claw_config_get("net.dns_secondary", dns_secondary,
            sizeof(dns_secondary))
            != OK
        || dns_secondary[0] == '\0') {
        strncpy(dns_secondary, "8.8.8.8", sizeof(dns_secondary) - 1);
        dns_secondary[sizeof(dns_secondary) - 1] = '\0';
    }

    /* Configure DNS */
    network_set_dns(dns_primary, dns_secondary);

    /* Start interface poll thread */
    g_poll_running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);

    ret = pthread_create(&g_poll_thread, &attr, iface_poll_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] Failed to create poll thread: %d\n",
            TAG, ret);
        g_poll_running = false;
        return ERROR;
    }

    pthread_setname_np(g_poll_thread, "net_poll");

    /* Register BLE state callback (stub — replace with real BLE API) */
    syslog(LOG_INFO,
        "[%s] BLE callback registered (stub, awaiting BLE framework)\n",
        TAG);
    (void)ble_state_callback; /* suppress unused warning */

    syslog(LOG_INFO, "[%s] O74I network init complete\n", TAG);
    return OK;
}

/* ── Resource guard ───────────────────────────────────────────── */

int network_acquire_resource(uint32_t timeout_ms)
{
    pthread_mutex_lock(&g_net_mutex);

    /* Fast-fail if BLE is disconnected */
    if (!g_net_status.ble_connected) {
        pthread_mutex_unlock(&g_net_mutex);
        return -ENETUNREACH;
    }

    /* Check TCP connection limit and IOB watermark */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (g_net_status.active_conns >= NET_MAX_TCP_CONNS
        || network_get_iob_usage() >= NET_IOB_HIGH_WATERMARK) {
        int rc = pthread_cond_timedwait(&g_net_cond, &g_net_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&g_net_mutex);
            syslog(LOG_WARNING, "[%s] acquire_resource timeout\n", TAG);
            return -ETIMEDOUT;
        }
        /* Re-check BLE after wakeup */
        if (!g_net_status.ble_connected) {
            pthread_mutex_unlock(&g_net_mutex);
            return -ENETUNREACH;
        }
    }

    g_net_status.active_conns++;
    pthread_mutex_unlock(&g_net_mutex);
    return OK;
}

void network_release_resource(void)
{
    pthread_mutex_lock(&g_net_mutex);
    if (g_net_status.active_conns > 0) {
        g_net_status.active_conns--;
    }
    pthread_cond_broadcast(&g_net_cond);
    pthread_mutex_unlock(&g_net_mutex);
}

/* ── WiFi stubs (redirect to rpmsg_init) ──────────────────────── */

int network_wifi_connect(const char* iface, const char* ssid,
    const char* pass)
{
    (void)iface;
    (void)ssid;
    (void)pass;
    syslog(LOG_INFO,
        "[%s] O74I: wifi_connect redirected to rpmsg_init\n", TAG);
    return network_rpmsg_init();
}

int network_wifi_reconnect(void)
{
    syslog(LOG_INFO,
        "[%s] O74I: wifi_reconnect redirected to rpmsg_init\n", TAG);
    return network_rpmsg_init();
}

#else
/* Hardware Wi-Fi has one owner. All callers, including legacy --wrap
 * bridges, enter the same checked and serialized transaction. */
#include "infra/config_store.h"
#include "agent_config.h"

#ifdef AGENT_CHECKED_WIFI
#include <ctype.h>
#include <nuttx/wireless/wireless.h>
#include <wireless/wapi.h>
#include <netutils/netlib.h>
#include <sys/ioctl.h>

#ifndef AGENT_WIFI_ASSOC_MS
#  define AGENT_WIFI_ASSOC_MS 15000
#endif
#define AGENT_WIFI_POLL_US 200000

static pthread_mutex_t s_wifi_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_wifi_status_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_wifi_iface[IFNAMSIZ] = "wlan0";
static enum { WIFI_UNMANAGED, WIFI_CONFIGURING, WIFI_READY, WIFI_FAILED } s_wifi_phase;
static const char *s_wifi_stage = "not-configured";
static int s_wifi_error;

static int wifi_error(int rc)
{
    if (rc == -1) return errno ? -errno : -EIO;
    return rc < 0 ? rc : -EIO;
}

static uint64_t wifi_milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* SSID can be configured before authentication succeeds. Also require a
 * running carrier and a real unicast AP BSSID, never a shell usage string. */
static int wifi_link(int fd, const char *dev, const char *expected)
{
    struct ifreq flags;
    struct iwreq ap;
    enum wapi_essid_flag_e flag;
    char ssid[WAPI_ESSID_MAX_SIZE + 1] = {0};
    unsigned int nonzero = 0, not_broadcast = 0;
    int rc;
    memset(&flags, 0, sizeof(flags));
    snprintf(flags.ifr_name, sizeof(flags.ifr_name), "%s", dev);
    errno = 0;
    rc = ioctl(fd, SIOCGIFFLAGS, (unsigned long)(uintptr_t)&flags);
    if (rc < 0) return wifi_error(rc);
    if ((flags.ifr_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING)) return 0;
    errno = 0;
    rc = wapi_get_essid(fd, dev, ssid, &flag);
    if (rc < 0) return wifi_error(rc);
    ssid[WAPI_ESSID_MAX_SIZE] = 0;
    if (flag != WAPI_ESSID_ON || (expected && strcmp(ssid, expected))) return 0;
    memset(&ap, 0, sizeof(ap));
    snprintf(ap.ifr_name, sizeof(ap.ifr_name), "%s", dev);
    errno = 0;
    rc = ioctl(fd, SIOCGIWAP, (unsigned long)(uintptr_t)&ap);
    if (rc < 0) return wifi_error(rc);
    const unsigned char *mac = (const unsigned char *)ap.u.ap_addr.sa_data;
    for (unsigned int i = 0; i < 6; i++) {
        nonzero |= mac[i];
        not_broadcast |= (unsigned int)(mac[i] ^ 0xff);
    }
    return nonzero && not_broadcast && !(mac[0] & 1);
}

static bool ag_wifi_iface_usable(const char *dev)
{
    bool blocked;
    pthread_mutex_lock(&s_wifi_status_lock);
    blocked = !strcmp(dev, s_wifi_iface) &&
              (s_wifi_phase == WIFI_CONFIGURING || s_wifi_phase == WIFI_FAILED);
    pthread_mutex_unlock(&s_wifi_status_lock);
    if (blocked) return false;
    int fd = wapi_make_socket();
    if (fd < 0) return false;
    int rc = wifi_link(fd, dev, NULL);
    close(fd);
    return rc == 1;
}

static void wifi_status(const char *dev, int phase, const char *stage, int error)
{
    pthread_mutex_lock(&s_wifi_status_lock);
    snprintf(s_wifi_iface, sizeof(s_wifi_iface), "%s", dev);
    s_wifi_phase = phase;
    s_wifi_stage = stage;
    s_wifi_error = error;
    pthread_mutex_unlock(&s_wifi_status_lock);
}

/* The contest image does not enable CONFIG_NETDB_RESOLVCONF, so writing a
 * resolv.conf file does not change getaddrinfo(). Keep a public resolver and
 * the DHCP gateway available without changing the Wi-Fi association. The
 * gateway is added last so it remains the first fallback when the two-slot
 * resolver table is full. */
static void wifi_install_fallback_dns(const char *dev)
{
    (void)dev;
#if defined(__NuttX__) && defined(CONFIG_NETDB_DNSCLIENT)
    struct sockaddr_in addr;
    struct in_addr gateway;
    int ret;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, "223.5.5.5", &addr.sin_addr) != 1)
      {
        syslog(LOG_ERR, "[%s] invalid fallback DNS address\n", TAG);
        return;
      }

    ret = dns_add_nameserver((const struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0)
      {
        syslog(LOG_WARNING,
               "[%s] fallback DNS registration failed rc=%d\n", TAG, ret);
      }
    else
      {
        syslog(LOG_INFO, "[%s] fallback DNS registered: 223.5.5.5\n", TAG);
      }

    memset(&gateway, 0, sizeof(gateway));
    if (dev != NULL && netlib_get_dripv4addr(dev, &gateway) == 0 &&
        gateway.s_addr != 0 && gateway.s_addr != addr.sin_addr.s_addr)
      {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr = gateway;
        ret = dns_add_nameserver((const struct sockaddr *)&addr,
                                 sizeof(addr));
        if (ret < 0)
          {
            syslog(LOG_WARNING,
                   "[%s] gateway DNS registration failed rc=%d\n",
                   TAG, ret);
          }
        else
          {
            char gateway_text[INET_ADDRSTRLEN] = "?";
            (void)inet_ntop(AF_INET, &gateway, gateway_text,
                            sizeof(gateway_text));
            syslog(LOG_INFO, "[%s] gateway DNS registered: %s\n",
                   TAG, gateway_text);
          }
      }
#endif
}

static bool wifi_valid_arguments(const char *dev, const char *ssid, const char *pass)
{
    if (!dev || !dev[0] || strlen(dev) >= IFNAMSIZ || !ssid ||
        !ssid[0] || strlen(ssid) > WAPI_ESSID_MAX_SIZE) return false;
    size_t n = pass ? strlen(pass) : 0;
    if (!n) return true;
    if (n >= 8 && n <= 63) return true;
    if (n != 64) return false;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)pass[i])) return false;
    return true;
}

/* Called only while s_wifi_lock is held. DNS and routing are installed by
 * the contest fallback resolver and the DHCP lease. */
static int wifi_configure_locked(const char *dev, const char *ssid, const char *pass)
{
    int fd = -1, ret = 0;
    const char *stage = "validate";
    struct in_addr zero = {0}, ip;
    bool secured = pass && pass[0];
    if (!wifi_valid_arguments(dev, ssid, pass)) return -EINVAL;
    wifi_status(dev, WIFI_CONFIGURING, "configuring", 0);
    pthread_mutex_lock(&s_ip_lock);
    snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
    pthread_mutex_unlock(&s_ip_lock);
#define WIFI_CHECK(label, operation) do { \
    stage = (label); errno = 0; int result_ = (operation); \
    if (result_ != 0) { ret = wifi_error(result_); goto failed; } \
} while (0)
    WIFI_CHECK("clear-ip", netlib_set_ipv4addr(dev, &zero));
    WIFI_CHECK("clear-route", netlib_set_dripv4addr(dev, &zero));
    WIFI_CHECK("clear-mask", netlib_set_ipv4netmask(dev, &zero));
    /* Deliberate user/boot transaction: stop a stale in-flight join first.
     * The ESP32-S3 backport cancels auto-reconnect before stopping the STA. */
    WIFI_CHECK("ifdown", netlib_ifdown(dev));
    WIFI_CHECK("ifup", netlib_ifup(dev));
    usleep(500000);
    stage = "socket"; errno = 0;
    fd = wapi_make_socket();
    if (fd < 0) { ret = wifi_error(fd); goto failed; }
    WIFI_CHECK("mode", wapi_set_mode(fd, dev, WAPI_MODE_MANAGED));
    WIFI_CHECK("auth", wpa_driver_wext_set_auth_param(fd, dev, IW_AUTH_WPA_VERSION,
        secured ? IW_AUTH_WPA_VERSION_WPA2 : IW_AUTH_WPA_VERSION_DISABLED));
    WIFI_CHECK("cipher", wpa_driver_wext_set_auth_param(fd, dev, IW_AUTH_CIPHER_PAIRWISE,
        secured ? IW_AUTH_CIPHER_CCMP : IW_AUTH_CIPHER_NONE));
    WIFI_CHECK("key", wpa_driver_wext_set_key_ext(fd, dev,
        secured ? WPA_ALG_CCMP : WPA_ALG_NONE, secured ? pass : "", secured ? strlen(pass) : 0));
    WIFI_CHECK("essid", wapi_set_essid(fd, dev, ssid, WAPI_ESSID_ON));
    stage = "associate";
    uint64_t start = wifi_milliseconds();
    for (;;) {
        int linked = wifi_link(fd, dev, ssid);
        if (linked == 1) break;
        if (linked < 0 && linked != -EAGAIN && linked != -ENOTCONN && linked != -ENETDOWN) {
            ret = linked; goto failed;
        }
        if (wifi_milliseconds() - start >= AGENT_WIFI_ASSOC_MS) {
            ret = -ETIMEDOUT; goto failed;
        }
        usleep(AGENT_WIFI_POLL_US);
    }
    syslog(LOG_INFO, "[%s] associated on %s; requesting DHCP\n", TAG, dev);
    WIFI_CHECK("dhcp", netlib_obtain_ipv4addr(dev));
    WIFI_CHECK("ipv4", netlib_get_ipv4addr(dev, &ip));
    if (!network_valid_ipv4(ip.s_addr)) { ret = -ENETUNREACH; stage = "lease-ip"; goto failed; }
    /* DHCP has installed the lease and gateway by this point, so retain both
     * the hotspot resolver and the public fallback in the resolver table. */
    wifi_install_fallback_dns(dev);
    stage = "post-dhcp-link";
    ret = wifi_link(fd, dev, ssid);
    if (ret != 1) { if (ret >= 0) ret = -ENOTCONN; goto failed; }
    close(fd); fd = -1;
    wifi_status(dev, WIFI_READY, "dhcp-ready", 0);
    pthread_mutex_lock(&s_ip_lock);
    inet_ntop(AF_INET, &ip, s_ip_str, sizeof(s_ip_str));
    pthread_mutex_unlock(&s_ip_lock);
    int save_ssid = claw_config_set(AGENT_CFG_KEY_WIFI_SSID, ssid);
    int save_pass = claw_config_set(AGENT_CFG_KEY_WIFI_PASS, secured ? pass : "");
    if (save_ssid != OK || save_pass != OK)
        syslog(LOG_WARNING, "[%s] link ready; credential persistence failed\n", TAG);
    char address[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip, address, sizeof(address));
    syslog(LOG_INFO, "[AG-WIFI-CHECKED] DHCP ready iface=%s ip=%s (Internet/DNS not yet probed)\n", dev, address);
    return OK;
failed:
    wifi_status(dev, WIFI_FAILED, stage, ret);
    if (fd >= 0) close(fd);
    /* A partial lease or residual address must never mask the failed step. */
    (void)netlib_set_ipv4addr(dev, &zero);
    (void)netlib_set_dripv4addr(dev, &zero);
    pthread_mutex_lock(&s_ip_lock);
    snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
    pthread_mutex_unlock(&s_ip_lock);
    syslog(LOG_ERR, "[AG-WIFI-CHECKED] failed iface=%s stage=%s rc=%d (%s); see RV-WIFI-DRV for the radio reason\n",
           dev, stage, ret, network_wifi_error_name(ret));
    return ret;
#undef WIFI_CHECK
}

int network_wifi_configure(const char *iface, const char *ssid, const char *pass)
{
    const char *dev = iface && iface[0] ? iface : "wlan0";
    pthread_mutex_lock(&s_wifi_lock);
    int ret = wifi_configure_locked(dev, ssid, pass);
    pthread_mutex_unlock(&s_wifi_lock);
    return ret;
}

int network_wifi_connect(const char *iface, const char *ssid, const char *pass)
{
    return network_wifi_configure(iface, ssid, pass);
}

int network_wifi_restore(void)
{
    char ssid[WAPI_ESSID_MAX_SIZE + 1] = {0}, pass[65] = {0};
    pthread_mutex_lock(&s_wifi_lock);
    /* Entering ai_agent must not replace an already usable NSH connection. */
    if (network_is_connected()) {
        pthread_mutex_unlock(&s_wifi_lock);
        syslog(LOG_INFO, "[%s] existing link is usable; preserving it\n", TAG);
        return OK;
    }
    int ret = claw_config_get(AGENT_CFG_KEY_WIFI_SSID, ssid, sizeof(ssid));
    if (ret != OK || !ssid[0]) ret = -ENOENT;
    else {
        ret = claw_config_get(AGENT_CFG_KEY_WIFI_PASS, pass, sizeof(pass));
        /* config_store reports empty values as ERROR, including saved open
         * networks. A readable SSID plus an absent/empty PSK means open. */
        if (ret != OK) pass[0] = 0;
        ret = wifi_configure_locked("wlan0", ssid, pass);
    }
    memset(pass, 0, sizeof(pass));
    pthread_mutex_unlock(&s_wifi_lock);
    return ret;
}

int network_wifi_reconnect(void)
{
    return network_wifi_restore();
}

static void ag_wifi_diag(void)
{
    char dev[IFNAMSIZ];
    const char *stage;
    int phase, error;
    pthread_mutex_lock(&s_wifi_status_lock);
    snprintf(dev, sizeof(dev), "%s", s_wifi_iface);
    phase = s_wifi_phase; stage = s_wifi_stage; error = s_wifi_error;
    pthread_mutex_unlock(&s_wifi_status_lock);
    bool connected = network_is_connected();
    syslog(LOG_INFO, "[%s] WiFi iface=%s phase=%d stage=%s rc=%d (%s) link+IPv4=%u ip=%s\n",
           TAG, dev, phase, stage, error, network_wifi_error_name(error), connected, s_ip_str);
}
#else
int network_wifi_connect(const char *iface, const char *ssid, const char *pass)
{
    (void)iface; (void)ssid; (void)pass;
    syslog(LOG_ERR, "[%s] enable CONFIG_WIRELESS_WAPI and CONFIG_NETUTILS_DHCPC for hardware WiFi\n", TAG);
    return -ENOSYS;
}
int network_wifi_reconnect(void) { return -ENOSYS; }
#endif /* AGENT_CHECKED_WIFI */
#endif /* platform */
