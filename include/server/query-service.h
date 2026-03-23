/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "common/recalld-config.h"
#include "common/recalld-storage.h"

typedef struct QueryService QueryService;

typedef struct QueryRequest {
        char *query_text;
        time_t time_from;
        time_t time_to;
        char *content_types;    /* comma-separated: "audio,video,screenshot" */
        int max_results;
        double confidence_threshold;
} QueryRequest;

typedef struct QueryResult {
        int64_t capture_id;
        double confidence;
        char *summary;
        char *content_type;
        time_t timestamp;
        int duration_ms;
} QueryResult;

typedef struct QueryResponse {
        QueryResult *results;
        int result_count;
        int total_matches;
        double processing_time_ms;
} QueryResponse;

int query_service_new(QueryService **ret, const QueryServiceConfig *config);
QueryService* query_service_free(QueryService *svc);

int query_service_start(QueryService *svc);
int query_service_stop(QueryService *svc);

int query_service_process(
                QueryService *svc,
                const QueryRequest *req,
                QueryResponse *ret_resp);

void query_response_free(QueryResponse *resp);
void query_request_free(QueryRequest *req);
