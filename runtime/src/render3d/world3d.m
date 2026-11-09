/* 3D world renderer (phases 2-3): the live map as depth-tested geometry, seen
 * through a camera fitted each frame to the game's own projection.
 *
 * Projection model (pinhole looking straight down, verified against the
 * game's vertex stream): for a world point (x, y, z) in tiles/levels,
 *   d  = camz - z               (the game hands 1/d to the renderer)
 *   sx = cx + f * (x - camx) / d
 *   sy = cy + f * (y - camy) / d
 * camz is the mean of z + 1/zf over observed vertices; (cx, f, camx) and
 * (cy, f, camy) come from linear least squares on the same vertices. */
#import <Metal/Metal.h>

#include <math.h>
#include <string.h>

#include "render3d/world3d.h"
#include "render3d/world_data.h"
#include "video/presenter.h"
#include "win32/imports.h"
#include "win32/win32.h"

/* ---- toggle ------------------------------------------------------------------ */

static int g_enabled = -1;

int world3d_enabled(void)
{
    if (g_enabled < 0) g_enabled = (int)reg_get_dword("screen", "mac_renderer3d", 0);
    return g_enabled;
}

void world3d_toggle(void)
{
    g_enabled = !world3d_enabled();
    reg_set_dword("screen", "mac_renderer3d", (uint32_t)g_enabled);
    rt_log("world3d: %s", g_enabled ? "3D renderer" : "original renderer");
}

/* ---- camera fit --------------------------------------------------------------- */

typedef struct {
    double n, sz;                       /* camz accumulation */
    double a[3][3], bx[3];              /* x normal equations */
} Fit;

static Fit g_acc;
static struct {
    int valid;
    float camx, camy, camz, f, cx, cy;
} g_cam;
static float g_screen[2] = { 640, 480 };

/* The y half of the fit has its own normal equations (design rows use wy). */
static double g_ay[3][3], g_by[3];

void world3d_observe(float sx, float sy, float zf, float wx, float wy, float wz)
{
    if (zf <= 0.0f) return;
    double d = 1.0 / zf;
    /* sx = cx + f*(wx/d) - (f*camx)*(1/d): unknowns (cx, f, f*camx); same for y */
    double rx[3] = { 1.0, wx / d, -1.0 / d }, ry[3] = { 1.0, wy / d, -1.0 / d };
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            g_acc.a[i][j] += rx[i] * rx[j];
            g_ay[i][j] += ry[i] * ry[j];
        }
        g_acc.bx[i] += rx[i] * sx;
        g_by[i] += ry[i] * sy;
    }
    g_acc.n += 1;
    g_acc.sz += wz + d;
}

static int solve3(double m[3][3], const double b[3], double x[3])
{
    double a[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) a[i][j] = m[i][j];
        a[i][3] = b[i];
    }
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-9) return 0;
        for (int k = 0; k < 4; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        for (int r = 0; r < 3; r++) {
            if (r == c) continue;
            double q = a[r][c] / a[c][c];
            for (int k = c; k < 4; k++) a[r][k] -= q * a[c][k];
        }
    }
    for (int i = 0; i < 3; i++) x[i] = a[i][3] / a[i][i];
    return 1;
}

/* Called at the start of every scene: finish the previous frame's fit. */
void world3d_begin_frame(float w, float h)
{
    g_screen[0] = w;
    g_screen[1] = h;
    double x[3], y[3];
    if (g_acc.n >= 8 && solve3(g_acc.a, g_acc.bx, x) && solve3(g_ay, g_by, y) && x[1] > 1 && y[1] > 1) {
        g_cam.cx = (float)x[0];
        g_cam.f = (float)((x[1] + y[1]) * 0.5);
        g_cam.camx = (float)(x[2] / x[1]);
        g_cam.cy = (float)y[0];
        g_cam.camy = (float)(y[2] / y[1]);
        g_cam.camz = (float)(g_acc.sz / g_acc.n);
        g_cam.valid = 1;
        static int logged;
        if (!logged++)
            rt_log("world3d: camera fit cam=(%.3f, %.3f, %.3f) f=%.1f centre=(%.1f, %.1f) from %.0f vertices",
                   g_cam.camx, g_cam.camy, g_cam.camz, g_cam.f, g_cam.cx, g_cam.cy, g_acc.n);
    }
    memset(&g_acc, 0, sizeof g_acc);
    memset(g_ay, 0, sizeof g_ay);
    memset(g_by, 0, sizeof g_by);
}

/* ---- mesh ------------------------------------------------------------------------- */

