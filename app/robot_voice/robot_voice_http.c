#include <nuttx/config.h>
#include "robot_voice_http.h"
#include "robot_voice_config.h"
#include "robot_voice_diag.h"
#include "mimo_client.h"
#include "mimo_llm_stream.h"
#include <agent_compat.h>
#include <infra/http_proxy.h>
#include <infra/network_manager.h>
#ifdef CONFIG_AI_AGENT_NET_RPMSG
#  include <network/network_manager.h>
#endif
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#if defined(MBEDTLS_X509_CRT_PARSE_C)
#  include <mbedtls/x509_crt.h>
#endif
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct rv_tls {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config cfg;
  mbedtls_net_context net;
  mbedtls_ctr_drbg_context random;
#if defined(MBEDTLS_X509_CRT_PARSE_C)
  mbedtls_x509_crt ca;
#endif
  const char *stage;
  int raw_error, sys_error;
  unsigned int bio_reads, bio_eagain, poll_ready, poll_timeouts;
  size_t bio_bytes;
  unsigned int bio_writes, bio_write_eagain, io_backoffs;
  size_t bio_sent, bio_send_max;
  size_t last_ready_progress;
  unsigned long long io_deadline;
  rv_http_entity_cb cancel_probe;
  void *cancel_arg;
#ifdef CONFIG_AI_AGENT_NET_RPMSG
  bool resource;
#endif
};
struct rv_connection {
  struct rv_tls *tls;
  bool busy;
  char host[160], port[8];
  unsigned long long last_used;
  unsigned int id;
  unsigned int generation;
  pid_t owner;
};
static struct rv_connection pool[4];
static unsigned int next_id;
static unsigned int network_generation;
static bool network_recovering;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_changed = PTHREAD_COND_INITIALIZER;

/* Do not force a standalone HTTP archive to pull the application entry. */
extern unsigned long robot_voice_llm_request_token(const char *) __attribute__((weak));
/* The two HTTP clients have separate idle pools. Release both on kernel
 * socket-memory pressure or before an exclusive Wi-Fi recovery. */
extern void vela_tls_pool_cleanup(void) __attribute__((weak));

/* Keep the RPMSG network service awake between ASR/Flash/TTS requests.
 * Each socket retains its own balanced lease. No radio/PM global changes. */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int session_users;
int robot_voice_http_session_begin(void)
{
  int ret = 0;
  pthread_mutex_lock(&session_lock);
#ifdef CONFIG_AI_AGENT_NET_RPMSG
  if (!session_users && network_acquire_resource(ROBOT_VOICE_HTTP_CONNECT_MS) != 0)
    ret = -ENETDOWN;
#endif
  if (!ret) session_users++;
  pthread_mutex_unlock(&session_lock);
  return ret;
}
void robot_voice_http_session_end(void)
{
  pthread_mutex_lock(&session_lock);
  if (session_users && --session_users == 0) {
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    network_release_resource();
#endif
  }
  pthread_mutex_unlock(&session_lock);
}

/* Optional board override: a read-only association/IP snapshot, never repair. */
__attribute__((weak)) void robot_voice_http_network_diag(void) {}
/* The platform records evidence only; Wi-Fi repair runs in the controller. */
__attribute__((weak)) void robot_voice_http_network_result(int result,
                                                         bool connect_failure)
{ (void)result; (void)connect_failure; }

static unsigned long long now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned int remaining_ms(unsigned long long deadline, unsigned int cap)
{
  unsigned long long now = now_ms();
  unsigned long long left = deadline > now ? deadline - now : 0;
  return left < cap ? (unsigned int)left : cap;
}

static int timeout_socket(int fd, unsigned int ms)
{
  struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
  if (!ms) return -ETIMEDOUT;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
    return -errno;
  return 0;
}

static int entropy(void *arg, unsigned char *out, size_t len)
{
  (void)arg;
  return agent_secure_random(out, len);  /* same secure source as official TLS */
}

/* Map temporary socket errors in the BIO, before SSL sees them. A terminal
 * SSL error must close the context; it cannot be turned into WANT_READ later.
 * Nonblocking I/O avoids the board's SO_RCVTIMEO -> NET_RECV_FAILED path. */
static int rv_net_recv(void *arg, unsigned char *out, size_t len)
{
  struct rv_tls *t = arg;
  int n = recv(t->net.fd, out, len, 0);
  t->bio_reads++;
  if (n >= 0) { t->bio_bytes += (size_t)n; return n; }
  t->sys_error = errno;
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    { t->bio_eagain++; return MBEDTLS_ERR_SSL_WANT_READ; }
  if (errno == ECONNRESET || errno == EPIPE) return MBEDTLS_ERR_NET_CONN_RESET;
  return MBEDTLS_ERR_NET_RECV_FAILED;
}

