/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "client/network-transmit.h"
#include "common/recalld-log.h"

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

/* Bulk ingest request header sent before payload */
typedef struct BulkIngestHeader {
        char magic[4];          /* "RCLD" */
        uint32_t version;       /* protocol version, currently 1 */
        uint32_t capture_type;  /* 0=audio, 1=video, 2=screenshot */
        uint32_t duration_ms;
        uint64_t data_len;
        uint64_t timestamp;     /* Unix epoch */
        char source[64];        /* source identifier */
} __attribute__((packed)) BulkIngestHeader;

struct NetworkTransmitter {
        NetworkTransmitConfig config;

        TransmitState state;
        pthread_mutex_t lock;

        /* TCP connection to server */
        int sock_fd;
        SSL_CTX *ssl_ctx;
        SSL *ssl;

        /* RTP state */
        uint16_t rtp_seq;
        uint32_t rtp_timestamp;

        /* VNC/RFB state */
        uint32_t vnc_frame_seq;

        /* Statistics */
        TransmitStats stats;
};

static const char *transmit_protocol_table[] = {
        [TRANSMIT_PROTOCOL_RTP]  = "rtp",
        [TRANSMIT_PROTOCOL_VNC]  = "vnc",
        [TRANSMIT_PROTOCOL_BULK] = "bulk",
};

static const char *transmit_state_table[] = {
        [TRANSMIT_STATE_DISCONNECTED] = "disconnected",
        [TRANSMIT_STATE_CONNECTING]   = "connecting",
        [TRANSMIT_STATE_CONNECTED]    = "connected",
        [TRANSMIT_STATE_ERROR]        = "error",
};

const char* transmit_protocol_to_string(TransmitProtocol proto) {
        if (proto >= 0 && proto < _TRANSMIT_PROTOCOL_MAX)
                return transmit_protocol_table[proto];
        return NULL;
}

TransmitProtocol transmit_protocol_from_string(const char *s) {
        if (!s)
                return _TRANSMIT_PROTOCOL_INVALID;

        for (int i = 0; i < _TRANSMIT_PROTOCOL_MAX; i++)
                if (strcmp(s, transmit_protocol_table[i]) == 0)
                        return (TransmitProtocol) i;

        return _TRANSMIT_PROTOCOL_INVALID;
}

const char* transmit_state_to_string(TransmitState state) {
        if (state >= 0 && state < _TRANSMIT_STATE_MAX)
                return transmit_state_table[state];
        return NULL;
}

static int transmit_send(NetworkTransmitter *t, const void *buf, size_t len) {
        ssize_t n;

        if (t->ssl)
                n = SSL_write(t->ssl, buf, (int) len);
        else
                n = write(t->sock_fd, buf, len);

        if (n < 0) {
                t->stats.bytes_failed += len;
                t->stats.packets_failed++;
                return -errno;
        }

        t->stats.bytes_sent += (uint64_t) n;
        t->stats.packets_sent++;
        return 0;
}

static int transmit_send_with_retry(NetworkTransmitter *t, const void *buf, size_t len) {
        int retries = t->config.retry_count > 0 ? t->config.retry_count : 3;

        for (int attempt = 0; attempt <= retries; attempt++) {
                int r = transmit_send(t, buf, len);
                if (r >= 0)
                        return 0;

                if (attempt < retries) {
                        t->stats.retransmissions++;
                        int delay = t->config.retry_delay_seconds > 0
                                ? t->config.retry_delay_seconds : 1;
                        log_debug("Transmit failed, retrying in %ds (attempt %d/%d).",
                                  delay, attempt + 1, retries);
                        sleep((unsigned int) delay);

                        /* Try to reconnect if the socket died */
                        if (t->state != TRANSMIT_STATE_CONNECTED) {
                                network_transmitter_disconnect(t);
                                r = network_transmitter_connect(t);
                                if (r < 0)
                                        continue;
                        }
                }
        }

        return -EIO;
}

int network_transmitter_new(
                NetworkTransmitter **ret,
                const NetworkTransmitConfig *config) {

        NetworkTransmitter *t;

        if (!ret || !config)
                return -EINVAL;

        if (!config->server_address || strlen(config->server_address) == 0)
                return log_error_errno(EINVAL, "No server address configured for network transmission.");

        t = calloc(1, sizeof(NetworkTransmitter));
        if (!t)
                return log_oom(), -ENOMEM;

        t->config = *config;
        t->config.server_address = strdup(config->server_address);
        if (config->auth_token)
                t->config.auth_token = strdup(config->auth_token);
        if (config->compression)
                t->config.compression = strdup(config->compression);

        t->sock_fd = -1;
        t->state = TRANSMIT_STATE_DISCONNECTED;
        t->rtp_seq = 0;
        t->rtp_timestamp = 0;
        t->vnc_frame_seq = 0;

        /* Default RTP payload type 111 (Opus) */
        if (t->config.rtp_payload_type == 0)
                t->config.rtp_payload_type = 111;

        /* Default server port */
        if (t->config.server_port == 0)
                t->config.server_port = 8080;

        pthread_mutex_init(&t->lock, NULL);

        *ret = t;
        return 0;
}

