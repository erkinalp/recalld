/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "client/audio-capture.h"
#include "common/recalld-log.h"

#if HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

#if HAVE_PULSE
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#endif

struct AudioCaptureContext {
        AudioCaptureConfig config;
        AudioDataCallback callback;
        void *userdata;

        bool running;
        bool paused;
        pthread_t capture_thread;
        pthread_mutex_t lock;

#if HAVE_ALSA
        snd_pcm_t *alsa_handle;
#endif
#if HAVE_PULSE
        pa_simple *pulse_handle;
#endif
};

static const char *audio_backend_table[] = {
        [AUDIO_BACKEND_ALSA]  = "alsa",
        [AUDIO_BACKEND_PULSE] = "pulse",
        [AUDIO_BACKEND_AUTO]  = "auto",
};

const char* audio_backend_to_string(AudioBackend backend) {
        if (backend >= 0 && backend < _AUDIO_BACKEND_MAX)
                return audio_backend_table[backend];
        return NULL;
}

AudioBackend audio_backend_from_string(const char *s) {
        if (!s)
                return _AUDIO_BACKEND_INVALID;

        for (int i = 0; i < _AUDIO_BACKEND_MAX; i++)
                if (strcmp(s, audio_backend_table[i]) == 0)
                        return (AudioBackend) i;

        return _AUDIO_BACKEND_INVALID;
}

static AudioBackend audio_detect_backend(void) {
#if HAVE_PULSE
        /* Try PulseAudio first as it is more common on modern desktops */
        pa_simple *test;
        pa_sample_spec ss = {
                .format = PA_SAMPLE_S16LE,
                .rate = 44100,
                .channels = 1,
        };
        int err;

        test = pa_simple_new(/* server= */ NULL, "systemd-recalld", PA_STREAM_RECORD,
                             /* dev= */ NULL, "probe", &ss, /* channel_map= */ NULL,
                             /* attr= */ NULL, &err);
        if (test) {
                pa_simple_free(test);
                return AUDIO_BACKEND_PULSE;
        }
#endif

#if HAVE_ALSA
        return AUDIO_BACKEND_ALSA;
#endif

        return _AUDIO_BACKEND_INVALID;
}

