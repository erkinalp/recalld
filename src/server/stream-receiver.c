/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "server/stream-receiver.h"
#include "server/query-service.h"
#include "common/recalld-config.h"
#include "common/recalld-log.h"
#include "common/recalld-storage.h"

/* Wire format structs — must match the client-side definitions in
 * src/client/network-transmit.c exactly. */

/* RTP header (RFC 3550), 12 bytes fixed part */
typedef struct RtpHeader {
        uint8_t cc:4;
        uint8_t x:1;
        uint8_t p:1;
        uint8_t version:2;
        uint8_t pt:7;
        uint8_t m:1;
        uint16_t seq;
        uint32_t timestamp;
        uint32_t ssrc;
} __attribute__((packed)) RtpHeader;

/* RFB (VNC) FramebufferUpdate message header */
typedef struct RfbFramebufferUpdateHeader {
        uint8_t message_type;   /* 0 = FramebufferUpdate */
        uint8_t padding;
        uint16_t num_rects;
} __attribute__((packed)) RfbFramebufferUpdateHeader;

typedef struct RfbRectHeader {
        uint16_t x;
        uint16_t y;
        uint16_t width;
        uint16_t height;
        int32_t encoding;       /* 0=Raw, 6=ZLib, 7=Tight */
} __attribute__((packed)) RfbRectHeader;

/* Bulk ingest request header */
typedef struct BulkIngestHeader {
        char magic[4];          /* "RCLD" */
        uint32_t version;
        uint32_t capture_type;  /* 0=audio, 1=video, 2=screenshot */
        uint32_t duration_ms;
        uint64_t data_len;
        uint64_t timestamp;
        char source[64];
} __attribute__((packed)) BulkIngestHeader;

#define STREAM_PEEK_SIZE        16
#define STREAM_MAX_PACKET_SIZE  (16 * 1024 * 1024)  /* 16 MiB safety limit */

typedef enum StreamProtocol {
        STREAM_PROTO_RTP,
        STREAM_PROTO_VNC,
        STREAM_PROTO_BULK,
        STREAM_PROTO_UNKNOWN,
} StreamProtocol;

struct StreamReceiver {
        QueryServiceConfig config;
        QueryService *query_svc;

        int listen_fd;
        SSL_CTX *ssl_ctx;
        bool running;
        pthread_t accept_thread;
};

/* ---- helpers ------------------------------------------------------------ */

static int recv_exact(int fd, SSL *ssl, void *buf, size_t len) {
        uint8_t *p = buf;
        size_t remaining = len;

        while (remaining > 0) {
                ssize_t n;

                if (ssl)
                        n = SSL_read(ssl, p, (int) remaining);
                else
                        n = read(fd, p, remaining);

                if (n <= 0)
                        return -EIO;

                p += n;
                remaining -= (size_t) n;
        }

        return 0;
}

/* Auto-detect the protocol from the first bytes on the wire.
 *
 * - Bulk ingest starts with the 4-byte magic "RCLD"
 * - RTP version 2 has bits [6:7] of the first byte == 2  (0x80 mask)
 * - RFB FramebufferUpdate has message_type == 0 (first byte 0x00)
 *
 * We peek (MSG_PEEK) so the bytes remain available for the actual reader. */
static StreamProtocol detect_protocol(int fd, SSL *ssl) {
        uint8_t peek[STREAM_PEEK_SIZE];
        ssize_t n;

        if (ssl) {
                /* SSL_peek is the TLS equivalent of MSG_PEEK */
                n = SSL_peek(ssl, peek, STREAM_PEEK_SIZE);
        } else {
                n = recv(fd, peek, STREAM_PEEK_SIZE, MSG_PEEK);
        }

        if (n < 4)
                return STREAM_PROTO_UNKNOWN;

        /* Check for bulk ingest magic "RCLD" */
        if (memcmp(peek, "RCLD", 4) == 0)
                return STREAM_PROTO_BULK;

        /* Check for RTP: version field (bits 6-7 of first byte) must be 2 */
        if ((peek[0] >> 6) == 2)
                return STREAM_PROTO_RTP;

        /* RFB FramebufferUpdate: message_type == 0, padding == 0 */
        if (peek[0] == 0 && n >= (ssize_t) sizeof(RfbFramebufferUpdateHeader))
                return STREAM_PROTO_VNC;

        return STREAM_PROTO_UNKNOWN;
}

/* ---- RTP receiver ------------------------------------------------------- */

