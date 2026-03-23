/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "common/recalld-storage.h"
#include "common/recalld-log.h"

struct StorageHandle {
        sqlite3 *db;
        char *data_dir;
};

static const char *capture_type_table[] = {
        [CAPTURE_AUDIO]      = "audio",
        [CAPTURE_VIDEO]      = "video",
        [CAPTURE_SCREENSHOT] = "screenshot",
};

const char* capture_type_to_string(CaptureType type) {
        if (type >= 0 && type < _CAPTURE_TYPE_MAX)
                return capture_type_table[type];
        return NULL;
}

CaptureType capture_type_from_string(const char *s) {
        if (!s)
                return _CAPTURE_TYPE_INVALID;

        for (int i = 0; i < _CAPTURE_TYPE_MAX; i++)
                if (strcmp(s, capture_type_table[i]) == 0)
                        return (CaptureType) i;

        return _CAPTURE_TYPE_INVALID;
}

static int storage_init_db(sqlite3 *db) {
        const char *sql =
                "CREATE TABLE IF NOT EXISTS captures ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "  type INTEGER NOT NULL,"
                "  timestamp INTEGER NOT NULL,"
                "  duration_ms INTEGER DEFAULT 0,"
                "  data_size INTEGER NOT NULL,"
                "  source TEXT,"
                "  checksum TEXT,"
                "  encrypted INTEGER DEFAULT 0,"
                "  data_path TEXT NOT NULL"
                ");"
                "CREATE INDEX IF NOT EXISTS idx_captures_timestamp ON captures(timestamp);"
                "CREATE INDEX IF NOT EXISTS idx_captures_type ON captures(type);"
                "CREATE TABLE IF NOT EXISTS metadata ("
                "  key TEXT PRIMARY KEY,"
                "  value TEXT"
                ");";
        char *err_msg = NULL;
        int r;

        r = sqlite3_exec(db, sql, /* callback= */ NULL, /* arg= */ NULL, &err_msg);
        if (r != SQLITE_OK) {
                log_error("Failed to initialize database: %s", err_msg);
                sqlite3_free(err_msg);
                return -EIO;
        }

        return 0;
}

int storage_open(StorageHandle **ret_handle, const char *db_path, const char *data_dir) {
        StorageHandle *h;
        char db_file[4096];
        int r;

        if (!ret_handle || !db_path || !data_dir)
                return -EINVAL;

        h = calloc(1, sizeof(StorageHandle));
        if (!h)
                return log_oom(), -ENOMEM;

        h->data_dir = strdup(data_dir);
        if (!h->data_dir) {
                free(h);
                return log_oom(), -ENOMEM;
        }

        /* Create data directory if it does not exist */
        (void) mkdir(data_dir, 0700);

        snprintf(db_file, sizeof(db_file), "%s/recalld.db", db_path);
        (void) mkdir(db_path, 0700);

        r = sqlite3_open(db_file, &h->db);
        if (r != SQLITE_OK) {
                log_error("Failed to open database %s: %s", db_file, sqlite3_errmsg(h->db));
                free(h->data_dir);
                free(h);
                return -EIO;
        }

        /* Enable WAL mode for better concurrent access */
        sqlite3_exec(h->db, "PRAGMA journal_mode=WAL;", /* callback= */ NULL, /* arg= */ NULL, /* errmsg= */ NULL);
        sqlite3_exec(h->db, "PRAGMA synchronous=NORMAL;", /* callback= */ NULL, /* arg= */ NULL, /* errmsg= */ NULL);

        r = storage_init_db(h->db);
        if (r < 0) {
                sqlite3_close(h->db);
                free(h->data_dir);
                free(h);
                return r;
        }

        *ret_handle = h;
        return 0;
}

void storage_close(StorageHandle *handle) {
        if (!handle)
                return;

        if (handle->db)
                sqlite3_close(handle->db);

        free(handle->data_dir);
        free(handle);
}

