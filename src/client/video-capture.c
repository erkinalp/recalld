/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "client/video-capture.h"
#include "common/recalld-log.h"

#if HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#endif

struct VideoCaptureContext {
        VideoCaptureConfig config;
        VideoDataCallback callback;
        void *userdata;

        bool running;
        bool paused;
        pthread_t capture_thread;
        pthread_mutex_t lock;

        int actual_width;
        int actual_height;

#if HAVE_X11
        Display *x_display;
        Window x_root;
        XImage *x_image;
#endif
};

static const char *display_backend_table[] = {
        [DISPLAY_BACKEND_X11]     = "x11",
        [DISPLAY_BACKEND_WAYLAND] = "wayland",
        [DISPLAY_BACKEND_AUTO]    = "auto",
};

static const char *video_codec_table[] = {
        [VIDEO_CODEC_H264] = "h264",
        [VIDEO_CODEC_HEVC] = "hevc",
        [VIDEO_CODEC_VP9]  = "vp9",
        [VIDEO_CODEC_RAW]  = "raw",
};

const char* display_backend_to_string(DisplayBackend backend) {
        if (backend >= 0 && backend < _DISPLAY_BACKEND_MAX)
                return display_backend_table[backend];
        return NULL;
}

DisplayBackend display_backend_from_string(const char *s) {
        if (!s)
                return _DISPLAY_BACKEND_INVALID;

        for (int i = 0; i < _DISPLAY_BACKEND_MAX; i++)
                if (strcmp(s, display_backend_table[i]) == 0)
                        return (DisplayBackend) i;

        return _DISPLAY_BACKEND_INVALID;
}

const char* video_codec_to_string(VideoCodec codec) {
        if (codec >= 0 && codec < _VIDEO_CODEC_MAX)
                return video_codec_table[codec];
        return NULL;
}

VideoCodec video_codec_from_string(const char *s) {
        if (!s)
                return _VIDEO_CODEC_INVALID;

        for (int i = 0; i < _VIDEO_CODEC_MAX; i++)
                if (strcmp(s, video_codec_table[i]) == 0)
                        return (VideoCodec) i;

        return _VIDEO_CODEC_INVALID;
}

static DisplayBackend video_detect_backend(void) {
        const char *session_type;

        session_type = getenv("XDG_SESSION_TYPE");
        if (session_type) {
                if (strcmp(session_type, "wayland") == 0)
                        return DISPLAY_BACKEND_WAYLAND;
                if (strcmp(session_type, "x11") == 0)
                        return DISPLAY_BACKEND_X11;
        }

        /* Fallback: check if DISPLAY is set (X11) or WAYLAND_DISPLAY (Wayland) */
        if (getenv("WAYLAND_DISPLAY"))
                return DISPLAY_BACKEND_WAYLAND;
        if (getenv("DISPLAY"))
                return DISPLAY_BACKEND_X11;

        return _DISPLAY_BACKEND_INVALID;
}

#if HAVE_X11
static int video_init_x11(VideoCaptureContext *ctx) {
        Screen *screen;
        const char *display_name;

        display_name = ctx->config.display ? ctx->config.display : getenv("DISPLAY");
        if (!display_name)
                return log_error_errno(ENODEV, "No X11 display available.");

        ctx->x_display = XOpenDisplay(display_name);
        if (!ctx->x_display)
                return log_error_errno(EIO, "Failed to open X11 display: %s", display_name);

        screen = DefaultScreenOfDisplay(ctx->x_display);
        ctx->x_root = RootWindowOfScreen(screen);

        ctx->actual_width = ctx->config.width > 0 ? ctx->config.width : WidthOfScreen(screen);
        ctx->actual_height = ctx->config.height > 0 ? ctx->config.height : HeightOfScreen(screen);

        log_info("X11 video capture initialized: %dx%d on display %s.",
                 ctx->actual_width, ctx->actual_height, display_name);
        return 0;
}

static void* video_x11_capture_thread(void *arg) {
        VideoCaptureContext *ctx = arg;
        int frame_interval_us;

        frame_interval_us = 1000000 / (ctx->config.frame_rate > 0 ? ctx->config.frame_rate : 15);

        while (ctx->running) {
                XImage *img;
                size_t data_size;

                if (ctx->paused) {
                        usleep(10000);
                        continue;
                }

                img = XGetImage(ctx->x_display, ctx->x_root,
                                0, 0, (unsigned int) ctx->actual_width,
                                (unsigned int) ctx->actual_height,
                                AllPlanes, ZPixmap);
                if (!img) {
                        log_warning("Failed to capture X11 frame.");
                        usleep((unsigned int) frame_interval_us);
                        continue;
                }

                data_size = (size_t)(img->bytes_per_line * img->height);

                if (ctx->callback)
                        ctx->callback((const uint8_t*) img->data, data_size,
                                      ctx->actual_width, ctx->actual_height, ctx->userdata);

                XDestroyImage(img);
                usleep((unsigned int) frame_interval_us);
        }

        return NULL;
}
#endif

