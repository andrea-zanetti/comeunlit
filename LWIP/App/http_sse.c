/**
 * @file    http_sse.c
 * @brief   HTTP server for the SimpleBus bridge API v1 (docs/API.md).
 *
 *          Serves /api/v1/... only; everything is signed (api_v1.c,
 *          sb_auth.c). There are no pages and no unsigned endpoints:
 *          anything outside /api/v1/ gets a plain 404.
 *
 *          One accept task handles requests one at a time. Streams
 *          (/api/v1/events, /api/v1/admin/frames) get their own task, which
 *          owns the socket until the client goes away; at most
 *          MAX_SSE_CLIENTS run at once.
 *
 *          Requests are read in full: the server keeps receiving until the
 *          end of the headers, then until Content-Length body bytes have
 *          arrived. Headers over HTTP_MAX_HEADER or bodies over HTTP_MAX_BODY
 *          get 413; a client that stalls is dropped after
 *          HTTP_RECV_TIMEOUT_MS (when lwIP is built with LWIP_SO_RCVTIMEO).
 */

#include "http_sse.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "api_v1.h"
#include "cmsis_os2.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "sb_auth.h"
#include "sb_crypto.h"
#include "sb_stream.h"
#include "sb_ring.h"
#include "stm32h5xx_hal.h"

/* --- Configuration -------------------------------------------------------- */

#define HTTP_PORT 80
#define HTTP_MAX_HEADER 1024 /* request line + headers                   */
#define HTTP_MAX_BODY 1024   /* API limit is 512 (signed 413 above that) */
#define HTTP_RECV_TIMEOUT_MS 3000
#define SSE_POLL_MS 50
#define SSE_TASK_STACK 4096 /* bytes: sb_stream_t (~1 KB, heap) + line */
#define MAX_CONNECTIONS (MAX_SSE_CLIENTS + 2)

static const char HTTP_413[] =
    "HTTP/1.1 413 Payload Too Large\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char HTTP_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n";

/* --- Streams -------------------------------------------------------------- */

typedef struct {
    int sock;
    sb_stream_type_t type;
    char req_mac[SB_AUTH_MAC_HEX + 1];
    char hdr_mac[SB_AUTH_MAC_HEX + 1];
} stream_arg_t;

/* Live streams. Incremented when a stream is accepted, decremented by the
 * stream's own task on exit; capped at MAX_SSE_CLIENTS so sockets stay free
 * for ordinary requests. */
static volatile int sse_active;

/* A client that has gone away must be noticed even when there is nothing
 * to send: a non-blocking recv returns 0 once its FIN arrives, and the
 * heartbeat write fails for clients that vanished without one. */
static int peer_closed(int sock) {
    char c;
    return lwip_recv(sock, &c, 1, MSG_DONTWAIT) == 0;
}

static void stream_task(void* argument) {
    stream_arg_t arg = *(stream_arg_t*)argument;
    vPortFree(argument);

    sb_stream_t* st = pvPortMalloc(sizeof(sb_stream_t));
    if (st == NULL) goto done;
    sb_stream_init(st, arg.type, arg.req_mac);

    char line[SB_STREAM_LINE_MAX];
    int n = snprintf(line, sizeof(line),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\n"
                     "X-SB-MAC: %s\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n",
                     arg.hdr_mac);
    if (lwip_write(arg.sock, line, n) < 0) goto done;

    sb_ring_reader_t consumer;
    sb_ring_reader_init(&consumer, &g_sb_ring);

    /* First heartbeat right away: the client sees a signed event at once. */
    uint32_t last_hb = HAL_GetTick() - SB_STREAM_HB_MS;

    for (;;) {
        const sb_ring_frame_t* f =
            sb_ring_read(&consumer, &g_sb_ring);

        if (f != NULL) {
            n = sb_stream_frame(st, f, line, sizeof(line));
            if (n < 0) break;
            if (n > 0 && lwip_write(arg.sock, line, n) < 0) break;
        } else {
            if (peer_closed(arg.sock)) break;
            osDelay(SSE_POLL_MS);
        }

        uint32_t now = HAL_GetTick();
        if (now - last_hb >= SB_STREAM_HB_MS) {
            n = sb_stream_heartbeat(st, now, line, sizeof(line));
            if (n < 0 || lwip_write(arg.sock, line, n) < 0) break;
            last_hb = now;
        }
    }

done:
    if (st) vPortFree(st);
    __atomic_sub_fetch(&sse_active, 1, __ATOMIC_SEQ_CST);
    lwip_close(arg.sock);
    osThreadTerminate(osThreadGetId());
}

