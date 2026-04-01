/*
 * Boxedwine Android - Audio Backend (OpenSL ES via SDL2 Audio)
 *
 * Implementation of the audio buffer queue backed by SDL2.
 */

#include "android_audio.h"
#include "SDL2/SDL.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Debug logging */
#if defined(_MSC_VER)
#  include <windows.h>
#  include <stdarg.h>
   static inline void _aud_dbg(const char *fmt, ...) {
       char buf[512];
       va_list ap;
       va_start(ap, fmt);
       vsnprintf(buf, sizeof(buf), fmt, ap);
       va_end(ap);
       OutputDebugStringA(buf);
   }
#  ifdef _DEBUG
#    define AUD_LOG_DEBUG(fmt, ...) _aud_dbg("[AUD DEBUG] " fmt "\n", ##__VA_ARGS__)
#  else
#    define AUD_LOG_DEBUG(fmt, ...) ((void)0)
#  endif
#  define AUD_LOG_ERR(fmt, ...) _aud_dbg("[AUD ERROR] " fmt "\n", ##__VA_ARGS__)
#  define AUD_LOG_INFO(fmt, ...) _aud_dbg("[AUD INFO]  " fmt "\n", ##__VA_ARGS__)
#else
#  if !defined(NDEBUG)
#    define AUD_LOG_DEBUG(fmt, ...) fprintf(stderr, "[AUD DEBUG] " fmt "\n", ##__VA_ARGS__)
#  else
#    define AUD_LOG_DEBUG(fmt, ...) ((void)0)
#  endif
#  define AUD_LOG_ERR(fmt, ...) fprintf(stderr, "[AUD ERROR] " fmt "\n", ##__VA_ARGS__)
#  define AUD_LOG_INFO(fmt, ...) fprintf(stderr, "[AUD INFO]  " fmt "\n", ##__VA_ARGS__)
#endif

/* -------------------------------------------------------------------------
 * Ring buffer for audio data
 * ---------------------------------------------------------------------- */
#define RING_SIZE (256 * 1024)  /* 256 KB ring buffer */

struct AndroidAudioContext {
    SDL_AudioDeviceID  device;
    SDL_AudioSpec      spec;
    AndroidAudioConfig config;

    /* Ring buffer for PCM data */
    uint8_t  ring[RING_SIZE];
    uint32_t ring_read;
    uint32_t ring_write;
    uint32_t ring_used;

    /* Buffer completion callback */
    AndroidAudioCallback callback;
    void                *callback_data;

    /* Stats */
    uint32_t buffers_enqueued;
    uint32_t buffers_played;
    bool     playing;
};

/* -------------------------------------------------------------------------
 * SDL audio callback — pulls data from ring buffer
 * ---------------------------------------------------------------------- */
