/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Client-side configuration for systemd-recalld */
typedef struct RecalldConfig {
        /* [Service] */
        bool enabled;
        char *log_level;
        char *storage_path;
        size_t max_storage_size;
        int retention_days;
        bool transmission_enabled;
        char *server_address;
        char *transmission_interval;

        /* [AudioCapture] */
        bool audio_enabled;
        char *audio_sources;
        char *audio_format;
        char *audio_quality;
        bool silence_detection;
        int buffer_size_ms;
        int sample_rate;
        int channels;
        int bitrate_kbps;
        bool noise_reduction;

        /* [VideoCapture] */
        bool video_enabled;
        char *video_codec;
        int frame_rate;
        char *resolution;
        char *quality_level;
        int keyframe_interval;
        char *capture_method;
        char *hw_accel;
        bool multi_monitor;
        bool capture_mouse;

        /* [Privacy] */
        char *exclude_applications;
        char *exclude_windows;
        char *privacy_mode;
        bool encryption_enabled;
        char *encryption_algorithm;
        bool allow_user_override;
        bool consent_required;
        bool data_minimization;

        /* [Network] */
        char *compression;
        char *encryption_level;
        int max_bandwidth_kbps;
        int retry_count;
        int retry_delay_seconds;
        bool use_metered;
} RecalldConfig;

/* Server-side configuration for systemd-recall-query-service */
typedef struct QueryServiceConfig {
        /* [Service] */
        bool enabled;
        char *log_level;
        char *storage_path;
        int worker_threads;
        int max_concurrent_queries;
        int shutdown_timeout_sec;

        /* [API] */
        int port;
        int stream_port;        /* separate port for RTP/VNC/Bulk stream receiver, 0 = disabled */
        int rate_limit_per_minute;
        bool auth_required;
        bool tls_enabled;
        char *tls_cert_path;
        char *tls_key_path;
        int request_timeout_sec;

        /* [AIProcessing] */
        char *model_path;
        bool gpu_acceleration;
        int batch_size;
        char *audio_model;
        char *video_model;
        int context_window_size;
        int max_tokens;

        /* [Storage] */
        char *data_path;
        char *index_path;
        size_t storage_max_size;
        int storage_retention_days;
        bool storage_encryption;

        /* [Authentication] */
        char *auth_method;
        char *pam_service_name;
        char *jwt_secret_file;
        int session_timeout;
        int failed_login_limit;
        int failed_login_lockout_minutes;

        /* [QueryProcessing] */
        int max_results_per_query;
        char *default_time_range;
        bool context_awareness;
        double confidence_threshold;
        bool cache_results;
        int cache_expiration_sec;
} QueryServiceConfig;

int recalld_config_load(RecalldConfig *config, const char *path);
void recalld_config_free(RecalldConfig *config);
void recalld_config_set_defaults(RecalldConfig *config);

int query_service_config_load(QueryServiceConfig *config, const char *path);
void query_service_config_free(QueryServiceConfig *config);
void query_service_config_set_defaults(QueryServiceConfig *config);
