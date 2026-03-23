/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

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
} CaptureMetadata;

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
                int *ret_count);

int storage_get_metadata(StorageHandle *handle, int64_t id, CaptureMetadata *ret_meta);

void storage_metadata_free(CaptureMetadata *meta, int count);

int storage_enforce_retention(StorageHandle *handle, int retention_days);
int storage_enforce_quota(StorageHandle *handle, size_t max_bytes);

int64_t storage_get_total_size(StorageHandle *handle);
int storage_get_capture_count(StorageHandle *handle);

const char* capture_type_to_string(CaptureType type);
CaptureType capture_type_from_string(const char *s);
