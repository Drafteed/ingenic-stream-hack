#include "main.h"

#define DEFAULT_LIBIMP_PATH "/usr/lib/libimp.so"
#define DEFAULT_LOG_PATH "/mnt/extsd/stream_hack.log"
#define DEFAULT_PORT 12345
#define LISTEN_BACKLOG 4
#define DEFAULT_STREAM_CHANNEL 0
#define DEFAULT_MAX_DROP_STREAK 90
#define MAX_PACK_COUNT 1024U
#define MAX_PACK_BUFFER (256U * 1024U)

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
  SEND_OK = 0,
  SEND_WOULD_BLOCK = 1,
  SEND_NOT_AVCC = 2,
  SEND_FATAL = -1,
};

typedef int (*imp_encoder_get_stream_fn)(int, IMPEncoderStream*, bool);
typedef int (*imp_encoder_request_idr_fn)(int);

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;

static void* g_real_lib = NULL;
static imp_encoder_get_stream_fn g_real_get_stream = NULL;
static imp_encoder_request_idr_fn g_real_request_idr = NULL;

static pthread_t g_server_thread;
static int g_server_thread_started = 0;
static int g_server_stop = 0;
static int g_server_fd = -1;
static int g_client_fd = -1;

static int g_stream_channel = DEFAULT_STREAM_CHANNEL;
static int g_drop_streak = 0;
static int g_max_drop_streak = DEFAULT_MAX_DROP_STREAK;
static int g_pending_idr_request = 0;
static int g_wait_keyframe = 1;

static int g_log_fd = -1;
static uint8_t g_pack_buffer[MAX_PACK_BUFFER];

static void hack_log(const char* fmt, ...) {
  if (g_log_fd < 0) {
    return;
  }

  char msg[512];
  char line[768];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }

  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
  }

  struct tm tmv;
  if (localtime_r(&ts.tv_sec, &tmv) == NULL) {
    memset(&tmv, 0, sizeof(tmv));
  }

  int m = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03ld %s\n", tmv.tm_year + 1900, tmv.tm_mon + 1,
                   tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000L, msg);
  if (m <= 0) {
    return;
  }
  if ((size_t)m > sizeof(line)) {
    m = (int)sizeof(line);
  }

  (void)write(g_log_fd, line, (size_t)m);
}

static int parse_env_int(const char* name, int default_value, long min_value, long max_value) {
  const char* raw = getenv(name);
  if (raw == NULL || raw[0] == '\0') {
    return default_value;
  }

  char* end = NULL;
  long parsed = strtol(raw, &end, 10);
  if (end == raw || *end != '\0' || parsed < min_value || parsed > max_value) {
    hack_log("invalid %s='%s', using default=%d", name, raw, default_value);
    return default_value;
  }

  return (int)parsed;
}

static void init_logging(void) {
  const char* raw = getenv("STREAM_HACK_LOG");
  if (raw == NULL || raw[0] == '\0' || (raw[0] == '0' && raw[1] == '\0')) {
    return;
  }

  const char* path = raw;
  if (raw[0] == '1' && raw[1] == '\0') {
    path = DEFAULT_LOG_PATH;
  }

  g_log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (g_log_fd < 0) {
    fprintf(stderr, "[bmstream] failed to open log '%s': %s\n", path, strerror(errno));
    return;
  }

  hack_log("file logging enabled at %s", path);
}

static void resolve_real_symbols(void) {
  const char* lib_path = getenv("STREAM_HACK_LIBIMP");
  if (lib_path == NULL || lib_path[0] == '\0') {
    lib_path = DEFAULT_LIBIMP_PATH;
  }

  g_real_lib = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
  if (g_real_lib == NULL) {
    hack_log("dlopen(%s) failed: %s", lib_path, dlerror());
    return;
  }

  g_real_get_stream = (imp_encoder_get_stream_fn)dlsym(g_real_lib, "IMP_Encoder_GetStream");
  g_real_request_idr = (imp_encoder_request_idr_fn)dlsym(g_real_lib, "IMP_Encoder_RequestIDR");

  hack_log("resolved IMP symbols from %s (get_stream=%p request_idr=%p)", lib_path, (void*)g_real_get_stream,
           (void*)g_real_request_idr);
}

static int make_socket_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return -1;
  }
  return 0;
}

