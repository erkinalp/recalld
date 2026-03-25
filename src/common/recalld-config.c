/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/recalld-config.h"
#include "common/recalld-log.h"

static char* strdup_safe(const char *s) {
        return s ? strdup(s) : NULL;
}

static void free_and_replace_str(char **dest, const char *src) {
        free(*dest);
        *dest = strdup_safe(src);
}

static int parse_bool(const char *s) {
        if (!s)
                return -EINVAL;
        if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 || strcmp(s, "1") == 0)
                return 1;
        if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 || strcmp(s, "0") == 0)
                return 0;
        return -EINVAL;
}

static size_t parse_size(const char *s) {
        char *end;
        double val;

        if (!s)
                return 0;

        val = strtod(s, &end);
        if (end == s)
                return 0;

        switch (*end) {
        case 'T':
        case 't':
                return (size_t)(val * 1024 * 1024 * 1024 * 1024);
        case 'G':
        case 'g':
                return (size_t)(val * 1024 * 1024 * 1024);
        case 'M':
        case 'm':
                return (size_t)(val * 1024 * 1024);
        case 'K':
        case 'k':
                return (size_t)(val * 1024);
        default:
                return (size_t) val;
        }
}

void recalld_config_set_defaults(RecalldConfig *config) {
        memset(config, 0, sizeof(*config));

        config->enabled = true;
        config->log_level = strdup("info");
        config->storage_path = strdup("/var/lib/systemd/recalld");
        config->max_storage_size = 50ULL * 1024 * 1024 * 1024; /* 50G */
        config->retention_days = 30;
        config->transmission_enabled = true;
        config->server_address = NULL;
        config->transmission_interval = strdup("hourly");

        config->audio_enabled = true;
        config->audio_sources = strdup("microphone,system");
        config->audio_format = strdup("opus");
        config->audio_quality = strdup("medium");
        config->silence_detection = true;
        config->buffer_size_ms = 500;
        config->sample_rate = 48000;
        config->channels = 2;
        config->bitrate_kbps = 128;
        config->noise_reduction = true;

        config->video_enabled = true;
        config->video_codec = strdup("h264");
        config->frame_rate = 15;
        config->resolution = strdup("screen");
        config->quality_level = strdup("medium");
        config->keyframe_interval = 150;
        config->capture_method = strdup("vnc");
        config->hw_accel = strdup("auto");
        config->multi_monitor = true;
        config->capture_mouse = true;

        config->exclude_applications = strdup("password-manager,banking-app");
        config->exclude_windows = strdup("*password*,*login*,*auth*");
        config->privacy_mode = strdup("smart");
        config->encryption_enabled = true;
        config->encryption_algorithm = strdup("aes-256-gcm");
        config->allow_user_override = true;
        config->consent_required = true;
        config->data_minimization = true;

        config->compression = strdup("high");
        config->encryption_level = strdup("high");
        config->max_bandwidth_kbps = 1000;
        config->retry_count = 5;
        config->retry_delay_seconds = 60;
        config->use_metered = false;
}

void recalld_config_free(RecalldConfig *config) {
        if (!config)
                return;

        free(config->log_level);
        free(config->storage_path);
        free(config->server_address);
        free(config->transmission_interval);
        free(config->audio_sources);
        free(config->audio_format);
        free(config->audio_quality);
        free(config->video_codec);
        free(config->resolution);
        free(config->quality_level);
        free(config->capture_method);
        free(config->hw_accel);
        free(config->exclude_applications);
        free(config->exclude_windows);
        free(config->privacy_mode);
        free(config->encryption_algorithm);
        free(config->compression);
        free(config->encryption_level);
}

