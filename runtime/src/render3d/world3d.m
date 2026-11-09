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
#include "render/lights.h"
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

void world3d_set(int on) { g_enabled = on; }

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

/* Directional shading, close to the game's own per-face shades */
#define SH_LID 1.00f
#define SH_LEFT 0.76f
#define SH_RIGHT 0.59f
#define SH_TOP 0.84f
#define SH_BOTTOM 0.67f
#define SH_DIAG 0.70f

/* A wall between two footprint points a -> b (seen from outside, a on the
 * left), from the block floor up to the lid heights ha/hb (fractions of a
 * level). The texture is trimmed at the top for a sloped edge. */
static void push_wall(float ax, float ay, float bx, float by, float z, float ha, float hb, uint16_t face, float shade)
{
    if (ha <= 0.001f && hb <= 0.001f) return;
    const float c[4][3] = { { ax, ay, z + ha }, { bx, by, z + hb }, { bx, by, z }, { ax, ay, z } };
    unsigned tile = FACE_TILE(face);
    if (!tile) return;
    /* Upright, u from a (left as seen from outside) to b: what the original
     * renderer produces for a face with no rotation or flip bits. The face's
     * own flip and quarter turns apply on top of that. */
    float uv[4][2] = { { 0, 1 - ha }, { 1, 1 - hb }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 4; i++) {
        float u = uv[i][0], w = uv[i][1];
        if (FACE_FLIP(face)) u = 1 - u;
        for (unsigned r = 0; r < FACE_ROT(face); r++) { float t = u; u = 1 - w; w = t; }
        uv[i][0] = u;
        uv[i][1] = w;
    }
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) push_vert(c[idx[i]], uv[idx[i]], (int)tile, shade);
}

/* Lid over a footprint polygon (up to 4 points, corners of the unit cell in
 * cell-local coords), heights interpolated from the four cell corners. */
static void push_lid(float X, float Y, float z, const float (*pts)[2], int n, const float h[4], uint16_t face)
{
    unsigned tile = FACE_TILE(face);
    if (!tile || n < 3) return;
    float v[4][3], uv[4][2];
    for (int i = 0; i < n; i++) {
        float px = pts[i][0], py = pts[i][1];
        /* bilinear height over the cell: h = NW, NE, SE, SW */
        float hz = (1 - px) * (1 - py) * h[0] + px * (1 - py) * h[1] + px * py * h[2] + (1 - px) * py * h[3];
        v[i][0] = X + px;
        v[i][1] = Y + py;
        v[i][2] = z + hz;
        float u = px, w = py;
        if (FACE_FLIP(face)) u = 1 - u;
        for (unsigned r = 0; r < FACE_ROT(face); r++) { float t = u; u = 1 - w; w = t; }
        uv[i][0] = u;
        uv[i][1] = w;
    }
    for (int i = 1; i + 1 < n; i++) {
        push_vert(v[0], uv[0], (int)tile, SH_LID);
        push_vert(v[i], uv[i], (int)tile, SH_LID);
        push_vert(v[i + 1], uv[i + 1], (int)tile, SH_LID);
    }
}

/* Lid corner heights (NW, NE, SE, SW) for ramp slope types; 0 if not a ramp. */
static int ramp_heights(unsigned st, float h[4])
{
    int dir, n, k;
    if (st >= 1 && st <= 8) { dir = (int)(st - 1) / 2; n = 2; k = (int)(st - 1) % 2; }
    else if (st >= 9 && st <= 40) { dir = (int)(st - 9) / 8; n = 8; k = (int)(st - 9) % 8; }
    else if (st >= 41 && st <= 44) { dir = (int)(st - 41); n = 1; k = 0; }
    else return 0;
    float lo = (float)k / n, hi = (float)(k + 1) / n;
    switch (dir) { /* the side named is the high side */
    case 0: h[0] = h[1] = hi; h[2] = h[3] = lo; break; /* up: north high */
    case 1: h[0] = h[1] = lo; h[2] = h[3] = hi; break; /* down: south high */
    case 2: h[0] = h[3] = hi; h[1] = h[2] = lo; break; /* left: west high */
    default: h[0] = h[3] = lo; h[1] = h[2] = hi; break; /* right: east high */
    }
    return 1;
}