/* Read one RTP packet and ingest the audio payload.
 * The client sends: [RtpHeader (12 bytes)] [audio payload (variable)].
 * On TCP the receiver must frame the packets.  The client transmitter
 * sends each packet as a contiguous write, so we first read the fixed
 * 12-byte header, then read the remaining payload.  Because TCP is a
 * stream, we rely on the fact that each packet is preceded by its header.
 *
 * For long-lived connections we keep reading until EOF / error. */
static int handle_rtp_stream(int fd, SSL *ssl, QueryService *query_svc) {
        RtpHeader hdr;
        uint8_t payload[STREAM_MAX_PACKET_SIZE];
        int r;

        log_info("Stream receiver: RTP audio session started.");

        for (;;) {
                r = recv_exact(fd, ssl, &hdr, sizeof(hdr));
                if (r < 0)
                        break;  /* client disconnected or error */

                /* Validate RTP version */
                if (hdr.version != 2) {
                        log_warning("Stream receiver: bad RTP version %u, dropping.", hdr.version);
                        break;
                }

                /* The client packs the full packet (header + payload) in a
                 * single write.  After reading the header, the rest of the
                 * TCP segment is the payload.  However, we don't know the
                 * payload length up front.  A practical approach: read up to
                 * a reasonable amount, then ingest what we got.
                 *
                 * For a better framing story, we read a 4-byte big-endian
                 * length prefix that the client should prepend. But since
                 * the current client doesn't do that, we use a heuristic:
                 * try a non-blocking peek to determine how much data is
                 * buffered, then read that.  Falls back to reading a fixed
                 * 960-sample Opus frame (~120 bytes). */
                ssize_t avail;
                if (ssl)
                        avail = SSL_pending(ssl);
                else {
                        /* Use MSG_PEEK + MSG_DONTWAIT to check available data */
                        uint8_t tmp[1];
                        avail = recv(fd, tmp, 1, MSG_PEEK | MSG_DONTWAIT);
                        if (avail <= 0)
                                avail = 0;
                }

                /* If nothing pending, try to read one standard frame */
                size_t payload_len = avail > 0 ? (size_t) avail : 960;
                if (payload_len > sizeof(payload))
                        payload_len = sizeof(payload);

                r = recv_exact(fd, ssl, payload, payload_len);
                if (r < 0)
                        break;

                r = query_service_ingest(query_svc, CAPTURE_AUDIO,
                                payload, payload_len,
                                /* duration_ms= */ 20,  /* ~20ms per Opus frame */
                                "rtp-stream");
                if (r < 0)
                        log_warning_errno(-r, "Stream receiver: failed to ingest RTP audio: %m");
        }

        log_info("Stream receiver: RTP audio session ended.");
        return 0;
}

/* ---- VNC/RFB receiver --------------------------------------------------- */

/* Read RFB FramebufferUpdate messages and ingest the pixel data. */
static int handle_vnc_stream(int fd, SSL *ssl, QueryService *query_svc) {
        RfbFramebufferUpdateHeader fb_hdr;
        RfbRectHeader rect_hdr;
        int r;

        log_info("Stream receiver: VNC video session started.");

        for (;;) {
                r = recv_exact(fd, ssl, &fb_hdr, sizeof(fb_hdr));
                if (r < 0)
                        break;

                if (fb_hdr.message_type != 0) {
                        log_warning("Stream receiver: unexpected RFB message type %u.", fb_hdr.message_type);
                        break;
                }

                uint16_t num_rects = ntohs(fb_hdr.num_rects);

                for (uint16_t i = 0; i < num_rects; i++) {
                        r = recv_exact(fd, ssl, &rect_hdr, sizeof(rect_hdr));
                        if (r < 0)
                                goto vnc_done;

                        uint16_t w = ntohs(rect_hdr.width);
                        uint16_t h = ntohs(rect_hdr.height);
                        int32_t encoding = ntohl((uint32_t) rect_hdr.encoding);

                        /* For Raw encoding (0), pixel data length = w * h * bytes_per_pixel.
                         * Assume 4 bytes per pixel (RGBA/BGRA). */
                        size_t pixel_len;
                        if (encoding == 0)
                                pixel_len = (size_t) w * (size_t) h * 4;
                        else {
                                /* For compressed encodings (ZLib, Tight) there is a
                                 * 4-byte length prefix before the compressed data. */
                                uint32_t comp_len_be;
                                r = recv_exact(fd, ssl, &comp_len_be, 4);
                                if (r < 0)
                                        goto vnc_done;
                                pixel_len = (size_t) ntohl(comp_len_be);
                        }

                        if (pixel_len > STREAM_MAX_PACKET_SIZE) {
                                log_warning("Stream receiver: VNC rect too large (%zu bytes), dropping.", pixel_len);
                                goto vnc_done;
                        }

                        uint8_t *pixels = malloc(pixel_len);
                        if (!pixels) {
                                log_oom();
                                goto vnc_done;
                        }

                        r = recv_exact(fd, ssl, pixels, pixel_len);
                        if (r < 0) {
                                free(pixels);
                                goto vnc_done;
                        }

                        r = query_service_ingest(query_svc, CAPTURE_VIDEO,
                                        pixels, pixel_len,
                                        /* duration_ms= */ 0,
                                        "vnc-stream");
                        free(pixels);

                        if (r < 0)
                                log_warning_errno(-r, "Stream receiver: failed to ingest VNC frame: %m");
                }
        }

vnc_done:
        log_info("Stream receiver: VNC video session ended.");
        return 0;
}

