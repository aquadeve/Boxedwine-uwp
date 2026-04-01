/*
 * GL ES 1.x emulation on top of GL ES 2.0 (ANGLE)
 *
 * Translates the fixed-function pipeline calls that Android NDK games
 * use (GL ES 1.1) into programmable pipeline calls on GL ES 2.0.
 *
 * Covers: matrix stacks, vertex arrays, texturing, blending, depth,
 * fog, alpha test, and basic lighting color.  Enough for titles like
 * Minecraft Pocket Edition v0.8.x.
 */

#ifndef __GLES1_ON_GLES2_H__
#define __GLES1_ON_GLES2_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque context */
typedef struct GLES1Context GLES1Context;

/* Create / destroy the emulation context.
 * Must be called while a host GL ES 2.0 context is current.
 * mem_base / mem_size describe the emulated flat address space so that
 * guest pointers can be translated to host addresses. */
GLES1Context *gles1_create(uint8_t *mem_base, uint32_t mem_size);
void gles1_destroy(GLES1Context *ctx);

/* ---- GL ES 1.x matrix operations ---- */
void gles1_matrixMode   (GLES1Context *ctx, uint32_t mode);
void gles1_loadIdentity (GLES1Context *ctx);
void gles1_pushMatrix   (GLES1Context *ctx);
void gles1_popMatrix    (GLES1Context *ctx);
void gles1_translatef   (GLES1Context *ctx, float x, float y, float z);
void gles1_scalef       (GLES1Context *ctx, float x, float y, float z);
void gles1_rotatef      (GLES1Context *ctx, float angle, float x, float y, float z);
void gles1_orthof       (GLES1Context *ctx, float l, float r, float b, float t, float n, float f);
void gles1_multMatrixf  (GLES1Context *ctx, const float *m);
void gles1_loadMatrixf  (GLES1Context *ctx, const float *m);

/* ---- GL state ---- */
void gles1_enable       (GLES1Context *ctx, uint32_t cap);
void gles1_disable      (GLES1Context *ctx, uint32_t cap);
void gles1_clear        (GLES1Context *ctx, uint32_t mask);
void gles1_clearColor   (GLES1Context *ctx, float r, float g, float b, float a);
void gles1_viewport     (GLES1Context *ctx, int x, int y, int w, int h);
void gles1_scissor      (GLES1Context *ctx, int x, int y, int w, int h);
void gles1_blendFunc    (GLES1Context *ctx, uint32_t sfactor, uint32_t dfactor);
void gles1_depthFunc    (GLES1Context *ctx, uint32_t func);
void gles1_depthMask    (GLES1Context *ctx, uint8_t flag);
void gles1_depthRangef  (GLES1Context *ctx, float n, float f);
void gles1_alphaFunc    (GLES1Context *ctx, uint32_t func, float ref);
void gles1_cullFace     (GLES1Context *ctx, uint32_t mode);
void gles1_shadeModel   (GLES1Context *ctx, uint32_t mode);
void gles1_hint         (GLES1Context *ctx, uint32_t target, uint32_t mode);
void gles1_stencilFunc  (GLES1Context *ctx, uint32_t func, int ref, uint32_t mask);
void gles1_stencilMask  (GLES1Context *ctx, uint32_t mask);
void gles1_stencilOp    (GLES1Context *ctx, uint32_t sfail, uint32_t dpfail, uint32_t dppass);
void gles1_lineWidth    (GLES1Context *ctx, float width);
void gles1_polygonOffset(GLES1Context *ctx, float factor, float units);
void gles1_colorMask    (GLES1Context *ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void gles1_color4f      (GLES1Context *ctx, float r, float g, float b, float a);

/* ---- Fog ---- */
void gles1_fogf         (GLES1Context *ctx, uint32_t pname, float param);
void gles1_fogfv        (GLES1Context *ctx, uint32_t pname, const float *params);
void gles1_fogx         (GLES1Context *ctx, uint32_t pname, int32_t param);

/* ---- Lighting (stub – just tracks material color) ---- */
void gles1_lightModelf  (GLES1Context *ctx, uint32_t pname, float param);
void gles1_lightfv      (GLES1Context *ctx, uint32_t light, uint32_t pname, const float *params);

/* ---- Client-side vertex arrays ---- */
void gles1_enableClientState  (GLES1Context *ctx, uint32_t cap);
void gles1_disableClientState (GLES1Context *ctx, uint32_t cap);
void gles1_vertexPointer      (GLES1Context *ctx, int size, uint32_t type, int stride, uint32_t guest_ptr);
void gles1_texCoordPointer    (GLES1Context *ctx, int size, uint32_t type, int stride, uint32_t guest_ptr);
void gles1_colorPointer       (GLES1Context *ctx, int size, uint32_t type, int stride, uint32_t guest_ptr);
void gles1_normalPointer      (GLES1Context *ctx, uint32_t type, int stride, uint32_t guest_ptr);

/* ---- Texture management (forwarded to host GL) ---- */
void     gles1_bindTexture   (GLES1Context *ctx, uint32_t target, uint32_t texture);
void     gles1_genTextures   (GLES1Context *ctx, int n, uint32_t guest_ptr);
void     gles1_deleteTextures(GLES1Context *ctx, int n, uint32_t guest_ptr);
void     gles1_texImage2D    (GLES1Context *ctx, uint32_t target, int level, int ifmt,
                              int w, int h, int border, uint32_t fmt, uint32_t type, uint32_t guest_ptr);
void     gles1_texSubImage2D (GLES1Context *ctx, uint32_t target, int level,
                              int xoff, int yoff, int w, int h,
                              uint32_t fmt, uint32_t type, uint32_t guest_ptr);
void     gles1_texParameteri (GLES1Context *ctx, uint32_t target, uint32_t pname, int param);

/* ---- VBO management (forwarded to host GL) ---- */
void     gles1_bindBuffer    (GLES1Context *ctx, uint32_t target, uint32_t buffer);
void     gles1_genBuffers    (GLES1Context *ctx, int n, uint32_t guest_ptr);
void     gles1_deleteBuffers (GLES1Context *ctx, int n, uint32_t guest_ptr);
void     gles1_bufferData    (GLES1Context *ctx, uint32_t target, int size, uint32_t guest_ptr, uint32_t usage);

/* ---- Draw calls ---- */
void gles1_drawArrays   (GLES1Context *ctx, uint32_t mode, int first, int count);
void gles1_drawElements  (GLES1Context *ctx, uint32_t mode, int count, uint32_t type, uint32_t guest_ptr);

/* ---- Queries ---- */
uint32_t gles1_getError  (GLES1Context *ctx);
uint32_t gles1_getString (GLES1Context *ctx, uint32_t name, uint8_t *mem, uint32_t mem_size, uint32_t *out_guest_ptr);
void     gles1_getFloatv (GLES1Context *ctx, uint32_t pname, uint32_t guest_ptr);
void     gles1_readPixels(GLES1Context *ctx, int x, int y, int w, int h,
                          uint32_t fmt, uint32_t type, uint32_t guest_ptr);

#ifdef __cplusplus
}
#endif

#endif /* __GLES1_ON_GLES2_H__ */