static int configure_client_socket(int fd) {
  int one = 1;
  int sndbuf = 512 * 1024;

  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
    hack_log("setsockopt(TCP_NODELAY) failed: %s", strerror(errno));
  }
  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) != 0) {
    hack_log("setsockopt(SO_KEEPALIVE) failed: %s", strerror(errno));
  }
  if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) != 0) {
    hack_log("setsockopt(SO_SNDBUF) failed: %s", strerror(errno));
  }
  if (make_socket_nonblocking(fd) != 0) {
    hack_log("failed to set client socket nonblocking: %s", strerror(errno));
    return -1;
  }

  return 0;
}

static void close_client_locked(const char* reason) {
  if (g_client_fd < 0) {
    return;
  }

  int fd = g_client_fd;
  g_client_fd = -1;
  g_drop_streak = 0;
  g_pending_idr_request = 0;

  shutdown(fd, SHUT_RDWR);
  close(fd);

  if (reason != NULL && reason[0] != '\0') {
    hack_log("client disconnected (%s)", reason);
  }
}

static void set_client_locked(int new_fd, const char* host, unsigned port) {
  if (g_client_fd >= 0) {
    int old_fd = g_client_fd;
    g_client_fd = -1;
    shutdown(old_fd, SHUT_RDWR);
    close(old_fd);
  }

  g_client_fd = new_fd;
  g_drop_streak = 0;
  g_pending_idr_request = 1;
  g_wait_keyframe = 1;
  hack_log("client connected: %s:%u", host, port);
}

static void handle_would_block_locked(void) {
  g_drop_streak++;
  g_pending_idr_request = 1;
  g_wait_keyframe = 1;

  if (g_drop_streak == 1 || (g_drop_streak % 30) == 0) {
    hack_log("socket busy, dropped frame (streak=%d)", g_drop_streak);
  }
  if (g_drop_streak >= g_max_drop_streak) {
    close_client_locked("client is too slow");
  }
}

static int send_buffer_locked(const uint8_t* data, size_t len) {
  if (g_client_fd < 0 || len == 0) {
    return SEND_OK;
  }

  size_t off = 0;
  while (off < len) {
    ssize_t n = send(g_client_fd, data + off, len - off, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (off > 0) {
        hack_log("send would-block after %zu/%zu bytes (dropping tail)", off, len);
      }
      return SEND_WOULD_BLOCK;
    }
    if (n == 0) {
      hack_log("send returned 0");
    } else {
      hack_log("send failed: %s", strerror(errno));
    }
    return SEND_FATAL;
  }

  return SEND_OK;
}

static int send_ring_slice_locked(const uint8_t* base, uint32_t stream_size, uint32_t offset, uint32_t len) {
  uint32_t first = stream_size - offset;
  if (first >= len) {
    return send_buffer_locked(base + offset, len);
  }

  int st = send_buffer_locked(base + offset, first);
  if (st != SEND_OK) {
    return st;
  }
  return send_buffer_locked(base, len - first);
}

static void copy_ring_slice(const uint8_t* base, uint32_t stream_size, uint32_t offset, uint8_t* dst, uint32_t len) {
  uint32_t first = stream_size - offset;
  if (first >= len) {
    memcpy(dst, base + offset, len);
    return;
  }

  memcpy(dst, base + offset, first);
  memcpy(dst + first, base, len - first);
}

static int ring_has_start_code(const uint8_t* base, uint32_t stream_size, uint32_t offset, uint32_t len) {
  if (len < 4U) {
    return 0;
  }

  uint32_t p0 = offset;
  uint32_t p1 = (offset + 1U) % stream_size;
  uint32_t p2 = (offset + 2U) % stream_size;
  uint32_t p3 = (offset + 3U) % stream_size;

  uint8_t b0 = base[p0];
  uint8_t b1 = base[p1];
  uint8_t b2 = base[p2];
  uint8_t b3 = base[p3];

  if (b0 == 0 && b1 == 0 && b2 == 1) {
    return 1;
  }
  if (b0 == 0 && b1 == 0 && b2 == 0 && b3 == 1) {
    return 1;
  }
  return 0;
}

static uint32_t be32_to_u32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int send_pack_avcc_as_annexb_locked(const uint8_t* pack_data, uint32_t pack_len) {
  static const uint8_t start_code[4] = {0, 0, 0, 1};

  uint32_t pos = 0;
  int converted = 0;
  while (pos + 4U <= pack_len) {
    uint32_t nal_len = be32_to_u32(pack_data + pos);
    pos += 4U;
    if (nal_len == 0 || pos + nal_len > pack_len) {
      return SEND_NOT_AVCC;
    }

    int st = send_buffer_locked(start_code, sizeof(start_code));
    if (st != SEND_OK) {
      return st;
    }
    st = send_buffer_locked(pack_data + pos, nal_len);
    if (st != SEND_OK) {
      return st;
    }

    pos += nal_len;
    converted = 1;
  }

  if (!converted || pos != pack_len) {
    return SEND_NOT_AVCC;
  }
  return SEND_OK;
}

