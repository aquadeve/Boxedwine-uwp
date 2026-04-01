/*
 * Boxedwine Android - Audio Backend (OpenSL ES via SDL2 Audio)
 *
 * Provides a minimal OpenSL ES emulation layer that routes audio
 * output from Android native apps through SDL2's audio subsystem.
 *
 * Supports the SLAndroidSimpleBufferQueueItf interface used by
 * most NDK games (Minecraft PE, etc.):
 *   - slCreateEngine() -> SL_RESULT_SUCCESS
 *   - Engine::CreateOutputMix()
 *   - Engine::CreateAudioPlayer() with BufferQueue
 *   - BufferQueue::Enqueue() -> SDL2 audio callback
 *   - Player::SetPlayState(SL_PLAYSTATE_PLAYING)
 */

#ifndef __ANDROID_AUDIO_H__
#define __ANDROID_AUDIO_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of queued audio buffers */
#define ANDROID_AUDIO_MAX_BUFFERS 16

/* Audio format parameters */
typedef struct {
    int      sample_rate;   /* e.g. 44100, 22050 */
    int      channels;      /* 1 = mono, 2 = stereo */
    int      bits_per_sample; /* 8 or 16 */
    int      buffer_size;   /* in bytes, per buffer */
} AndroidAudioConfig;

/* Opaque audio context */
typedef struct AndroidAudioContext AndroidAudioContext;

/**
 * Initialise the audio backend using SDL2.
 * Must be called after SDL_Init(SDL_INIT_AUDIO).
 * Returns a context, or NULL on failure.
 */
AndroidAudioContext *android_audio_create(const AndroidAudioConfig *config);

/**
 * Enqueue a buffer of PCM audio data for playback.
 * The data is copied internally; the caller can reuse the buffer
 * after this call returns.
 * Returns true on success, false if the queue is full.
 */
bool android_audio_enqueue(AndroidAudioContext *ctx, const uint8_t *data, uint32_t size);

/**
 * Start or resume audio playback.
 */
void android_audio_play(AndroidAudioContext *ctx);

/**
 * Pause audio playback.
 */
void android_audio_pause(AndroidAudioContext *ctx);

/**
 * Stop and destroy the audio context.
 */
void android_audio_destroy(AndroidAudioContext *ctx);

/**
 * Set the callback that is invoked when a buffer finishes playing.
 * This mirrors SLAndroidSimpleBufferQueueItf's RegisterCallback.
 * The callback receives the user_data pointer.
 */
typedef void (*AndroidAudioCallback)(void *user_data);
void android_audio_set_callback(AndroidAudioContext *ctx,
                                 AndroidAudioCallback cb, void *user_data);

/**
 * Get the number of buffers currently queued.
 */
int android_audio_queued_count(const AndroidAudioContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* __ANDROID_AUDIO_H__ */