int storage_store(
                StorageHandle *handle,
                CaptureType type,
                const uint8_t *data,
                size_t data_len,
                const char *source,
                int duration_ms) {

        sqlite3_stmt *stmt = NULL;
        char data_path[4096];
        char id_str[64];
        time_t now;
        FILE *f;
        int r;

        if (!handle || !data)
                return -EINVAL;

        now = time(NULL);
        snprintf(id_str, sizeof(id_str), "%ld_%d", (long) now, rand());
        snprintf(data_path, sizeof(data_path), "%s/%s_%s.dat",
                 handle->data_dir, capture_type_to_string(type), id_str);

        /* Write data to file */
        f = fopen(data_path, "wbe");
        if (!f)
                return log_error_errno(errno, "Failed to create data file %s: %m", data_path);

        if (fwrite(data, 1, data_len, f) != data_len) {
                fclose(f);
                unlink(data_path);
                return log_error_errno(errno, "Failed to write data file: %m");
        }
        fclose(f);

        /* Insert metadata into database */
        r = sqlite3_prepare_v2(handle->db,
                "INSERT INTO captures (type, timestamp, duration_ms, data_size, source, data_path) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK) {
                unlink(data_path);
                return log_error_errno(EIO, "Failed to prepare SQL statement: %s",
                                       sqlite3_errmsg(handle->db));
        }

        sqlite3_bind_int(stmt, 1, (int) type);
        sqlite3_bind_int64(stmt, 2, (int64_t) now);
        sqlite3_bind_int(stmt, 3, duration_ms);
        sqlite3_bind_int64(stmt, 4, (int64_t) data_len);
        sqlite3_bind_text(stmt, 5, source ? source : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, data_path, -1, SQLITE_STATIC);

        r = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (r != SQLITE_DONE) {
                unlink(data_path);
                return log_error_errno(EIO, "Failed to insert capture record: %s",
                                       sqlite3_errmsg(handle->db));
        }

        log_debug("Stored %s capture: %zu bytes, duration %d ms",
                  capture_type_to_string(type), data_len, duration_ms);

        return 0;
}

