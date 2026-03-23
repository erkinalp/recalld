/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <systemd/sd-bus.h>

#include "common/recalld-config.h"
#include "common/recalld-storage.h"
#include "client/audio-capture.h"
#include "client/video-capture.h"

typedef struct RecalldDaemon {
        RecalldConfig config;
        StorageHandle *storage;
        AudioCaptureContext *audio;
        VideoCaptureContext *video;
        sd_bus *bus;

        bool capturing;
        bool paused;
        bool should_exit;

        int watchdog_usec;
} RecalldDaemon;

int daemon_new(RecalldDaemon **ret, const char *config_path);
RecalldDaemon* daemon_free(RecalldDaemon *daemon);
int daemon_run(RecalldDaemon *daemon);
int daemon_start_capture(RecalldDaemon *daemon);
int daemon_stop_capture(RecalldDaemon *daemon);
int daemon_pause(RecalldDaemon *daemon);
int daemon_resume(RecalldDaemon *daemon);
