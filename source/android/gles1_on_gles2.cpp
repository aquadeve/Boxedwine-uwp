/*
 * GL ES 1.x emulation on top of GL ES 2.0 (ANGLE)
 *
 * Implements the fixed-function pipeline using a single "uber-shader"
 * that is configured at draw time via uniforms.
 *
 * Limitations / simplifications:
 *   - Only GL_MODELVIEW and GL_PROJECTION matrix modes (no GL_TEXTURE)
 *   - Single texture unit (TEXTURE0)
 *   - Fog: linear mode only (GL_EXP / GL_EXP2 approximated as linear)
 *   - Lighting: not emulated (glLightfv is a no-op)
 *   - glReadPixels: forwarded but may be slow
 */

#include "gles1_on_gles2.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _MSC_VER
#include <GLES2/gl2.h>
#else
#include <GLES2/gl2.h>
#endif

/* -----------------------------------------------------------------------
 * GL ES 1.x constants not in GL ES 2.0 headers
 * ----------------------------------------------------------------------- */
#define GL1_MODELVIEW                0x1700
#define GL1_PROJECTION               0x1701
#define GL1_TEXTURE                  0x1702

#define GL1_VERTEX_ARRAY             0x8074
#define GL1_NORMAL_ARRAY             0x8075
#define GL1_COLOR_ARRAY              0x8076
#define GL1_TEXTURE_COORD_ARRAY      0x8078

#define GL1_FLAT                     0x1D00
#define GL1_SMOOTH                   0x1D01

#define GL1_FOG                      0x0B60
#define GL1_FOG_MODE                 0x0B65
#define GL1_FOG_DENSITY              0x0B62
#define GL1_FOG_START                0x0B63
#define GL1_FOG_END                  0x0B64
#define GL1_FOG_COLOR                0x0B66
#define GL1_LINEAR                   0x2601
#define GL1_EXP                      0x0800
#define GL1_EXP2                     0x0801

#define GL1_ALPHA_TEST               0x0BC0
#define GL1_LIGHTING                 0x0B50

#define GL1_LIGHT0                   0x4000
#define GL1_AMBIENT                  0x1200
#define GL1_DIFFUSE                  0x1201
#define GL1_POSITION                 0x1203
#define GL1_LIGHT_MODEL_AMBIENT      0x0B53

#define GL1_MAX_MODELVIEW_STACK_DEPTH  32
#define GL1_MAX_PROJECTION_STACK_DEPTH 4

/* -----------------------------------------------------------------------
 * 4×4 matrix helpers (column-major, OpenGL convention)
 * ----------------------------------------------------------------------- */
static void mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* C = A * B  (column-major) */
static void mat4_multiply(float *C, const float *A, const float *B) {
    float tmp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            tmp[c * 4 + r] =
                A[0 * 4 + r] * B[c * 4 + 0] +
                A[1 * 4 + r] * B[c * 4 + 1] +
                A[2 * 4 + r] * B[c * 4 + 2] +
                A[3 * 4 + r] * B[c * 4 + 3];
        }
    }
    memcpy(C, tmp, sizeof(tmp));
}

static void mat4_translate(float *m, float x, float y, float z) {
    float t[16];
    mat4_identity(t);
    t[12] = x; t[13] = y; t[14] = z;
    float r[16];
    mat4_multiply(r, m, t);
    memcpy(m, r, sizeof(r));
}

static void mat4_scale(float *m, float x, float y, float z) {
    float s[16];
    mat4_identity(s);
    s[0] = x; s[5] = y; s[10] = z;
    float r[16];
    mat4_multiply(r, m, s);
    memcpy(m, r, sizeof(r));
}

static void mat4_rotate(float *m, float angle_deg, float ax, float ay, float az) {
    float rad = angle_deg * 3.14159265358979323846f / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    float len = sqrtf(ax * ax + ay * ay + az * az);
    if (len < 1e-6f) return;
    ax /= len; ay /= len; az /= len;
    float nc = 1.0f - c;

    float rot[16];
    rot[0]  = ax*ax*nc + c;      rot[4]  = ax*ay*nc - az*s;   rot[8]  = ax*az*nc + ay*s;   rot[12] = 0;
    rot[1]  = ay*ax*nc + az*s;   rot[5]  = ay*ay*nc + c;      rot[9]  = ay*az*nc - ax*s;   rot[13] = 0;
    rot[2]  = az*ax*nc - ay*s;   rot[6]  = az*ay*nc + ax*s;   rot[10] = az*az*nc + c;      rot[14] = 0;
    rot[3]  = 0;                 rot[7]  = 0;                 rot[11] = 0;                 rot[15] = 1;

    float r[16];
    mat4_multiply(r, m, rot);
    memcpy(m, r, sizeof(r));
}