int storage_retrieve(
                StorageHandle *handle,
                int64_t id,
                uint8_t **ret_data,
                size_t *ret_data_len) {

        sqlite3_stmt *stmt = NULL;
        const char *data_path;
        FILE *f;
        uint8_t *buf;
        long file_size;
        int r;

        if (!handle || !ret_data || !ret_data_len)
                return -EINVAL;

        r = sqlite3_prepare_v2(handle->db,
                "SELECT data_path FROM captures WHERE id = ?",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return -EIO;

        sqlite3_bind_int64(stmt, 1, id);

        r = sqlite3_step(stmt);
        if (r != SQLITE_ROW) {
                sqlite3_finalize(stmt);
                return -ENOENT;
        }

        data_path = (const char*) sqlite3_column_text(stmt, 0);

        f = fopen(data_path, "rbe");
        if (!f) {
                sqlite3_finalize(stmt);
                return -errno;
        }

        fseek(f, 0, SEEK_END);
        file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        buf = malloc(file_size);
        if (!buf) {
                fclose(f);
                sqlite3_finalize(stmt);
                return log_oom(), -ENOMEM;
        }

        if ((long) fread(buf, 1, file_size, f) != file_size) {
                free(buf);
                fclose(f);
                sqlite3_finalize(stmt);
                return -EIO;
        }

        fclose(f);
        sqlite3_finalize(stmt);

        *ret_data = buf;
        *ret_data_len = (size_t) file_size;
        return 0;
}

int storage_delete(StorageHandle *handle, int64_t id) {
        sqlite3_stmt *stmt = NULL;
        const char *data_path;
        int r;

        if (!handle)
                return -EINVAL;

        /* Get data path first */
        r = sqlite3_prepare_v2(handle->db,
                "SELECT data_path FROM captures WHERE id = ?",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return -EIO;

        sqlite3_bind_int64(stmt, 1, id);

        r = sqlite3_step(stmt);
        if (r == SQLITE_ROW) {
                data_path = (const char*) sqlite3_column_text(stmt, 0);
                (void) unlink(data_path);
        }
        sqlite3_finalize(stmt);

        /* Delete from database */
        r = sqlite3_prepare_v2(handle->db,
                "DELETE FROM captures WHERE id = ?",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return -EIO;

        sqlite3_bind_int64(stmt, 1, id);
        r = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return r == SQLITE_DONE ? 0 : -EIO;
}

int storage_list(
                StorageHandle *handle,
                CaptureType type,
                time_t from,
                time_t to,
                CaptureMetadata **ret_entries,
                int *ret_count) {

        sqlite3_stmt *stmt = NULL;
        CaptureMetadata *entries = NULL;
        int count = 0, capacity = 64;
        int r;

        if (!handle || !ret_entries || !ret_count)
                return -EINVAL;

        entries = calloc(capacity, sizeof(CaptureMetadata));
        if (!entries)
                return log_oom(), -ENOMEM;

        r = sqlite3_prepare_v2(handle->db,
                "SELECT id, type, timestamp, duration_ms, data_size, source, checksum, encrypted "
                "FROM captures WHERE type = ? AND timestamp >= ? AND timestamp <= ? "
                "ORDER BY timestamp DESC",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK) {
                free(entries);
                return -EIO;
        }

        sqlite3_bind_int(stmt, 1, (int) type);
        sqlite3_bind_int64(stmt, 2, (int64_t) from);
        sqlite3_bind_int64(stmt, 3, (int64_t)(to > 0 ? to : time(NULL)));

        while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (count >= capacity) {
                        CaptureMetadata *tmp;
                        capacity *= 2;
                        tmp = realloc(entries, capacity * sizeof(CaptureMetadata));
                        if (!tmp) {
                                storage_metadata_free(entries, count);
                                sqlite3_finalize(stmt);
                                return log_oom(), -ENOMEM;
                        }
                        entries = tmp;
                }

                CaptureMetadata *m = &entries[count];
                m->id = sqlite3_column_int64(stmt, 0);
                m->type = (CaptureType) sqlite3_column_int(stmt, 1);
                m->timestamp = (time_t) sqlite3_column_int64(stmt, 2);
                m->duration_ms = sqlite3_column_int(stmt, 3);
                m->data_size = (size_t) sqlite3_column_int64(stmt, 4);

                const char *src = (const char*) sqlite3_column_text(stmt, 5);
                m->source = src ? strdup(src) : NULL;

                const char *cksum = (const char*) sqlite3_column_text(stmt, 6);
                m->checksum = cksum ? strdup(cksum) : NULL;

                m->encrypted = sqlite3_column_int(stmt, 7);
                count++;
        }

        sqlite3_finalize(stmt);

        *ret_entries = entries;
        *ret_count = count;
        return 0;
}

int storage_get_metadata(StorageHandle *handle, int64_t id, CaptureMetadata *ret_meta) {
        sqlite3_stmt *stmt = NULL;
        int r;

        if (!handle || !ret_meta)
                return -EINVAL;

        r = sqlite3_prepare_v2(handle->db,
                "SELECT id, type, timestamp, duration_ms, data_size, source, checksum, encrypted "
                "FROM captures WHERE id = ?",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return -EIO;

        sqlite3_bind_int64(stmt, 1, id);

        r = sqlite3_step(stmt);
        if (r != SQLITE_ROW) {
                sqlite3_finalize(stmt);
                return -ENOENT;
        }

        ret_meta->id = sqlite3_column_int64(stmt, 0);
        ret_meta->type = (CaptureType) sqlite3_column_int(stmt, 1);
        ret_meta->timestamp = (time_t) sqlite3_column_int64(stmt, 2);
        ret_meta->duration_ms = sqlite3_column_int(stmt, 3);
        ret_meta->data_size = (size_t) sqlite3_column_int64(stmt, 4);

        const char *src = (const char*) sqlite3_column_text(stmt, 5);
        ret_meta->source = src ? strdup(src) : NULL;

        const char *cksum = (const char*) sqlite3_column_text(stmt, 6);
        ret_meta->checksum = cksum ? strdup(cksum) : NULL;

        ret_meta->encrypted = sqlite3_column_int(stmt, 7);

        sqlite3_finalize(stmt);
        return 0;
}

void storage_metadata_free(CaptureMetadata *meta, int count) {
        if (!meta)
                return;

        for (int i = 0; i < count; i++) {
                free(meta[i].source);
                free(meta[i].checksum);
        }

        free(meta);
}

int storage_enforce_retention(StorageHandle *handle, int retention_days) {
        sqlite3_stmt *stmt = NULL;
        time_t cutoff;
        int r;

        if (!handle || retention_days <= 0)
                return -EINVAL;

        cutoff = time(NULL) - ((time_t) retention_days * 24 * 60 * 60);

        /* Delete data files first */
        r = sqlite3_prepare_v2(handle->db,
                "SELECT id, data_path FROM captures WHERE timestamp < ?",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return -EIO;

        sqlite3_bind_int64(stmt, 1, (int64_t) cutoff);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *path = (const char*) sqlite3_column_text(stmt, 1);
                if (path)
                        (void) unlink(path);
        }
        sqlite3_finalize(stmt);

        /* Delete from database */
        r = sqlite3_prepare_v2(handle->db,
                "DELETE FROM captures WHERE timestamp < ?",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return -EIO;

        sqlite3_bind_int64(stmt, 1, (int64_t) cutoff);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        log_info("Retention enforcement complete, removed captures older than %d days.", retention_days);
        return 0;
}

int storage_enforce_quota(StorageHandle *handle, size_t max_bytes) {
        sqlite3_stmt *stmt = NULL;
        int64_t total;
        int r;

        if (!handle || max_bytes == 0)
                return -EINVAL;

        total = storage_get_total_size(handle);
        if (total < 0)
                return (int) total;

        if ((size_t) total <= max_bytes)
                return 0;

        /* Delete oldest captures until under quota */
        r = sqlite3_prepare_v2(handle->db,
                "SELECT id, data_path, data_size FROM captures ORDER BY timestamp ASC",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return -EIO;

        while (sqlite3_step(stmt) == SQLITE_ROW && (size_t) total > max_bytes) {
                int64_t id = sqlite3_column_int64(stmt, 0);
                const char *path = (const char*) sqlite3_column_text(stmt, 1);
                int64_t size = sqlite3_column_int64(stmt, 2);

                if (path)
                        (void) unlink(path);
                total -= size;

                /* We'll batch-delete from DB after */
                (void) storage_delete(handle, id);
        }

        sqlite3_finalize(stmt);

        log_info("Storage quota enforcement complete.");
        return 0;
}

int64_t storage_get_total_size(StorageHandle *handle) {
        sqlite3_stmt *stmt = NULL;
        int64_t total = 0;
        int r;

        if (!handle)
                return -EINVAL;

        r = sqlite3_prepare_v2(handle->db,
                "SELECT COALESCE(SUM(data_size), 0) FROM captures",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return -EIO;

        if (sqlite3_step(stmt) == SQLITE_ROW)
                total = sqlite3_column_int64(stmt, 0);

        sqlite3_finalize(stmt);
        return total;
}

int storage_get_capture_count(StorageHandle *handle) {
        sqlite3_stmt *stmt = NULL;
        int count = 0;
        int r;

        if (!handle)
                return -EINVAL;

        r = sqlite3_prepare_v2(handle->db,
                "SELECT COUNT(*) FROM captures",
                -1, &stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return -EIO;

        if (sqlite3_step(stmt) == SQLITE_ROW)
                count = sqlite3_column_int(stmt, 0);

        sqlite3_finalize(stmt);
        return count;
}
