/* getaddrinfo/freeaddrinfo (dial_tcp) need glibc's non-strict feature set;
 * must be defined before any system header is pulled in, including
 * transitively via emu_link.h. Guarded because test_emu_link.c includes
 * this file after unity.h, which has already pulled in <features.h> and
 * defined this itself. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "emu_link.h"

/* CONFIG_IDF_TARGET_LINUX lives in sdkconfig.h, not on the compiler command
 * line: without this include the guard below cannot see it on the IDF linux
 * target and silently compiles the device stubs there (making every virtual
 * driver inert on the node). Guarded because the plain-gcc test harness has
 * no sdkconfig.h. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Host-only: on-device (esp32s3) builds never compile this body (see
 * CMakeLists.txt, which only adds emu_link.c as a source on the IDF linux
 * target). This guard is defense in depth in case the file is ever pulled
 * into a device build by mistake, matching the composite host-gate
 * convention used by components/gps/gps.c. */
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)

int emu_link_connect(const char *node_id, const char *caps_csv) {
    (void)node_id;
    (void)caps_csv;
    return -1;
}

int emu_link_on(const char *type, emu_link_handler_t h, void *ctx) {
    (void)type;
    (void)h;
    (void)ctx;
    return -1;
}

int emu_link_send(cJSON *msg) {
    if (msg)
        cJSON_Delete(msg);
    return -1;
}

void emu_link_close(void) {}

#else /* host build: IDF linux target or the plain-gcc test harness */

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define EMU_LINK_PROTOCOL_VERSION 1

#ifndef EMU_LINK_FW_VERSION
#define EMU_LINK_FW_VERSION "unknown"
#endif

/* Reader buffer size. A single inbound line larger than this is dropped
 * (buffer reset) rather than grown without bound; the resulting fragment
 * fails JSON parsing and is silently ignored, same as any other malformed
 * line, so a hostile or buggy broker can't grow this component's memory
 * without limit. */
#define EMU_LINK_MAX_LINE 65536u

#define EMU_LINK_MAX_HANDLERS 32
#define EMU_LINK_MAX_TYPE_LEN 31

typedef struct {
    bool used;
    char type[EMU_LINK_MAX_TYPE_LEN + 1];
    emu_link_handler_t fn;
    void *ctx;
} emu_link_handler_slot_t;

/* s_send_mu guards s_fd (connection identity) and serializes writes to it.
 * s_handlers_mu guards the handler table independently so a slow handler
 * callback (invoked with the lock dropped, see dispatch_line) never blocks
 * a concurrent send. */
static pthread_mutex_t s_send_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_handlers_mu = PTHREAD_MUTEX_INITIALIZER;

static int s_fd = -1;
static pthread_t s_reader_thread;
static bool s_reader_running = false;

static emu_link_handler_slot_t s_handlers[EMU_LINK_MAX_HANDLERS];

/* --- small helpers -------------------------------------------------- */

/* Writes len bytes to fd in full, looping over partial writes and EINTR.
 * Uses send()+MSG_NOSIGNAL (not write()) so a broker that has hung up
 * doesn't raise SIGPIPE and kill the process. Returns 0 on success, -1 on
 * any I/O error. */
static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* Serializes msg to one JSON line and writes it to the current connection
 * under s_send_mu. Always takes ownership of msg. */
static int send_locked(cJSON *msg) {
    if (!msg)
        return -1;
    if (!cJSON_HasObjectItem(msg, "t")) {
        cJSON_Delete(msg);
        return -1;
    }
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!text)
        return -1;

    size_t len = strlen(text);
    int rc = -1;
    pthread_mutex_lock(&s_send_mu);
    if (s_fd != -1) {
        rc = write_all(s_fd, text, len);
        if (rc == 0)
            rc = write_all(s_fd, "\n", 1);
    }
    pthread_mutex_unlock(&s_send_mu);
    free(text);
    return rc;
}