static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
    AndroidAudioContext *ctx = (AndroidAudioContext *)userdata;

    uint32_t available = ctx->ring_used;
    uint32_t to_copy = (uint32_t)len < available ? (uint32_t)len : available;

    /* Copy from ring buffer to SDL stream */
    for (uint32_t i = 0; i < to_copy; i++) {
        stream[i] = ctx->ring[(ctx->ring_read + i) % RING_SIZE];
    }
    ctx->ring_read = (ctx->ring_read + to_copy) % RING_SIZE;
    ctx->ring_used -= to_copy;

    /* Fill remainder with silence */
    if (to_copy < (uint32_t)len) {
        memset(stream + to_copy, ctx->spec.silence, (uint32_t)len - to_copy);
    }

    ctx->buffers_played++;

    /* Invoke buffer completion callback (outside the lock would be ideal,
     * but for simplicity we do it here) */
    if (ctx->callback && to_copy > 0) {
        ctx->callback(ctx->callback_data);
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

AndroidAudioContext *android_audio_create(const AndroidAudioConfig *config) {
    if (!config) return NULL;

    AUD_LOG_DEBUG("create: rate=%d ch=%d bits=%d buf_size=%d",
                  config->sample_rate, config->channels,
                  config->bits_per_sample, config->buffer_size);

    AndroidAudioContext *ctx = (AndroidAudioContext *)calloc(1, sizeof(AndroidAudioContext));
    if (!ctx) return NULL;

    ctx->config = *config;

    SDL_AudioSpec desired;
    memset(&desired, 0, sizeof(desired));
    desired.freq     = config->sample_rate > 0 ? config->sample_rate : 44100;
    desired.format   = (config->bits_per_sample == 8) ? AUDIO_U8 : AUDIO_S16LSB;
    desired.channels = (Uint8)(config->channels > 0 ? config->channels : 2);
    desired.samples  = 1024;  /* Buffer size in frames */
    desired.callback = sdl_audio_callback;
    desired.userdata = ctx;

    ctx->device = SDL_OpenAudioDevice(NULL, 0, &desired, &ctx->spec,
                                       SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (ctx->device == 0) {
        AUD_LOG_ERR("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        free(ctx);
        return NULL;
    }

    AUD_LOG_INFO("audio device opened: freq=%d ch=%d format=0x%04X",
                 ctx->spec.freq, ctx->spec.channels, ctx->spec.format);

    return ctx;
}

bool android_audio_enqueue(AndroidAudioContext *ctx, const uint8_t *data, uint32_t size) {
    if (!ctx || !data || size == 0) return false;

    /* Check if there's room in the ring buffer */
    SDL_LockAudioDevice(ctx->device);
    uint32_t free_space = RING_SIZE - ctx->ring_used;
    if (size > free_space) {
        SDL_UnlockAudioDevice(ctx->device);
        AUD_LOG_DEBUG("enqueue: ring buffer full (need %u, have %u)", size, free_space);
        return false;
    }

    /* Copy into ring buffer */
    for (uint32_t i = 0; i < size; i++) {
        ctx->ring[(ctx->ring_write + i) % RING_SIZE] = data[i];
    }
    ctx->ring_write = (ctx->ring_write + size) % RING_SIZE;
    ctx->ring_used += size;
    ctx->buffers_enqueued++;

    SDL_UnlockAudioDevice(ctx->device);

    AUD_LOG_DEBUG("enqueue: %u bytes queued (total_used=%u, enqueued=%u)",
                  size, ctx->ring_used, ctx->buffers_enqueued);

    return true;
}

void android_audio_play(AndroidAudioContext *ctx) {
    if (!ctx) return;
    AUD_LOG_INFO("audio playback started");
    ctx->playing = true;
    SDL_PauseAudioDevice(ctx->device, 0); /* unpause */
}

void android_audio_pause(AndroidAudioContext *ctx) {
    if (!ctx) return;
    AUD_LOG_INFO("audio playback paused");
    ctx->playing = false;
    SDL_PauseAudioDevice(ctx->device, 1); /* pause */
}

void android_audio_destroy(AndroidAudioContext *ctx) {
    if (!ctx) return;
    AUD_LOG_DEBUG("destroy: enqueued=%u played=%u", ctx->buffers_enqueued, ctx->buffers_played);
    if (ctx->device) {
        SDL_CloseAudioDevice(ctx->device);
    }
    free(ctx);
}

void android_audio_set_callback(AndroidAudioContext *ctx,
                                 AndroidAudioCallback cb, void *user_data) {
    if (!ctx) return;
    SDL_LockAudioDevice(ctx->device);
    ctx->callback = cb;
    ctx->callback_data = user_data;
    SDL_UnlockAudioDevice(ctx->device);
    AUD_LOG_DEBUG("callback registered: %p (data=%p)", (void*)cb, user_data);
}

int android_audio_queued_count(const AndroidAudioContext *ctx) {
    if (!ctx) return 0;
    return (int)(ctx->ring_used > 0 ? 1 : 0);
}