static int rv_net_send(void *arg, const unsigned char *data, size_t len)
{
  struct rv_tls *t = arg;
  int flags = 0;
  int original_flags = -1;
  /* Limit the socket offer, including ClientHello/handshake records. TLS
   * itself handles partial BIO writes without changing the encrypted bytes. */
  if (len > ROBOT_VOICE_TCP_SEND_CHUNK) len = ROBOT_VOICE_TCP_SEND_CHUNK;
  if (ROBOT_VOICE_TCP_TIMED_SEND) {
    unsigned int ms = t->io_deadline ?
      remaining_ms(t->io_deadline, ROBOT_VOICE_HTTP_POLL_MS) : ROBOT_VOICE_HTTP_POLL_MS;
    if (!ms) { errno = ETIMEDOUT; t->sys_error = errno; return MBEDTLS_ERR_NET_SEND_FAILED; }
    struct timeval tv = {.tv_sec=ms/1000, .tv_usec=(ms%1000)*1000};
    if (setsockopt(t->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0 ||
        (original_flags = fcntl(t->net.fd, F_GETFL, 0)) < 0 ||
        fcntl(t->net.fd, F_SETFL, original_flags & ~O_NONBLOCK) < 0) {
      t->sys_error = errno;
      return MBEDTLS_ERR_NET_SEND_FAILED;
    }
  }
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  int n = send(t->net.fd, data, len, flags);
  int error = n < 0 ? errno : 0;
  t->bio_writes++;
  if (len > t->bio_send_max) t->bio_send_max = len;
  /* The slot has one borrower. Restore flags before TLS can call recv(). */
  if (ROBOT_VOICE_TCP_TIMED_SEND &&
      fcntl(t->net.fd, F_SETFL, original_flags) < 0) {
    t->sys_error = errno;
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  if (n >= 0) { t->bio_sent += (size_t)n; return n; }
  t->sys_error = error;
  errno = error;
  if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
    t->bio_write_eagain++;
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  }
  if (error == ECONNRESET || error == EPIPE) return MBEDTLS_ERR_NET_CONN_RESET;
  return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int wait_io(struct rv_tls *t, int want, unsigned long long deadline)
{
  struct pollfd p = {.fd = t->net.fd,
    .events = want == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN};
  for (;;) {
    if (t->cancel_probe) {
      int probe = t->cancel_probe(NULL, 0, t->cancel_arg);
      if (probe < 0) return probe;
    }
    unsigned int ms = remaining_ms(deadline, ROBOT_VOICE_HTTP_POLL_MS);
    if (!ms) return -ETIMEDOUT;
    p.revents = 0;
    unsigned long long poll_started = now_ms();
    int n = poll(&p, 1, (int)ms);
    if (n < 0) {
      if (errno == EINTR) continue;
      t->sys_error = errno;
      return -errno;
    }
    if (n == 0) t->poll_timeouts++;
    if (n > 0) {
      t->poll_ready++;
      if (p.revents & POLLNVAL) return -EBADF;
      /* Let SSL consume buffered bytes or observe EOF/reset on HUP/ERR. */
      if (p.revents & (POLLHUP | POLLERR)) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(t->net.fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error) {
          t->sys_error = error;
          return -error;
        }
      }
      if (p.revents & (p.events | POLLHUP | POLLERR)) {
        /* Readiness is a hint, not evidence that a BIO made progress. In
         * particular POLLOUT can remain set while send() returns EAGAIN.
         * A real timed block lets lower priority TX/RX workers return IOBs;
         * sched_yield() only helps tasks at the same priority. */
        size_t progress = t->bio_sent + t->bio_bytes;
        if (progress == t->last_ready_progress && now_ms() == poll_started) {
          unsigned int delay_ms = remaining_ms(deadline, ROBOT_VOICE_HTTP_BACKOFF_MS);
          if (!delay_ms) return -ETIMEDOUT;
          struct timespec delay = {.tv_sec=delay_ms/1000,
            .tv_nsec=(long)(delay_ms%1000)*1000000L};
          while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
          t->io_backoffs++;
        }
        t->last_ready_progress = progress;
        return remaining_ms(deadline, 1) ? 0 : -ETIMEDOUT;
      }
    }
  }
}

static void tls_close(struct rv_tls *t)
{
  if (!t) return;
  /* Do not wait for a peer close_notify on an error or application boundary. */
  mbedtls_net_free(&t->net);
  mbedtls_ssl_free(&t->ssl);
  mbedtls_ssl_config_free(&t->cfg);
  mbedtls_ctr_drbg_free(&t->random);
#if defined(MBEDTLS_X509_CRT_PARSE_C)
  mbedtls_x509_crt_free(&t->ca);
#endif
#ifdef CONFIG_AI_AGENT_NET_RPMSG
  if (t->resource) network_release_resource();
#endif
  free(t);
}

static int receive_buffer_target(const char *kind)
{
  int desired = kind && !strcmp(kind, "TTS") ?
    ROBOT_VOICE_TTS_TCP_RCVBUF / ROBOT_VOICE_TTS_WORKERS :
    ROBOT_VOICE_TCP_RCVBUF;
#if defined(CONFIG_IOB_NBUFFERS) && defined(CONFIG_IOB_BUFSIZE)
  /* Reserve the configured throttle plus transport/other-service headroom.
   * This is a policy bound, not a reservation of physical IOBs. Actual load
   * still needs the on-board NOBUF counters; never grow the DRAM pool here.
   */
  int reserve = 16;
#  ifdef CONFIG_IOB_THROTTLE
  reserve = CONFIG_IOB_THROTTLE;
#  endif
  int budget = (CONFIG_IOB_NBUFFERS - reserve) * CONFIG_IOB_BUFSIZE;
  int headroom = 2 * ROBOT_VOICE_TCP_RCVBUF;
  int ceiling = (budget > headroom ? budget - headroom : 0) /
                ROBOT_VOICE_TTS_WORKERS;
  if (kind && !strcmp(kind, "TTS") && desired > ceiling)
    desired = ceiling;
#endif
  return desired;
}

static void tune_receive_buffer(int fd, int desired)
{
  int actual = 0;
  socklen_t length = sizeof(actual);
  if (desired <= 0) return;
  int read_rc = getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &length);
  int rc = 0, error = 0;
  /* OpenVela/NuttX may implement SO_RCVBUF. Probe it instead of compiling
   * tuning out for every NuttX target. If unsupported, leave the connection
   * usable and report the failed option; do not claim the window changed.
   * Set also when a pooled socket previously served a different request kind.
   */
  if (read_rc < 0 || actual != desired) {
    rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &desired, sizeof(desired));
    error = rc < 0 ? errno : 0;
    length = sizeof(actual);
    read_rc = getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &length);
  }
  RV_EVENT("[RV-NET-TUNE] rcvbuf_requested=%d actual=%d set_rc=%d errno=%d read_rc=%d\n",
         desired, read_rc == 0 ? actual : -1, rc, error, read_rc);
}

/* Connect callbacks are scarce on embedded TCP stacks. Serialize only the
 * pre-HTTP TCP setup, never TLS reads, ASR, Flash generation or TTS download. */
static pthread_mutex_t connect_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t connect_changed = PTHREAD_COND_INITIALIZER;
static bool connect_busy;
static int connect_enter(unsigned long long deadline)
{
  int ret = 0;
  pthread_mutex_lock(&connect_lock);
  while (connect_busy) {
    unsigned int ms = remaining_ms(deadline, 50);
    if (!ms) { ret = -ETIMEDOUT; break; }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)ms * 1000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&connect_changed, &connect_lock, &until);
  }
  if (!ret) connect_busy = true;
  pthread_mutex_unlock(&connect_lock);
  return ret;
}
static void connect_leave(void)
{
  pthread_mutex_lock(&connect_lock);
  connect_busy = false;
  pthread_cond_broadcast(&connect_changed);
  pthread_mutex_unlock(&connect_lock);
}
static int connect_busy_backoff(unsigned int attempt, unsigned long long deadline)
{
  unsigned int ms = remaining_ms(deadline, ROBOT_VOICE_TCP_BUSY_DELAY_MS * attempt);
  if (!ms) return -ETIMEDOUT;
  struct timespec delay = {.tv_sec=ms/1000, .tv_nsec=(long)(ms%1000)*1000000};
  while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
  return remaining_ms(deadline, 1) ? 0 : -ETIMEDOUT;
}

