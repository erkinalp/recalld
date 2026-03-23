/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AudioCaptureContext AudioCaptureContext;

typedef enum AudioBackend {
        AUDIO_BACKEND_ALSA,
        AUDIO_BACKEND_PULSE,
        AUDIO_BACKEND_AUTO,
        _AUDIO_BACKEND_MAX,
        _AUDIO_BACKEND_INVALID = -1,
} AudioBackend;

typedef struct AudioCaptureConfig {
        AudioBackend backend;
        char *device;
        int sample_rate;
        int channels;
        int bitrate_kbps;
        int buffer_size_ms;
        bool silence_detection;
        int silence_threshold_ms;
        bool noise_reduction;
} AudioCaptureConfig;

typedef void (*AudioDataCallback)(
                const uint8_t *data,
                size_t len,
                int sample_rate,
                int channels,
                void *userdata);

int audio_capture_new(AudioCaptureContext **ret, const AudioCaptureConfig *config);
AudioCaptureContext* audio_capture_free(AudioCaptureContext *ctx);

int audio_capture_start(AudioCaptureContext *ctx, AudioDataCallback callback, void *userdata);
int audio_capture_stop(AudioCaptureContext *ctx);
int audio_capture_pause(AudioCaptureContext *ctx);
int audio_capture_resume(AudioCaptureContext *ctx);

bool audio_capture_is_running(const AudioCaptureContext *ctx);
bool audio_capture_is_paused(const AudioCaptureContext *ctx);

const char* audio_backend_to_string(AudioBackend backend);
AudioBackend audio_backend_from_string(const char *s);