static void mat4_ortho(float *m, float l, float r, float b, float t, float n, float f) {
    float o[16];
    memset(o, 0, sizeof(o));
    o[0]  =  2.0f / (r - l);
    o[5]  =  2.0f / (t - b);
    o[10] = -2.0f / (f - n);
    o[12] = -(r + l) / (r - l);
    o[13] = -(t + b) / (t - b);
    o[14] = -(f + n) / (f - n);
    o[15] = 1.0f;

    float res[16];
    mat4_multiply(res, m, o);
    memcpy(m, res, sizeof(res));
}

/* -----------------------------------------------------------------------
 * Vertex array descriptor
 * ----------------------------------------------------------------------- */
typedef struct {
    int      size;       /* components per vertex (1-4) */
    uint32_t type;       /* GL_FLOAT, GL_UNSIGNED_BYTE, etc. */
    int      stride;
    uint32_t guest_ptr;  /* guest VA or VBO offset */
    uint32_t vbo;        /* VBO that was bound when this was set (0 = client) */
    bool     enabled;
} VertexArrayDesc;

/* -----------------------------------------------------------------------
 * Emulation context
 * ----------------------------------------------------------------------- */
struct GLES1Context {
    /* Host memory mapping */
    uint8_t  *mem;
    uint32_t  mem_size;

    /* Shader program */
    GLuint program;
    GLint  a_position;
    GLint  a_texcoord;
    GLint  a_color;
    GLint  u_mvp;
    GLint  u_use_texture;
    GLint  u_alpha_test_enabled;
    GLint  u_alpha_ref;
    GLint  u_fog_enabled;
    GLint  u_fog_start;
    GLint  u_fog_end;
    GLint  u_fog_color;

    /* Matrix stacks */
    uint32_t matrix_mode;  /* GL1_MODELVIEW, GL1_PROJECTION */
    float    mv_stack[GL1_MAX_MODELVIEW_STACK_DEPTH][16];
    int      mv_top;
    float    proj_stack[GL1_MAX_PROJECTION_STACK_DEPTH][16];
    int      proj_top;

    /* Current color (glColor4f) */
    float current_color[4];

    /* Vertex arrays */
    VertexArrayDesc vertex;
    VertexArrayDesc texcoord;
    VertexArrayDesc color;
    VertexArrayDesc normal;

    /* Current bound VBO (GL_ARRAY_BUFFER) */
    uint32_t bound_array_buffer;
    /* Current bound element buffer */
    uint32_t bound_element_buffer;

    /* Texture state */
    bool texture_2d_enabled;
    uint32_t bound_texture;

    /* Alpha test */
    bool  alpha_test_enabled;
    float alpha_ref;

    /* Fog */
    bool  fog_enabled;
    float fog_start;
    float fog_end;
    float fog_color[4];

    /* Static strings for glGetString (stored in guest memory) */
    uint32_t str_vendor_va;
    uint32_t str_renderer_va;
    uint32_t str_version_va;
    uint32_t str_extensions_va;
};

/* -----------------------------------------------------------------------
 * Shader source
 * ----------------------------------------------------------------------- */
static const char *vert_src =
    "attribute vec4 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "attribute vec4 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec4 v_color;\n"
    "varying float v_eye_dist;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * a_position;\n"
    "    v_texcoord = a_texcoord;\n"
    "    v_color = a_color;\n"
    "    vec4 eye_pos = u_mvp * a_position;\n"
    "    v_eye_dist = length(eye_pos.xyz);\n"
    "    gl_PointSize = 1.0;\n"
    "}\n";

