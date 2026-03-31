/*
 * Boxedwine Android Emulator - ANGLE/OpenGL ES 2.0 Renderer
 *
 * Provides a portable GLES2 rendering backend that works on top of ANGLE's
 * D3D11 translation layer.  This allows the emulator to present its
 * framebuffer (and, in future, forward guest OpenGL ES calls) on platforms
 * where only Direct3D is available — most notably Xbox One.
 *
 * Rendering pipeline:
 *   Host GLES2 calls  →  ANGLE  →  D3D11  →  GPU
 *
 * The renderer maintains a single fullscreen textured quad.  Each frame the
 * emulator's software framebuffer (RGBA8, width × height) is uploaded to a
 * GLES2 texture and drawn.
 *
 * Reference:
 *   referenceCode/Bridge/AppProcessAngle/SimpleRenderer.cpp
 */

#ifndef __ANGLE_RENDERER_H__
#define __ANGLE_RENDERER_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque renderer state */
typedef struct AngleRenderer AngleRenderer;

/**
 * Create and initialise the GLES2 renderer.
 * Must be called after an OpenGL ES context is current (e.g. after
 * SDL_GL_CreateContext on a window created with SDL_WINDOW_OPENGL).
 *
 * @param width   Emulator framebuffer width  (pixels)
 * @param height  Emulator framebuffer height (pixels)
 * @return  A new AngleRenderer, or NULL on failure.
 */
AngleRenderer *angle_renderer_create(int width, int height);

/**
 * Upload new pixel data to the framebuffer texture.
 *
 * @param pixels  RGBA8 pixel data (width * height * 4 bytes), or NULL to
 *                skip the upload (keeps the previous frame).
 */
void angle_renderer_upload(AngleRenderer *r, const uint8_t *pixels);

/**
 * Draw the current framebuffer texture as a fullscreen quad and present.
 * Call once per frame, after angle_renderer_upload().
 *
 * @param screen_w  Current window width  (may differ from framebuffer)
 * @param screen_h  Current window height
 */
void angle_renderer_draw(AngleRenderer *r, int screen_w, int screen_h);

/**
 * Draw a crosshair at the given screen position (for gamepad virtual cursor).
 * Call between angle_renderer_draw() and SDL_GL_SwapWindow().
 */
void angle_renderer_draw_cursor(AngleRenderer *r, float x, float y,
                                int screen_w, int screen_h);

/**
 * Resize the internal framebuffer texture (e.g. if the emulator resolution
 * changes at run-time).
 */
void angle_renderer_resize(AngleRenderer *r, int new_width, int new_height);

/**
 * Destroy the renderer and release all GPU resources.
 */
void angle_renderer_destroy(AngleRenderer *r);

#ifdef __cplusplus
}
#endif

#endif /* __ANGLE_RENDERER_H__ */