/* Wayland capture uses the wlr-screencopy or xdg-desktop-portal protocol.
 * This is a stub implementation that will be expanded with proper Wayland support. */
static void* video_wayland_capture_thread(void *arg) {
        VideoCaptureContext *ctx = arg;
        int frame_interval_us;

        frame_interval_us = 1000000 / (ctx->config.frame_rate > 0 ? ctx->config.frame_rate : 15);

        log_info("Wayland capture thread started (portal-based capture).");

        while (ctx->running) {
                if (ctx->paused) {
                        usleep(10000);
                        continue;
                }

                /* TODO: Implement xdg-desktop-portal or wlr-screencopy based capture.
                 * For now, sleep at the configured frame interval. */
                usleep((unsigned int) frame_interval_us);
        }

        return NULL;
}

int video_capture_new(VideoCaptureContext **ret, const VideoCaptureConfig *config) {
        VideoCaptureContext *ctx;

        if (!ret || !config)
                return -EINVAL;

        ctx = calloc(1, sizeof(VideoCaptureContext));
        if (!ctx)
                return log_oom(), -ENOMEM;

        ctx->config = *config;
        if (config->display)
                ctx->config.display = strdup(config->display);
        if (config->hw_accel)
                ctx->config.hw_accel = strdup(config->hw_accel);
        if (config->exclude_windows)
                ctx->config.exclude_windows = strdup(config->exclude_windows);

        pthread_mutex_init(&ctx->lock, /* attr= */ NULL);

        if (ctx->config.backend == DISPLAY_BACKEND_AUTO) {
                ctx->config.backend = video_detect_backend();
                if (ctx->config.backend == _DISPLAY_BACKEND_INVALID) {
                        log_error("No display backend available.");
                        free(ctx->config.display);
                        free(ctx->config.hw_accel);
                        free(ctx->config.exclude_windows);
                        free(ctx);
                        return -ENODEV;
                }
                log_info("Auto-detected display backend: %s",
                         display_backend_to_string(ctx->config.backend));
        }

        *ret = ctx;
        return 0;
}

VideoCaptureContext* video_capture_free(VideoCaptureContext *ctx) {
        if (!ctx)
                return NULL;

        if (ctx->running)
                video_capture_stop(ctx);

#if HAVE_X11
        if (ctx->x_display)
                XCloseDisplay(ctx->x_display);
#endif

        pthread_mutex_destroy(&ctx->lock);
        free(ctx->config.display);
        free(ctx->config.hw_accel);
        free(ctx->config.exclude_windows);
        free(ctx);
        return NULL;
}

int video_capture_start(VideoCaptureContext *ctx, VideoDataCallback callback, void *userdata) {
        int r;

        if (!ctx || !callback)
                return -EINVAL;
        if (ctx->running)
                return -EALREADY;

        ctx->callback = callback;
        ctx->userdata = userdata;

        switch (ctx->config.backend) {
#if HAVE_X11
        case DISPLAY_BACKEND_X11:
                r = video_init_x11(ctx);
                if (r < 0)
                        return r;

                ctx->running = true;
                r = pthread_create(&ctx->capture_thread, /* attr= */ NULL,
                                   video_x11_capture_thread, ctx);
                if (r != 0) {
                        ctx->running = false;
                        return -r;
                }
                break;
#endif
        case DISPLAY_BACKEND_WAYLAND:
                ctx->actual_width = ctx->config.width > 0 ? ctx->config.width : 1920;
                ctx->actual_height = ctx->config.height > 0 ? ctx->config.height : 1080;

                ctx->running = true;
                r = pthread_create(&ctx->capture_thread, /* attr= */ NULL,
                                   video_wayland_capture_thread, ctx);
                if (r != 0) {
                        ctx->running = false;
                        return -r;
                }
                break;

        default:
                return log_error_errno(ENOSYS, "Display backend %s not supported.",
                                       display_backend_to_string(ctx->config.backend));
        }

        log_info("Video capture started with %s backend, %dx%d @ %d fps.",
                 display_backend_to_string(ctx->config.backend),
                 ctx->actual_width, ctx->actual_height, ctx->config.frame_rate);
        return 0;
}

int video_capture_stop(VideoCaptureContext *ctx) {
        if (!ctx)
                return -EINVAL;
        if (!ctx->running)
                return 0;

        ctx->running = false;
        pthread_join(ctx->capture_thread, /* retval= */ NULL);

        log_info("Video capture stopped.");
        return 0;
}

int video_capture_pause(VideoCaptureContext *ctx) {
        if (!ctx || !ctx->running)
                return -EINVAL;

        ctx->paused = true;
        log_info("Video capture paused.");
        return 0;
}

int video_capture_resume(VideoCaptureContext *ctx) {
        if (!ctx || !ctx->running)
                return -EINVAL;

        ctx->paused = false;
        log_info("Video capture resumed.");
        return 0;
}

bool video_capture_is_running(const VideoCaptureContext *ctx) {
        return ctx ? ctx->running : false;
}

bool video_capture_is_paused(const VideoCaptureContext *ctx) {
        return ctx ? ctx->paused : false;
}