static int tcp_connect(const char *host, const char *port,
                       unsigned long long deadline, struct rv_tls *t,
                       int receive_bytes)
{
  struct addrinfo hints, *addresses = NULL, *a;
  int fd = -1, ret = -EHOSTUNREACH;
  unsigned int dns_attempt;
  bool entered = false;
  unsigned int busy_attempt = 0;
  unsigned long long start = now_ms();
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  /* POSIX getaddrinfo itself has no deadline API. Its platform resolver
   * timeout remains in force; all following I/O is deadline bounded. */
  t->stage = "dns";
  for (dns_attempt = 0; dns_attempt < ROBOT_VOICE_DNS_RETRIES;
       dns_attempt++)
    {
      errno = 0;
      t->raw_error = getaddrinfo(host, port, &hints, &addresses);
      if (t->raw_error == 0)
        {
          break;
        }

      /* EAI_AGAIN is expected during a hotspot DNS handoff. Keep the
       * existing Wi-Fi association and retry the resolver before failing the
       * HTTP request. */
      if (t->raw_error != EAI_AGAIN ||
          dns_attempt + 1 >= ROBOT_VOICE_DNS_RETRIES)
        {
          return -EHOSTUNREACH;
        }

      RV_EVENT("[RV-DNS] retry=%u/%u host=%s gai=%d\n",
               dns_attempt + 1, ROBOT_VOICE_DNS_RETRIES, host,
               t->raw_error);
      unsigned int delay = remaining_ms(deadline,
                                        ROBOT_VOICE_DNS_RETRY_DELAY_MS);
      struct timespec nap = {.tv_sec = delay / 1000,
                             .tv_nsec = (long)(delay % 1000) * 1000000L};
      if (!delay)
        {
          return -ETIMEDOUT;
        }
      while (nanosleep(&nap, &nap) < 0 && errno == EINTR) {}
      addresses = NULL;
    }
  if (addresses == NULL) return -EHOSTUNREACH;
  RV_VERBOSE("[RV-HTTP] DNS host=%s ms=%llu\n", host, now_ms() - start);
  if (ROBOT_VOICE_TCP_BLOCKING_CONNECT) {
    ret = connect_enter(deadline);
    if (ret < 0) { freeaddrinfo(addresses); return ret; }
    entered = true;
  }
  for (a = addresses; a; ) {
    int flags;
    int poll_events = 0, so_error = -1;
    unsigned long long connect_start = now_ms();
    char peer[INET6_ADDRSTRLEN] = "unknown";
    const void *ip = NULL;
    if (a->ai_family == AF_INET)
      ip = &((struct sockaddr_in *)a->ai_addr)->sin_addr;
    else if (a->ai_family == AF_INET6)
      ip = &((struct sockaddr_in6 *)a->ai_addr)->sin6_addr;
    if (ip) (void)inet_ntop(a->ai_family, ip, peer, sizeof(peer));
    unsigned int wait_ms = remaining_ms(deadline, ROBOT_VOICE_HTTP_CONNECT_MS);
    if (!wait_ms) { ret = -ETIMEDOUT; break; }
    t->stage = "tcp-socket";
    fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) { ret = -errno; a = a->ai_next; continue; }
    tune_receive_buffer(fd, receive_bytes);
    t->stage = "tcp-connect";
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
      ret = -errno;
    } else if (ROBOT_VOICE_TCP_BLOCKING_CONNECT) {
      t->stage = "tcp-timed-connect";
      ret = fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0 ? -errno :
            timeout_socket(fd, remaining_ms(deadline, ROBOT_VOICE_HTTP_CONNECT_MS));
      if (!ret) ret = connect(fd, a->ai_addr, a->ai_addrlen) == 0 ? 0 : -errno;
      if (!ret && !remaining_ms(deadline, 1)) ret = -ETIMEDOUT;
    } else if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      ret = -errno;
    } else if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
      ret = 0;
    } else if (errno == EINPROGRESS || errno == EAGAIN) {
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      int ready;
      t->stage = "tcp-poll";
      do {
        wait_ms = remaining_ms(deadline, ROBOT_VOICE_HTTP_CONNECT_MS);
        ready = wait_ms ? poll(&pfd, 1, (int)wait_ms) : 0;
      } while (ready < 0 && errno == EINTR);
      if (ready > 0) {
        int error = 0;
        socklen_t size = sizeof(error);
        poll_events = pfd.revents;
        t->stage = "tcp-so-error";
        int so_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
        ret = so_rc == 0 ? -error : -errno;
        so_error = so_rc == 0 ? error : -1;
        if (pfd.revents & POLLNVAL) ret = -EBADF;
        else if (ret == 0 && !(pfd.revents & POLLOUT)) {
          /* Distinguish our readiness guard from a real SO_ERROR=103. */
          t->stage = "tcp-poll-not-writable";
          ret = -ECONNABORTED;
        }
      } else ret = ready == 0 ? -ETIMEDOUT : -errno;
#ifdef __NuttX__
      /* Retry a local poll/SO_ERROR compatibility failure on a fresh socket,
       * before sending HTTP. NuttX connect uses SO_SNDTIMEO. */
      if (ret == -EIO || ret == -ENOSYS || ret == -ENOTSUP || ret == -ENOPROTOOPT ||
          (ret == -ECONNABORTED && so_error == 0)) {
        RV_EVENT("[RV-HTTP] connect_fallback stage=%s rc=%d so_error=%d poll=0x%x\n",
                 t->stage, ret, so_error, (unsigned int)poll_events);
        close(fd);
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        t->stage = "tcp-blocking-fallback";
        if (fd < 0) ret = -errno;
        else {
          tune_receive_buffer(fd, receive_bytes);
          ret = timeout_socket(fd, remaining_ms(deadline, ROBOT_VOICE_HTTP_CONNECT_MS));
          if (ret == 0) ret = connect(fd, a->ai_addr, a->ai_addrlen) == 0 ? 0 : -errno;
          flags = 0;
        }
      }
#endif
    } else ret = -errno;
    if (ret == 0 && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0) break;
    if (ret == 0) ret = -errno;
    t->raw_error = ret; t->sys_error = ret < 0 ? -ret : 0;
    RV_EVENT("[RV-TCP-ERROR] host=%s port=%s peer=%s family=%d fd=%d "
             "stage=%s rc=%d so_error=%d poll=0x%x elapsed_ms=%llu\n",
             host, port, peer, a->ai_family, fd, t->stage, ret,
             so_error, (unsigned int)poll_events, now_ms() - connect_start);
    if (fd >= 0) close(fd);
    fd = -1;
    if ((ret == -EBUSY || ret == -EAGAIN) &&
        busy_attempt < ROBOT_VOICE_TCP_BUSY_RETRIES) {
      busy_attempt++;
      RV_EVENT("[RV-TCP] local-busy retry=%u stage=%s; bounded backoff before HTTP\n",
               busy_attempt, t->stage);
      int delayed = connect_busy_backoff(busy_attempt, deadline);
      if (delayed < 0) { ret = delayed; break; }
      continue; /* fresh socket, same address, shared original deadline */
    }
    busy_attempt = 0;
    a = a->ai_next;
  }
  if (entered) connect_leave();
  freeaddrinfo(addresses);
  if (fd >= 0) {
    int enabled = 1;
    /* Optional; unsupported socket tuning does not fail a request. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
  }
  return fd >= 0 ? fd : ret;
}

static int tls_open_inner(struct rv_connection *slot, unsigned long long deadline,
                          int receive_bytes, rv_http_entity_cb probe, void *arg)
{
  struct rv_tls *t = calloc(1, sizeof(*t));
  int ret;
  unsigned long long start = now_ms();
  unsigned long long request_deadline = deadline;
  if (deadline > start + ROBOT_VOICE_HTTP_CONNECT_MS)
    deadline = start + ROBOT_VOICE_HTTP_CONNECT_MS;
  if (!t) return -ENOMEM;
  slot->tls = t;
  t->cancel_probe = probe;
  t->cancel_arg = arg;
  t->stage = "entropy-seed";
  mbedtls_ssl_init(&t->ssl);
  mbedtls_ssl_config_init(&t->cfg);
  mbedtls_net_init(&t->net);
  mbedtls_ctr_drbg_init(&t->random);
#if defined(MBEDTLS_X509_CRT_PARSE_C)
  mbedtls_x509_crt_init(&t->ca);
#endif
  ret = mbedtls_ctr_drbg_seed(&t->random, entropy, NULL,
      (const unsigned char *)"contest_robot_voice", 19);
  if (ret) { t->raw_error = ret; return -EIO; }
#ifdef CONFIG_AI_AGENT_NET_RPMSG
  if (network_acquire_resource(remaining_ms(deadline, ROBOT_VOICE_HTTP_CONNECT_MS)) != 0)
    return -ENETDOWN;
  t->resource = true;
#endif
  if (http_proxy_is_enabled())
    t->net.fd = proxy_open_tunnel(slot->host, atoi(slot->port),
                                 remaining_ms(deadline, ROBOT_VOICE_HTTP_CONNECT_MS));
  else t->net.fd = tcp_connect(slot->host, slot->port, deadline, t, receive_bytes);
  if (t->net.fd < 0) { ret = t->net.fd; t->net.fd = -1; return ret; }
  ret = timeout_socket(t->net.fd, remaining_ms(deadline, ROBOT_VOICE_HTTP_CONNECT_MS));
  if (ret < 0) return ret;
  t->stage = "tcp-nonblock";
  int flags = fcntl(t->net.fd, F_GETFL, 0);
  if (flags < 0 || fcntl(t->net.fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return -errno;
  deadline = now_ms() + ROBOT_VOICE_HTTP_TLS_MS;
  if (deadline > request_deadline) deadline = request_deadline;
  t->io_deadline = deadline;
  t->stage = "tls-config";
  ret = mbedtls_ssl_config_defaults(&t->cfg, MBEDTLS_SSL_IS_CLIENT,
      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) { t->raw_error = ret; return -EIO; }
  /* Preserve the official board TLS version, ALPN, entropy and verification
   * policy. This adapter changes HTTP completion, not TLS credentials. */
  mbedtls_ssl_conf_min_tls_version(&t->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
  mbedtls_ssl_conf_max_tls_version(&t->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
  mbedtls_ssl_conf_max_tls_version(&t->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif
#if defined(MBEDTLS_SSL_ALPN)
  static const char *alpn[] = {"http/1.1", NULL};
  ret = mbedtls_ssl_conf_alpn_protocols(&t->cfg, alpn);
  if (ret != 0) { t->raw_error = ret; return -EIO; }
#endif
  if (ROBOT_VOICE_TLS_CA_FILE[0]) {
    t->stage = "tls-ca-load";
#if defined(MBEDTLS_X509_CRT_PARSE_C)
    int fd = open(ROBOT_VOICE_TLS_CA_FILE, O_RDONLY);
    if (fd < 0) { t->sys_error = errno; return -errno; }
    const size_t limit = 64 * 1024;
    unsigned char *pem = malloc(limit + 1);
    size_t used = 0;
    if (!pem) { close(fd); return -ENOMEM; }
    for (;;) {
      ssize_t n = read(fd, pem + used, limit + 1 - used);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) { ret = -errno; break; }
      if (!n) { ret = used ? 0 : -EPROTO; break; }
      used += (size_t)n;
      if (used > limit) { ret = -EOVERFLOW; break; }
    }
    close(fd);
    if (ret == 0) {
      pem[used] = 0;
      bool text = used >= 10 && !memcmp(pem, "-----BEGIN", 10);
      ret = mbedtls_x509_crt_parse(&t->ca, pem, used + (text ? 1 : 0));
      t->raw_error = ret;
      if (ret != 0) ret = -EACCES;
    }
    free(pem);
    if (ret != 0) return ret;
    mbedtls_ssl_conf_ca_chain(&t->cfg, &t->ca, NULL);
    mbedtls_ssl_conf_authmode(&t->cfg, MBEDTLS_SSL_VERIFY_REQUIRED);
#else
    return -ENOTSUP; /* Explicit strict configuration must never downgrade. */
#endif
  } else {
    mbedtls_ssl_conf_authmode(&t->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
  }
  mbedtls_ssl_conf_rng(&t->cfg, mbedtls_ctr_drbg_random, &t->random);
  t->stage = "tls-setup";
  ret = mbedtls_ssl_setup(&t->ssl, &t->cfg);
  if (ret != 0) { t->raw_error = ret; return -EIO; }
  t->stage = "tls-hostname";
  ret = mbedtls_ssl_set_hostname(&t->ssl, slot->host);
  if (ret != 0) { t->raw_error = ret; return -EIO; }
  mbedtls_ssl_set_bio(&t->ssl, t, rv_net_send, rv_net_recv, NULL);
  do {
    if (probe && (ret = probe(NULL, 0, arg)) < 0) return ret;
    unsigned int ms = remaining_ms(deadline, ROBOT_VOICE_HTTP_CONNECT_MS);
    if (!ms) return -ETIMEDOUT;
    ret = timeout_socket(t->net.fd, ms);
    if (ret < 0) return ret;
    t->stage = "tls-handshake"; errno = 0;
    ret = mbedtls_ssl_handshake(&t->ssl);
    t->raw_error = ret; t->sys_error = errno;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      int ready = wait_io(t, ret, deadline);
      if (ready < 0) return ready;
    }
  } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
  RV_VERBOSE("[RV-HTTP] TLS host=%s ms=%llu rc=%d\n", slot->host, now_ms() - start, ret);
  return ret == 0 ? 0 : -EIO;
}

/* Keep overlapping ASR/Flash/TTS handshakes from consuming scarce TCP/IOB
 * resources together. The gate ends at handshake completion; established
 * streams, ASR reception and TTS prefetch continue concurrently. */
static pthread_mutex_t tls_setup_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tls_setup_changed = PTHREAD_COND_INITIALIZER;
static bool tls_setup_busy;

static int tls_open(struct rv_connection *slot, unsigned long long deadline,
                    int receive_bytes, rv_http_entity_cb probe, void *arg)
{
  int ret = 0;
  pthread_mutex_lock(&tls_setup_lock);
  while (tls_setup_busy) {
    if (probe) {
      int cancelled = probe(NULL, 0, arg);
      if (cancelled < 0) { ret = cancelled; break; }
    }
    unsigned int ms = remaining_ms(deadline, 50);
    if (!ms) { ret = -ETIMEDOUT; break; }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)ms * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&tls_setup_changed, &tls_setup_lock, &until);
  }
  if (!ret && probe) {
    int cancelled = probe(NULL, 0, arg);
    if (cancelled < 0) ret = cancelled;
  }
  if (!ret) tls_setup_busy = true;
  pthread_mutex_unlock(&tls_setup_lock);
  if (ret < 0) return ret;
  ret = tls_open_inner(slot, deadline, receive_bytes, probe, arg);
  pthread_mutex_lock(&tls_setup_lock);
  tls_setup_busy = false;
  pthread_cond_broadcast(&tls_setup_changed);
  pthread_mutex_unlock(&tls_setup_lock);
  return ret;
}