static int pack_has_keyframe(const IMPEncoderPack* pack) {
  int h264_type = (int)pack->nalType.h264NalType;
  int h265_type = (int)pack->nalType.h265NalType;

  if (h264_type == IMP_H264_NAL_SLICE_IDR) {
    return 1;
  }
  if (h265_type == IMP_H265_NAL_SLICE_IDR_W_RADL || h265_type == IMP_H265_NAL_SLICE_IDR_N_LP ||
      h265_type == IMP_H265_NAL_SLICE_CRA) {
    return 1;
  }
  return 0;
}

static int stream_has_keyframe(const IMPEncoderStream* stream) {
  for (uint32_t i = 0; i < stream->packCount; ++i) {
    if (pack_has_keyframe(&stream->pack[i])) {
      return 1;
    }
  }
  return 0;
}

static int send_stream_locked(const IMPEncoderStream* stream) {
  if (stream == NULL || stream->pack == NULL) {
    return SEND_FATAL;
  }
  if (stream->packCount == 0) {
    return SEND_OK;
  }
  if (stream->packCount > MAX_PACK_COUNT) {
    hack_log("invalid packCount=%u", stream->packCount);
    return SEND_FATAL;
  }
  if (stream->streamSize == 0 || stream->virAddr == 0) {
    hack_log("invalid stream buffer addr=0x%08x size=%u", stream->virAddr, stream->streamSize);
    return SEND_FATAL;
  }

  const uint8_t* base = (const uint8_t*)(uintptr_t)stream->virAddr;
  uint32_t stream_size = stream->streamSize;

  for (uint32_t i = 0; i < stream->packCount; ++i) {
    uint32_t offset = stream->pack[i].offset;
    uint32_t len = stream->pack[i].length;

    if (len == 0) {
      continue;
    }
    if (offset >= stream_size || len > stream_size) {
      hack_log("invalid pack[%u]: offset=%u len=%u stream_size=%u", i, offset, len, stream_size);
      return SEND_FATAL;
    }

    int st = SEND_OK;
    if (ring_has_start_code(base, stream_size, offset, len)) {
      st = send_ring_slice_locked(base, stream_size, offset, len);
      if (st != SEND_OK) {
        return st;
      }
      continue;
    }

    if (len <= MAX_PACK_BUFFER) {
      copy_ring_slice(base, stream_size, offset, g_pack_buffer, len);
      st = send_pack_avcc_as_annexb_locked(g_pack_buffer, len);
      if (st == SEND_OK) {
        continue;
      }
      if (st == SEND_WOULD_BLOCK || st == SEND_FATAL) {
        return st;
      }
    }

    /* Fallback: send original bytes if packet is neither Annex-B nor AVCC. */
    st = send_ring_slice_locked(base, stream_size, offset, len);
    if (st != SEND_OK) {
      return st;
    }
  }

  return SEND_OK;
}

static void process_stream_locked(int encChn, IMPEncoderStream* stream) {
  if (g_client_fd < 0) {
    return;
  }

  if (g_stream_channel < 0) {
    g_stream_channel = encChn;
    hack_log("auto-selected encoder channel=%d", g_stream_channel);
  }
  if (encChn != g_stream_channel) {
    return;
  }

  if (g_pending_idr_request) {
    if (g_real_request_idr != NULL) {
      int rc = g_real_request_idr(encChn);
      hack_log("IMP_Encoder_RequestIDR(channel=%d) rc=%d", encChn, rc);
    }
    g_pending_idr_request = 0;
  }

  if (g_wait_keyframe) {
    if (!stream_has_keyframe(stream)) {
      return;
    }
    g_wait_keyframe = 0;
    hack_log("keyframe acquired on channel=%d", encChn);
  }

  int st = send_stream_locked(stream);
  if (st == SEND_OK) {
    g_drop_streak = 0;
    return;
  }
  if (st == SEND_WOULD_BLOCK) {
    handle_would_block_locked();
    return;
  }

  close_client_locked("send_stream failed");
}