static const char *frag_src =
    "precision mediump float;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_use_texture;\n"
    "uniform float u_alpha_test_enabled;\n"
    "uniform float u_alpha_ref;\n"
    "uniform float u_fog_enabled;\n"
    "uniform float u_fog_start;\n"
    "uniform float u_fog_end;\n"
    "uniform vec4  u_fog_color;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec4 v_color;\n"
    "varying float v_eye_dist;\n"
    "void main() {\n"
    "    vec4 color = v_color;\n"
    "    if (u_use_texture > 0.5) {\n"
    "        color *= texture2D(u_texture, v_texcoord);\n"
    "    }\n"
    "    if (u_alpha_test_enabled > 0.5 && color.a < u_alpha_ref)\n"
    "        discard;\n"
    "    if (u_fog_enabled > 0.5) {\n"
    "        float fog_range = u_fog_end - u_fog_start;\n"
    "        float fog = clamp((u_fog_end - v_eye_dist) / max(fog_range, 0.001), 0.0, 1.0);\n"
    "        color.rgb = mix(u_fog_color.rgb, color.rgb, fog);\n"
    "    }\n"
    "    gl_FragColor = color;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
#ifdef _MSC_VER
        OutputDebugStringA("[GLES1] shader compile error: ");
        OutputDebugStringA(log);
        OutputDebugStringA("\n");
#else
        fprintf(stderr, "[GLES1] shader compile error: %s\n", log);
#endif
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* -----------------------------------------------------------------------
 * Create / destroy
 * ----------------------------------------------------------------------- */
GLES1Context *gles1_create(uint8_t *mem_base, uint32_t mem_size) {
    GLES1Context *ctx = (GLES1Context *)calloc(1, sizeof(GLES1Context));
    if (!ctx) return NULL;
    ctx->mem = mem_base;
    ctx->mem_size = mem_size;

    /* Compile shader */
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vs || !fs) { free(ctx); return NULL; }

    ctx->program = glCreateProgram();
    glAttachShader(ctx->program, vs);
    glAttachShader(ctx->program, fs);
    glLinkProgram(ctx->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(ctx->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(ctx->program, sizeof(log), NULL, log);
#ifdef _MSC_VER
        OutputDebugStringA("[GLES1] program link error: ");
        OutputDebugStringA(log);
        OutputDebugStringA("\n");
#else
        fprintf(stderr, "[GLES1] program link error: %s\n", log);
#endif
        glDeleteProgram(ctx->program);
        free(ctx);
        return NULL;
    }

    ctx->a_position = glGetAttribLocation(ctx->program, "a_position");
    ctx->a_texcoord = glGetAttribLocation(ctx->program, "a_texcoord");
    ctx->a_color    = glGetAttribLocation(ctx->program, "a_color");
    ctx->u_mvp      = glGetUniformLocation(ctx->program, "u_mvp");
    ctx->u_use_texture       = glGetUniformLocation(ctx->program, "u_use_texture");
    ctx->u_alpha_test_enabled = glGetUniformLocation(ctx->program, "u_alpha_test_enabled");
    ctx->u_alpha_ref          = glGetUniformLocation(ctx->program, "u_alpha_ref");
    ctx->u_fog_enabled = glGetUniformLocation(ctx->program, "u_fog_enabled");
    ctx->u_fog_start   = glGetUniformLocation(ctx->program, "u_fog_start");
    ctx->u_fog_end     = glGetUniformLocation(ctx->program, "u_fog_end");
    ctx->u_fog_color   = glGetUniformLocation(ctx->program, "u_fog_color");

    /* Init matrix stacks to identity */
    mat4_identity(ctx->mv_stack[0]);
    mat4_identity(ctx->proj_stack[0]);
    ctx->mv_top = 0;
    ctx->proj_top = 0;
    ctx->matrix_mode = GL1_MODELVIEW;

    /* Default current color = white */
    ctx->current_color[0] = 1.0f;
    ctx->current_color[1] = 1.0f;
    ctx->current_color[2] = 1.0f;
    ctx->current_color[3] = 1.0f;

    /* Default vertex array state */
    ctx->vertex.size = 4;
    ctx->vertex.type = GL_FLOAT;
    ctx->texcoord.size = 4;
    ctx->texcoord.type = GL_FLOAT;
    ctx->color.size = 4;
    ctx->color.type = GL_FLOAT;
    ctx->normal.size = 3;
    ctx->normal.type = GL_FLOAT;

    /* Default fog */
    ctx->fog_start = 0.0f;
    ctx->fog_end = 1.0f;
    ctx->fog_color[0] = ctx->fog_color[1] = ctx->fog_color[2] = 0.0f;
    ctx->fog_color[3] = 1.0f;

    /* Default alpha ref */
    ctx->alpha_ref = 0.0f;

    return ctx;
}

