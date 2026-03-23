/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "common/recalld-config.h"
#include "server/query-service.h"
#include "server/jwt.h"

typedef struct HttpServer HttpServer;

int http_server_new(
                HttpServer **ret,
                const QueryServiceConfig *config,
                QueryService *query_svc,
                JwtContext *jwt);

HttpServer* http_server_free(HttpServer *srv);

int http_server_start(HttpServer *srv);
int http_server_stop(HttpServer *srv);

bool http_server_is_running(const HttpServer *srv);