int api_v1_stream_open(int sock, sb_stream_type_t type, const char* req_mac,
                       const char* hdr_mac) {
    if (__atomic_add_fetch(&sse_active, 1, __ATOMIC_SEQ_CST) >
        MAX_SSE_CLIENTS) {
        __atomic_sub_fetch(&sse_active, 1, __ATOMIC_SEQ_CST);
        return -1;
    }

    stream_arg_t* arg = pvPortMalloc(sizeof(stream_arg_t));
    if (arg == NULL) {
        __atomic_sub_fetch(&sse_active, 1, __ATOMIC_SEQ_CST);
        return -2;
    }
    arg->sock = sock;
    arg->type = type;
    memcpy(arg->req_mac, req_mac, SB_AUTH_MAC_HEX + 1);
    memcpy(arg->hdr_mac, hdr_mac, SB_AUTH_MAC_HEX + 1);

    const osThreadAttr_t attr = {
        .name = (type == SB_STREAM_EVENTS) ? "sse_ev" : "sse_fr",
        .stack_size = SSE_TASK_STACK,
        .priority = osPriorityBelowNormal,
    };
    if (osThreadNew(stream_task, arg, &attr) == NULL) {
        vPortFree(arg);
        __atomic_sub_fetch(&sse_active, 1, __ATOMIC_SEQ_CST);
        return -2;
    }
    return 0;
}

int http_sse_stream_count(void) {
    return __atomic_load_n(&sse_active, __ATOMIC_SEQ_CST);
}

/* --- Request reader ------------------------------------------------------- */

/* One request at a time: requests are handled synchronously in the accept
 * task (stream tasks never touch this buffer), so a static buffer is safe. */
static char req_buf[HTTP_MAX_HEADER + HTTP_MAX_BODY + 1];

typedef struct {
    int body_off; /* offset of the body in req_buf          */
    int body_len; /* body bytes (NUL-terminated at the end) */
} http_req_t;

static int ci_prefix(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *prefix) return 0;
    }
    return 1;
}

/* Content-Length from the header block, -1 if absent or malformed. */
static long parse_content_length(const char* hdr, const char* hdr_end) {
    for (const char* p = hdr; p && p < hdr_end;) {
        if (ci_prefix(p, "content-length:")) {
            p += 15;
            while (*p == ' ' || *p == '\t') p++;
            if (*p < '0' || *p > '9') return -1;
            long n = 0;
            while (*p >= '0' && *p <= '9') {
                n = n * 10 + (*p - '0');
                if (n > 1000000) return -1;
                p++;
            }
            return n;
        }
        p = strstr(p, "\r\n");
        if (p) p += 2;
    }
    return -1;
}

/**
 * Read one complete request into req_buf.
 * Returns 0 on success, -1 on connection error/timeout, -2 if too large.
 */