void gles1_destroy(GLES1Context *ctx) {
    if (!ctx) return;
    if (ctx->program) glDeleteProgram(ctx->program);
    free(ctx);
}

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */
static float *current_matrix(GLES1Context *ctx) {
    if (ctx->matrix_mode == GL1_PROJECTION)
        return ctx->proj_stack[ctx->proj_top];
    return ctx->mv_stack[ctx->mv_top];
}

static inline const void *guest_to_host(GLES1Context *ctx, uint32_t guest_ptr) {
    if (guest_ptr == 0) return NULL;
    if (guest_ptr < ctx->mem_size)
        return ctx->mem + guest_ptr;
    return NULL;
}

/* -----------------------------------------------------------------------
 * Matrix operations
 * ----------------------------------------------------------------------- */
void gles1_matrixMode(GLES1Context *ctx, uint32_t mode) {
    ctx->matrix_mode = mode;
}

void gles1_loadIdentity(GLES1Context *ctx) {
    mat4_identity(current_matrix(ctx));
}

void gles1_pushMatrix(GLES1Context *ctx) {
    if (ctx->matrix_mode == GL1_PROJECTION) {
        if (ctx->proj_top < GL1_MAX_PROJECTION_STACK_DEPTH - 1) {
            memcpy(ctx->proj_stack[ctx->proj_top + 1],
                   ctx->proj_stack[ctx->proj_top], 16 * sizeof(float));
            ctx->proj_top++;
        }
    } else {
        if (ctx->mv_top < GL1_MAX_MODELVIEW_STACK_DEPTH - 1) {
            memcpy(ctx->mv_stack[ctx->mv_top + 1],
                   ctx->mv_stack[ctx->mv_top], 16 * sizeof(float));
            ctx->mv_top++;
        }
    }
}

void gles1_popMatrix(GLES1Context *ctx) {
    if (ctx->matrix_mode == GL1_PROJECTION) {
        if (ctx->proj_top > 0) ctx->proj_top--;
    } else {
        if (ctx->mv_top > 0) ctx->mv_top--;
    }
}

void gles1_translatef(GLES1Context *ctx, float x, float y, float z) {
    mat4_translate(current_matrix(ctx), x, y, z);
}

void gles1_scalef(GLES1Context *ctx, float x, float y, float z) {
    mat4_scale(current_matrix(ctx), x, y, z);
}

void gles1_rotatef(GLES1Context *ctx, float angle, float x, float y, float z) {
    mat4_rotate(current_matrix(ctx), angle, x, y, z);
}

void gles1_orthof(GLES1Context *ctx, float l, float r, float b, float t, float n, float f) {
    mat4_ortho(current_matrix(ctx), l, r, b, t, n, f);
}

void gles1_multMatrixf(GLES1Context *ctx, const float *m) {
    if (!m) return;
    float *cur = current_matrix(ctx);
    float result[16];
    mat4_multiply(result, cur, m);
    memcpy(cur, result, sizeof(result));
}

void gles1_loadMatrixf(GLES1Context *ctx, const float *m) {
    if (!m) return;
    memcpy(current_matrix(ctx), m, 16 * sizeof(float));
}

/* -----------------------------------------------------------------------
 * GL state (forwarded directly to host GLES2 where possible)
 * ----------------------------------------------------------------------- */
void gles1_enable(GLES1Context *ctx, uint32_t cap) {
    switch (cap) {
    case GL_DEPTH_TEST:
    case GL_BLEND:
    case GL_SCISSOR_TEST:
    case GL_CULL_FACE:
    case GL_STENCIL_TEST:
    case GL_DITHER:
    case GL_POLYGON_OFFSET_FILL:
        glEnable(cap);
        break;
    case GL_TEXTURE_2D:
        ctx->texture_2d_enabled = true;
        break;
    case GL1_ALPHA_TEST:
        ctx->alpha_test_enabled = true;
        break;
    case GL1_FOG:
        ctx->fog_enabled = true;
        break;
    case GL1_LIGHTING:
        /* Lighting not emulated; silently ignore */
        break;
    default:
        /* Other caps: try forwarding; ignore errors */
        glEnable(cap);
        break;
    }
}