/* ---- Bulk ingest receiver ----------------------------------------------- */

/* Read a RCLD-framed bulk ingest message. */
static int handle_bulk_stream(int fd, SSL *ssl, QueryService *query_svc) {
        BulkIngestHeader hdr;
        int r;

        log_info("Stream receiver: bulk ingest session started.");

        for (;;) {
                r = recv_exact(fd, ssl, &hdr, sizeof(hdr));
                if (r < 0)
                        break;

                if (memcmp(hdr.magic, "RCLD", 4) != 0) {
                        log_warning("Stream receiver: bad bulk magic, closing connection.");
                        break;
                }

                uint32_t version = ntohl(hdr.version);
                if (version != 1) {
                        log_warning("Stream receiver: unsupported bulk version %u.", version);
                        break;
                }

                uint32_t capture_type = ntohl(hdr.capture_type);
                uint32_t duration_ms = ntohl(hdr.duration_ms);
                uint64_t data_len = be64toh(hdr.data_len);

                if (capture_type >= _CAPTURE_TYPE_MAX) {
                        log_warning("Stream receiver: invalid capture type %u.", capture_type);
                        break;
                }

                if (data_len > STREAM_MAX_PACKET_SIZE) {
                        log_warning("Stream receiver: bulk payload too large (%"PRIu64" bytes).", data_len);
                        break;
                }

                /* Ensure source is null-terminated */
                char source[65];
                memcpy(source, hdr.source, 64);
                source[64] = '\0';

                uint8_t *data = malloc((size_t) data_len);
                if (!data) {
                        log_oom();
                        break;
                }

                r = recv_exact(fd, ssl, data, (size_t) data_len);
                if (r < 0) {
                        free(data);
                        break;
                }

                r = query_service_ingest(query_svc, (CaptureType) capture_type,
                                data, (size_t) data_len,
                                (int) duration_ms,
                                strlen(source) > 0 ? source : "bulk-stream");
                free(data);

                if (r < 0)
                        log_warning_errno(-r, "Stream receiver: failed to ingest bulk data: %m");
                else
                        log_debug("Stream receiver: ingested bulk %s, %"PRIu64" bytes.",
                                  capture_type_to_string((CaptureType) capture_type), data_len);
        }

        log_info("Stream receiver: bulk ingest session ended.");
        return 0;
}

/* ---- per-client handler ------------------------------------------------- */

static void handle_stream_client(StreamReceiver *recv, int client_fd) {
        SSL *ssl = NULL;
        StreamProtocol proto;

        if (recv->ssl_ctx) {
                ssl = SSL_new(recv->ssl_ctx);
                SSL_set_fd(ssl, client_fd);
                if (SSL_accept(ssl) <= 0) {
                        log_warning("Stream receiver: TLS handshake failed.");
                        SSL_free(ssl);
                        close(client_fd);
                        return;
                }
        }

        proto = detect_protocol(client_fd, ssl);

        switch (proto) {
        case STREAM_PROTO_RTP:
                handle_rtp_stream(client_fd, ssl, recv->query_svc);
                break;
        case STREAM_PROTO_VNC:
                handle_vnc_stream(client_fd, ssl, recv->query_svc);
                break;
        case STREAM_PROTO_BULK:
                handle_bulk_stream(client_fd, ssl, recv->query_svc);
                break;
        default:
                log_warning("Stream receiver: could not detect protocol, closing connection.");
                break;
        }

        if (ssl) {
                SSL_shutdown(ssl);
                SSL_free(ssl);
        }
        close(client_fd);
}

