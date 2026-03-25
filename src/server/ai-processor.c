/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server/ai-processor.h"
#include "common/recalld-log.h"
#include "common/recalld-storage.h"

#define AI_POLL_INTERVAL_SEC 5
#define AI_MAX_OUTPUT_SIZE   (4 * 1024 * 1024)  /* 4 MiB */

struct AiProcessor {
        QueryServiceConfig config;
        StorageHandle *storage;
        bool running;

        pthread_t *workers;
        int n_workers;
};

/* Run an external command and capture its stdout into a heap-allocated
 * string.  Returns 0 on success, negative errno on failure.  The
 * caller must free *ret_output. */
static int run_command(const char *cmd, char **ret_output) {
        FILE *fp;
        char *buf = NULL;
        size_t len = 0, capacity = 4096;

        if (!cmd || !ret_output)
                return -EINVAL;

        fp = popen(cmd, "re");
        if (!fp)
                return log_error_errno(errno, "Failed to run command '%s': %m", cmd), -errno;

        buf = malloc(capacity);
        if (!buf) {
                pclose(fp);
                return log_oom(), -ENOMEM;
        }

        while (!feof(fp) && len < AI_MAX_OUTPUT_SIZE) {
                size_t n = fread(buf + len, 1, capacity - len - 1, fp);
                if (n == 0)
                        break;
                len += n;
                if (len + 1 >= capacity) {
                        capacity *= 2;
                        if (capacity > AI_MAX_OUTPUT_SIZE)
                                capacity = AI_MAX_OUTPUT_SIZE;
                        char *tmp = realloc(buf, capacity);
                        if (!tmp) {
                                free(buf);
                                pclose(fp);
                                return log_oom(), -ENOMEM;
                        }
                        buf = tmp;
                }
        }

        buf[len] = '\0';
        int status = pclose(fp);
        if (status != 0) {
                log_warning("Command '%s' exited with status %d.", cmd, status);
                /* Still return what we captured — partial output may be useful */
        }

        *ret_output = buf;
        return 0;
}

/* Process an audio capture through Whisper for speech-to-text. */
static int process_audio(const AiProcessor *proc, const CaptureMetadata *meta) {
        char cmd[8192];
        char *transcript = NULL;
        int r;

        /* Retrieve the data path from storage so we can point whisper at it */
        uint8_t *data = NULL;
        size_t data_len = 0;

        r = storage_retrieve(proc->storage, meta->id, &data, &data_len);
        if (r < 0) {
                log_warning_errno(-r, "Failed to retrieve audio data for capture %" PRId64 ": %m",
                                  meta->id);
                return r;
        }

        /* Write data to a temporary file for whisper to consume */
        char tmp_path[] = "/tmp/recalld-audio-XXXXXX";
        int fd = mkstemp(tmp_path);
        if (fd < 0) {
                free(data);
                return log_error_errno(errno, "Failed to create temp file: %m"), -errno;
        }

        if (write(fd, data, data_len) < 0) {
                close(fd);
                unlink(tmp_path);
                free(data);
                return log_error_errno(errno, "Failed to write temp file: %m"), -errno;
        }
        close(fd);
        free(data);

        /* Build whisper command — use configured audio_model if available */
        const char *model = proc->config.audio_model;
        if (model && *model)
                snprintf(cmd, sizeof(cmd),
                         "whisper '%s' --model '%s' --output_format txt --output_dir /tmp 2>/dev/null",
                         tmp_path, model);
        else
                snprintf(cmd, sizeof(cmd),
                         "whisper '%s' --model base --output_format txt --output_dir /tmp 2>/dev/null",
                         tmp_path);

        r = run_command(cmd, &transcript);
        unlink(tmp_path);

        if (r < 0) {
                log_warning("Whisper failed for capture %" PRId64 ", marking as processed anyway.",
                            meta->id);
                /* Mark as processed so we don't retry endlessly */
                storage_update_text_fields(proc->storage, meta->id, NULL, NULL, NULL);
                return 0;
        }

        /* Also try to read the .txt output file whisper may have created */
        char txt_path[4096];
        snprintf(txt_path, sizeof(txt_path), "%s.txt", tmp_path);
        FILE *f = fopen(txt_path, "re");
        if (f) {
                free(transcript);
                transcript = NULL;

                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);

                if (sz > 0) {
                        transcript = malloc((size_t) sz + 1);
                        if (transcript) {
                                size_t n = fread(transcript, 1, (size_t) sz, f);
                                transcript[n] = '\0';
                        }
                }
                fclose(f);
                unlink(txt_path);
        }

        r = storage_update_text_fields(proc->storage, meta->id,
                                       transcript, /* ocr_text= */ NULL, /* summary= */ NULL);
        free(transcript);

        if (r < 0)
                log_warning_errno(-r, "Failed to store transcript for capture %" PRId64 ": %m",
                                  meta->id);
        else
                log_info("Transcribed audio capture %" PRId64 ".", meta->id);

        return 0;
}

