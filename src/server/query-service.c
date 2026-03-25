/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "server/query-service.h"
#include "common/recalld-log.h"
#include "common/recalld-storage.h"

struct QueryService {
        QueryServiceConfig config;
        StorageHandle *storage;
        bool running;
};

int query_service_new(QueryService **ret, const QueryServiceConfig *config) {
        QueryService *svc;
        int r;

        if (!ret || !config)
                return -EINVAL;

        svc = calloc(1, sizeof(QueryService));
        if (!svc)
                return log_oom(), -ENOMEM;

        /* Copy configuration — we only reference strings from config, which must outlive us */
        svc->config = *config;

        r = storage_open(&svc->storage, config->data_path, config->data_path);
        if (r < 0) {
                log_error_errno(-r, "Failed to open query service storage: %m");
                free(svc);
                return r;
        }

        log_info("Query service initialized.");
        *ret = svc;
        return 0;
}

QueryService* query_service_free(QueryService *svc) {
        if (!svc)
                return NULL;

        if (svc->running)
                query_service_stop(svc);

        storage_close(svc->storage);
        free(svc);
        return NULL;
}

int query_service_start(QueryService *svc) {
        if (!svc)
                return -EINVAL;
        if (svc->running)
                return -EALREADY;

        svc->running = true;
        log_info("Query service started.");
        return 0;
}

int query_service_stop(QueryService *svc) {
        if (!svc)
                return -EINVAL;
        if (!svc->running)
                return 0;

        svc->running = false;
        log_info("Query service stopped.");
        return 0;
}

/* Convert an FTS5 rank score (negative, closer to 0 = more relevant) to
 * a 0.0–1.0 confidence value.  We use 1/(1+|rank|) which maps the
 * entire negative half-line onto (0, 1]. */
static double rank_to_confidence(double rank) {
        return 1.0 / (1.0 + fabs(rank));
}