#if HAVE_ALSA
static int audio_init_alsa(AudioCaptureContext *ctx) {
        snd_pcm_hw_params_t *params;
        unsigned int rate;
        int r;

        r = snd_pcm_open(&ctx->alsa_handle,
                          ctx->config.device ? ctx->config.device : "default",
                          SND_PCM_STREAM_CAPTURE, 0);
        if (r < 0)
                return log_error_errno(EIO, "Failed to open ALSA device: %s", snd_strerror(r));

        snd_pcm_hw_params_alloca(&params);
        snd_pcm_hw_params_any(ctx->alsa_handle, params);
        snd_pcm_hw_params_set_access(ctx->alsa_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(ctx->alsa_handle, params, SND_PCM_FORMAT_S16_LE);

        rate = (unsigned int) ctx->config.sample_rate;
        snd_pcm_hw_params_set_rate_near(ctx->alsa_handle, params, &rate, /* dir= */ NULL);
        snd_pcm_hw_params_set_channels(ctx->alsa_handle, params, (unsigned int) ctx->config.channels);

        r = snd_pcm_hw_params(ctx->alsa_handle, params);
        if (r < 0) {
                snd_pcm_close(ctx->alsa_handle);
                ctx->alsa_handle = NULL;
                return log_error_errno(EIO, "Failed to set ALSA parameters: %s", snd_strerror(r));
        }

        log_info("ALSA audio capture initialized: %u Hz, %d channels.",
                 rate, ctx->config.channels);
        return 0;
}

static void* audio_alsa_capture_thread(void *arg) {
        AudioCaptureContext *ctx = arg;
        size_t frame_size, buffer_frames, buffer_bytes;
        uint8_t *buffer;

        frame_size = (size_t)(2 * ctx->config.channels); /* S16_LE * channels */
        buffer_frames = (size_t)(ctx->config.sample_rate * ctx->config.buffer_size_ms / 1000);
        buffer_bytes = buffer_frames * frame_size;

        buffer = malloc(buffer_bytes);
        if (!buffer) {
                log_oom();
                return NULL;
        }

        while (ctx->running) {
                snd_pcm_sframes_t frames;

                if (ctx->paused) {
                        usleep(10000);
                        continue;
                }

                frames = snd_pcm_readi(ctx->alsa_handle, buffer, buffer_frames);
                if (frames < 0) {
                        frames = snd_pcm_recover(ctx->alsa_handle, (int) frames, /* silent= */ 0);
                        if (frames < 0) {
                                log_error("ALSA read failed: %s", snd_strerror((int) frames));
                                break;
                        }
                }

                if (frames > 0 && ctx->callback)
                        ctx->callback(buffer, (size_t)(frames * (snd_pcm_sframes_t) frame_size),
                                      ctx->config.sample_rate, ctx->config.channels, ctx->userdata);
        }

        free(buffer);
        return NULL;
}
#endif

#if HAVE_PULSE
static int audio_init_pulse(AudioCaptureContext *ctx) {
        pa_sample_spec ss = {
                .format = PA_SAMPLE_S16LE,
                .rate = (uint32_t) ctx->config.sample_rate,
                .channels = (uint8_t) ctx->config.channels,
        };
        int err;

        ctx->pulse_handle = pa_simple_new(
                /* server= */ NULL, "systemd-recalld",
                PA_STREAM_RECORD,
                ctx->config.device,
                "audio-capture",
                &ss, /* channel_map= */ NULL, /* attr= */ NULL, &err);

        if (!ctx->pulse_handle)
                return log_error_errno(EIO, "Failed to open PulseAudio stream: %s",
                                       pa_strerror(err));

        log_info("PulseAudio audio capture initialized: %d Hz, %d channels.",
                 ctx->config.sample_rate, ctx->config.channels);
        return 0;
}

static void* audio_pulse_capture_thread(void *arg) {
        AudioCaptureContext *ctx = arg;
        size_t buffer_bytes;
        uint8_t *buffer;

        buffer_bytes = (size_t)(2 * ctx->config.channels * ctx->config.sample_rate *
                                ctx->config.buffer_size_ms / 1000);

        buffer = malloc(buffer_bytes);
        if (!buffer) {
                log_oom();
                return NULL;
        }

        while (ctx->running) {
                int err;

                if (ctx->paused) {
                        usleep(10000);
                        continue;
                }

                if (pa_simple_read(ctx->pulse_handle, buffer, buffer_bytes, &err) < 0) {
                        log_error("PulseAudio read failed: %s", pa_strerror(err));
                        break;
                }

                if (ctx->callback)
                        ctx->callback(buffer, buffer_bytes,
                                      ctx->config.sample_rate, ctx->config.channels, ctx->userdata);
        }

        free(buffer);
        return NULL;
}
#endif

int audio_capture_new(AudioCaptureContext **ret, const AudioCaptureConfig *config) {
        AudioCaptureContext *ctx;

        if (!ret || !config)
                return -EINVAL;

        ctx = calloc(1, sizeof(AudioCaptureContext));
        if (!ctx)
                return log_oom(), -ENOMEM;

        ctx->config = *config;
        if (config->device)
                ctx->config.device = strdup(config->device);

        pthread_mutex_init(&ctx->lock, /* attr= */ NULL);

        /* Resolve auto backend */
        if (ctx->config.backend == AUDIO_BACKEND_AUTO) {
                ctx->config.backend = audio_detect_backend();
                if (ctx->config.backend == _AUDIO_BACKEND_INVALID) {
                        log_error("No audio backend available.");
                        free(ctx->config.device);
                        free(ctx);
                        return -ENODEV;
                }
                log_info("Auto-detected audio backend: %s",
                         audio_backend_to_string(ctx->config.backend));
        }

        *ret = ctx;
        return 0;
}

AudioCaptureContext* audio_capture_free(AudioCaptureContext *ctx) {
        if (!ctx)
                return NULL;

        if (ctx->running)
                audio_capture_stop(ctx);

#if HAVE_ALSA
        if (ctx->alsa_handle)
                snd_pcm_close(ctx->alsa_handle);
#endif
#if HAVE_PULSE
        if (ctx->pulse_handle)
                pa_simple_free(ctx->pulse_handle);
#endif

        pthread_mutex_destroy(&ctx->lock);
        free(ctx->config.device);
        free(ctx);
        return NULL;
}

int audio_capture_start(AudioCaptureContext *ctx, AudioDataCallback callback, void *userdata) {
        int r;

        if (!ctx || !callback)
                return -EINVAL;
        if (ctx->running)
                return -EALREADY;

        ctx->callback = callback;
        ctx->userdata = userdata;

        switch (ctx->config.backend) {
#if HAVE_ALSA
        case AUDIO_BACKEND_ALSA:
                r = audio_init_alsa(ctx);
                if (r < 0)
                        return r;

                ctx->running = true;
                r = pthread_create(&ctx->capture_thread, /* attr= */ NULL,
                                   audio_alsa_capture_thread, ctx);
                if (r != 0) {
                        ctx->running = false;
                        return -r;
                }
                break;
#endif
#if HAVE_PULSE
        case AUDIO_BACKEND_PULSE:
                r = audio_init_pulse(ctx);
                if (r < 0)
                        return r;

                ctx->running = true;
                r = pthread_create(&ctx->capture_thread, /* attr= */ NULL,
                                   audio_pulse_capture_thread, ctx);
                if (r != 0) {
                        ctx->running = false;
                        return -r;
                }
                break;
#endif
        default:
                return log_error_errno(ENOSYS, "Audio backend %s not compiled in.",
                                       audio_backend_to_string(ctx->config.backend));
        }

        log_info("Audio capture started with %s backend.",
                 audio_backend_to_string(ctx->config.backend));
        return 0;
}

int audio_capture_stop(AudioCaptureContext *ctx) {
        if (!ctx)
                return -EINVAL;
        if (!ctx->running)
                return 0;

        ctx->running = false;
        pthread_join(ctx->capture_thread, /* retval= */ NULL);

        log_info("Audio capture stopped.");
        return 0;
}

int audio_capture_pause(AudioCaptureContext *ctx) {
        if (!ctx || !ctx->running)
                return -EINVAL;

        ctx->paused = true;
        log_info("Audio capture paused.");
        return 0;
}

int audio_capture_resume(AudioCaptureContext *ctx) {
        if (!ctx || !ctx->running)
                return -EINVAL;

        ctx->paused = false;
        log_info("Audio capture resumed.");
        return 0;
}

bool audio_capture_is_running(const AudioCaptureContext *ctx) {
        return ctx ? ctx->running : false;
}

bool audio_capture_is_paused(const AudioCaptureContext *ctx) {
        return ctx ? ctx->paused : false;
}