static struct rv_connection *acquire(const char *host, const char *port,
                                     bool allow_reuse, bool *reused, unsigned long long deadline)
{
  struct rv_connection *slot = NULL;
  pid_t owner = getpid();
  pthread_mutex_lock(&pool_lock);
retry_slot:
  if (!network_recovering) {
  for (unsigned int i = 0; i < 4; i++) {
    if (!pool[i].busy && pool[i].tls && pool[i].owner == owner && !strcmp(pool[i].host, host) && !strcmp(pool[i].port, port)) {
      slot = &pool[i]; break;
    }
  }
  if (!slot) for (unsigned int i = 0; i < 4; i++) if (!pool[i].busy && !pool[i].tls) {
    slot = &pool[i]; break;
  }
  if (!slot) for (unsigned int i = 0; i < 4; i++) if (!pool[i].busy && pool[i].owner == owner) {
    slot = &pool[i]; break;
  }
  }
  if (!slot && remaining_ms(deadline, 1)) {
    /* ASR/Flash may temporarily occupy all slots. Sleep without allocating
     * TLS/IOB resources; the request's original deadline still applies. */
    struct timespec until;
    unsigned int ms = remaining_ms(deadline, 50);
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)ms * 1000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&pool_changed, &pool_lock, &until);
    goto retry_slot;
  }
  if (slot) {
    slot->busy = true;
    slot->owner = owner;
    slot->id = ++next_id;
    *reused = allow_reuse && slot->generation == network_generation &&
              slot->tls && !strcmp(slot->host, host) && !strcmp(slot->port, port) &&
              now_ms() - slot->last_used < 25000;
    slot->generation = network_generation;
  }
  pthread_mutex_unlock(&pool_lock);
  if (slot && !*reused) { tls_close(slot->tls); slot->tls = NULL; }
  if (slot) {
    snprintf(slot->host, sizeof(slot->host), "%s", host);
    snprintf(slot->port, sizeof(slot->port), "%s", port);
  }
  return slot;
}