NetworkTransmitter* network_transmitter_free(NetworkTransmitter *t) {
        if (!t)
                return NULL;

        if (t->state == TRANSMIT_STATE_CONNECTED)
                network_transmitter_disconnect(t);

        pthread_mutex_destroy(&t->lock);
        free(t->config.server_address);
        free(t->config.auth_token);
        free(t->config.compression);
        free(t);
        return NULL;
}

int network_transmitter_connect(NetworkTransmitter *t) {
        struct addrinfo hints = {}, *result, *rp;
        char port_str[16];
        int r;

        if (!t)
                return -EINVAL;

        pthread_mutex_lock(&t->lock);

        if (t->state == TRANSMIT_STATE_CONNECTED) {
                pthread_mutex_unlock(&t->lock);
                return 0;
        }

        t->state = TRANSMIT_STATE_CONNECTING;

        snprintf(port_str, sizeof(port_str), "%d", t->config.server_port);

        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        r = getaddrinfo(t->config.server_address, port_str, &hints, &result);
        if (r != 0) {
                log_error("Failed to resolve server address '%s': %s",
                          t->config.server_address, gai_strerror(r));
                t->state = TRANSMIT_STATE_ERROR;
                pthread_mutex_unlock(&t->lock);
                return -ENOENT;
        }

        for (rp = result; rp; rp = rp->ai_next) {
                t->sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
                if (t->sock_fd < 0)
                        continue;

                if (connect(t->sock_fd, rp->ai_addr, rp->ai_addrlen) == 0)
                        break;

                close(t->sock_fd);
                t->sock_fd = -1;
        }

        freeaddrinfo(result);

        if (t->sock_fd < 0) {
                log_error("Failed to connect to server %s:%d.",
                          t->config.server_address, t->config.server_port);
                t->state = TRANSMIT_STATE_ERROR;
                pthread_mutex_unlock(&t->lock);
                return -ECONNREFUSED;
        }

        /* Set up TLS if enabled */
        if (t->config.tls_enabled) {
                t->ssl_ctx = SSL_CTX_new(TLS_client_method());
                if (!t->ssl_ctx) {
                        log_error("Failed to create TLS context.");
                        close(t->sock_fd);
                        t->sock_fd = -1;
                        t->state = TRANSMIT_STATE_ERROR;
                        pthread_mutex_unlock(&t->lock);
                        return -EIO;
                }

                t->ssl = SSL_new(t->ssl_ctx);
                SSL_set_fd(t->ssl, t->sock_fd);

                if (SSL_connect(t->ssl) <= 0) {
                        log_error("TLS handshake with server failed.");
                        SSL_free(t->ssl);
                        t->ssl = NULL;
                        SSL_CTX_free(t->ssl_ctx);
                        t->ssl_ctx = NULL;
                        close(t->sock_fd);
                        t->sock_fd = -1;
                        t->state = TRANSMIT_STATE_ERROR;
                        pthread_mutex_unlock(&t->lock);
                        return -EIO;
                }
        }

        t->state = TRANSMIT_STATE_CONNECTED;
        log_info("Connected to server %s:%d%s.",
                 t->config.server_address, t->config.server_port,
                 t->config.tls_enabled ? " (TLS)" : "");

        pthread_mutex_unlock(&t->lock);
        return 0;
}

int network_transmitter_disconnect(NetworkTransmitter *t) {
        if (!t)
                return -EINVAL;

        pthread_mutex_lock(&t->lock);

        if (t->ssl) {
                SSL_shutdown(t->ssl);
                SSL_free(t->ssl);
                t->ssl = NULL;
        }

        if (t->ssl_ctx) {
                SSL_CTX_free(t->ssl_ctx);
                t->ssl_ctx = NULL;
        }

        if (t->sock_fd >= 0) {
                close(t->sock_fd);
                t->sock_fd = -1;
        }

        t->state = TRANSMIT_STATE_DISCONNECTED;
        log_info("Disconnected from server.");

        pthread_mutex_unlock(&t->lock);
        return 0;
}

