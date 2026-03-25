/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-daemon.h>
#include <systemd/sd-bus.h>

#include "client/recalld-daemon.h"
#include "client/audio-capture.h"
#include "client/video-capture.h"
#include "client/dbus-service.h"
#include "common/recalld-config.h"
#include "common/recalld-log.h"
#include "common/recalld-storage.h"

static uint32_t audio_rtp_timestamp = 0;
static uint32_t video_frame_number = 0;

static void audio_data_handler(
                const uint8_t *data,
                size_t len,
                int sample_rate,
                int channels,
                void *userdata) {

        RecalldDaemon *daemon = userdata;
        int duration_ms;

        if (!daemon || !daemon->storage)
                return;

        duration_ms = (int)(len * 1000 / ((size_t) sample_rate * (size_t) channels * 2));

        /* Store locally */
        storage_store(daemon->storage, CAPTURE_AUDIO, data, len, "audio-capture", duration_ms);

        /* Transmit to server via reverse RTP if enabled */
        if (daemon->transmitter &&
            network_transmitter_get_state(daemon->transmitter) == TRANSMIT_STATE_CONNECTED) {
                audio_rtp_timestamp += (uint32_t)(len / ((size_t) channels * 2));
                network_transmit_audio_rtp(daemon->transmitter, data, len,
                                           sample_rate, channels, audio_rtp_timestamp);
        }
}

static void video_data_handler(
                const uint8_t *data,
                size_t len,
                int width,
                int height,
                void *userdata) {

        RecalldDaemon *daemon = userdata;

        if (!daemon || !daemon->storage)
                return;

        /* Store locally */
        storage_store(daemon->storage, CAPTURE_VIDEO, data, len, "video-capture", 0);

        /* Transmit to server via reverse VNC (RFB) if enabled */
        if (daemon->transmitter &&
            network_transmitter_get_state(daemon->transmitter) == TRANSMIT_STATE_CONNECTED) {
                network_transmit_video_vnc(daemon->transmitter, data, len,
                                           width, height, video_frame_number++);
        }
}

int daemon_new(RecalldDaemon **ret, const char *config_path) {
        RecalldDaemon *d;
        int r;

        if (!ret)
                return -EINVAL;

        d = calloc(1, sizeof(RecalldDaemon));
        if (!d)
                return log_oom(), -ENOMEM;

        /* Load configuration */
        r = recalld_config_load(&d->config,
                        config_path ? config_path : "/etc/systemd/recalld/recalld.conf");
        if (r < 0)
                goto fail;

        /* Set log level */
        if (d->config.log_level) {
                RecalldLogLevel lvl = recalld_log_level_from_string(d->config.log_level);
                if (lvl != _RECALLD_LOG_LEVEL_INVALID)
                        recalld_log_set_level(lvl);
        }

        /* Open storage */
        r = storage_open(&d->storage, d->config.storage_path,
                         d->config.storage_path);
        if (r < 0) {
                log_error_errno(-r, "Failed to open storage: %m");
                goto fail;
        }

        /* Initialize audio capture */
        if (d->config.audio_enabled) {
                AudioCaptureConfig acfg = {
                        .backend = AUDIO_BACKEND_AUTO,
                        .device = NULL,
                        .sample_rate = d->config.sample_rate,
                        .channels = d->config.channels,
                        .bitrate_kbps = d->config.bitrate_kbps,
                        .buffer_size_ms = d->config.buffer_size_ms,
                        .silence_detection = d->config.silence_detection,
                        .silence_threshold_ms = 11,
                        .noise_reduction = d->config.noise_reduction,
                };

                r = audio_capture_new(&d->audio, &acfg);
                if (r < 0)
                        log_warning_errno(-r, "Failed to initialize audio capture, continuing without: %m");
        }

        /* Initialize video capture */
        if (d->config.video_enabled) {
                VideoCaptureConfig vcfg = {
                        .backend = DISPLAY_BACKEND_AUTO,
                        .display = NULL,
                        .width = 0,
                        .height = 0,
                        .frame_rate = d->config.frame_rate,
                        .codec = video_codec_from_string(d->config.video_codec),
                        .keyframe_interval = d->config.keyframe_interval,
                        .hw_accel = d->config.hw_accel,
                        .multi_monitor = d->config.multi_monitor,
                        .capture_mouse = d->config.capture_mouse,
                        .exclude_windows = d->config.exclude_windows,
                };

                r = video_capture_new(&d->video, &vcfg);
                if (r < 0)
                        log_warning_errno(-r, "Failed to initialize video capture, continuing without: %m");
        }

        /* Initialize network transmitter if transmission is enabled */
        if (d->config.transmission_enabled && d->config.server_address) {
                NetworkTransmitConfig ncfg = {
                        .server_address = d->config.server_address,
                        .server_port = 8080,
                        .tls_enabled = true,
                        .auth_token = NULL,
                        .max_bandwidth_kbps = d->config.max_bandwidth_kbps,
                        .compression = d->config.compression,
                        .retry_count = d->config.retry_count,
                        .retry_delay_seconds = d->config.retry_delay_seconds,
                        .use_metered = d->config.use_metered,
                        .rtp_payload_type = 111, /* Opus */
                        .rtp_ssrc = (int) getpid(),
                        .vnc_encoding = 0, /* Raw */
                        .vnc_quality = 8,
                };

                r = network_transmitter_new(&d->transmitter, &ncfg);
                if (r < 0)
                        log_warning_errno(-r, "Failed to initialize network transmitter, continuing without: %m");
                else {
                        r = network_transmitter_connect(d->transmitter);
                        if (r < 0)
                                log_warning_errno(-r, "Failed to connect to server, will retry later: %m");
                }
        }

        /* Initialize D-Bus */
        r = dbus_service_init(&d->bus);
        if (r < 0) {
                log_error_errno(-r, "Failed to initialize D-Bus: %m");
                goto fail;
        }

        r = dbus_service_register(d->bus, d);
        if (r < 0) {
                log_error_errno(-r, "Failed to register D-Bus service: %m");
                goto fail;
        }

        *ret = d;
        return 0;

fail:
        daemon_free(d);
        return r;
}

