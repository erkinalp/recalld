/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <sqlite3.h>

typedef struct StorageHandle StorageHandle;

typedef enum CaptureType {
        CAPTURE_AUDIO,
        CAPTURE_VIDEO,
        CAPTURE_SCREENSHOT,
        _CAPTURE_TYPE_MAX,
        _CAPTURE_TYPE_INVALID = -1,
} CaptureType;

typedef struct CaptureMetadata {
        int64_t id;
        CaptureType type;
        time_t timestamp;
        int duration_ms;
        size_t data_size;
        char *source;
        char *checksum;
        bool encrypted;
        bool ai_processed;
} CaptureMetadata;

/* Result from full-text search queries */
typedef struct SearchResult {
        int64_t capture_id;
        CaptureType type;
        time_t timestamp;
        int duration_ms;
        size_t data_size;
        char *source;
        char *transcript;
        char *ocr_text;
        char *summary;
        double rank;    /* FTS5 rank score (negative = more relevant) */
} SearchResult;

int storage_open(StorageHandle **ret_handle, const char *db_path, const char *data_dir);
void storage_close(StorageHandle *handle);

int storage_store(
                StorageHandle *handle,
                CaptureType type,
                const uint8_t *data,
                size_t data_len,
                const char *source,
                int duration_ms);

int storage_retrieve(
                StorageHandle *handle,
                int64_t id,
                uint8_t **ret_data,
                size_t *ret_data_len);

int storage_delete(StorageHandle *handle, int64_t id);

int storage_list(
                StorageHandle *handle,
                CaptureType type,
                time_t from,
                time_t to,
                CaptureMetadata **ret_entries,
                int *ret_count,
                int limit);

/* Full-text search across transcript, ocr_text, and summary fields.
 * Joins FTS5 results back to captures for metadata.  Respects optional
 * type / time-range filters.  Results are ordered by FTS5 rank. */
int storage_search(
                StorageHandle *handle,
                const char *query_text,
                CaptureType type,
                time_t from,
                time_t to,
                SearchResult **ret_results,
                int *ret_count,
                int limit);

/* Update the AI-generated text fields and sync the FTS5 index. */
int storage_update_text_fields(
                StorageHandle *handle,
                int64_t id,
                const char *transcript,
                const char *ocr_text,
                const char *summary);

/* List captures that have not yet been processed by the AI pipeline. */
int storage_list_unprocessed(
                StorageHandle *handle,
                CaptureMetadata **ret_entries,
                int *ret_count,
                int limit);

void storage_search_result_free(SearchResult *results, int count);

int storage_get_metadata(StorageHandle *handle, int64_t id, CaptureMetadata *ret_meta);

void storage_metadata_free(CaptureMetadata *meta, int count);

sqlite3* storage_get_db(StorageHandle *handle);

int storage_enforce_retention(StorageHandle *handle, int retention_days);
int storage_enforce_quota(StorageHandle *handle, size_t max_bytes);

int64_t storage_get_total_size(StorageHandle *handle);
int storage_get_capture_count(StorageHandle *handle);

const char* capture_type_to_string(CaptureType type);
CaptureType capture_type_from_string(const char *s);