void gles1_disable(GLES1Context *ctx, uint32_t cap) {
    switch (cap) {
    case GL_DEPTH_TEST:
    case GL_BLEND:
    case GL_SCISSOR_TEST:
    case GL_CULL_FACE:
    case GL_STENCIL_TEST:
    case GL_DITHER:
    case GL_POLYGON_OFFSET_FILL:
        glDisable(cap);
        break;
    case GL_TEXTURE_2D:
        ctx->texture_2d_enabled = false;
        break;
    case GL1_ALPHA_TEST:
        ctx->alpha_test_enabled = false;
        break;
    case GL1_FOG:
        ctx->fog_enabled = false;
        break;
    case GL1_LIGHTING:
        break;
    default:
        glDisable(cap);
        break;
    }
}

void gles1_clear(GLES1Context *ctx, uint32_t mask) {
    (void)ctx;
    glClear(mask);
}

void gles1_clearColor(GLES1Context *ctx, float r, float g, float b, float a) {
    (void)ctx;
    glClearColor(r, g, b, a);
}

void gles1_viewport(GLES1Context *ctx, int x, int y, int w, int h) {
    (void)ctx;
    glViewport(x, y, w, h);
}

void gles1_scissor(GLES1Context *ctx, int x, int y, int w, int h) {
    (void)ctx;
    glScissor(x, y, w, h);
}

void gles1_blendFunc(GLES1Context *ctx, uint32_t sfactor, uint32_t dfactor) {
    (void)ctx;
    glBlendFunc(sfactor, dfactor);
}

void gles1_depthFunc(GLES1Context *ctx, uint32_t func) {
    (void)ctx;
    glDepthFunc(func);
}

void gles1_depthMask(GLES1Context *ctx, uint8_t flag) {
    (void)ctx;
    glDepthMask(flag);
}

void gles1_depthRangef(GLES1Context *ctx, float n, float f) {
    (void)ctx;
    glDepthRangef(n, f);
}

void gles1_alphaFunc(GLES1Context *ctx, uint32_t func, float ref) {
    (void)func; /* We only support GL_GREATER-style; just store the ref */
    ctx->alpha_ref = ref;
}

void gles1_cullFace(GLES1Context *ctx, uint32_t mode) {
    (void)ctx;
    glCullFace(mode);
}

void gles1_shadeModel(GLES1Context *ctx, uint32_t mode) {
    (void)ctx;
    (void)mode;
    /* GL ES 2.0 always interpolates; GL_FLAT would require flat qualifier
       which isn't available in GLSL ES 1.00.  Ignore. */
}

void gles1_hint(GLES1Context *ctx, uint32_t target, uint32_t mode) {
    (void)ctx;
    glHint(target, mode);
}

void gles1_stencilFunc(GLES1Context *ctx, uint32_t func, int ref, uint32_t mask) {
    (void)ctx;
    glStencilFunc(func, ref, mask);
}

void gles1_stencilMask(GLES1Context *ctx, uint32_t mask) {
    (void)ctx;
    glStencilMask(mask);
}

void gles1_stencilOp(GLES1Context *ctx, uint32_t sfail, uint32_t dpfail, uint32_t dppass) {
    (void)ctx;
    glStencilOp(sfail, dpfail, dppass);
}

void gles1_lineWidth(GLES1Context *ctx, float width) {
    (void)ctx;
    glLineWidth(width);
}

void gles1_polygonOffset(GLES1Context *ctx, float factor, float units) {
    (void)ctx;
    glPolygonOffset(factor, units);
}

void gles1_colorMask(GLES1Context *ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    (void)ctx;
    glColorMask(r, g, b, a);
}

void gles1_color4f(GLES1Context *ctx, float r, float g, float b, float a) {
    ctx->current_color[0] = r;
    ctx->current_color[1] = g;
    ctx->current_color[2] = b;
    ctx->current_color[3] = a;
}

/* -----------------------------------------------------------------------
 * Fog
 * ----------------------------------------------------------------------- */
void gles1_fogf(GLES1Context *ctx, uint32_t pname, float param) {
    switch (pname) {
    case GL1_FOG_START:   ctx->fog_start = param; break;
    case GL1_FOG_END:     ctx->fog_end = param; break;
    case GL1_FOG_DENSITY: /* store but linear fog ignores it */ break;
    case GL1_FOG_MODE:    /* we only do linear */ break;
    default: break;
    }
}