int query_service_process(
                QueryService *svc,
                const QueryRequest *req,
                QueryResponse *ret_resp) {

        struct timespec ts_start, ts_end;
        double elapsed;
        int r;

        if (!svc || !req || !ret_resp)
                return -EINVAL;
        if (!svc->running)
                return -ENOTCONN;

        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        memset(ret_resp, 0, sizeof(*ret_resp));

        /* ---- Full-text search path ---- */
        if (req->query_text && req->query_text[0] != '\0') {
                SearchResult *search_results = NULL;
                int search_count = 0;

                /* Determine the type filter: _CAPTURE_TYPE_INVALID (-1) means "any" */
                CaptureType type_filter = _CAPTURE_TYPE_INVALID;
                if (req->content_types) {
                        /* If exactly one type is requested, pass it through.
                         * For multiple types or "all", use -1. */
                        int n_types = 0;
                        CaptureType last = _CAPTURE_TYPE_INVALID;
                        for (int t = 0; t < _CAPTURE_TYPE_MAX; t++) {
                                const char *ts = capture_type_to_string((CaptureType) t);
                                if (strstr(req->content_types, ts)) {
                                        last = (CaptureType) t;
                                        n_types++;
                                }
                        }
                        if (n_types == 1)
                                type_filter = last;
                }

                r = storage_search(svc->storage, req->query_text,
                                   type_filter,
                                   req->time_from, req->time_to,
                                   &search_results, &search_count,
                                   req->max_results > 0 ? req->max_results : 0);
                if (r < 0) {
                        log_warning_errno(-r, "Full-text search failed: %m");
                        /* Fall through to time-based listing below */
                        goto time_based;
                }

                if (search_count > 0) {
                        ret_resp->results = calloc((size_t) search_count, sizeof(QueryResult));
                        if (!ret_resp->results) {
                                storage_search_result_free(search_results, search_count);
                                return log_oom(), -ENOMEM;
                        }

                        for (int i = 0; i < search_count; i++) {
                                QueryResult *qr = &ret_resp->results[i];
                                qr->capture_id = search_results[i].capture_id;
                                qr->confidence = rank_to_confidence(search_results[i].rank);
                                qr->summary = search_results[i].summary ? strdup(search_results[i].summary) : NULL;
                                qr->content_type = strdup(capture_type_to_string(search_results[i].type));
                                qr->timestamp = search_results[i].timestamp;
                                qr->duration_ms = search_results[i].duration_ms;
                        }

                        ret_resp->result_count = search_count;
                }

                storage_search_result_free(search_results, search_count);
                ret_resp->total_matches = ret_resp->result_count;
                goto finish;
        }

time_based:
        /* ---- Time-range listing path (original behaviour) ---- */
        for (int t = 0; t < _CAPTURE_TYPE_MAX; t++) {
                CaptureMetadata *type_entries = NULL;
                int type_count = 0;

                if (req->content_types) {
                        const char *type_str = capture_type_to_string((CaptureType) t);
                        if (!strstr(req->content_types, type_str))
                                continue;
                }

                r = storage_list(svc->storage, (CaptureType) t,
                                 req->time_from, req->time_to,
                                 &type_entries, &type_count,
                                 req->max_results);
                if (r < 0) {
                        log_warning_errno(-r, "Failed to query %s captures: %m",
                                          capture_type_to_string((CaptureType) t));
                        continue;
                }

                /* Convert metadata to query results */
                if (type_count > 0) {
                        int new_total = ret_resp->result_count + type_count;
                        QueryResult *new_results = realloc(ret_resp->results,
                                        (size_t) new_total * sizeof(QueryResult));
                        if (!new_results) {
                                storage_metadata_free(type_entries, type_count);
                                query_response_free(ret_resp);
                                return log_oom(), -ENOMEM;
                        }
                        ret_resp->results = new_results;

                        for (int i = 0; i < type_count; i++) {
                                QueryResult *qr = &ret_resp->results[ret_resp->result_count + i];
                                qr->capture_id = type_entries[i].id;
                                qr->confidence = 1.0; /* exact match for time-based queries */
                                qr->summary = NULL;
                                qr->content_type = strdup(capture_type_to_string(type_entries[i].type));
                                qr->timestamp = type_entries[i].timestamp;
                                qr->duration_ms = type_entries[i].duration_ms;
                        }

                        ret_resp->result_count = new_total;
                }

                storage_metadata_free(type_entries, type_count);
        }

        ret_resp->total_matches = ret_resp->result_count;

        /* Apply max_results limit */
        if (req->max_results > 0 && ret_resp->result_count > req->max_results)
                ret_resp->result_count = req->max_results;

finish:
        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        elapsed = (double)(ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                  (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;
        ret_resp->processing_time_ms = elapsed;

        log_info("Query processed: %d results in %.1f ms.", ret_resp->result_count, elapsed);
        return 0;
}

int query_service_ingest(
                QueryService *svc,
                CaptureType type,
                const uint8_t *data,
                size_t data_len,
                int duration_ms,
                const char *source) {

        int r;

        if (!svc || !data || data_len == 0)
                return -EINVAL;
        if (!svc->running)
                return -ENOTCONN;
        if (type < 0 || type >= _CAPTURE_TYPE_MAX)
                return -EINVAL;

        r = storage_store(svc->storage, type, data, data_len,
                          source ? source : "remote-ingest", duration_ms);
        if (r < 0) {
                log_error_errno(-r, "Failed to ingest %s data (%zu bytes): %m",
                                capture_type_to_string(type), data_len);
                return r;
        }

        log_info("Ingested %s data: %zu bytes, %d ms from '%s'.",
                 capture_type_to_string(type), data_len, duration_ms,
                 source ? source : "remote-ingest");
        return 0;
}

StorageHandle* query_service_get_storage(QueryService *svc) {
        return svc ? svc->storage : NULL;
}

void query_response_free(QueryResponse *resp) {
        if (!resp)
                return;

        for (int i = 0; i < resp->result_count; i++) {
                free(resp->results[i].summary);
                free(resp->results[i].content_type);
        }

        free(resp->results);
        memset(resp, 0, sizeof(*resp));
}

void query_request_free(QueryRequest *req) {
        if (!req)
                return;

        free(req->query_text);
        free(req->content_types);
}