static void release(struct rv_connection *slot, bool keep)
{
  /* Two idle connections maximum leaves capacity for transient task groups.
   * Never close another group's fd through the caller's descriptor table. */
  pthread_mutex_lock(&pool_lock);
  unsigned int idle = 0;
  for (unsigned int i = 0; i < 4; i++)
    if (&pool[i] != slot && !pool[i].busy && pool[i].tls) idle++;
  if (idle >= 2) keep = false;
  if (!keep) { tls_close(slot->tls); slot->tls = NULL; }
  slot->last_used = now_ms();
  slot->busy = false;
  pthread_cond_broadcast(&pool_changed);
  pthread_mutex_unlock(&pool_lock);
}

void robot_voice_http_close_idle(void)
{
  pthread_mutex_lock(&pool_lock);
  for (unsigned int i = 0; i < 4; i++) {
    if (!pool[i].busy && pool[i].tls && pool[i].owner == getpid()) {
      tls_close(pool[i].tls); pool[i].tls = NULL;
    }
  }
  pthread_mutex_unlock(&pool_lock);
}

bool robot_voice_http_network_recovery_begin(void)
{
  pthread_mutex_lock(&pool_lock);
  bool idle = !network_recovering;
  for (unsigned int i = 0; i < 4; i++)
    if (pool[i].busy) idle = false;
  if (idle) {
    network_recovering = true;
    network_generation++;
  }
  pthread_mutex_unlock(&pool_lock);
  /* Other task groups retire their own stale descriptors on their next use. */
  if (idle) {
    robot_voice_http_close_idle();
    if (vela_tls_pool_cleanup != NULL) vela_tls_pool_cleanup();
  }
  return idle;
}

void robot_voice_http_network_recovery_end(void)
{
  pthread_mutex_lock(&pool_lock);
  network_recovering = false;
  pthread_cond_broadcast(&pool_changed);
  pthread_mutex_unlock(&pool_lock);
}

static int write_bytes(struct rv_tls *t, const char *data, size_t len,
                       unsigned long long deadline, size_t *sent)
{
  while (len) {
    unsigned int ms = remaining_ms(deadline, ROBOT_VOICE_HTTP_IDLE_MS);
    int ret = timeout_socket(t->net.fd, ms);
    if (ret < 0) return ret;
    t->stage = "tls-write"; errno = 0;
    t->io_deadline = deadline;
    ret = mbedtls_ssl_write(&t->ssl, (const unsigned char *)data,
                             len > 4096 ? 4096 : len);
    t->raw_error = ret; t->sys_error = errno;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      ret = wait_io(t, ret, deadline);
      if (ret < 0) return ret;
      continue;
    }
    if (ret <= 0) {
      t->raw_error = ret; t->sys_error = errno;
      return remaining_ms(deadline, 1) ? -EIO : -ETIMEDOUT;
    }
    data += ret;
    len -= (size_t)ret;
    *sent += (size_t)ret;
  }
  return 0;
}

struct rv_response {
  struct rv_http_parser *parser;
  char *buffer;
  size_t capacity, used;
  rv_http_entity_cb callback;
  void *arg;
  unsigned long long start, first, upload;
  unsigned int id;
};

/* Log a short, printable server error body for LLM requests only. This is
 * response data, never the request (which may contain user text or secrets).
 * Control bytes are replaced so a provider message cannot forge log lines. */
static void rv_log_llm_error_body(int status, const char *response,
                                  size_t capacity)
{
  char preview[193];
  size_t len;
  size_t i;
  size_t used = 0;

  if (response == NULL || capacity == 0)
    {
      printf("[RV-LLM-ERROR] http_status=%d body_bytes=0 body_preview=<empty>\n",
             status);
      return;
    }

  len = strnlen(response, capacity);
  for (i = 0; i < len && used < sizeof(preview) - 1; i++)
    {
      unsigned char ch = (unsigned char)response[i];
      preview[used++] = (ch >= 0x20 && ch != 0x7f) ? (char)ch : ' ';
    }
  preview[used] = '\0';

  printf("[RV-LLM-ERROR] http_status=%d body_bytes=%zu preview_bytes=%zu "
         "truncated=%u body_preview=\"%s\"\n",
         status, len, used, len > used, preview);
}

static int entity(const char *data, size_t len, void *arg)
{
  struct rv_response *r = arg;
  if (!r->first) {
    r->first = now_ms();
    RV_VERBOSE("[RV-HTTP] id=%u first_body_ms=%llu status=%d framing=%s\n",
           r->id, r->first - r->start, r->parser->status, r->parser->framing);
  }
  if (r->callback) {
    /* Never feed an HTTP error document to a PCM/SSE consumer. */
    if (r->parser->status < 200 || r->parser->status >= 300) return 0;
    return r->callback(data, len, r->arg);
  }
  if (len > r->capacity - 1 - r->used) return -EOVERFLOW;
  memcpy(r->buffer + r->used, data, len);
  r->used += len;
  r->buffer[r->used] = 0;
  return 0;
}