static int config_parse_line(RecalldConfig *config, const char *section, const char *key, const char *value) {
        int b;

        if (strcmp(section, "Service") == 0) {
                if (strcmp(key, "Enabled") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->enabled = b;
                } else if (strcmp(key, "LogLevel") == 0)
                        free_and_replace_str(&config->log_level, value);
                else if (strcmp(key, "StoragePath") == 0)
                        free_and_replace_str(&config->storage_path, value);
                else if (strcmp(key, "MaxStorageSize") == 0)
                        config->max_storage_size = parse_size(value);
                else if (strcmp(key, "RetentionDays") == 0)
                        config->retention_days = atoi(value);
                else if (strcmp(key, "TransmissionEnabled") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->transmission_enabled = b;
                } else if (strcmp(key, "ServerAddress") == 0)
                        free_and_replace_str(&config->server_address, value);
                else if (strcmp(key, "TransmissionInterval") == 0)
                        free_and_replace_str(&config->transmission_interval, value);
        } else if (strcmp(section, "AudioCapture") == 0) {
                if (strcmp(key, "Enabled") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->audio_enabled = b;
                } else if (strcmp(key, "Sources") == 0)
                        free_and_replace_str(&config->audio_sources, value);
                else if (strcmp(key, "Format") == 0)
                        free_and_replace_str(&config->audio_format, value);
                else if (strcmp(key, "Quality") == 0)
                        free_and_replace_str(&config->audio_quality, value);
                else if (strcmp(key, "SilenceDetection") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->silence_detection = b;
                } else if (strcmp(key, "BufferSize") == 0)
                        config->buffer_size_ms = atoi(value);
                else if (strcmp(key, "SampleRate") == 0)
                        config->sample_rate = atoi(value);
                else if (strcmp(key, "Channels") == 0)
                        config->channels = atoi(value);
                else if (strcmp(key, "BitrateKbps") == 0)
                        config->bitrate_kbps = atoi(value);
                else if (strcmp(key, "NoiseReduction") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->noise_reduction = b;
                }
        } else if (strcmp(section, "VideoCapture") == 0) {
                if (strcmp(key, "Enabled") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->video_enabled = b;
                } else if (strcmp(key, "Codec") == 0)
                        free_and_replace_str(&config->video_codec, value);
                else if (strcmp(key, "FrameRate") == 0)
                        config->frame_rate = atoi(value);
                else if (strcmp(key, "Resolution") == 0)
                        free_and_replace_str(&config->resolution, value);
                else if (strcmp(key, "QualityLevel") == 0)
                        free_and_replace_str(&config->quality_level, value);
                else if (strcmp(key, "KeyframeInterval") == 0)
                        config->keyframe_interval = atoi(value);
                else if (strcmp(key, "CaptureMethod") == 0)
                        free_and_replace_str(&config->capture_method, value);
                else if (strcmp(key, "HardwareAcceleration") == 0)
                        free_and_replace_str(&config->hw_accel, value);
                else if (strcmp(key, "MultiMonitor") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->multi_monitor = b;
                } else if (strcmp(key, "CaptureMouseCursor") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->capture_mouse = b;
                }
        } else if (strcmp(section, "Privacy") == 0) {
                if (strcmp(key, "ExcludeApplications") == 0)
                        free_and_replace_str(&config->exclude_applications, value);
                else if (strcmp(key, "ExcludeWindows") == 0)
                        free_and_replace_str(&config->exclude_windows, value);
                else if (strcmp(key, "PrivacyMode") == 0)
                        free_and_replace_str(&config->privacy_mode, value);
                else if (strcmp(key, "EncryptionEnabled") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->encryption_enabled = b;
                } else if (strcmp(key, "EncryptionAlgorithm") == 0)
                        free_and_replace_str(&config->encryption_algorithm, value);
                else if (strcmp(key, "AllowUserOverride") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->allow_user_override = b;
                } else if (strcmp(key, "ConsentRequired") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->consent_required = b;
                } else if (strcmp(key, "DataMinimization") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->data_minimization = b;
                }
        } else if (strcmp(section, "Network") == 0) {
                if (strcmp(key, "Compression") == 0)
                        free_and_replace_str(&config->compression, value);
                else if (strcmp(key, "EncryptionLevel") == 0)
                        free_and_replace_str(&config->encryption_level, value);
                else if (strcmp(key, "MaxBandwidthKbps") == 0)
                        config->max_bandwidth_kbps = atoi(value);
                else if (strcmp(key, "RetryCount") == 0)
                        config->retry_count = atoi(value);
                else if (strcmp(key, "RetryDelaySeconds") == 0)
                        config->retry_delay_seconds = atoi(value);
                else if (strcmp(key, "UseMeteredConnections") == 0) {
                        b = parse_bool(value);
                        if (b >= 0)
                                config->use_metered = b;
                }
        }

        return 0;
}