/* Process a video or screenshot capture through Tesseract for OCR. */
static int process_visual(const AiProcessor *proc, const CaptureMetadata *meta) {
        char cmd[8192];
        char *ocr_text = NULL;
        int r;

        uint8_t *data = NULL;
        size_t data_len = 0;

        r = storage_retrieve(proc->storage, meta->id, &data, &data_len);
        if (r < 0) {
                log_warning_errno(-r, "Failed to retrieve visual data for capture %" PRId64 ": %m",
                                  meta->id);
                return r;
        }

        /* Write data to a temporary file for tesseract */
        char tmp_path[] = "/tmp/recalld-visual-XXXXXX";
        int fd = mkstemp(tmp_path);
        if (fd < 0) {
                free(data);
                return log_error_errno(errno, "Failed to create temp file: %m"), -errno;
        }

        if (write(fd, data, data_len) < 0) {
                close(fd);
                unlink(tmp_path);
                free(data);
                return log_error_errno(errno, "Failed to write temp file: %m"), -errno;
        }
        close(fd);
        free(data);

        snprintf(cmd, sizeof(cmd), "tesseract '%s' stdout 2>/dev/null", tmp_path);

        r = run_command(cmd, &ocr_text);
        unlink(tmp_path);

        if (r < 0) {
                log_warning("Tesseract failed for capture %" PRId64 ", marking as processed anyway.",
                            meta->id);
                storage_update_text_fields(proc->storage, meta->id, NULL, NULL, NULL);
                return 0;
        }

        r = storage_update_text_fields(proc->storage, meta->id,
                                       /* transcript= */ NULL, ocr_text, /* summary= */ NULL);
        free(ocr_text);

        if (r < 0)
                log_warning_errno(-r, "Failed to store OCR text for capture %" PRId64 ": %m",
                                  meta->id);
        else
                log_info("OCR-processed %s capture %" PRId64 ".",
                         capture_type_to_string(meta->type), meta->id);

        return 0;
}

static void* worker_thread(void *arg) {
        AiProcessor *proc = arg;

        log_info("AI processor worker started.");

        while (proc->running) {
                CaptureMetadata *entries = NULL;
                int count = 0;
                int r;

                r = storage_list_unprocessed(proc->storage, &entries, &count,
                                             proc->config.batch_size > 0 ? proc->config.batch_size : 16);
                if (r < 0 || count == 0) {
                        storage_metadata_free(entries, count);
                        if (proc->running)
                                sleep(AI_POLL_INTERVAL_SEC);
                        continue;
                }

                for (int i = 0; i < count && proc->running; i++) {
                        CaptureMetadata *m = &entries[i];

                        switch (m->type) {
                        case CAPTURE_AUDIO:
                                process_audio(proc, m);
                                break;
                        case CAPTURE_VIDEO:
                        case CAPTURE_SCREENSHOT:
                                process_visual(proc, m);
                                break;
                        default:
                                /* Unknown type — mark processed to avoid infinite retry */
                                storage_update_text_fields(proc->storage, m->id,
                                                           NULL, NULL, NULL);
                                break;
                        }
                }

                storage_metadata_free(entries, count);
        }

        log_info("AI processor worker exiting.");
        return NULL;
}

int ai_processor_new(
                AiProcessor **ret,
                const QueryServiceConfig *config,
                StorageHandle *storage) {

        AiProcessor *proc;

        if (!ret || !config || !storage)
                return -EINVAL;

        proc = calloc(1, sizeof(AiProcessor));
        if (!proc)
                return log_oom(), -ENOMEM;

        proc->config = *config;
        proc->storage = storage;
        proc->n_workers = config->worker_threads > 0 ? config->worker_threads : 2;

        log_info("AI processor initialized with %d worker threads.", proc->n_workers);
        *ret = proc;
        return 0;
}

AiProcessor* ai_processor_free(AiProcessor *proc) {
        if (!proc)
                return NULL;

        if (proc->running)
                ai_processor_stop(proc);

        free(proc->workers);
        free(proc);
        return NULL;
}

int ai_processor_start(AiProcessor *proc) {
        if (!proc)
                return -EINVAL;
        if (proc->running)
                return -EALREADY;

        proc->running = true;

        proc->workers = calloc((size_t) proc->n_workers, sizeof(pthread_t));
        if (!proc->workers) {
                proc->running = false;
                return log_oom(), -ENOMEM;
        }

        for (int i = 0; i < proc->n_workers; i++) {
                int r = pthread_create(&proc->workers[i], /* attr= */ NULL, worker_thread, proc);
                if (r != 0) {
                        log_error_errno(r, "Failed to create AI worker thread %d: %m", i);
                        proc->n_workers = i;
                        if (i == 0) {
                                proc->running = false;
                                free(proc->workers);
                                proc->workers = NULL;
                                return -r;
                        }
                        break;
                }
        }

        log_info("AI processor started with %d workers.", proc->n_workers);
        return 0;
}

int ai_processor_stop(AiProcessor *proc) {
        if (!proc)
                return -EINVAL;
        if (!proc->running)
                return 0;

        proc->running = false;

        for (int i = 0; i < proc->n_workers; i++)
                (void) pthread_join(proc->workers[i], /* retval= */ NULL);

        free(proc->workers);
        proc->workers = NULL;

        log_info("AI processor stopped.");
        return 0;
}

bool ai_processor_is_running(const AiProcessor *proc) {
        return proc && proc->running;
}
