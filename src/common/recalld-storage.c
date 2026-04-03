/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <inttypes.h>
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

        /* Prepared statement cache — prepared once in storage_open(),
         * reset+clear between uses. */
        sqlite3_stmt *stmt_store;
        sqlite3_stmt *stmt_list;
        sqlite3_stmt *stmt_search;
        sqlite3_stmt *stmt_update_text;
        sqlite3_stmt *stmt_list_unprocessed;
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
                "  data_path TEXT NOT NULL,"
                "  transcript TEXT,"
                "  ocr_text TEXT,"
                "  summary TEXT,"
                "  ai_processed INTEGER DEFAULT 0"
                ");"

                /* Composite index replaces the two individual ones */
                "DROP INDEX IF EXISTS idx_captures_timestamp;"
                "DROP INDEX IF EXISTS idx_captures_type;"
                "CREATE INDEX IF NOT EXISTS idx_captures_type_timestamp "
                "  ON captures(type, timestamp DESC);"

                /* FTS5 full-text search index */
                "CREATE VIRTUAL TABLE IF NOT EXISTS captures_fts USING fts5("
                "  transcript, ocr_text, summary,"
                "  content='captures', content_rowid='id'"
                ");"

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

        /* Migrate existing databases: add new columns if they don't exist.
         * ALTER TABLE ... ADD COLUMN is a no-op-safe idiom in SQLite — it
         * returns SQLITE_ERROR when the column already exists, which we
         * silently ignore. */
        sqlite3_exec(db, "ALTER TABLE captures ADD COLUMN transcript TEXT;",
                     /* callback= */ NULL, /* arg= */ NULL, /* errmsg= */ NULL);
        sqlite3_exec(db, "ALTER TABLE captures ADD COLUMN ocr_text TEXT;",
                     /* callback= */ NULL, /* arg= */ NULL, /* errmsg= */ NULL);
        sqlite3_exec(db, "ALTER TABLE captures ADD COLUMN summary TEXT;",
                     /* callback= */ NULL, /* arg= */ NULL, /* errmsg= */ NULL);
        sqlite3_exec(db, "ALTER TABLE captures ADD COLUMN ai_processed INTEGER DEFAULT 0;",
                     /* callback= */ NULL, /* arg= */ NULL, /* errmsg= */ NULL);

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
        sqlite3_exec(h->db, "PRAGMA mmap_size=268435456;", /* callback= */ NULL, /* arg= */ NULL, /* errmsg= */ NULL);

        r = storage_init_db(h->db);
        if (r < 0) {
                sqlite3_close(h->db);
                free(h->data_dir);
                free(h);
                return r;
        }

        /* Prepare cached statements */
        sqlite3_prepare_v2(h->db,
                "INSERT INTO captures (type, timestamp, duration_ms, data_size, source, data_path) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                -1, &h->stmt_store, /* tail= */ NULL);

        sqlite3_prepare_v2(h->db,
                "SELECT id, type, timestamp, duration_ms, data_size, source, checksum, encrypted "
                "FROM captures WHERE type = ? AND timestamp >= ? AND timestamp <= ? "
                "ORDER BY timestamp DESC LIMIT ?",
                -1, &h->stmt_list, /* tail= */ NULL);

        sqlite3_prepare_v2(h->db,
                "SELECT c.id, c.type, c.timestamp, c.duration_ms, c.data_size, c.source, "
                "       c.transcript, c.ocr_text, c.summary, f.rank "
                "FROM captures_fts f "
                "JOIN captures c ON c.id = f.rowid "
                "WHERE captures_fts MATCH ? "
                "  AND (? < 0 OR c.type = ?) "
                "  AND c.timestamp >= ? AND c.timestamp <= ? "
                "ORDER BY f.rank "
                "LIMIT ?",
                -1, &h->stmt_search, /* tail= */ NULL);

        sqlite3_prepare_v2(h->db,
                "UPDATE captures SET transcript = ?, ocr_text = ?, summary = ?, ai_processed = 1 "
                "WHERE id = ?",
                -1, &h->stmt_update_text, /* tail= */ NULL);

        sqlite3_prepare_v2(h->db,
                "SELECT id, type, timestamp, duration_ms, data_size, source, checksum, encrypted "
                "FROM captures WHERE ai_processed = 0 ORDER BY timestamp ASC LIMIT ?",
                -1, &h->stmt_list_unprocessed, /* tail= */ NULL);

        *ret_handle = h;
        return 0;
}

void storage_close(StorageHandle *handle) {
        if (!handle)
                return;

        sqlite3_finalize(handle->stmt_store);
        sqlite3_finalize(handle->stmt_list);
        sqlite3_finalize(handle->stmt_search);
        sqlite3_finalize(handle->stmt_update_text);
        sqlite3_finalize(handle->stmt_list_unprocessed);

        if (handle->db)
                sqlite3_close(handle->db);

        free(handle->data_dir);
        free(handle);
}