RecalldDaemon* daemon_free(RecalldDaemon *daemon) {
        if (!daemon)
                return NULL;

        if (daemon->capturing)
                daemon_stop_capture(daemon);

        if (daemon->bus)
                dbus_service_cleanup(daemon->bus);

        audio_capture_free(daemon->audio);
        video_capture_free(daemon->video);
        network_transmitter_free(daemon->transmitter);
        storage_close(daemon->storage);
        recalld_config_free(&daemon->config);
        free(daemon);
        return NULL;
}

int daemon_start_capture(RecalldDaemon *daemon) {
        int r;

        if (!daemon)
                return -EINVAL;
        if (daemon->capturing)
                return -EALREADY;

        if (daemon->audio) {
                r = audio_capture_start(daemon->audio, audio_data_handler, daemon);
                if (r < 0)
                        log_warning_errno(-r, "Failed to start audio capture: %m");
        }

        if (daemon->video) {
                r = video_capture_start(daemon->video, video_data_handler, daemon);
                if (r < 0)
                        log_warning_errno(-r, "Failed to start video capture: %m");
        }

        daemon->capturing = true;
        daemon->paused = false;

        log_info("Capture started.");
        sd_notify(/* unset= */ 0, "STATUS=Capturing");
        return 0;
}

int daemon_stop_capture(RecalldDaemon *daemon) {
        if (!daemon)
                return -EINVAL;
        if (!daemon->capturing)
                return 0;

        if (daemon->audio)
                audio_capture_stop(daemon->audio);

        if (daemon->video)
                video_capture_stop(daemon->video);

        daemon->capturing = false;
        daemon->paused = false;

        log_info("Capture stopped.");
        sd_notify(/* unset= */ 0, "STATUS=Idle");
        return 0;
}

int daemon_pause(RecalldDaemon *daemon) {
        if (!daemon || !daemon->capturing)
                return -EINVAL;
        if (daemon->paused)
                return -EALREADY;

        if (daemon->audio)
                audio_capture_pause(daemon->audio);
        if (daemon->video)
                video_capture_pause(daemon->video);

        daemon->paused = true;

        log_info("Capture paused.");
        sd_notify(/* unset= */ 0, "STATUS=Paused");
        return 0;
}

int daemon_resume(RecalldDaemon *daemon) {
        if (!daemon || !daemon->capturing)
                return -EINVAL;
        if (!daemon->paused)
                return -EALREADY;

        if (daemon->audio)
                audio_capture_resume(daemon->audio);
        if (daemon->video)
                video_capture_resume(daemon->video);

        daemon->paused = false;

        log_info("Capture resumed.");
        sd_notify(/* unset= */ 0, "STATUS=Capturing");
        return 0;
}

int daemon_run(RecalldDaemon *daemon) {
        int r;

        if (!daemon)
                return -EINVAL;

        /* Notify systemd we're ready */
        sd_notify(/* unset= */ 0, "READY=1\nSTATUS=Idle");

        /* Auto-start capture if enabled */
        if (daemon->config.enabled) {
                r = daemon_start_capture(daemon);
                if (r < 0 && r != -EALREADY)
                        log_warning_errno(-r, "Failed to auto-start capture: %m");
        }

        log_info("Entering main loop.");

        while (!daemon->should_exit) {
                r = sd_bus_process(daemon->bus, /* ret_m= */ NULL);
                if (r < 0) {
                        log_error_errno(-r, "Bus processing failed: %m");
                        break;
                }

                if (r > 0)
                        continue;

                /* Periodic maintenance */
                if (daemon->config.retention_days > 0)
                        storage_enforce_retention(daemon->storage, daemon->config.retention_days);
                if (daemon->config.max_storage_size > 0)
                        storage_enforce_quota(daemon->storage, daemon->config.max_storage_size);

                /* Watchdog notification */
                sd_notify(/* unset= */ 0, "WATCHDOG=1");

                r = sd_bus_wait(daemon->bus, 5 * 1000000ULL); /* 5 seconds */
                if (r < 0 && r != -EINTR) {
                        log_error_errno(-r, "Bus wait failed: %m");
                        break;
                }
        }

        daemon_stop_capture(daemon);
        sd_notify(/* unset= */ 0, "STOPPING=1");

        return 0;
}
