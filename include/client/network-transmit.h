/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NetworkTransmitter NetworkTransmitter;

typedef enum TransmitProtocol {
        TRANSMIT_PROTOCOL_RTP,    /* Reverse RTP for audio streams */
        TRANSMIT_PROTOCOL_VNC,    /* Reverse VNC (RFB) for video frames */
        TRANSMIT_PROTOCOL_BULK,   /* Bulk HTTPS upload for stored data */
        _TRANSMIT_PROTOCOL_MAX,
        _TRANSMIT_PROTOCOL_INVALID = -1,
} TransmitProtocol;

typedef enum TransmitState {
        TRANSMIT_STATE_DISCONNECTED,
        TRANSMIT_STATE_CONNECTING,
        TRANSMIT_STATE_CONNECTED,
        TRANSMIT_STATE_ERROR,
        _TRANSMIT_STATE_MAX,
        _TRANSMIT_STATE_INVALID = -1,
} TransmitState;

typedef struct NetworkTransmitConfig {
        char *server_address;       /* host:port of the server */
        int server_port;            /* default: 8080 */
        bool tls_enabled;           /* use TLS for connections */
        char *auth_token;           /* JWT auth token */

        /* Network tuning */
        int max_bandwidth_kbps;     /* 0 = unlimited */
        char *compression;          /* "none", "low", "medium", "high" */
        int retry_count;
        int retry_delay_seconds;
        bool use_metered;           /* transmit over metered connections */

        /* RTP settings for audio */
        int rtp_payload_type;       /* default: 111 (Opus) */
        int rtp_ssrc;               /* synchronization source ID */

        /* VNC/RFB settings for video */
        int vnc_encoding;           /* 0=Raw, 6=ZLib, 7=Tight */
        int vnc_quality;            /* 0-9, JPEG quality for Tight */
} NetworkTransmitConfig;

typedef struct TransmitStats {
        uint64_t bytes_sent;
        uint64_t packets_sent;
        uint64_t bytes_failed;
        uint64_t packets_failed;
        uint64_t retransmissions;
        double avg_latency_ms;
} TransmitStats;

int network_transmitter_new(
                NetworkTransmitter **ret,
                const NetworkTransmitConfig *config);
NetworkTransmitter* network_transmitter_free(NetworkTransmitter *t);

int network_transmitter_connect(NetworkTransmitter *t);
int network_transmitter_disconnect(NetworkTransmitter *t);

/* Send audio data using reverse RTP protocol.
 * The client acts as the RTP sender, pushing audio frames to the server.
 * Packets are formatted as: RTP header (12 bytes) + Opus payload.
 * "Reverse" because the client initiates the connection to the server
 * (instead of the traditional model where the server sends media). */
int network_transmit_audio_rtp(
                NetworkTransmitter *t,
                const uint8_t *data,
                size_t len,
                int sample_rate,
                int channels,
                uint32_t timestamp);

/* Send video frame using reverse VNC (RFB) protocol.
 * The client acts as the VNC server, pushing framebuffer updates to
 * the server (which acts as a VNC client/viewer).
 * Frames are sent as RFB FramebufferUpdate messages.
 * "Reverse" because the desktop owner pushes frames to the remote
 * storage/processing server, inverting the traditional VNC direction. */
int network_transmit_video_vnc(
                NetworkTransmitter *t,
                const uint8_t *data,
                size_t len,
                int width,
                int height,
                uint32_t frame_number);

/* Bulk upload stored capture data via HTTPS POST to /api/v1/ingest */
int network_transmit_bulk(
                NetworkTransmitter *t,
                int capture_type,
                const uint8_t *data,
                size_t len,
                int duration_ms,
                const char *source);

TransmitState network_transmitter_get_state(const NetworkTransmitter *t);
int network_transmitter_get_stats(const NetworkTransmitter *t, TransmitStats *ret_stats);

const char* transmit_protocol_to_string(TransmitProtocol proto);
TransmitProtocol transmit_protocol_from_string(const char *s);
const char* transmit_state_to_string(TransmitState state);