static void build_block(int x, int y, int z, const GBlock *b)
{
    float X = (float)x, Y = (float)y, Z = (float)z, X1 = X + 1, Y1 = Y + 1;
    unsigned st = b->slope >> 2;
    float h[4] = { 1, 1, 1, 1 };
    static const float cell[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

    if (st >= 45 && st <= 52) {
        /* Diagonal: triangular footprint, slanted wall along the diagonal.
         * 45 faces up-left (solid SE), 46 up-right (solid SW),
         * 47 down-left (solid NE), 48 down-right (solid NW); 49-52 the same
         * footprints (their sloped lids are drawn flat for now). */
        int kind = (int)(st >= 49 ? st - 49 : st - 45);
        float tri[3][2];
        switch (kind) {
        case 0: /* solid SE: NE, SE, SW */
            tri[0][0] = 1; tri[0][1] = 0; tri[1][0] = 1; tri[1][1] = 1; tri[2][0] = 0; tri[2][1] = 1;
            push_wall(X, Y1, X1, Y, Z, 1, 1, b->left, SH_DIAG);       /* SW -> NE, facing NW */
            push_wall(X1, Y, X1, Y1, Z, 1, 1, b->right, SH_RIGHT);
            push_wall(X1, Y1, X, Y1, Z, 1, 1, b->bottom, SH_BOTTOM);
            break;
        case 1: /* solid SW: NW, SE, SW */
            tri[0][0] = 0; tri[0][1] = 0; tri[1][0] = 1; tri[1][1] = 1; tri[2][0] = 0; tri[2][1] = 1;
            push_wall(X, Y, X1, Y1, Z, 1, 1, b->right, SH_DIAG);      /* NW -> SE, facing NE */
            push_wall(X, Y1, X, Y, Z, 1, 1, b->left, SH_LEFT);
            push_wall(X1, Y1, X, Y1, Z, 1, 1, b->bottom, SH_BOTTOM);
            break;
        case 2: /* solid NE: NW, NE, SE */
            tri[0][0] = 0; tri[0][1] = 0; tri[1][0] = 1; tri[1][1] = 0; tri[2][0] = 1; tri[2][1] = 1;
            push_wall(X1, Y1, X, Y, Z, 1, 1, b->left, SH_DIAG);       /* SE -> NW, facing SW */
            push_wall(X1, Y, X1, Y1, Z, 1, 1, b->right, SH_RIGHT);
            push_wall(X, Y, X1, Y, Z, 1, 1, b->top, SH_TOP);
            break;
        default: /* solid NW: NW, NE, SW */
            tri[0][0] = 0; tri[0][1] = 0; tri[1][0] = 1; tri[1][1] = 0; tri[2][0] = 0; tri[2][1] = 1;
            push_wall(X1, Y, X, Y1, Z, 1, 1, b->right, SH_DIAG);      /* NE -> SW, facing SE */
            push_wall(X, Y1, X, Y, Z, 1, 1, b->left, SH_LEFT);
            push_wall(X, Y, X1, Y, Z, 1, 1, b->top, SH_TOP);
            break;
        }
        push_lid(X, Y, Z, (const float(*)[2])tri, 3, h, b->lid);
        return;
    }

    if (st >= 53 && st <= 61) {
        /* Partial blocks: a thin slab against one side, a corner post, or a centre post. */
        const float t = 0.375f;
        float x0 = 0, x1 = 1, y0 = 0, y1 = 1;
        switch (st) {
        case 53: x1 = t; break;            /* left */
        case 54: x0 = 1 - t; break;        /* right */
        case 55: y1 = t; break;            /* top */
        case 56: y0 = 1 - t; break;        /* bottom */
        case 57: x1 = t; y1 = t; break;    /* top-left */
        case 58: x0 = 1 - t; y1 = t; break;/* top-right */
        case 59: x0 = 1 - t; y0 = 1 - t; break; /* bottom-right */
        case 60: x1 = t; y0 = 1 - t; break;/* bottom-left */
        default: x0 = y0 = 0.5f - t / 2; x1 = y1 = 0.5f + t / 2; break; /* centre */
        }
        const float rect[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
        push_lid(X, Y, Z, rect, 4, h, b->lid);
        push_wall(X + x0, Y + y1, X + x0, Y + y0, Z, 1, 1, b->left, SH_LEFT);
        push_wall(X + x1, Y + y0, X + x1, Y + y1, Z, 1, 1, b->right, SH_RIGHT);
        push_wall(X + x0, Y + y0, X + x1, Y + y0, Z, 1, 1, b->top, SH_TOP);
        push_wall(X + x1, Y + y1, X + x0, Y + y1, Z, 1, 1, b->bottom, SH_BOTTOM);
        return;
    }

    ramp_heights(st, h); /* leaves 1,1,1,1 for ordinary blocks */
    push_lid(X, Y, Z, cell, 4, h, b->lid);
    push_wall(X, Y1, X, Y, Z, h[3], h[0], b->left, SH_LEFT);      /* SW -> NW */
    push_wall(X1, Y, X1, Y1, Z, h[1], h[2], b->right, SH_RIGHT);  /* NE -> SE */
    push_wall(X, Y, X1, Y, Z, h[0], h[1], b->top, SH_TOP);        /* NW -> NE */
    push_wall(X1, Y1, X, Y1, Z, h[2], h[3], b->bottom, SH_BOTTOM);/* SE -> SW */
}

static void build_mesh(const WorldMap *m, int x0, int y0, int x1, int y1)
{
    g_nmesh = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            for (int z = 0; z < MAP_LEVELS; z++) {
                const GBlock *b = world_map_block(m, x, y, z);
                if (b) build_block(x, y, z, b);
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
     "struct Cam { float4 fit; float2 screen; float2 centre; packed_float3 cam; float f; float near_d; float far_d;\n"
     "             float ambient; int nlights; };\n"
     "struct Light { packed_float3 p; float radius; float radius2; float intensity; packed_float3 rgb; };\n"
     "struct VOut { float4 pos [[position]]; float2 uv; float layer [[flat]]; float shade; float3 world; };\n"
     "vertex VOut w_vs(uint vid [[vertex_id]], const device WVert *v [[buffer(0)]], constant Cam &c [[buffer(1)]]) {\n"
     "    WVert in = v[vid];\n"
     "    float3 p = float3(in.p);\n"
     "    float3 cam = float3(c.cam);\n"
     "    float d = max(cam.z - p.z, 0.05);\n"
     "    float2 s = c.centre + c.f * (p.xy - cam.xy) / d;\n"
     "    float2 t = s / c.screen;\n"
     "    float2 ndc = float2(mix(c.fit.x, c.fit.z, t.x), mix(c.fit.w, c.fit.y, t.y));\n"
     "    float z01 = clamp((d - c.near_d) / (c.far_d - c.near_d), 0.0, 1.0);\n"
     "    VOut o;\n"
     "    o.pos = float4(ndc * d, z01 * d, d);\n"
     "    o.uv = float2(in.uv);\n"
     "    o.layer = in.layer;\n"
     "    o.shade = in.shade;\n"
     "    o.world = p;\n"
     "    return o;\n"
     "}\n"
     /* Lighting, per pixel, following the original renderer's rule: at full
      * ambient (daylight) only the face's directional shade applies; below
      * it, ambient + shade^2 * sum(light falloff * intensity * colour). */
     "fragment float4 w_fs(VOut in [[stage_in]], texture2d_array<float> t [[texture(0)]], sampler s [[sampler(0)]],\n"
     "                     constant Cam &c [[buffer(1)]], const device Light *lights [[buffer(2)]]) {\n"
     "    float4 tex = t.sample(s, in.uv, uint(in.layer));\n"
     "    if (tex.a < 0.5) discard_fragment();\n"
     "    if (c.ambient >= 255.0) return float4(tex.rgb * in.shade, 1);\n"
     "    float3 sum = float3(0);\n"
     "    for (int i = 0; i < c.nlights; i++) {\n"
     "        float3 dv = in.world - float3(lights[i].p);\n"
     "        float d2 = dot(dv, dv);\n"
     "        if (d2 > lights[i].radius2) continue;\n"
     "        float f = (lights[i].radius - sqrt(d2)) / lights[i].radius;\n"
     "        sum += max(f, 0.0) * lights[i].intensity * float3(lights[i].rgb);\n"
     "    }\n"
     "    float3 l = in.shade * in.shade * sum + c.ambient / 255.0;\n"
     "    return float4(tex.rgb * min(l, float3(1.0)), 1);\n"
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
        float near_d, far_d;
        float ambient;
        int32_t nlights;
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
    const GameLight *gl;
    u.nlights = render_lights(&gl);
    u.ambient = render_ambient();
    /* Light: packed float3 position, radius, radius2, intensity, packed float3 colour = 36 bytes,
     * the same as GameLight's field order. */
    id<MTLBuffer> lb = [presenter_device() newBufferWithBytes:u.nlights ? (const void *)gl : (const void *)&u
                                                       length:u.nlights ? sizeof(GameLight) * (size_t)u.nlights : 16
                                                      options:MTLResourceStorageModeShared];

    id<MTLBuffer> vb = [presenter_device() newBufferWithBytes:g_mesh
                                                       length:sizeof *g_mesh * (size_t)g_nmesh
                                                      options:MTLResourceStorageModeShared];
    [enc setRenderPipelineState:g_pipe];
    [enc setDepthStencilState:g_depth_on];
    [enc setCullMode:MTLCullModeNone];
    [enc setVertexBuffer:vb offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentBuffer:lb offset:0 atIndex:2];
    [enc setFragmentTexture:g_tiles atIndex:0];
    [enc setFragmentSamplerState:g_samp atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)g_nmesh];
}