void gles1_fogfv(GLES1Context *ctx, uint32_t pname, const float *params) {
    if (!params) return;
    switch (pname) {
    case GL1_FOG_COLOR:
        memcpy(ctx->fog_color, params, 4 * sizeof(float));
        break;
    default:
        gles1_fogf(ctx, pname, params[0]);
        break;
    }
}

void gles1_fogx(GLES1Context *ctx, uint32_t pname, int32_t param) {
    /* Fixed-point 16.16 → float */
    gles1_fogf(ctx, pname, (float)param / 65536.0f);
}

/* -----------------------------------------------------------------------
 * Lighting stubs
 * ----------------------------------------------------------------------- */
void gles1_lightModelf(GLES1Context *ctx, uint32_t pname, float param) {
    (void)ctx; (void)pname; (void)param;
}

void gles1_lightfv(GLES1Context *ctx, uint32_t light, uint32_t pname, const float *params) {
    (void)ctx; (void)light; (void)pname; (void)params;
}

/* -----------------------------------------------------------------------
 * Client-side vertex arrays
 * ----------------------------------------------------------------------- */
void gles1_enableClientState(GLES1Context *ctx, uint32_t cap) {
    switch (cap) {
    case GL1_VERTEX_ARRAY:        ctx->vertex.enabled = true; break;
    case GL1_TEXTURE_COORD_ARRAY: ctx->texcoord.enabled = true; break;
    case GL1_COLOR_ARRAY:         ctx->color.enabled = true; break;
    case GL1_NORMAL_ARRAY:        ctx->normal.enabled = true; break;
    default: break;
    }
}

void gles1_disableClientState(GLES1Context *ctx, uint32_t cap) {
    switch (cap) {
    case GL1_VERTEX_ARRAY:        ctx->vertex.enabled = false; break;
    case GL1_TEXTURE_COORD_ARRAY: ctx->texcoord.enabled = false; break;
    case GL1_COLOR_ARRAY:         ctx->color.enabled = false; break;
    case GL1_NORMAL_ARRAY:        ctx->normal.enabled = false; break;
    default: break;
    }
}

void gles1_vertexPointer(GLES1Context *ctx, int size, uint32_t type, int stride, uint32_t guest_ptr) {
    ctx->vertex.size = size;
    ctx->vertex.type = type;
    ctx->vertex.stride = stride;
    ctx->vertex.guest_ptr = guest_ptr;
    ctx->vertex.vbo = ctx->bound_array_buffer;
}

void gles1_texCoordPointer(GLES1Context *ctx, int size, uint32_t type, int stride, uint32_t guest_ptr) {
    ctx->texcoord.size = size;
    ctx->texcoord.type = type;
    ctx->texcoord.stride = stride;
    ctx->texcoord.guest_ptr = guest_ptr;
    ctx->texcoord.vbo = ctx->bound_array_buffer;
}

void gles1_colorPointer(GLES1Context *ctx, int size, uint32_t type, int stride, uint32_t guest_ptr) {
    ctx->color.size = size;
    ctx->color.type = type;
    ctx->color.stride = stride;
    ctx->color.guest_ptr = guest_ptr;
    ctx->color.vbo = ctx->bound_array_buffer;
}

void gles1_normalPointer(GLES1Context *ctx, uint32_t type, int stride, uint32_t guest_ptr) {
    ctx->normal.size = 3;
    ctx->normal.type = type;
    ctx->normal.stride = stride;
    ctx->normal.guest_ptr = guest_ptr;
    ctx->normal.vbo = ctx->bound_array_buffer;
}

/* -----------------------------------------------------------------------
 * Texture management (forwarded)
 * ----------------------------------------------------------------------- */
void gles1_bindTexture(GLES1Context *ctx, uint32_t target, uint32_t texture) {
    ctx->bound_texture = texture;
    glBindTexture(target, texture);
}

void gles1_genTextures(GLES1Context *ctx, int n, uint32_t guest_ptr) {
    if (n <= 0 || guest_ptr + (uint32_t)(n * 4) > ctx->mem_size) return;
    GLuint *ids = (GLuint *)(ctx->mem + guest_ptr);
    glGenTextures(n, ids);
}