typedef struct {
    float x, y, z;      /* world: tiles, tiles, levels */
    float u, v, layer;  /* tile texture coordinates + array layer */
    float shade;
} WVert;

static WVert *g_mesh;
static int g_nmesh, g_capmesh;

static void push_vert(const float p[3], const float uv[2], int tile, float shade)
{
    if (g_nmesh == g_capmesh) {
        g_capmesh = g_capmesh ? g_capmesh * 2 : 65536;
        g_mesh = realloc(g_mesh, sizeof *g_mesh * (size_t)g_capmesh);
    }
    g_mesh[g_nmesh++] = (WVert){ p[0], p[1], p[2], uv[0], uv[1], (float)tile, shade };
}

/* A face: corners in viewing order TL, TR, BR, BL; the face word's flip and
 * rotation are applied to the texture corners. */
static void push_face(const float c[4][3], uint16_t face, float shade)
{
    unsigned tile = FACE_TILE(face);
    if (!tile) return;
    float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    if (FACE_FLIP(face))
        for (int i = 0; i < 4; i++) uv[i][0] = 1 - uv[i][0];
    for (unsigned r = 0; r < FACE_ROT(face); r++) { /* rotate texture a quarter turn */
        float t[2] = { uv[3][0], uv[3][1] };
        memmove(uv[1], uv[0], sizeof(float) * 6);
        uv[0][0] = t[0];
        uv[0][1] = t[1];
    }
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) push_vert(c[idx[i]], uv[idx[i]], (int)tile, shade);
}

/* Directional shading, close to the game's own per-face shades */
#define SH_LID 1.00f
#define SH_LEFT 0.76f
#define SH_RIGHT 0.59f
#define SH_TOP 0.84f
#define SH_BOTTOM 0.67f

static void build_mesh(const WorldMap *m, int x0, int y0, int x1, int y1)
{
    g_nmesh = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            for (int z = 0; z < MAP_LEVELS; z++) {
                const GBlock *b = world_map_block(m, x, y, z);
                if (!b) continue;
                float X = (float)x, Y = (float)y, Z = (float)z, X1 = X + 1, Y1 = Y + 1, Z1 = Z + 1;
                const float lid[4][3] = { { X, Y, Z1 }, { X1, Y, Z1 }, { X1, Y1, Z1 }, { X, Y1, Z1 } };
                const float left[4][3] = { { X, Y1, Z1 }, { X, Y, Z1 }, { X, Y, Z }, { X, Y1, Z } };
                const float right[4][3] = { { X1, Y, Z1 }, { X1, Y1, Z1 }, { X1, Y1, Z }, { X1, Y, Z } };
                const float top[4][3] = { { X, Y, Z1 }, { X1, Y, Z1 }, { X1, Y, Z }, { X, Y, Z } };
                const float bottom[4][3] = { { X1, Y1, Z1 }, { X, Y1, Z1 }, { X, Y1, Z }, { X1, Y1, Z } };
                push_face(lid, b->lid, SH_LID);
                push_face(left, b->left, SH_LEFT);
                push_face(right, b->right, SH_RIGHT);
                push_face(top, b->top, SH_TOP);
                push_face(bottom, b->bottom, SH_BOTTOM);
            }
        }
    }
}

/* ---- Metal ---------------------------------------------------------------------------- */

static id<MTLRenderPipelineState> g_pipe;
static id<MTLDepthStencilState> g_depth_on;
static id<MTLTexture> g_tiles;
static id<MTLSamplerState> g_samp;
static char g_tiles_for[1024];

static NSString *const kShaders =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct WVert { packed_float3 p; packed_float2 uv; float layer; float shade; };\n"
     "struct Cam { float4 fit; float2 screen; float2 centre; packed_float3 cam; float f; float near_d; float far_d; };\n"
     "struct VOut { float4 pos [[position]]; float2 uv; float layer [[flat]]; float shade; };\n"
     "vertex VOut w_vs(uint vid [[vertex_id]], const device WVert *v [[buffer(0)]], constant Cam &c [[buffer(1)]]) {\n"
     "    WVert in = v[vid];\n"
     "    float3 p = float3(in.p);\n"
     "    float3 cam = float3(c.cam);\n    float d = max(cam.z - p.z, 0.05);\n"
     "    float2 s = c.centre + c.f * (p.xy - cam.xy) / d;\n"
     "    float2 t = s / c.screen;\n"
     "    float2 ndc = float2(mix(c.fit.x, c.fit.z, t.x), mix(c.fit.w, c.fit.y, t.y));\n"
     "    float z01 = clamp((d - c.near_d) / (c.far_d - c.near_d), 0.0, 1.0);\n"
     "    VOut o;\n"
     "    o.pos = float4(ndc * d, z01 * d, d);\n"
     "    o.uv = float2(in.uv);\n"
     "    o.layer = in.layer;\n"
     "    o.shade = in.shade;\n"
     "    return o;\n"
     "}\n"
     "fragment float4 w_fs(VOut in [[stage_in]], texture2d_array<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    float4 c = t.sample(s, in.uv, uint(in.layer));\n"
     "    if (c.a < 0.5) discard_fragment();\n"
     "    return float4(c.rgb * in.shade, 1);\n"
     "}\n";