sqlite3* storage_get_db(StorageHandle *handle) {
        return handle ? handle->db : NULL;
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

        /* Insert metadata into database using the cached statement */
        stmt = handle->stmt_store;
        if (!stmt) {
                unlink(data_path);
                return log_error_errno(EIO, "Prepared statement not available");
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_int(stmt, 1, (int) type);
        sqlite3_bind_int64(stmt, 2, (int64_t) now);
        sqlite3_bind_int(stmt, 3, duration_ms);
        sqlite3_bind_int64(stmt, 4, (int64_t) data_len);
        sqlite3_bind_text(stmt, 5, source ? source : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, data_path, -1, SQLITE_STATIC);

        r = sqlite3_step(stmt);

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
                int *ret_count,
                int limit) {

        sqlite3_stmt *stmt;
        CaptureMetadata *entries = NULL;
        int count = 0, capacity = 64;

        if (!handle || !ret_entries || !ret_count)
                return -EINVAL;

        entries = calloc(capacity, sizeof(CaptureMetadata));
        if (!entries)
                return log_oom(), -ENOMEM;

        stmt = handle->stmt_list;
        if (!stmt) {
                free(entries);
                return -EIO;
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_int(stmt, 1, (int) type);
        sqlite3_bind_int64(stmt, 2, (int64_t) from);
        sqlite3_bind_int64(stmt, 3, (int64_t)(to > 0 ? to : time(NULL)));
        sqlite3_bind_int(stmt, 4, limit > 0 ? limit : 1000000);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (count >= capacity) {
                        CaptureMetadata *tmp;
                        capacity *= 2;
                        tmp = realloc(entries, (size_t) capacity * sizeof(CaptureMetadata));
                        if (!tmp) {
                                storage_metadata_free(entries, count);
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

        *ret_entries = entries;
        *ret_count = count;
        return 0;
}

/* Sanitise user input for FTS5 MATCH.  Each whitespace-delimited token
 * is wrapped in double-quotes so that FTS5 operators (AND, OR, NOT,
 * NEAR, *, ^, etc.) and special characters are treated as literals.
 * Embedded double-quotes inside tokens are doubled ("") per FTS5 rules.
 *
 * Returns a heap-allocated sanitised string, or NULL on OOM.
 * The caller must free the result. */
static char* sanitize_fts5_query(const char *raw) {
        size_t raw_len, out_cap, out_len;
        char *out;
        const char *p;

        if (!raw || !*raw)
                return NULL;

        raw_len = strlen(raw);
        /* Worst case: every char is a '"' => doubled, plus quotes around
         * each single-char token, plus spaces.  4x + 3 is generous. */
        out_cap = raw_len * 4 + 3;
        out = malloc(out_cap);
        if (!out)
                return NULL;

        out_len = 0;
        p = raw;

        while (*p) {
                /* Skip leading whitespace */
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                        p++;
                if (!*p)
                        break;

                /* Separate tokens with a space (implicit AND in FTS5) */
                if (out_len > 0)
                        out[out_len++] = ' ';

                /* Opening quote */
                out[out_len++] = '"';

                /* Copy token characters, doubling any embedded quotes */
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                        if (*p == '"') {
                                out[out_len++] = '"';
                                out[out_len++] = '"';
                        } else {
                                out[out_len++] = *p;
                        }
                        p++;

                        /* Grow buffer if needed */
                        if (out_len + 4 >= out_cap) {
                                out_cap *= 2;
                                char *tmp = realloc(out, out_cap);
                                if (!tmp) {
                                        free(out);
                                        return NULL;
                                }
                                out = tmp;
                        }
                }

                /* Closing quote */
                out[out_len++] = '"';
        }

        out[out_len] = '\0';
        return out;
}

int storage_search(
                StorageHandle *handle,
                const char *query_text,
                CaptureType type,
                time_t from,
                time_t to,
                SearchResult **ret_results,
                int *ret_count,
                int limit) {

        sqlite3_stmt *stmt;
        SearchResult *results = NULL;
        int count = 0, capacity = 64;
        char *safe_query = NULL;

        if (!handle || !query_text || !ret_results || !ret_count)
                return -EINVAL;

        /* Sanitise the raw user query to prevent FTS5 syntax errors */
        safe_query = sanitize_fts5_query(query_text);
        if (!safe_query)
                return log_oom(), -ENOMEM;

        results = calloc(capacity, sizeof(SearchResult));
        if (!results) {
                free(safe_query);
                return log_oom(), -ENOMEM;
        }

        stmt = handle->stmt_search;
        if (!stmt) {
                free(results);
                free(safe_query);
                return -EIO;
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_text(stmt, 1, safe_query, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, (int) type);  /* -1 = any type */
        sqlite3_bind_int(stmt, 3, (int) type);
        sqlite3_bind_int64(stmt, 4, (int64_t) from);
        sqlite3_bind_int64(stmt, 5, (int64_t)(to > 0 ? to : time(NULL)));
        sqlite3_bind_int(stmt, 6, limit > 0 ? limit : 1000000);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (count >= capacity) {
                        SearchResult *tmp;
                        capacity *= 2;
                        tmp = realloc(results, (size_t) capacity * sizeof(SearchResult));
                                if (!tmp) {
                                        storage_search_result_free(results, count);
                                        free(safe_query);
                                        return log_oom(), -ENOMEM;
                                }
                        results = tmp;
                }

                SearchResult *sr = &results[count];
                sr->capture_id = sqlite3_column_int64(stmt, 0);
                sr->type = (CaptureType) sqlite3_column_int(stmt, 1);
                sr->timestamp = (time_t) sqlite3_column_int64(stmt, 2);
                sr->duration_ms = sqlite3_column_int(stmt, 3);
                sr->data_size = (size_t) sqlite3_column_int64(stmt, 4);

                const char *src = (const char*) sqlite3_column_text(stmt, 5);
                sr->source = src ? strdup(src) : NULL;

                const char *txt = (const char*) sqlite3_column_text(stmt, 6);
                sr->transcript = txt ? strdup(txt) : NULL;

                const char *ocr = (const char*) sqlite3_column_text(stmt, 7);
                sr->ocr_text = ocr ? strdup(ocr) : NULL;

                const char *sum = (const char*) sqlite3_column_text(stmt, 8);
                sr->summary = sum ? strdup(sum) : NULL;

                sr->rank = sqlite3_column_double(stmt, 9);
                count++;
        }

        free(safe_query);

        *ret_results = results;
        *ret_count = count;
        return 0;
}

int storage_update_text_fields(
                StorageHandle *handle,
                int64_t id,
                const char *transcript,
                const char *ocr_text,
                const char *summary) {

        sqlite3_stmt *stmt;
        char *err_msg = NULL;
        int r;

        if (!handle)
                return -EINVAL;

        /* Update the main table */
        stmt = handle->stmt_update_text;
        if (!stmt)
                return -EIO;

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_text(stmt, 1, transcript, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, ocr_text, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, summary, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, id);

        r = sqlite3_step(stmt);
        if (r != SQLITE_DONE)
                return log_error_errno(EIO, "Failed to update text fields for capture %" PRId64 ": %s",
                                       id, sqlite3_errmsg(handle->db)), -EIO;

        /* Sync the FTS5 index: delete stale entry then re-insert */
        r = sqlite3_exec(handle->db,
                "INSERT INTO captures_fts(captures_fts, rowid, transcript, ocr_text, summary) "
                "SELECT 'delete', id, transcript, ocr_text, summary FROM captures WHERE id = 0;",
                /* callback= */ NULL, /* arg= */ NULL, &err_msg);
        /* Ignore errors — the row may not exist in the FTS index yet */
        sqlite3_free(err_msg);
        err_msg = NULL;

        /* Build a one-shot INSERT ... SELECT to populate FTS from the
         * just-updated row in captures. */
        sqlite3_stmt *fts_stmt = NULL;
        r = sqlite3_prepare_v2(handle->db,
                "INSERT INTO captures_fts(rowid, transcript, ocr_text, summary) "
                "SELECT id, transcript, ocr_text, summary FROM captures WHERE id = ?",
                -1, &fts_stmt, /* tail= */ NULL);
        if (r != SQLITE_OK)
                return log_error_errno(EIO, "Failed to prepare FTS sync: %s",
                                       sqlite3_errmsg(handle->db)), -EIO;

        sqlite3_bind_int64(fts_stmt, 1, id);
        r = sqlite3_step(fts_stmt);
        sqlite3_finalize(fts_stmt);

        if (r != SQLITE_DONE)
                return log_error_errno(EIO, "Failed to sync FTS index for capture %" PRId64 ": %s",
                                       id, sqlite3_errmsg(handle->db)), -EIO;

        log_debug("Updated text fields and FTS index for capture %" PRId64 ".", id);
        return 0;
}

int storage_list_unprocessed(
                StorageHandle *handle,
                CaptureMetadata **ret_entries,
                int *ret_count,
                int limit) {

        sqlite3_stmt *stmt;
        CaptureMetadata *entries = NULL;
        int count = 0, capacity = 64;

        if (!handle || !ret_entries || !ret_count)
                return -EINVAL;

        entries = calloc(capacity, sizeof(CaptureMetadata));
        if (!entries)
                return log_oom(), -ENOMEM;

        stmt = handle->stmt_list_unprocessed;
        if (!stmt) {
                free(entries);
                return -EIO;
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_int(stmt, 1, limit > 0 ? limit : 100);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (count >= capacity) {
                        CaptureMetadata *tmp;
                        capacity *= 2;
                        tmp = realloc(entries, (size_t) capacity * sizeof(CaptureMetadata));
                        if (!tmp) {
                                storage_metadata_free(entries, count);
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
                m->ai_processed = false;
                count++;
        }

        *ret_entries = entries;
        *ret_count = count;
        return 0;
}

void storage_search_result_free(SearchResult *results, int count) {
        if (!results)
                return;

        for (int i = 0; i < count; i++) {
                free(results[i].source);
                free(results[i].transcript);
                free(results[i].ocr_text);
                free(results[i].summary);
        }

        free(results);
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