static int robot_voice_http_request_locked(const char *host, const char *port,
  const char *method, const char *path, const vela_header_t *headers,
  const char *body, size_t body_len, char *response, size_t capacity,
  rv_http_entity_cb callback, void *arg, unsigned int timeout_ms)
{
  struct rv_connection *slot;
  struct rv_http_parser *parser = NULL;
  struct rv_response sink = {0};
  bool reused = false, keep = false;
  size_t sent = 0;
  unsigned int attempts = 0;
  char *request = NULL, *input = NULL;
  size_t used = 0;
  unsigned long long start = now_ms(), deadline = start + timeout_ms;
  unsigned long long last_read = 0, read_gap = 0, parse_ms = 0, poll_ms = 0;
  size_t read_bytes = 0;
  unsigned long long ssl_read_ms = 0, ssl_read_max_ms = 0;
  unsigned int read_calls = 0;
  int ret = -EINVAL;
  if (!host || !port || !method || !path || !timeout_ms ||
      strlen(host) >= sizeof(pool[0].host) || strlen(port) >= sizeof(pool[0].port) ||
      strchr(host, '\r') || strchr(host, '\n') || strchr(path, '\r') || strchr(path, '\n') ||
      (strcmp(method, "GET") && strcmp(method, "POST")) ||
      (!callback && (!response || capacity < 2)) || (!body && body_len) ||
      body_len > ROBOT_VOICE_MAX_BODY || path[0] != '/' ||
      strpbrk(path, " \t") || strpbrk(host, " /\t") || strpbrk(port, "\r\n \t")) return -EINVAL;
  if (response && capacity) response[0] = 0;
  const char *kind = !strcmp(method, "GET") ? "weather" :
    (body && strstr(body, "\"model\":\"" ROBOT_VOICE_ASR_MODEL "\"") != NULL ? "ASR" :
     (body && strstr(body, "\"model\":\"" ROBOT_VOICE_TTS_MODEL "\"") != NULL ? "TTS" : "LLM"));
  bool model_chat = body && !strcmp(kind, "LLM") && strstr(body, "\"model\":\"mimo-v2.");
  bool allow_reuse = !model_chat || ROBOT_VOICE_HTTP_LLM_REUSE;
  rv_http_entity_cb probe = callback &&
    (!strcmp(kind, "ASR") || !strcmp(kind, "LLM")) ? callback : NULL;
  if (probe && (ret = probe(NULL, 0, arg)) < 0) return ret;
  slot = acquire(host, port, allow_reuse, &reused, deadline);
  if (!slot) return -ETIMEDOUT;
  parser = calloc(1, sizeof(*parser));
  request = malloc(8192);
  input = malloc(8192);
  if (!parser || !request || !input) { ret = -ENOMEM; goto out; }
  memset(&sink, 0, sizeof(sink));
  sink.parser = parser; sink.buffer = response; sink.capacity = capacity;
  sink.callback = callback; sink.arg = arg; sink.start = start;
  sink.id = slot->id;
#define APPEND(...) do { \
  int n = snprintf(request + used, 8192 - used, __VA_ARGS__); \
  if (n < 0 || (size_t)n >= 8192 - used) {ret = -EOVERFLOW; goto out;} \
  used += (size_t)n; \
} while (0)
  APPEND("%s %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\nAccept-Encoding: identity\r\nAccept: %s\r\n", method, path, host,
         callback ? "text/event-stream" : "application/json");
  if (!strcmp(method, "POST")) APPEND("Content-Type: application/json\r\nContent-Length: %zu\r\n", body_len);
  if (headers) {
    unsigned int count;
    for (count = 0; count < 32 && headers[count].name; count++) {
      const char *name = headers[count].name, *value = headers[count].value;
      if (!value || !name[0] || strspn(name, "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != strlen(name) ||
          strchr(value, '\r') || strchr(value, '\n')) { ret = -EINVAL; goto out; }
      if (!strcasecmp(name, "Content-Length") || !strcasecmp(name, "Content-Type") ||
          !strcasecmp(name, "Connection") || !strcasecmp(name, "Host") ||
          !strcasecmp(name, "Transfer-Encoding") || !strcasecmp(name, "Accept-Encoding") ||
          !strcasecmp(name, "Accept")) continue;
      APPEND("%s: %s\r\n", name, value);
    }
    if (count == 32) { ret = -EOVERFLOW; goto out; }
  }
  APPEND("\r\n");
#undef APPEND
  RV_VERBOSE("[RV-HTTP] id=%u begin kind=%s host=%s request=%zu reused=%u owner=%ld budget_ms=%u io=nonblock\n", slot->id, kind, host, body_len, reused, (long)slot->owner, timeout_ms);
  for (unsigned int attempt = 0; attempt < 2; attempt++) {
    attempts++;
    unsigned long long read_deadline = deadline;
    rv_http_parser_init(parser, entity, &sink);
    if (!slot->tls) {
      ret = tls_open(slot, deadline, receive_buffer_target(kind), probe, arg);
      if (ret < 0) {
        /* The POST has not been sent: one reconnect is safe. Certificate,
         * protocol, memory and configuration failures are never retried. */
        if (!attempt && !sent && remaining_ms(deadline, 1) &&
            (ret == -ETIMEDOUT || ret == -ECONNRESET || ret == -ECONNABORTED ||
             ret == -EHOSTUNREACH || ret == -ENETUNREACH)) {
          RV_EVENT("[连接重试] kind=%s stage=%s rc=%d 尚未发送请求，重建TCP/TLS连接一次（不重启Wi-Fi）\n",
            kind, slot->tls && slot->tls->stage ? slot->tls->stage : "connect", ret);
          tls_close(slot->tls); slot->tls = NULL;
          /* A handshake timeout can leave another idle voice connection
           * holding stale TCP/TLS state or scarce socket resources.  Retire
           * those idle slots before the bounded reconnect; the current slot
           * is busy and is already closed above. */
          robot_voice_http_close_idle();
          reused = false;
          unsigned int delay = remaining_ms(deadline, ROBOT_VOICE_HTTP_CONNECT_RETRY_DELAY_MS);
          struct timespec nap = {.tv_sec = delay / 1000,
                                 .tv_nsec = (long)(delay % 1000) * 1000000L};
          while (nanosleep(&nap, &nap) < 0 && errno == EINTR) {}
          if (!remaining_ms(deadline, 1)) { ret = -ETIMEDOUT; goto out; }
          continue;
        }
        goto out;
      }
    }
    slot->tls->cancel_probe = probe;
    slot->tls->cancel_arg = arg;
    if (slot->tls->cancel_probe) {
      ret = slot->tls->cancel_probe(NULL, 0, arg);
      if (ret < 0) goto out;
    }
    if (reused || http_proxy_is_enabled())
      tune_receive_buffer(slot->tls->net.fd, receive_buffer_target(kind));
    slot->tls->bio_reads = slot->tls->bio_eagain = 0;
    slot->tls->bio_bytes = 0;
    slot->tls->poll_ready = slot->tls->poll_timeouts = 0;
    ret = write_bytes(slot->tls, request, used, deadline, &sent);
    if (ret >= 0 && body_len) ret = write_bytes(slot->tls, body, body_len, deadline, &sent);
    if (ret >= 0) {
      sink.upload = now_ms();
      RV_VERBOSE("[RV-HTTP] id=%u upload_done_ms=%llu\n", slot->id, sink.upload - start);
    }
    while (ret >= 0) {
      if (slot->tls->cancel_probe) {
        ret = slot->tls->cancel_probe(NULL, 0, arg);
        if (ret < 0) break;
      }
      if (!remaining_ms(read_deadline, 1)) { ret = -ETIMEDOUT; break; }
      slot->tls->stage = "tls-read"; errno = 0;
      slot->tls->io_deadline = read_deadline;
      unsigned long long ssl_at = now_ms();
      int n = mbedtls_ssl_read(&slot->tls->ssl, (unsigned char *)input, 8192);
      int ssl_errno = errno;
      unsigned long long ssl_elapsed = now_ms() - ssl_at;
      ssl_read_ms += ssl_elapsed;
      if (ssl_elapsed > ssl_read_max_ms) ssl_read_max_ms = ssl_elapsed;
      slot->tls->raw_error = n; slot->tls->sys_error = ssl_errno;
      if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
        unsigned long long poll_start = now_ms();
        ret = wait_io(slot->tls, n, read_deadline);
        poll_ms += now_ms() - poll_start;
        if (ret < 0) break;
        continue;
      }
      if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) ret = rv_http_parser_eof(parser);
      else if (n < 0) ret = !remaining_ms(deadline, 1) ? -ETIMEDOUT : -EIO;
      else {
        unsigned long long read_at = now_ms();
        if (last_read && read_at - last_read > read_gap) read_gap = read_at - last_read;
        last_read = read_at; read_calls++; read_bytes += (size_t)n;
        /* A negative callback result is an application protocol error, not a
         * TLS read failure. Preserve that distinction in diagnostics so an
         * SSE/schema mismatch does not look like a Wi-Fi outage. */
        slot->tls->stage = "http-parse";
        ret = rv_http_parser_feed(parser, input, (size_t)n);
        parse_ms += now_ms() - read_at;
        /* LLM may emit headers early while generating a non-streamed reply.
         * Its entire wait uses the total budget, not an 8-second gap timer. */
        if (strcmp(kind, "LLM")) {
          read_deadline = now_ms() + ROBOT_VOICE_HTTP_IDLE_MS;
          if (read_deadline > deadline) read_deadline = deadline;
        }
      }
      if (ret != 0) break;
    }
    if (ret > 0) { keep = parser->keep_alive && allow_reuse; ret = parser->status; break; }
    /* Once any bytes are accepted by TLS the server may execute the POST.
     * Do not replay a possibly delivered request or a partial response. */
    if (!reused || attempt || sent || ret == -ETIMEDOUT || parser->used || parser->bytes || parser->status) break;
    tls_close(slot->tls); slot->tls = NULL; reused = false;
    RV_VERBOSE("[RV-HTTP] stale idle connection; reconnect once within original budget\n");
  }