int network_transmit_audio_rtp(
                NetworkTransmitter *t,
                const uint8_t *data,
                size_t len,
                int sample_rate,
                int channels,
                uint32_t timestamp) {

        uint8_t *packet;
        size_t packet_len;
        RtpHeader hdr = {};
        int r;

        (void) sample_rate;
        (void) channels;

        if (!t || !data || len == 0)
                return -EINVAL;
        if (t->state != TRANSMIT_STATE_CONNECTED)
                return -ENOTCONN;

        /* Build RTP header */
        hdr.version = 2;
        hdr.p = 0;
        hdr.x = 0;
        hdr.cc = 0;
        hdr.m = 1;     /* marker bit: start of talkspurt */
        hdr.pt = (uint8_t) t->config.rtp_payload_type;
        hdr.seq = htons(t->rtp_seq++);
        hdr.timestamp = htonl(timestamp > 0 ? timestamp : t->rtp_timestamp);
        hdr.ssrc = htonl((uint32_t) t->config.rtp_ssrc);

        /* Advance RTP timestamp by number of samples.
         * For Opus at 48kHz, each 20ms frame = 960 samples. */
        t->rtp_timestamp += (uint32_t)(len / ((size_t) channels * 2));

        /* Assemble packet: RTP header + audio payload */
        packet_len = sizeof(RtpHeader) + len;
        packet = malloc(packet_len);
        if (!packet)
                return -ENOMEM;

        memcpy(packet, &hdr, sizeof(RtpHeader));
        memcpy(packet + sizeof(RtpHeader), data, len);

        r = transmit_send_with_retry(t, packet, packet_len);
        free(packet);

        if (r < 0)
                log_debug("Failed to send RTP audio packet (seq=%u).", (unsigned) t->rtp_seq - 1);

        return r;
}

int network_transmit_video_vnc(
                NetworkTransmitter *t,
                const uint8_t *data,
                size_t len,
                int width,
                int height,
                uint32_t frame_number) {

        RfbFramebufferUpdateHeader fbhdr = {};
        RfbRectHeader rect = {};
        struct iovec iov[3];
        uint8_t *packet;
        size_t packet_len;
        int r;

        (void) frame_number;

        if (!t || !data || len == 0)
                return -EINVAL;
        if (t->state != TRANSMIT_STATE_CONNECTED)
                return -ENOTCONN;

        /* Build RFB FramebufferUpdate header */
        fbhdr.message_type = 0; /* FramebufferUpdate */
        fbhdr.padding = 0;
        fbhdr.num_rects = htons(1);

        /* Build rectangle header — full screen update */
        rect.x = 0;
        rect.y = 0;
        rect.width = htons((uint16_t) width);
        rect.height = htons((uint16_t) height);
        rect.encoding = htonl(t->config.vnc_encoding); /* 0=Raw by default */

        /* Assemble packet: FBUpdate header + rect header + pixel data */
        packet_len = sizeof(fbhdr) + sizeof(rect) + len;
        packet = malloc(packet_len);
        if (!packet)
                return -ENOMEM;

        memcpy(packet, &fbhdr, sizeof(fbhdr));
        memcpy(packet + sizeof(fbhdr), &rect, sizeof(rect));
        memcpy(packet + sizeof(fbhdr) + sizeof(rect), data, len);

        (void) iov; /* suppress unused warning — we use flat buffer instead */

        r = transmit_send_with_retry(t, packet, packet_len);
        free(packet);

        t->vnc_frame_seq++;

        if (r < 0)
                log_debug("Failed to send VNC frame %u.", t->vnc_frame_seq - 1);

        return r;
}

int network_transmit_bulk(
                NetworkTransmitter *t,
                int capture_type,
                const uint8_t *data,
                size_t len,
                int duration_ms,
                const char *source) {

        BulkIngestHeader hdr = {};
        uint8_t *packet;
        size_t packet_len;
        int r;

        if (!t || !data || len == 0)
                return -EINVAL;
        if (t->state != TRANSMIT_STATE_CONNECTED)
                return -ENOTCONN;

        /* Build bulk ingest header */
        memcpy(hdr.magic, "RCLD", 4);
        hdr.version = htonl(1);
        hdr.capture_type = htonl((uint32_t) capture_type);
        hdr.duration_ms = htonl((uint32_t) duration_ms);
        hdr.data_len = htobe64((uint64_t) len);
        hdr.timestamp = htobe64((uint64_t) time(NULL));
        if (source)
                snprintf(hdr.source, sizeof(hdr.source), "%s", source);

        /* If we have an auth token, send it as an HTTP-style request
         * to /api/v1/ingest so the server's HTTP handler can route it */
        packet_len = sizeof(BulkIngestHeader) + len;
        packet = malloc(packet_len);
        if (!packet)
                return -ENOMEM;

        memcpy(packet, &hdr, sizeof(BulkIngestHeader));
        memcpy(packet + sizeof(BulkIngestHeader), data, len);

        r = transmit_send_with_retry(t, packet, packet_len);
        free(packet);

        if (r < 0)
                log_debug("Failed to send bulk ingest data (%zu bytes).", len);
        else
                log_debug("Sent bulk ingest: type=%d, %zu bytes.", capture_type, len);

        return r;
}

TransmitState network_transmitter_get_state(const NetworkTransmitter *t) {
        return t ? t->state : TRANSMIT_STATE_DISCONNECTED;
}

int network_transmitter_get_stats(const NetworkTransmitter *t, TransmitStats *ret_stats) {
        if (!t || !ret_stats)
                return -EINVAL;

        *ret_stats = t->stats;
        return 0;
}