int recalld_config_load(RecalldConfig *config, const char *path) {
        FILE *f;
        char line[4096];
        char section[256] = "";
        int r;

        recalld_config_set_defaults(config);

        f = fopen(path, "re");
        if (!f) {
                if (errno == ENOENT) {
                        log_info("Configuration file %s not found, using defaults.", path);
                        return 0;
                }
                return log_error_errno(errno, "Failed to open configuration file %s: %m", path);
        }

        while (fgets(line, sizeof(line), f)) {
                char *p, *key, *value;

                /* Strip trailing newline */
                p = strchr(line, '\n');
                if (p)
                        *p = '\0';

                /* Strip leading whitespace */
                p = line;
                while (*p == ' ' || *p == '\t')
                        p++;

                /* Skip empty lines and comments */
                if (*p == '\0' || *p == '#' || *p == ';')
                        continue;

                /* Section header */
                if (*p == '[') {
                        char *end = strchr(p, ']');
                        if (end) {
                                *end = '\0';
                                snprintf(section, sizeof(section), "%s", p + 1);
                        }
                        continue;
                }

                /* Key=Value pair */
                key = p;
                value = strchr(p, '=');
                if (!value)
                        continue;

                *value = '\0';
                value++;

                /* Trim trailing whitespace from key */
                p = key + strlen(key) - 1;
                while (p > key && (*p == ' ' || *p == '\t'))
                        *p-- = '\0';

                /* Trim leading whitespace from value */
                while (*value == ' ' || *value == '\t')
                        value++;

                r = config_parse_line(config, section, key, value);
                if (r < 0)
                        log_warning("Failed to parse config line: %s=%s", key, value);
        }

        fclose(f);
        return 0;
}

void query_service_config_set_defaults(QueryServiceConfig *config) {
        memset(config, 0, sizeof(*config));

        config->enabled = true;
        config->log_level = strdup("info");
        config->storage_path = strdup("/var/lib/systemd/recall-query-service");
        config->worker_threads = 8;
        config->max_concurrent_queries = 100;
        config->shutdown_timeout_sec = 30;

        config->port = 8080;
        config->stream_port = 8081;
        config->rate_limit_per_minute = 60;
        config->auth_required = true;
        config->tls_enabled = true;
        config->tls_cert_path = strdup("/etc/systemd/recall-query-service/cert.pem");
        config->tls_key_path = strdup("/etc/systemd/recall-query-service/key.pem");
        config->request_timeout_sec = 30;

        config->model_path = strdup("/var/lib/systemd/recall-query-service/models");
        config->gpu_acceleration = true;
        config->batch_size = 16;
        config->audio_model = strdup("whisper-large-v3");
        config->video_model = strdup("clip-vit-large-patch14");
        config->context_window_size = 4096;
        config->max_tokens = 1024;

        config->data_path = strdup("/var/lib/systemd/recall-query-service/data");
        config->index_path = strdup("/var/lib/systemd/recall-query-service/indexes");
        config->storage_max_size = 500ULL * 1024 * 1024 * 1024;
        config->storage_retention_days = 90;
        config->storage_encryption = true;

        config->auth_method = strdup("pam");
        config->pam_service_name = strdup("systemd-recall");
        config->jwt_secret_file = strdup("/etc/systemd/recall-query-service/jwt-secret");
        config->session_timeout = 3600;
        config->failed_login_limit = 5;
        config->failed_login_lockout_minutes = 15;

        config->max_results_per_query = 100;
        config->default_time_range = strdup("7d");
        config->context_awareness = true;
        config->confidence_threshold = 0.7;
        config->cache_results = true;
        config->cache_expiration_sec = 3600;
}

void query_service_config_free(QueryServiceConfig *config) {
        if (!config)
                return;

        free(config->log_level);
        free(config->storage_path);
        free(config->tls_cert_path);
        free(config->tls_key_path);
        free(config->model_path);
        free(config->audio_model);
        free(config->video_model);
        free(config->data_path);
        free(config->index_path);
        free(config->auth_method);
        free(config->pam_service_name);
        free(config->jwt_secret_file);
        free(config->default_time_range);
}