out:
  if (ret < 0 && ret != -ECANCELED && slot->tls) {
    printf("[RV-NET-ERROR] id=%u owner=%ld stage=%s rc=%d raw=%d raw_hex=%08x errno=%d fd=%d kind=%s sent=%zu attempts=%u reused=%u elapsed_ms=%llu\n",
      slot->id, (long)slot->owner, slot->tls->stage ? slot->tls->stage : "unknown", ret,
      slot->tls->raw_error, (unsigned int)slot->tls->raw_error,
      slot->tls->sys_error, slot->tls->net.fd, kind, sent, attempts, reused, now_ms() - start);
    robot_voice_http_network_diag();
  }
  if (slot->tls && (ret < 0 || slot->tls->bio_write_eagain))
    RV_EVENT("[RV-NET-TX] id=%u mode=%s calls=%u bytes=%zu max_offer=%zu "
             "eagain=%u backoffs=%u last_want=%s\n",
      slot->id, ROBOT_VOICE_TCP_TIMED_SEND ? "timed-send" : "nonblock",
      slot->tls->bio_writes, slot->tls->bio_sent, slot->tls->bio_send_max,
      slot->tls->bio_write_eagain, slot->tls->io_backoffs,
      slot->tls->raw_error == MBEDTLS_ERR_SSL_WANT_WRITE ? "write" :
      (slot->tls->raw_error == MBEDTLS_ERR_SSL_WANT_READ ? "read" : "none"));
  RV_VERBOSE("[RV-HTTP] id=%u done host=%s status=%d entity=%zu framing=%s complete=%u total_ms=%llu upload_ms=%llu wait_body_ms=%llu after_body_ms=%llu sent=%zu attempts=%u\n",
      slot->id, host, ret, parser ? parser->bytes : 0,
      parser && parser->framing ? parser->framing : "none",
      parser && parser->state == RV_HTTP_DONE, now_ms() - start,
      sink.upload ? sink.upload - start : 0,
      sink.first && sink.upload && sink.first >= sink.upload ? sink.first - sink.upload : 0,
      sink.first ? now_ms() - sink.first : 0, sent, attempts);
  if (!strcmp(kind, "TTS") || !strcmp(kind, "LLM"))
    RV_EVENT("[耗时][网络] kind=%s id=%u 建连及上传=%lldms 上传到首响应内容=%lldms 完整请求=%lldms body=%zuB rc=%d\n",
      kind, slot->id, rv_elapsed_ms(start, sink.upload), rv_elapsed_ms(sink.upload, sink.first),
      rv_elapsed_ms(start, now_ms()), body_len, ret);
  if (!strcmp(kind, "TTS")) {
    int rcvbuf = 0;
    socklen_t length = sizeof(rcvbuf);
    if (!slot->tls || slot->tls->net.fd < 0 ||
        getsockopt(slot->tls->net.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &length) < 0) rcvbuf = -1;
    RV_EVENT("[网络接收] kind=TTS id=%u 读取=%u次/%zuB 最大读取间隔=%llums 解析及回调=%llums poll等待=%llums 接收窗口=%dB\n",
      slot->id, read_calls, read_bytes, read_gap, parse_ms, poll_ms, rcvbuf);
    RV_EVENT("[RV-NET-RX] id=%u ssl_read_ms=%llu ssl_read_max_ms=%llu "
             "bio_reads=%u bio_bytes=%zu eagain=%u poll_ready=%u poll_timeouts=%u "
             "avg_http_Bps=%llu\n",
      slot->id, ssl_read_ms, ssl_read_max_ms,
      slot->tls ? slot->tls->bio_reads : 0,
      slot->tls ? slot->tls->bio_bytes : 0,
      slot->tls ? slot->tls->bio_eagain : 0,
      slot->tls ? slot->tls->poll_ready : 0,
      slot->tls ? slot->tls->poll_timeouts : 0,
      now_ms() > sink.upload && sink.upload ?
        (unsigned long long)read_bytes * 1000 / (now_ms() - sink.upload) : 0);
  }
  if (!strcmp(kind, "LLM") && (ret < 0 || ret >= 300))
    {
      RV_VERBOSE("[RV-LLM-DIAG] outcome=%s rc=%d http_status=%d entity_bytes=%zu "
             "complete=%u total_ms=%llu stage=%s tls_raw=%d errno=%d\n",
             ret < 0 ? "transport_or_parse_error" : "http_error", ret,
             parser ? parser->status : 0, parser ? parser->bytes : 0,
             parser && parser->state == RV_HTTP_DONE,
             now_ms() - start,
             slot->tls && slot->tls->stage ? slot->tls->stage : "none",
             slot->tls ? slot->tls->raw_error : 0,
             slot->tls ? slot->tls->sys_error : 0);
      if (ret >= 300 && !callback)
        rv_log_llm_error_body(ret, response, capacity);
    }
  int parser_status = parser ? parser->status : 0;
  if (ret < 0 && response && capacity) response[0] = 0; /* no partial JSON */
  if (request) { memset(request, 0, 8192); free(request); }
  free(input); free(parser);
  /* A timed-out TLS upload with no HTTP status is also a broken transport.
   * The request may have partially filled the TCP send window, so checking
   * sent == 0 alone misses the recurring post-ASR failure seen on ESP32-S3. */
  bool resource_pressure = ret == -ENOMEM || ret == -ENOBUFS ||
    ret == -EMFILE || ret == -ENFILE || ret == -EAGAIN;
  bool transport_failure = !resource_pressure && ret < 0 && slot->tls && slot->tls->stage &&
    ((!strcmp(slot->tls->stage, "dns") ||
      !strncmp(slot->tls->stage, "tcp-", 4)) ||
     !strcmp(slot->tls->stage, "tls-handshake") ||
     (!strcmp(slot->tls->stage, "tls-write") &&
      parser_status == 0));
  if (ret != -ECANCELED)
    robot_voice_http_network_result(ret, transport_failure);
  release(slot, keep && ret >= 200 && !callback && strcmp(method, "GET"));
  if (resource_pressure) {
    RV_EVENT("[RV-NET-MEM] rc=%d; retired idle TLS pools, no Wi-Fi reset\n", ret);
    robot_voice_http_close_idle();
    if (vela_tls_pool_cleanup != NULL) vela_tls_pool_cleanup();
  }
  return ret;
}

