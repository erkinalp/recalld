/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct VideoCaptureContext VideoCaptureContext;

typedef enum DisplayBackend {
        DISPLAY_BACKEND_X11,
        DISPLAY_BACKEND_WAYLAND,
        DISPLAY_BACKEND_AUTO,
        _DISPLAY_BACKEND_MAX,
        _DISPLAY_BACKEND_INVALID = -1,
} DisplayBackend;

typedef enum VideoCodec {
        VIDEO_CODEC_H264,
        VIDEO_CODEC_HEVC,
        VIDEO_CODEC_VP9,
        VIDEO_CODEC_RAW,
        _VIDEO_CODEC_MAX,
        _VIDEO_CODEC_INVALID = -1,
} VideoCodec;

typedef struct VideoCaptureConfig {
        DisplayBackend backend;
        char *display;
        int width;
        int height;
        int frame_rate;
        VideoCodec codec;
        int keyframe_interval;
        char *hw_accel;
        bool multi_monitor;
        bool capture_mouse;
        char *exclude_windows;
} VideoCaptureConfig;

typedef void (*VideoDataCallback)(
                const uint8_t *data,
                size_t len,
                int width,
                int height,
                void *userdata);

int video_capture_new(VideoCaptureContext **ret, const VideoCaptureConfig *config);
VideoCaptureContext* video_capture_free(VideoCaptureContext *ctx);

int video_capture_start(VideoCaptureContext *ctx, VideoDataCallback callback, void *userdata);
int video_capture_stop(VideoCaptureContext *ctx);
int video_capture_pause(VideoCaptureContext *ctx);
int video_capture_resume(VideoCaptureContext *ctx);

bool video_capture_is_running(const VideoCaptureContext *ctx);
bool video_capture_is_paused(const VideoCaptureContext *ctx);

const char* display_backend_to_string(DisplayBackend backend);
DisplayBackend display_backend_from_string(const char *s);
const char* video_codec_to_string(VideoCodec codec);
VideoCodec video_codec_from_string(const char *s);