static int read_request(int sock, http_req_t* req) {
    int len = 0;
    char* hdr_end = NULL;

    /* 1. Headers: receive until the blank line. */
    while (hdr_end == NULL) {
        if (len >= HTTP_MAX_HEADER) return -2;
        int n = lwip_recv(sock, req_buf + len, HTTP_MAX_HEADER - len, 0);
        if (n <= 0) return -1;
        len += n;
        req_buf[len] = '\0';
        hdr_end = strstr(req_buf, "\r\n\r\n");
    }
    int hlen = (int)(hdr_end - req_buf) + 4;

    /* 2. Body: Content-Length bytes (or whatever already arrived if the
     *    client sent none, e.g. a bare GET). */
    long clen = parse_content_length(req_buf, hdr_end);
    int body_len;
    if (clen >= 0) {
        if (clen > HTTP_MAX_BODY) return -2;
        int want = hlen + (int)clen;
        while (len < want) {
            int n = lwip_recv(sock, req_buf + len, want - len, 0);
            if (n <= 0) return -1;
            len += n;
        }
        body_len = (int)clen; /* ignore anything pipelined after it */
    } else {
        body_len = len - hlen;
        if (body_len > HTTP_MAX_BODY) return -2;
    }

    req_buf[hlen + body_len] = '\0';
    req->body_off = hlen;
    req->body_len = body_len;
    return 0;
}

/* --- Request handler ------------------------------------------------------ */

static void handle_request(int sock) {
    http_req_t req;
    int rc = read_request(sock, &req);
    if (rc == -2) {
        lwip_write(sock, HTTP_413, sizeof(HTTP_413) - 1);
        lwip_close(sock);
        return;
    }
    if (rc < 0) {
        lwip_close(sock);
        return;
    }

    /* Path starts after the method and one space. */
    const char* sp = strchr(req_buf, ' ');
    if (sp && strncmp(sp + 1, API_V1_PREFIX, sizeof(API_V1_PREFIX) - 1) == 0) {
        if (api_v1_handle(sock, req_buf, req_buf + req.body_off,
                          req.body_len) == API_V1_TAKEN)
            return; /* a stream task owns the socket now */
    } else {
        lwip_write(sock, HTTP_404, sizeof(HTTP_404) - 1);
    }
    lwip_close(sock);
}

/* --- Accept task ---------------------------------------------------------- */

static void http_accept_task(void* argument) {
    (void)argument;

    /* Crypto first: HASH + RNG init and self-tests (needs task context).
     * The API refuses to work if this fails; it is logged either way. */
    if (sb_crypto_init() == 0) {
        /* API session for this boot: 64 random bits (API.md 3.2). */
        uint8_t session[8];
        if (sb_random(session, sizeof(session)) == 0) {
            sb_auth_init(session);
            printf("[API] session %s\r\n", sb_auth_session_hex());
        } else {
            printf("[API] RNG failed: API disabled\r\n");
        }
    }

    int listenfd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        printf("[HTTP] socket() failed\r\n");
        osThreadTerminate(osThreadGetId());
        return;
    }

    int reuse = 1;
    lwip_setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (lwip_bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[HTTP] bind() failed\r\n");
        lwip_close(listenfd);
        osThreadTerminate(osThreadGetId());
        return;
    }

    if (lwip_listen(listenfd, MAX_CONNECTIONS) < 0) {
        printf("[HTTP] listen() failed\r\n");
        lwip_close(listenfd);
        osThreadTerminate(osThreadGetId());
        return;
    }

    printf("[HTTP] Listening on port %d (API v1 only)\r\n", HTTP_PORT);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock =
            lwip_accept(listenfd, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock >= 0) {
#if LWIP_SO_RCVTIMEO
            /* Don't let a stalled client block the (single) accept task. */
#if LWIP_SO_SNDRCVTIMEO_NONSTANDARD
            int tmo = HTTP_RECV_TIMEOUT_MS;
#else
            struct timeval tmo = {.tv_sec = HTTP_RECV_TIMEOUT_MS / 1000,
                                  .tv_usec = (HTTP_RECV_TIMEOUT_MS % 1000) * 1000};
#endif
            lwip_setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tmo,
                            sizeof(tmo));
#endif
            handle_request(client_sock);
        }
    }
}

/* --- Public API ----------------------------------------------------------- */

void http_sse_init(void) {
    const osThreadAttr_t attr = {
        .name = "http",
        .stack_size = 4096,
        .priority = osPriorityBelowNormal,
    };
    osThreadNew(http_accept_task, NULL, &attr);
}
