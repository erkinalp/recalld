/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "common/recalld-config.h"
#include "server/query-service.h"

/* Server-side streaming protocol receiver.
 *
 * Listens on a dedicated TCP port for raw streaming connections from
 * recalld clients.  Each client connection is auto-detected as one of:
 *
 *   - Reverse RTP  (audio) — 12-byte RTP header, payload type identifies codec
 *   - Reverse VNC  (video) — RFB FramebufferUpdate messages
 *   - Bulk ingest  (any)   — "RCLD" magic header + binary payload
 *
 * Received data is fed into query_service_ingest() so it becomes
 * immediately queryable through the HTTP /api/v1/query endpoint. */

typedef struct StreamReceiver StreamReceiver;

int stream_receiver_new(
                StreamReceiver **ret,
                const QueryServiceConfig *config,
                QueryService *query_svc);

StreamReceiver* stream_receiver_free(StreamReceiver *recv);

int stream_receiver_start(StreamReceiver *recv);
int stream_receiver_stop(StreamReceiver *recv);

bool stream_receiver_is_running(const StreamReceiver *recv);