/* ---- accept thread ------------------------------------------------------ */

static void* stream_accept_thread_fn(void *arg) {
        StreamReceiver *recv = arg;

        while (recv->running) {
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_fd;

                client_fd = accept(recv->listen_fd, (struct sockaddr *) &client_addr, &addr_len);
                if (client_fd < 0) {
                        if (recv->running)
                                log_warning_errno(errno, "Stream receiver: accept failed: %m");
                        continue;
                }

                log_debug("Stream receiver: connection from %s:%d.",
                          inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

                /* Handle in the same thread for simplicity.  For production
                 * use a thread pool or fork a handler thread per connection. */
                handle_stream_client(recv, client_fd);
        }

        return NULL;
}

/* ---- public API --------------------------------------------------------- */

int stream_receiver_new(
                StreamReceiver **ret,
                const QueryServiceConfig *config,
                QueryService *query_svc) {

        StreamReceiver *recv;

        if (!ret || !config || !query_svc)
                return -EINVAL;

        if (config->stream_port <= 0) {
                log_info("Stream receiver disabled (StreamPort not configured).");
                *ret = NULL;
                return 0;
        }

        recv = calloc(1, sizeof(StreamReceiver));
        if (!recv)
                return log_oom(), -ENOMEM;

        recv->config = *config;
        recv->query_svc = query_svc;
        recv->listen_fd = -1;

        /* Set up TLS if the same TLS config as the HTTP server is provided */
        if (config->tls_enabled && config->tls_cert_path && config->tls_key_path) {
                recv->ssl_ctx = SSL_CTX_new(TLS_server_method());
                if (recv->ssl_ctx) {
                        if (SSL_CTX_use_certificate_file(recv->ssl_ctx, config->tls_cert_path,
                                                         SSL_FILETYPE_PEM) <= 0 ||
                            SSL_CTX_use_PrivateKey_file(recv->ssl_ctx, config->tls_key_path,
                                                        SSL_FILETYPE_PEM) <= 0) {
                                log_warning("Stream receiver: failed to load TLS cert/key, falling back to plain TCP.");
                                SSL_CTX_free(recv->ssl_ctx);
                                recv->ssl_ctx = NULL;
                        }
                }
        }

        *ret = recv;
        return 0;
}

StreamReceiver* stream_receiver_free(StreamReceiver *recv) {
        if (!recv)
                return NULL;

        if (recv->running)
                stream_receiver_stop(recv);

        if (recv->listen_fd >= 0)
                close(recv->listen_fd);

        if (recv->ssl_ctx)
                SSL_CTX_free(recv->ssl_ctx);

        free(recv);
        return NULL;
}

int stream_receiver_start(StreamReceiver *recv) {
        struct sockaddr_in addr = {};
        int opt = 1;
        int r;

        if (!recv)
                return -EINVAL;
        if (recv->running)
                return -EALREADY;

        recv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (recv->listen_fd < 0)
                return log_error_errno(errno, "Stream receiver: failed to create socket: %m");

        setsockopt(recv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t) recv->config.stream_port);

        if (bind(recv->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
                close(recv->listen_fd);
                recv->listen_fd = -1;
                return log_error_errno(errno, "Stream receiver: failed to bind to port %d: %m",
                                       recv->config.stream_port);
        }

        if (listen(recv->listen_fd, 64) < 0) {
                close(recv->listen_fd);
                recv->listen_fd = -1;
                return log_error_errno(errno, "Stream receiver: failed to listen: %m");
        }

        recv->running = true;

        r = pthread_create(&recv->accept_thread, /* attr= */ NULL, stream_accept_thread_fn, recv);
        if (r != 0) {
                recv->running = false;
                close(recv->listen_fd);
                recv->listen_fd = -1;
                return -r;
        }

        log_info("Stream receiver started on port %d%s (RTP/VNC/Bulk auto-detect).",
                 recv->config.stream_port, recv->ssl_ctx ? " (TLS)" : "");
        return 0;
}

int stream_receiver_stop(StreamReceiver *recv) {
        if (!recv)
                return -EINVAL;
        if (!recv->running)
                return 0;

        recv->running = false;

        if (recv->listen_fd >= 0) {
                close(recv->listen_fd);
                recv->listen_fd = -1;
        }

        pthread_join(recv->accept_thread, /* retval= */ NULL);

        log_info("Stream receiver stopped.");
        return 0;
}

bool stream_receiver_is_running(const StreamReceiver *recv) {
        return recv ? recv->running : false;
}