int query_service_config_load(QueryServiceConfig *config, const char *path) {
        FILE *f;
        char line[4096];
        char section[256] = "";

        query_service_config_set_defaults(config);

        f = fopen(path, "re");
        if (!f) {
                if (errno == ENOENT) {
                        log_info("Configuration file %s not found, using defaults.", path);
                        return 0;
                }
                return log_error_errno(errno, "Failed to open configuration file %s: %m", path);
        }

        while (fgets(line, sizeof(line), f)) {
                char *p, *key, *value;
                int b;

                p = strchr(line, '\n');
                if (p)
                        *p = '\0';

                p = line;
                while (*p == ' ' || *p == '\t')
                        p++;

                if (*p == '\0' || *p == '#' || *p == ';')
                        continue;

                if (*p == '[') {
                        char *end = strchr(p, ']');
                        if (end) {
                                *end = '\0';
                                snprintf(section, sizeof(section), "%s", p + 1);
                        }
                        continue;
                }

                key = p;
                value = strchr(p, '=');
                if (!value)
                        continue;

                *value = '\0';
                value++;

                p = key + strlen(key) - 1;
                while (p > key && (*p == ' ' || *p == '\t'))
                        *p-- = '\0';

                while (*value == ' ' || *value == '\t')
                        value++;

                if (strcmp(section, "Service") == 0) {
                        if (strcmp(key, "Enabled") == 0) {
                                b = parse_bool(value);
                                if (b >= 0)
                                        config->enabled = b;
                        } else if (strcmp(key, "LogLevel") == 0)
                                free_and_replace_str(&config->log_level, value);
                        else if (strcmp(key, "StoragePath") == 0)
                                free_and_replace_str(&config->storage_path, value);
                        else if (strcmp(key, "WorkerThreads") == 0)
                                config->worker_threads = atoi(value);
                        else if (strcmp(key, "MaxConcurrentQueries") == 0)
                                config->max_concurrent_queries = atoi(value);
                        else if (strcmp(key, "ShutdownTimeoutSec") == 0)
                                config->shutdown_timeout_sec = atoi(value);
                } else if (strcmp(section, "API") == 0) {
                        if (strcmp(key, "Port") == 0)
                                config->port = atoi(value);
                        else if (strcmp(key, "StreamPort") == 0)
                                config->stream_port = atoi(value);
                        else if (strcmp(key, "RateLimitPerMinute") == 0)
                                config->rate_limit_per_minute = atoi(value);
                        else if (strcmp(key, "AuthenticationRequired") == 0) {
                                b = parse_bool(value);
                                if (b >= 0)
                                        config->auth_required = b;
                        } else if (strcmp(key, "TLSEnabled") == 0) {
                                b = parse_bool(value);
                                if (b >= 0)
                                        config->tls_enabled = b;
                        } else if (strcmp(key, "TLSCertPath") == 0)
                                free_and_replace_str(&config->tls_cert_path, value);
                        else if (strcmp(key, "TLSKeyPath") == 0)
                                free_and_replace_str(&config->tls_key_path, value);
                        else if (strcmp(key, "RequestTimeoutSec") == 0)
                                config->request_timeout_sec = atoi(value);
                } else if (strcmp(section, "Authentication") == 0) {
                        if (strcmp(key, "AuthMethod") == 0)
                                free_and_replace_str(&config->auth_method, value);
                        else if (strcmp(key, "PAMServiceName") == 0)
                                free_and_replace_str(&config->pam_service_name, value);
                        else if (strcmp(key, "JWTSecret") == 0)
                                free_and_replace_str(&config->jwt_secret_file, value);
                        else if (strcmp(key, "SessionTimeout") == 0)
                                config->session_timeout = atoi(value);
                        else if (strcmp(key, "FailedLoginLimit") == 0)
                                config->failed_login_limit = atoi(value);
                        else if (strcmp(key, "FailedLoginLockoutMinutes") == 0)
                                config->failed_login_lockout_minutes = atoi(value);
                }
        }

        fclose(f);
        return 0;
}