int robot_voice_http_request(const char *host, const char *port,
  const char *method, const char *path, const vela_header_t *headers,
  const char *body, size_t body_len, char *response, size_t capacity,
  rv_http_entity_cb callback, void *arg, unsigned int timeout_ms)
{
  int ret = network_acquire_http_transport(timeout_ms);
  if (ret < 0)
    {
      RV_EVENT("[RV-NET-SERIAL] HTTPS transport busy rc=%d\n", ret);
      return ret;
    }

  ret = robot_voice_http_request_locked(host, port, method, path, headers,
                                        body, body_len, response, capacity,
                                        callback, arg, timeout_ms);
  network_release_http_transport();
  return ret;
}

static bool mimo_host(const char *host)
{
  return host && (!strcmp(host, ROBOT_VOICE_HOST) || !strcmp(host, "api.xiaomimimo.com"));
}

/* The voice app overrides this notification. Transport failures remain real
 * failures; no synthetic successful model response is fabricated. */
__attribute__((weak)) void robot_voice_http_on_agent_error(const char *body, int status)
{
  (void)body; (void)status;
}

__attribute__((weak)) void robot_voice_http_on_agent_error_for_token(
    const char *body, int status, unsigned long token)
{ (void)token; robot_voice_http_on_agent_error(body, status); }

int robot_voice_http_post_json_timeout(const char *host, const char *port,
  const char *path, const vela_header_t *headers, const char *body,
  char *response, size_t cap, unsigned int timeout_ms)
{
  char *prepared = NULL;
  unsigned long request_token = 0;
  int ret;
  unsigned int timeout = ROBOT_VOICE_HTTP_LLM_MS;
  if (!body || !host || !path) return -EINVAL;
  if (!mimo_host(host)) {
    /* Leave non-MiMo service policy/transport with the official stack. */
    vela_header_t merged[34];
    unsigned int n = 1;
    merged[0] = (vela_header_t){"Content-Type", "application/json"};
    if (headers) while (headers[n - 1].name && n < 33) { merged[n] = headers[n - 1]; n++; }
    merged[n] = (vela_header_t){NULL, NULL};
    return vela_https_request(host, port, "POST", path, merged, body, strlen(body), response, cap, NULL);
  }
  if (strncmp(body, "{\"model\":\"" ROBOT_VOICE_ASR_MODEL "\"", strlen("{\"model\":\"" ROBOT_VOICE_ASR_MODEL "\"")) == 0)
    timeout = ROBOT_VOICE_HTTP_ASR_MS;
  else if (strncmp(body, "{\"model\":\"" ROBOT_VOICE_TTS_MODEL "\"", strlen("{\"model\":\"" ROBOT_VOICE_TTS_MODEL "\"")) == 0)
    timeout = ROBOT_VOICE_HTTP_TTS_MS;
  ret = robot_mimo_prepare_llm_body(body, &prepared);
  if (ret >= 0 && prepared) {
    request_token = robot_voice_llm_request_token ? robot_voice_llm_request_token(body) : 0;
    ret = mimo_llm_stream_request_timeout(host, port, path, headers, prepared,
      response, cap, timeout_ms ? timeout_ms : timeout);
  } else if (ret >= 0)
    ret = robot_voice_http_request(host, port, "POST", path, headers,
        body, strlen(body), response, cap, NULL, NULL,
        timeout_ms ? timeout_ms : timeout);
  if (prepared && (ret < 0 || ret >= 300))
    robot_voice_http_on_agent_error_for_token(body, ret, request_token);
  free(prepared);
  return ret;
}

int robot_voice_http_post_json(const char *host, const char *port, const char *path,
  const vela_header_t *headers, const char *body, char *response, size_t cap)
{
  return robot_voice_http_post_json_timeout(host, port, path, headers, body,
                                            response, cap, 0);
}

#ifdef ROBOT_VOICE_AGENT_INTERPOSE
/* Strong contest definition; official definition is weak only in the build
 * output, via robot_voice_agent_hooks.h. No official file is changed. */
int vela_https_post_json(const char *host, const char *port, const char *path,
  const vela_header_t *headers, const char *body, char *response, size_t cap)
{
  return robot_voice_http_post_json(host, port, path, headers, body, response, cap);
}
#endif

void robot_voice_http_link_anchor(void)
{
  RV_EVENT("[RV-STREAM] Flash=incremental TTS_mode=%s interrupt=%s workers=%d phrase_target=%d phrase_max=%d merge_wait=%dms prebuffer=%uB\n",
    ROBOT_VOICE_TTS_LIVE ? "live-stream" :
      (ROBOT_VOICE_TTS_CONTINUITY ? "validated-turn-cache" : "sentence-cache/prefetch-next"),
    "asr-before-flash-v4",
    ROBOT_VOICE_TTS_WORKERS,
    ROBOT_VOICE_TTS_CHUNK_TARGET_BYTES, ROBOT_VOICE_TTS_SENTENCE_BYTES,
    ROBOT_VOICE_TTS_CHUNK_TARGET_BYTES > 1 ? ROBOT_VOICE_TTS_CHUNK_MERGE_WAIT_MS : 0,
    (unsigned int)ROBOT_VOICE_TTS_PREBUFFER);
#if defined(CONFIG_IOB_NBUFFERS) && defined(CONFIG_IOB_BUFSIZE)
  RV_EVENT("[RV-NET-CONFIG] IOB=%d*%d recv_buf=%d delayed_ack=%d\n",
    CONFIG_IOB_NBUFFERS, CONFIG_IOB_BUFSIZE,
#ifdef CONFIG_NET_RECV_BUFSIZE
    CONFIG_NET_RECV_BUFSIZE,
#else
    0,
#endif
#ifdef CONFIG_NET_TCP_DELAYED_ACK
    1
#else
    0
#endif
  );
#endif
  RV_EVENT("[RV-TCP-POLICY] connect=%s busy_retries=%d\n",
           ROBOT_VOICE_TCP_BLOCKING_CONNECT ? "timed-blocking" : "posix-nonblock",
           ROBOT_VOICE_TCP_BUSY_RETRIES);
  RV_EVENT("[RV-TLS-POLICY] tx=%s chunk=%u backoff_ms=%u setup_gate=1\n",
           ROBOT_VOICE_TCP_TIMED_SEND ? "timed-send" : "nonblock",
           (unsigned)ROBOT_VOICE_TCP_SEND_CHUNK, (unsigned)ROBOT_VOICE_HTTP_BACKOFF_MS);
  RV_VERBOSE("[RV-HTTP] transport=contest-framed-http pool=4 Agent_hook=%s\n",
#ifdef ROBOT_VOICE_AGENT_INTERPOSE
         "enabled"
#else
         "disabled"
#endif
  );
  RV_VERBOSE("[RV-TLS] certificate_policy=%s\n", ROBOT_VOICE_TLS_CA_FILE[0] ?
         "required-configured-CA" : "legacy-optional-no-CA");
}