void gles1_deleteTextures(GLES1Context *ctx, int n, uint32_t guest_ptr) {
    if (n <= 0 || guest_ptr + (uint32_t)(n * 4) > ctx->mem_size) return;
    const GLuint *ids = (const GLuint *)(ctx->mem + guest_ptr);
    glDeleteTextures(n, ids);
}

void gles1_texImage2D(GLES1Context *ctx, uint32_t target, int level, int ifmt,
                      int w, int h, int border, uint32_t fmt, uint32_t type, uint32_t guest_ptr) {
    const void *pixels = NULL;
    if (guest_ptr != 0) {
        pixels = guest_to_host(ctx, guest_ptr);
    }
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, pixels);
}

void gles1_texSubImage2D(GLES1Context *ctx, uint32_t target, int level,
                         int xoff, int yoff, int w, int h,
                         uint32_t fmt, uint32_t type, uint32_t guest_ptr) {
    const void *pixels = NULL;
    if (guest_ptr != 0) {
        pixels = guest_to_host(ctx, guest_ptr);
    }
    glTexSubImage2D(target, level, xoff, yoff, w, h, fmt, type, pixels);
}

void gles1_texParameteri(GLES1Context *ctx, uint32_t target, uint32_t pname, int param) {
    (void)ctx;
    glTexParameteri(target, pname, param);
}

/* -----------------------------------------------------------------------
 * VBO management (forwarded)
 * ----------------------------------------------------------------------- */
void gles1_bindBuffer(GLES1Context *ctx, uint32_t target, uint32_t buffer) {
    if (target == GL_ARRAY_BUFFER)
        ctx->bound_array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        ctx->bound_element_buffer = buffer;
    glBindBuffer(target, buffer);
}

void gles1_genBuffers(GLES1Context *ctx, int n, uint32_t guest_ptr) {
    if (n <= 0 || guest_ptr + (uint32_t)(n * 4) > ctx->mem_size) return;
    GLuint *ids = (GLuint *)(ctx->mem + guest_ptr);
    glGenBuffers(n, ids);
}

void gles1_deleteBuffers(GLES1Context *ctx, int n, uint32_t guest_ptr) {
    if (n <= 0 || guest_ptr + (uint32_t)(n * 4) > ctx->mem_size) return;
    const GLuint *ids = (const GLuint *)(ctx->mem + guest_ptr);
    glDeleteBuffers(n, ids);
}

void gles1_bufferData(GLES1Context *ctx, uint32_t target, int size, uint32_t guest_ptr, uint32_t usage) {
    const void *data = NULL;
    if (guest_ptr != 0) {
        data = guest_to_host(ctx, guest_ptr);
    }
    glBufferData(target, size, data, usage);
}

/* -----------------------------------------------------------------------
 * Draw calls – the heart of the translation
 * ----------------------------------------------------------------------- */

static void setup_attrib(GLES1Context *ctx, GLint loc, const VertexArrayDesc *desc,
                         const float *default_val, int default_size) {
    if (loc < 0) return;

    if (desc->enabled) {
        /* Bind the VBO that was active when this pointer was set */
        glBindBuffer(GL_ARRAY_BUFFER, desc->vbo);

        const void *ptr;
        if (desc->vbo != 0) {
            /* VBO: pointer is an offset */
            ptr = (const void *)(uintptr_t)desc->guest_ptr;
        } else {
            /* Client-side: translate guest pointer to host */
            ptr = guest_to_host(ctx, desc->guest_ptr);
            if (!ptr) {
                glDisableVertexAttribArray(loc);
                glVertexAttrib4fv(loc, default_val);
                return;
            }
        }

        GLboolean normalized = (desc->type == GL_UNSIGNED_BYTE) ? GL_TRUE : GL_FALSE;
        glVertexAttribPointer(loc, desc->size, desc->type, normalized,
                              desc->stride, ptr);
        glEnableVertexAttribArray(loc);
    } else {
        glDisableVertexAttribArray(loc);
        if (default_val) {
            if (default_size == 4)
                glVertexAttrib4fv(loc, default_val);
            else if (default_size == 2) {
                glVertexAttrib2fv(loc, default_val);
            }
        }
    }
}