static int setup(void)
{
    if (g_pipe) return 1;
    id<MTLDevice> dev = presenter_device();
    if (!dev) return 0;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kShaders options:nil error:&err];
    if (!lib) {
        rt_log("world3d: shader compile failed: %s", err.localizedDescription.UTF8String);
        return 0;
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"w_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"w_fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    g_pipe = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!g_pipe) {
        rt_log("world3d: pipeline failed: %s", err.localizedDescription.UTF8String);
        return 0;
    }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionLessEqual;
    dd.depthWriteEnabled = YES;
    g_depth_on = [dev newDepthStencilStateWithDescriptor:dd];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_samp = [dev newSamplerStateWithDescriptor:sd];
    return 1;
}

/* (Re)build the tile texture array when the level's style changes. */
static int ensure_tiles(void)
{
    const char *path = world_style_path();
    if (!path) return 0;
    if (g_tiles && !strcmp(path, g_tiles_for)) return 1;
    WorldStyle st;
    if (!world_style_load(path, &st)) return 0;
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2DArray;
    td.pixelFormat = MTLPixelFormatBGRA8Unorm;
    td.width = td.height = TILE_PX;
    td.arrayLength = (NSUInteger)st.ntiles;
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    g_tiles = [presenter_device() newTextureWithDescriptor:td];
    for (int t = 0; t < st.ntiles; t++)
        [g_tiles replaceRegion:MTLRegionMake2D(0, 0, TILE_PX, TILE_PX)
                   mipmapLevel:0
                         slice:(NSUInteger)t
                     withBytes:st.rgba + (size_t)t * TILE_PX * TILE_PX
                   bytesPerRow:TILE_PX * 4
                 bytesPerImage:TILE_PX * TILE_PX * 4];
    snprintf(g_tiles_for, sizeof g_tiles_for, "%s", path);
    world_style_free(&st);
    return 1;
}

void world3d_draw(id<MTLRenderCommandEncoder> enc, const float fit[4])
{
    if (!world3d_enabled() || !g_cam.valid || !setup() || !ensure_tiles()) return;
    WorldMap m;
    if (!world_map_live(&m)) return;

    /* Region: what the camera sees at ground level and below, plus a margin
     * for buildings leaning into view. */
    float reach_x = (g_screen[0] * 0.5f) * (g_cam.camz) / g_cam.f + 3.0f;
    float reach_y = (g_screen[1] * 0.5f) * (g_cam.camz) / g_cam.f + 3.0f;
    int x0 = (int)floorf(g_cam.camx - reach_x), x1 = (int)ceilf(g_cam.camx + reach_x);
    int y0 = (int)floorf(g_cam.camy - reach_y), y1 = (int)ceilf(g_cam.camy + reach_y);
    build_mesh(&m, x0, y0, x1, y1);
    if (!g_nmesh) return;

    struct {
        float fit[4];
        float screen[2], centre[2];
        float cam[3], f;
        float near_d, far_d, pad[2];
    } u;
    memcpy(u.fit, fit, sizeof u.fit);
    u.screen[0] = g_screen[0];
    u.screen[1] = g_screen[1];
    u.centre[0] = g_cam.cx;
    u.centre[1] = g_cam.cy;
    u.cam[0] = g_cam.camx;
    u.cam[1] = g_cam.camy;
    u.cam[2] = g_cam.camz;
    u.f = g_cam.f;
    u.near_d = 0.05f;
    u.far_d = g_cam.camz + 1.0f;

    id<MTLBuffer> vb = [presenter_device() newBufferWithBytes:g_mesh
                                                       length:sizeof *g_mesh * (size_t)g_nmesh
                                                      options:MTLResourceStorageModeShared];
    [enc setRenderPipelineState:g_pipe];
    [enc setDepthStencilState:g_depth_on];
    [enc setCullMode:MTLCullModeNone];
    [enc setVertexBuffer:vb offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentTexture:g_tiles atIndex:0];
    [enc setFragmentSamplerState:g_samp atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)g_nmesh];
}