/* Parses one NUL-terminated line and dispatches it to its registered
 * handler, if any. Unknown types and malformed JSON are silently ignored
 * (DESIGN.md section 8: forward compatibility). */
static void dispatch_line(char *line) {
    cJSON *msg = cJSON_Parse(line);
    if (!msg)
        return;

    const cJSON *t = cJSON_GetObjectItem(msg, "t");
    if (!cJSON_IsString(t) || !t->valuestring) {
        cJSON_Delete(msg);
        return;
    }

    emu_link_handler_t fn = NULL;
    void *ctx = NULL;
    pthread_mutex_lock(&s_handlers_mu);
    for (int i = 0; i < EMU_LINK_MAX_HANDLERS; i++) {
        if (s_handlers[i].used && strcmp(s_handlers[i].type, t->valuestring) == 0) {
            fn = s_handlers[i].fn;
            ctx = s_handlers[i].ctx;
            break;
        }
    }
    pthread_mutex_unlock(&s_handlers_mu);

    if (fn)
        fn(msg, ctx);

    cJSON_Delete(msg);
}

/* Reader thread body. Owns its own fd (passed in as arg, independent of
 * s_fd) so emu_link_close can clear s_fd for the send path without racing
 * this loop's use of the descriptor; the loop only stops via EOF/error on
 * the socket (emu_link_close shuts it down to induce that) or the process
 * exiting. */
static void *reader_main(void *arg) {
    int fd = (int)(intptr_t)arg;
    uint8_t *buf = (uint8_t *)malloc(EMU_LINK_MAX_LINE);
    if (!buf)
        return NULL;
    size_t len = 0;

    for (;;) {
        if (len == EMU_LINK_MAX_LINE) {
            /* Oversized line with no newline yet: drop what we have and
             * resync on whatever arrives next. */
            len = 0;
        }

        ssize_t n = recv(fd, buf + len, EMU_LINK_MAX_LINE - len, 0);
        if (n <= 0)
            break; /* broker gone (EOF) or socket error */
        len += (size_t)n;

        size_t start = 0;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                dispatch_line((char *)buf + start);
                start = i + 1;
            }
        }
        if (start > 0) {
            memmove(buf, buf + start, len - start);
            len -= start;
        }
    }

    free(buf);
    return NULL;
}

/* Full disconnect/teardown, shared by emu_link_close and emu_link_attach's
 * failure path; defined below, forward-declared here since attach needs it. */
static void teardown(void);

/* --- connection setup ------------------------------------------------ */

/* Dials a unix-domain stream socket at path. Returns a connected fd, or -1
 * on any failure (never crashes on a bad path). */
static int dial_unix(const char *path) {
    if (!path || !*path)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path))
        return -1;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Dials a TCP stream socket at "host:port". Returns a connected fd, or -1
 * on any failure. */