static void prepare_draw(GLES1Context *ctx) {
    glUseProgram(ctx->program);

    /* Compute MVP = projection * modelview */
    float mvp[16];
    mat4_multiply(mvp, ctx->proj_stack[ctx->proj_top], ctx->mv_stack[ctx->mv_top]);
    glUniformMatrix4fv(ctx->u_mvp, 1, GL_FALSE, mvp);

    /* Texture */
    float use_tex = (ctx->texture_2d_enabled && ctx->bound_texture != 0) ? 1.0f : 0.0f;
    glUniform1f(ctx->u_use_texture, use_tex);
    if (use_tex > 0.5f) {
        glActiveTexture(GL_TEXTURE0);
        /* texture is already bound via gles1_bindTexture */
    }

    /* Alpha test */
    glUniform1f(ctx->u_alpha_test_enabled, ctx->alpha_test_enabled ? 1.0f : 0.0f);
    glUniform1f(ctx->u_alpha_ref, ctx->alpha_ref);

    /* Fog */
    glUniform1f(ctx->u_fog_enabled, ctx->fog_enabled ? 1.0f : 0.0f);
    glUniform1f(ctx->u_fog_start, ctx->fog_start);
    glUniform1f(ctx->u_fog_end, ctx->fog_end);
    glUniform4fv(ctx->u_fog_color, 1, ctx->fog_color);

    /* Vertex attributes */
    static const float default_texcoord[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    setup_attrib(ctx, ctx->a_position, &ctx->vertex, NULL, 0);
    setup_attrib(ctx, ctx->a_texcoord, &ctx->texcoord, default_texcoord, 2);
    setup_attrib(ctx, ctx->a_color, &ctx->color, ctx->current_color, 4);

    /* Restore the array buffer binding that the guest expects */
    glBindBuffer(GL_ARRAY_BUFFER, ctx->bound_array_buffer);
}

void gles1_drawArrays(GLES1Context *ctx, uint32_t mode, int first, int count) {
    prepare_draw(ctx);
    glDrawArrays(mode, first, count);
}

void gles1_drawElements(GLES1Context *ctx, uint32_t mode, int count, uint32_t type, uint32_t guest_ptr) {
    prepare_draw(ctx);

    /* Element buffer: if bound, pointer is offset; otherwise translate */
    if (ctx->bound_element_buffer != 0) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->bound_element_buffer);
        glDrawElements(mode, count, type, (const void *)(uintptr_t)guest_ptr);
    } else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        const void *indices = guest_to_host(ctx, guest_ptr);
        if (indices) {
            glDrawElements(mode, count, type, indices);
        }
    }
}

/* -----------------------------------------------------------------------
 * Queries
 * ----------------------------------------------------------------------- */
uint32_t gles1_getError(GLES1Context *ctx) {
    (void)ctx;
    return glGetError();
}

uint32_t gles1_getString(GLES1Context *ctx, uint32_t name, uint8_t *mem, uint32_t mem_sz,
                         uint32_t *out_guest_ptr) {
    (void)ctx;
    const char *str = (const char *)glGetString(name);
    if (!str) { *out_guest_ptr = 0; return 0; }

    /* We need to write the string into guest memory so the guest can read it.
     * Use a fixed area in high emulated memory for each known string name. */
    /* For simplicity, return 0 (NULL) – most games don't rely on glGetString
       returning valid data for rendering to work. */
    *out_guest_ptr = 0;
    return 0;
}

void gles1_getFloatv(GLES1Context *ctx, uint32_t pname, uint32_t guest_ptr) {
    if (guest_ptr == 0 || guest_ptr + 64 > ctx->mem_size) return;
    float *out = (float *)(ctx->mem + guest_ptr);

    /* Handle matrix queries that GLES2 doesn't have */
    switch (pname) {
    case 0x0BA6: /* GL_MODELVIEW_MATRIX */
        memcpy(out, ctx->mv_stack[ctx->mv_top], 16 * sizeof(float));
        return;
    case 0x0BA7: /* GL_PROJECTION_MATRIX */
        memcpy(out, ctx->proj_stack[ctx->proj_top], 16 * sizeof(float));
        return;
    default:
        glGetFloatv(pname, out);
        break;
    }
}

void gles1_readPixels(GLES1Context *ctx, int x, int y, int w, int h,
                      uint32_t fmt, uint32_t type, uint32_t guest_ptr) {
    if (guest_ptr == 0) return;
    void *pixels = (void *)(ctx->mem + guest_ptr);
    if (guest_ptr + (uint32_t)(w * h * 4) > ctx->mem_size) return;
    glReadPixels(x, y, w, h, fmt, type, pixels);
}