static void* server_thread_main(void* arg) {
  (void)arg;

  for (;;) {
    struct sockaddr_in cli;
    socklen_t slen = (socklen_t)sizeof(cli);
    int connfd = accept(g_server_fd, (struct sockaddr*)&cli, &slen);
    if (connfd < 0) {
      int saved_errno = errno;
      pthread_mutex_lock(&g_state_lock);
      int stop = g_server_stop;
      pthread_mutex_unlock(&g_state_lock);
      if (stop) {
        break;
      }
      if (saved_errno != EINTR && saved_errno != EAGAIN && saved_errno != EWOULDBLOCK) {
        hack_log("accept failed: %s", strerror(saved_errno));
        usleep(200000);
      }
      continue;
    }

    if (configure_client_socket(connfd) != 0) {
      close(connfd);
      continue;
    }

    char host[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &cli.sin_addr, host, sizeof(host)) == NULL) {
      strncpy(host, "unknown", sizeof(host));
      host[sizeof(host) - 1] = '\0';
    }

    pthread_mutex_lock(&g_state_lock);
    set_client_locked(connfd, host, (unsigned)ntohs(cli.sin_port));
    pthread_mutex_unlock(&g_state_lock);
  }

  hack_log("server thread exited");
  return NULL;
}

static int start_server(void) {
  int port = parse_env_int("STREAM_HACK_PORT", DEFAULT_PORT, 1, 65535);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    hack_log("socket() failed: %s", strerror(errno));
    return -1;
  }

  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
    hack_log("setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
    close(fd);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);

  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    hack_log("bind() failed on port %d: %s", port, strerror(errno));
    close(fd);
    return -1;
  }

  if (listen(fd, LISTEN_BACKLOG) != 0) {
    hack_log("listen() failed: %s", strerror(errno));
    close(fd);
    return -1;
  }

  g_server_fd = fd;
  g_server_stop = 0;

  if (pthread_create(&g_server_thread, NULL, server_thread_main, NULL) != 0) {
    hack_log("pthread_create() failed: %s", strerror(errno));
    close(fd);
    g_server_fd = -1;
    return -1;
  }

  g_server_thread_started = 1;
  hack_log("raw TCP server listening on 0.0.0.0:%d", port);
  return 0;
}

static void hack_init_once(void) {
  init_logging();

  g_stream_channel = parse_env_int("STREAM_HACK_CHANNEL", DEFAULT_STREAM_CHANNEL, -1, 255);
  g_max_drop_streak = parse_env_int("STREAM_HACK_MAX_DROP_STREAK", DEFAULT_MAX_DROP_STREAK, 1, 5000);

  if (g_stream_channel >= 0) {
    hack_log("using fixed STREAM_HACK_CHANNEL=%d", g_stream_channel);
  } else {
    hack_log("using auto channel detect");
  }
  hack_log("network tuning: max_drop_streak=%d", g_max_drop_streak);

  resolve_real_symbols();
  if (g_real_get_stream == NULL) {
    hack_log("failed to resolve IMP_Encoder_GetStream");
    return;
  }

  if (start_server() != 0) {
    hack_log("server start failed, hook remains passive");
  }
}

static void ensure_initialized(void) { (void)pthread_once(&g_init_once, hack_init_once); }

public void __attribute__((destructor)) hack_shutdown(void) {
  pthread_mutex_lock(&g_state_lock);
  g_server_stop = 1;

  if (g_server_fd >= 0) {
    int fd = g_server_fd;
    g_server_fd = -1;
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }

  close_client_locked("shutdown");
  pthread_mutex_unlock(&g_state_lock);

  if (g_server_thread_started) {
    pthread_join(g_server_thread, NULL);
    g_server_thread_started = 0;
  }

  if (g_real_lib != NULL) {
    dlclose(g_real_lib);
    g_real_lib = NULL;
  }

  if (g_log_fd >= 0) {
    close(g_log_fd);
    g_log_fd = -1;
  }
}

public int IMP_Encoder_GetStream(int encChn, IMPEncoderStream* stream, bool blockFlag) {
  ensure_initialized();

  if (g_real_get_stream == NULL) {
    errno = ENOSYS;
    return -1;
  }

  int rval = g_real_get_stream(encChn, stream, blockFlag);

  if (rval == 0 && stream != NULL) {
    pthread_mutex_lock(&g_state_lock);
    process_stream_locked(encChn, stream);
    pthread_mutex_unlock(&g_state_lock);
  }

  return rval;
}