static int dial_tcp(const char *hostport) {
    if (!hostport || !*hostport)
        return -1;

    const char *colon = strrchr(hostport, ':');
    if (!colon || colon == hostport || !colon[1])
        return -1;

    size_t hlen = (size_t)(colon - hostport);
    char host[256];
    if (hlen == 0 || hlen >= sizeof(host))
        return -1;
    memcpy(host, hostport, hlen);
    host[hlen] = '\0';
    const char *port = colon + 1;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return -1;

    int fd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Adopts an already-connected fd as the current connection: starts the
 * reader thread and sends hello. Used by emu_link_connect after dialing
 * EMU_BROKER, and directly by tests with one end of a socketpair (the
 * "socketpair test double" in the task brief) to exercise framing and
 * dispatch without a real broker process. Atomic from the caller's point
 * of view: returns 0 with the connection fully up (reader thread running,
 * hello sent), or negative with the fd closed and nothing left running. */
static int emu_link_attach(int fd, const char *node_id, const char *caps_csv) {
    pthread_mutex_lock(&s_send_mu);
    if (s_fd != -1) {
        pthread_mutex_unlock(&s_send_mu);
        close(fd);
        return -1; /* already connected */
    }
    s_fd = fd;
    pthread_mutex_unlock(&s_send_mu);

    if (pthread_create(&s_reader_thread, NULL, reader_main, (void *)(intptr_t)fd) != 0) {
        pthread_mutex_lock(&s_send_mu);
        s_fd = -1;
        pthread_mutex_unlock(&s_send_mu);
        close(fd);
        return -1;
    }
    s_reader_running = true;

    cJSON *hello = cJSON_CreateObject();
    if (!hello) {
        teardown();
        return -1;
    }
    cJSON_AddStringToObject(hello, "t", "hello");
    cJSON_AddStringToObject(hello, "node", node_id ? node_id : "");
    cJSON_AddNumberToObject(hello, "version", EMU_LINK_PROTOCOL_VERSION);
    cJSON_AddStringToObject(hello, "fw", EMU_LINK_FW_VERSION);
    cJSON_AddStringToObject(hello, "caps", caps_csv ? caps_csv : "");
    if (send_locked(hello) != 0) {
        teardown();
        return -1;
    }
    return 0;
}

/* --- public API -------------------------------------------------------*/

int emu_link_connect(const char *node_id, const char *caps_csv) {
    if (!node_id)
        return -1;

    pthread_mutex_lock(&s_send_mu);
    bool already = (s_fd != -1);
    pthread_mutex_unlock(&s_send_mu);
    if (already)
        return -1;

    const char *env = getenv("EMU_BROKER");
    if (!env || !*env)
        return -1;

    int fd;
    if (strncmp(env, "unix:", 5) == 0) {
        fd = dial_unix(env + 5);
    } else if (strncmp(env, "tcp:", 4) == 0) {
        fd = dial_tcp(env + 4);
    } else {
        return -1;
    }
    if (fd < 0)
        return -1;

    return emu_link_attach(fd, node_id, caps_csv);
}

int emu_link_on(const char *type, emu_link_handler_t h, void *ctx) {
    if (!type || !h)
        return -1;
    size_t tlen = strlen(type);
    if (tlen == 0 || tlen > EMU_LINK_MAX_TYPE_LEN)
        return -1;

    int rc = -1;
    pthread_mutex_lock(&s_handlers_mu);
    int free_slot = -1;
    for (int i = 0; i < EMU_LINK_MAX_HANDLERS; i++) {
        if (s_handlers[i].used && strcmp(s_handlers[i].type, type) == 0) {
            s_handlers[i].fn = h;
            s_handlers[i].ctx = ctx;
            rc = 0;
            break;
        }
        if (!s_handlers[i].used && free_slot < 0)
            free_slot = i;
    }
    if (rc != 0 && free_slot >= 0) {
        s_handlers[free_slot].used = true;
        strncpy(s_handlers[free_slot].type, type, EMU_LINK_MAX_TYPE_LEN);
        s_handlers[free_slot].type[EMU_LINK_MAX_TYPE_LEN] = '\0';
        s_handlers[free_slot].fn = h;
        s_handlers[free_slot].ctx = ctx;
        rc = 0;
    }
    pthread_mutex_unlock(&s_handlers_mu);
    return rc;
}

int emu_link_send(cJSON *msg) { return send_locked(msg); }

/* Shared teardown for emu_link_close and emu_link_attach's failure path:
 * clears s_fd, unblocks and joins the reader thread if one is running, and
 * closes the descriptor. Leaves emu_link fully disconnected either way. */
static void teardown(void) {
    pthread_mutex_lock(&s_send_mu);
    int fd = s_fd;
    s_fd = -1;
    pthread_mutex_unlock(&s_send_mu);

    if (fd != -1)
        shutdown(fd, SHUT_RDWR); /* unblocks the reader thread's recv() */

    if (s_reader_running) {
        pthread_join(s_reader_thread, NULL);
        s_reader_running = false;
    }

    if (fd != -1)
        close(fd);
}

void emu_link_close(void) { teardown(); }

#endif /* host build */
