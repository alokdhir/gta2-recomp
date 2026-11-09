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
#include "render3d/upscale.h"
#include "render/render.h"
#include "video/presenter.h"
#include "win32/imports.h"
#include "win32/win32.h"

/* ---- toggle ------------------------------------------------------------------ */

static int g_enabled = -1;
static int g_halos = 1; /* GTA2_NO_HALOS=1 to compare */

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
static void sprites_reset(void);

int world3d_unproject_zf(float sx, float sy, float zf, float out[3])
{
    if (!g_cam.valid || zf <= 0) return 0;
    out[2] = g_cam.camz - 1.0f / zf;
    return world3d_unproject(sx, sy, out[2], &out[0], &out[1]);
}

int world3d_unproject(float sx, float sy, float z, float *wx, float *wy)
{
    if (!g_cam.valid || g_cam.f <= 0) return 0;
    float d = g_cam.camz - z;
    *wx = g_cam.camx + (sx - g_cam.cx) * d / g_cam.f;
    *wy = g_cam.camy + (sy - g_cam.cy) * d / g_cam.f;
    return 1;
}

void world3d_begin_frame(float w, float h)
{
    sprites_reset();
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
    float u, v, layer;  /* tile texture coordinates + array layer (front side) */
    float shade;
    float u2, v2, layer2; /* what the back of the face shows (flat faces) */
} WVert;

static WVert *g_mesh;
static int g_nmesh, g_capmesh;

static void push_vert2(const float p[3], const float uv[2], int tile, float shade, const float uv2[2], int tile2)
{
    if (g_nmesh == g_capmesh) {
        g_capmesh = g_capmesh ? g_capmesh * 2 : 65536;
        g_mesh = realloc(g_mesh, sizeof *g_mesh * (size_t)g_capmesh);
    }
    g_mesh[g_nmesh++] = (WVert){ p[0], p[1], p[2], uv[0], uv[1], (float)tile, shade, uv2[0], uv2[1], (float)tile2 };
}

/* Most faces show the same texture from both sides (the back just mirrors). */
static void push_vert(const float p[3], const float uv[2], int tile, float shade)
{
    push_vert2(p, uv, tile, shade, uv, tile);
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
/* ---- ambient occlusion (baked into vertex brightness) ------------------------ */
static const WorldMap *g_bm; /* map being meshed */
static int g_ao = 1;          /* GTA2_NO_AO=1 to compare */

/* A cell that blocks light at this level: an ordinary full block with an
 * opaque wall (not see-through panels, slopes, partial posts or air). */
static int solid(int x, int y, int z)
{
    const GBlock *b = world_map_block(g_bm, x, y, z);
    if (!b) return 0;
    /* only ordinary full blocks: ramps are mostly open, diagonals half, and
     * partial blocks thin -- counting them darkened whole tiles around stairs */
    if (b->slope >> 2) return 0;
    uint16_t f[4] = { b->left, b->right, b->top, b->bottom };
    for (int i = 0; i < 4; i++)
        if (FACE_TILE(f[i]) && !FACE_FLAT(f[i])) return 1;
    return 0;
}

/* Does the block at (x,y,z) show an opaque wall towards direction (-dx,-dy)?
 * i.e. a wall that faces the cell we are shading. */
static int wall_towards(int x, int y, int z, int dx, int dy)
{
    const GBlock *b = world_map_block(g_bm, x, y, z);
    if (!b || (b->slope >> 2)) return 0;
    uint16_t f = dx > 0 ? b->left : dx < 0 ? b->right : dy > 0 ? b->top : b->bottom;
    return FACE_TILE(f) && !FACE_FLAT(f);
}

/* Brightness of a lid corner on top of block (x,y,z): walls rising beside it
 * one level up, facing this cell (classic voxel AO). */
static float lid_ao(int x, int y, int z, int cx, int cy)
{
    if (!g_ao) return 1;
    int dx = cx ? 1 : -1, dy = cy ? 1 : -1;
    int s1 = wall_towards(x + dx, y, z + 1, dx, 0);
    int s2 = wall_towards(x, y + dy, z + 1, 0, dy);
    int c = wall_towards(x + dx, y + dy, z + 1, dx, 0) || wall_towards(x + dx, y + dy, z + 1, 0, dy);
    static const float k[4] = { 0.50f, 0.66f, 0.83f, 1.0f };
    return k[(s1 && s2) ? 0 : 3 - (s1 + s2 + c)];
}

/* Contact shadow at the foot of a wall facing cell (fx,fy): only where that
 * cell is open at this level and has ground just below (a wall meeting a
 * floor), so tall buildings don't band storey by storey. */
static float wall_foot_ao(int fx, int fy, int z)
{
    if (!g_ao) return 1;
    return (!solid(fx, fy, z) && world_map_block(g_bm, fx, fy, z - 1)) ? 0.62f : 1.0f;
}

static void wall_uv(uint16_t face, float ha, float hb, float uv[4][2])
{
    /* Upright, u from a (left as seen from outside) to b: what the original
     * renderer produces for a face with no rotation or flip bits. The face's
     * own flip and quarter turns apply on top of that. */
    const float base[4][2] = { { 0, 1 - ha }, { 1, 1 - hb }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 4; i++) {
        float u = base[i][0], w = base[i][1];
        if (FACE_FLIP(face)) u = 1 - u;
        for (unsigned r = 0; r < FACE_ROT(face); r++) { float t = u; u = 1 - w; w = t; }
        uv[i][0] = u;
        uv[i][1] = w;
    }
}

/* A wall between two footprint points a -> b (seen from outside, a on the
 * left), from the block floor up to the lid heights ha/hb (fractions of a
 * level). `back`, if it has a tile, is what the reverse side shows (GTA2's
 * flat faces); otherwise the back is the front seen through. */
static void push_wall_2s_ao(float ax, float ay, float bx, float by, float z, float ha, float hb, uint16_t face,
                            uint16_t back, float shade, float foot);

static void push_wall_2s(float ax, float ay, float bx, float by, float z, float ha, float hb, uint16_t face,
                         uint16_t back, float shade)
{
    push_wall_2s_ao(ax, ay, bx, by, z, ha, hb, face, back, shade, 1.0f);
}

static void push_wall_2s_ao(float ax, float ay, float bx, float by, float z, float ha, float hb, uint16_t face,
                            uint16_t back, float shade, float foot)
{
    if (ha <= 0.001f && hb <= 0.001f) return;
    unsigned tile = FACE_TILE(face);
    if (!tile) return;
    const float c[4][3] = { { ax, ay, z + ha }, { bx, by, z + hb }, { bx, by, z }, { ax, ay, z } };
    float uv[4][2], uv2[4][2];
    wall_uv(face, ha, hb, uv);
    unsigned tile2 = tile;
    if (FACE_TILE(back)) {
        /* seen from behind, b is on the left: mirror, then the back face's own bits */
        wall_uv(back, hb, ha, uv2);
        for (int i = 0; i < 4; i++) uv2[i][0] = 1 - uv2[i][0];
        float t[2];
        memcpy(t, uv2[0], 8); memcpy(uv2[0], uv2[1], 8); memcpy(uv2[1], t, 8);
        memcpy(t, uv2[2], 8); memcpy(uv2[2], uv2[3], 8); memcpy(uv2[3], t, 8);
        tile2 = FACE_TILE(back);
    } else {
        memcpy(uv2, uv, sizeof uv);
    }
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) /* corners 2,3 are the wall's foot */
        push_vert2(c[idx[i]], uv[idx[i]], (int)tile, idx[i] >= 2 ? shade * foot : shade, uv2[idx[i]], (int)tile2);
}

static void push_wall(float ax, float ay, float bx, float by, float z, float ha, float hb, uint16_t face, float shade)
{
    push_wall_2s(ax, ay, bx, by, z, ha, hb, face, 0, shade);
}

static void push_wall_ao(float ax, float ay, float bx, float by, float z, float ha, float hb, uint16_t face, float shade,
                         float foot)
{
    push_wall_2s_ao(ax, ay, bx, by, z, ha, hb, face, 0, shade, foot);
}

/* Lid over a footprint polygon (up to 4 points, corners of the unit cell in
 * cell-local coords), heights interpolated from the four cell corners. */
static float g_lid_ao[4] = { 1, 1, 1, 1 }; /* NW NE SE SW brightness for the next lid */

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
        /* The original's rule (derived from its own UVs): the texture is
         * turned clockwise by the face's quarter turns, then mirrored
         * east-west if the flip bit is set. Invert that for this point. */
        float u = FACE_FLIP(face) ? 1 - px : px, w = py;
        for (unsigned r = 0; r < FACE_ROT(face); r++) { float t = u; u = w; w = 1 - t; }
        uv[i][0] = u;
        uv[i][1] = w;
    }
    float sh[4];
    for (int i = 0; i < n; i++) { /* bilinear AO from the cell's corners */
        float px = pts[i][0], py = pts[i][1];
        sh[i] = SH_LID * ((1 - px) * (1 - py) * g_lid_ao[0] + px * (1 - py) * g_lid_ao[1] + px * py * g_lid_ao[2] +
                          (1 - px) * py * g_lid_ao[3]);
    }
    for (int i = 1; i + 1 < n; i++) {
        push_vert(v[0], uv[0], (int)tile, sh[0]);
        push_vert(v[i], uv[i], (int)tile, sh[i]);
        push_vert(v[i + 1], uv[i + 1], (int)tile, sh[i + 1]);
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

    if (st >= 49 && st <= 52) {
        /* Diagonal slope (a corner of a ramp): the surface rises from the
         * diagonal edge to the opposite corner; no vertical diagonal wall.
         * 49 up-left (high SE), 50 up-right (high SW), 51 down-left (high NE),
         * 52 down-right (high NW). Corner heights NW, NE, SE, SW. */
        float hs[4] = { 0, 0, 0, 0 };
        float tri[3][2];
        switch (st) {
        case 49: hs[2] = 1; tri[0][0] = 1; tri[0][1] = 0; tri[1][0] = 1; tri[1][1] = 1; tri[2][0] = 0; tri[2][1] = 1; break;
        case 50: hs[3] = 1; tri[0][0] = 0; tri[0][1] = 0; tri[1][0] = 1; tri[1][1] = 1; tri[2][0] = 0; tri[2][1] = 1; break;
        case 51: hs[1] = 1; tri[0][0] = 0; tri[0][1] = 0; tri[1][0] = 1; tri[1][1] = 0; tri[2][0] = 1; tri[2][1] = 1; break;
        default: hs[0] = 1; tri[0][0] = 0; tri[0][1] = 0; tri[1][0] = 1; tri[1][1] = 0; tri[2][0] = 0; tri[2][1] = 1; break;
        }
        push_lid(X, Y, Z, (const float(*)[2])tri, 3, hs, b->lid);
        push_wall(X, Y1, X, Y, Z, hs[3], hs[0], b->left, SH_LEFT);
        push_wall(X1, Y, X1, Y1, Z, hs[1], hs[2], b->right, SH_RIGHT);
        push_wall(X, Y, X1, Y, Z, hs[0], hs[1], b->top, SH_TOP);
        push_wall(X1, Y1, X, Y1, Z, hs[2], hs[3], b->bottom, SH_BOTTOM);
        return;
    }

    if (st >= 45 && st <= 48) {
        /* Diagonal wall: triangular footprint, slanted wall along the diagonal.
         * 45 faces up-left (solid SE), 46 up-right (solid SW),
         * 47 down-left (solid NE), 48 down-right (solid NW). */
        int kind = (int)(st - 45);
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

    int ramp = ramp_heights(st, h); /* leaves 1,1,1,1 for ordinary blocks */
    if (!ramp) {
        g_lid_ao[0] = lid_ao(x, y, z, 0, 0);
        g_lid_ao[1] = lid_ao(x, y, z, 1, 0);
        g_lid_ao[2] = lid_ao(x, y, z, 1, 1);
        g_lid_ao[3] = lid_ao(x, y, z, 0, 1);
    }
    push_lid(X, Y, Z, cell, 4, h, b->lid);
    g_lid_ao[0] = g_lid_ao[1] = g_lid_ao[2] = g_lid_ao[3] = 1;
    /* Opposite sides. A flat (see-through) side opposite a normal one is a
     * single two-sided quad at its own edge, the opposite tile showing on its
     * back (one fence, not two); two flat sides are two panels. Both rules
     * checked against the original's draw stream. */
    if (FACE_FLAT(b->left) && FACE_FLAT(b->right)) {
        /* both sides see-through: the original draws a panel at each edge
         * (e.g. the two planes that make a lamp post's arm) */
        push_wall(X, Y1, X, Y, Z, h[3], h[0], b->left, SH_LEFT);
        push_wall(X1, Y, X1, Y1, Z, h[1], h[2], b->right, SH_RIGHT);
    } else if (FACE_FLAT(b->left))
        push_wall_2s(X, Y1, X, Y, Z, h[3], h[0], b->left, b->right, SH_LEFT);
    else if (FACE_FLAT(b->right))
        push_wall_2s(X1, Y, X1, Y1, Z, h[1], h[2], b->right, b->left, SH_RIGHT);
    else {
        push_wall_ao(X, Y1, X, Y, Z, h[3], h[0], b->left, SH_LEFT, wall_foot_ao(x - 1, y, z));      /* SW -> NW */
        push_wall_ao(X1, Y, X1, Y1, Z, h[1], h[2], b->right, SH_RIGHT, wall_foot_ao(x + 1, y, z));  /* NE -> SE */
    }
    if (FACE_FLAT(b->top) && FACE_FLAT(b->bottom)) {
        push_wall(X, Y, X1, Y, Z, h[0], h[1], b->top, SH_TOP);
        push_wall(X1, Y1, X, Y1, Z, h[2], h[3], b->bottom, SH_BOTTOM);
    } else if (FACE_FLAT(b->top))
        push_wall_2s(X, Y, X1, Y, Z, h[0], h[1], b->top, b->bottom, SH_TOP);
    else if (FACE_FLAT(b->bottom))
        push_wall_2s(X1, Y1, X, Y1, Z, h[2], h[3], b->bottom, b->top, SH_BOTTOM);
    else {
        push_wall_ao(X, Y, X1, Y, Z, h[0], h[1], b->top, SH_TOP, wall_foot_ao(x, y - 1, z));         /* NW -> NE */
        push_wall_ao(X1, Y1, X, Y1, Z, h[2], h[3], b->bottom, SH_BOTTOM, wall_foot_ao(x, y + 1, z)); /* SE -> SW */
    }
}

static void build_mesh(const WorldMap *m, int x0, int y0, int x1, int y1)
{
    g_nmesh = 0;
    g_bm = m;
    static int env;
    if (!env++) g_ao = getenv("GTA2_NO_AO") == NULL;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            for (int z = 0; z < MAP_LEVELS; z++) {
                const GBlock *b = world_map_block(m, x, y, z);
                if (b) build_block(x, y, z, b);
            }
}

/* ---- sprites -------------------------------------------------------------------
 * Collected during the scene in the game's draw order; each is lifted a hair
 * above the previous so that order (shadow, car, driver) survives without
 * depth fighting, while the world still hides them. */
typedef struct {
    float x, y, z, u, v;
    uint32_t argb;
} SVert;
typedef struct { int first, count, tex, blend; } SBatch;

static SVert *g_sv;
static int g_nsv, g_capsv;
static SBatch *g_sb;
static int g_nsb, g_capsb;
static int g_sprite_order;

static void sprites_reset(void)
{
    g_nsv = g_nsb = 0;
    g_sprite_order = 0;
}

void world3d_sprite(const WSpriteVert q[4], int tex, int blend)
{
    float lift = 0.004f + 0.0006f * (float)(g_sprite_order++ % 2000);
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    if (g_nsv + 6 > g_capsv) {
        g_capsv = (g_nsv + 6) * 2 + 1024;
        g_sv = realloc(g_sv, sizeof *g_sv * (size_t)g_capsv);
    }
    for (int i = 0; i < 6; i++) {
        const WSpriteVert *p = &q[idx[i]];
        g_sv[g_nsv + i] = (SVert){ p->x, p->y, p->z + lift, p->u, p->v, p->argb };
    }
    SBatch *last = g_nsb ? &g_sb[g_nsb - 1] : NULL;
    if (last && last->tex == tex && last->blend == blend && last->first + last->count == g_nsv) {
        last->count += 6;
    } else {
        if (g_nsb == g_capsb) {
            g_capsb = g_capsb * 2 + 64;
            g_sb = realloc(g_sb, sizeof *g_sb * (size_t)g_capsb);
        }
        g_sb[g_nsb++] = (SBatch){ g_nsv, 6, tex, blend };
    }
    g_nsv += 6;
}

/* ---- Metal ---------------------------------------------------------------------------- */

static id<MTLRenderPipelineState> g_pipe;
static id<MTLDepthStencilState> g_depth_on;
static id<MTLRenderPipelineState> g_halo_pipe;
static id<MTLDepthStencilState> g_depth_test_nowrite;
static id<MTLRenderPipelineState> g_spr_pipe[3];
static id<MTLSamplerState> g_spr_samp;
static id<MTLTexture> g_tiles;
static id<MTLSamplerState> g_samp;
static char g_tiles_for[1024];

static NSString *const kShaders =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct WVert { packed_float3 p; packed_float2 uv; float layer; float shade; packed_float2 uv2; float layer2; };\n"
     "struct Cam { float4 fit; float2 screen; float2 centre; packed_float3 cam; float f; float near_d; float far_d;\n"
     "             float ambient; int nlights; int emissive; };\n"
     "struct Light { packed_float3 p; float radius; float radius2; float intensity; packed_float3 rgb; };\n"
     "struct VOut { float4 pos [[position]]; float2 uv; float layer [[flat]]; float shade; float3 world;\n"
     "              float2 uv2; float layer2 [[flat]]; };\n"
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
     "    o.uv2 = float2(in.uv2);\n"
     "    o.layer2 = in.layer2;\n"
     "    return o;\n"
     "}\n"
     /* Lighting, per pixel, following the original renderer's rule: at full
      * ambient (daylight) only the face's directional shade applies; below
      * it, ambient + shade^2 * sum(light falloff * intensity * colour). */
     "fragment float4 w_fs(VOut in [[stage_in]], bool front [[front_facing]], texture2d_array<float> t [[texture(0)]],\n"
     "                     sampler s [[sampler(0)]], constant Cam &c [[buffer(1)]], const device Light *lights [[buffer(2)]]) {\n"
     "    float4 tex = front ? t.sample(s, in.uv, uint(in.layer)) : t.sample(s, in.uv2, uint(in.layer2));\n"
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
     "    float3 lit = tex.rgb * min(l, float3(1.0));\n"
     /* Self-lit texels (neon tubes, signal lamps): very bright and strongly
      * saturated ones keep their own colour in the dark, so bloom finds them.
      * A heuristic until hand-made emissive maps exist. */
     "    if (c.emissive != 0) {\n"
     "        float mx = max(tex.r, max(tex.g, tex.b)), mn = min(tex.r, min(tex.g, tex.b));\n"
     "        float sat = mx > 0.0 ? (mx - mn) / mx : 0.0;\n"
     "        float e = smoothstep(0.70, 0.90, mx) * smoothstep(0.55, 0.80, sat);\n"
     "        lit = max(lit, tex.rgb * e * 1.1);\n"
     "    }\n"
     "    return float4(lit, 1);\n"
     "}\n"
     /* Light halos: a soft additive disc on the ground plane at each light,
      * instanced, projected with the same camera. */
     "struct HOut { float4 pos [[position]]; float2 q; float3 col; };\n"
     "vertex HOut h_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant Cam &c [[buffer(1)]],\n"
     "                 const device Light *lights [[buffer(2)]]) {\n"
     "    const float2 corner[6] = { float2(-1,-1), float2(1,-1), float2(1,1), float2(-1,-1), float2(1,1), float2(-1,1) };\n"
     "    Light L = lights[iid];\n"
     "    float2 q = corner[vid];\n"
     "    float size = L.radius * 0.38;\n"
     "    float3 p = float3(L.p) + float3(q * size, 0.02);\n"
     "    float3 cam = float3(c.cam);\n"
     "    float d = max(cam.z - p.z, 0.05);\n"
     "    float2 s = c.centre + c.f * (p.xy - cam.xy) / d;\n"
     "    float2 t = s / c.screen;\n"
     "    float2 ndc = float2(mix(c.fit.x, c.fit.z, t.x), mix(c.fit.w, c.fit.y, t.y));\n"
     "    float z01 = clamp((d - c.near_d) / (c.far_d - c.near_d), 0.0, 1.0);\n"
     "    HOut o;\n"
     "    o.pos = float4(ndc * d, z01 * d, d);\n"
     "    o.q = q;\n"
     "    float night = clamp(1.0 - c.ambient / 255.0, 0.0, 1.0);\n"
     "    o.col = float3(L.rgb) * L.intensity * night * 0.28;\n"
     "    return o;\n"
     "}\n"
     /* Sprites: the same camera, lit like the world (the game marks them lit too). */
     "struct SVert { packed_float3 p; packed_float2 uv; uint argb; };\n"
     "struct SOut { float4 pos [[position]]; float2 uv; float4 col; float3 world; };\n"
     "vertex SOut s_vs(uint vid [[vertex_id]], const device SVert *v [[buffer(0)]], constant Cam &c [[buffer(1)]]) {\n"
     "    SVert in = v[vid];\n"
     "    float3 p = float3(in.p);\n"
     "    float3 cam = float3(c.cam);\n"
     "    float d = max(cam.z - p.z, 0.05);\n"
     "    float2 s = c.centre + c.f * (p.xy - cam.xy) / d;\n"
     "    float2 t = s / c.screen;\n"
     "    float2 ndc = float2(mix(c.fit.x, c.fit.z, t.x), mix(c.fit.w, c.fit.y, t.y));\n"
     "    float z01 = clamp((d - c.near_d) / (c.far_d - c.near_d), 0.0, 1.0);\n"
     "    SOut o;\n"
     "    o.pos = float4(ndc * d, z01 * d, d);\n"
     "    o.uv = float2(in.uv);\n"
     "    uint k = in.argb;\n"
     "    o.col = float4((k >> 16) & 255, (k >> 8) & 255, k & 255, k >> 24) / 255.0;\n"
     "    o.world = p;\n"
     "    return o;\n"
     "}\n"
     "fragment float4 s_fs(SOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler sm [[sampler(0)]],\n"
     "                     constant Cam &c [[buffer(1)]], const device Light *lights [[buffer(2)]],\n"
     "                     constant int &mode [[buffer(3)]]) {\n"
     "    float4 tex = t.sample(sm, in.uv);\n"
     "    if (mode == 0 && tex.a < 0.5) discard_fragment();\n"
     /* sprite colours arrive already lit, per corner, the way the original lights them */
     "    float3 rgb = tex.rgb * in.col.rgb;\n"
     "    if (mode == 1) return float4(rgb * tex.a * in.col.a, 0);\n"
     "    return float4(rgb, mode == 2 ? 1.0 : in.col.a);\n"
     "}\n"
     "fragment float4 h_fs(HOut in [[stage_in]]) {\n"
     "    float r2 = dot(in.q, in.q);\n"
     "    float a = exp(-3.5 * r2) * (1.0 - smoothstep(0.85, 1.0, r2));\n"
     "    return float4(in.col * a, 0);\n"
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
    MTLRenderPipelineDescriptor *hd = [MTLRenderPipelineDescriptor new];
    hd.vertexFunction = [lib newFunctionWithName:@"h_vs"];
    hd.fragmentFunction = [lib newFunctionWithName:@"h_fs"];
    hd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    hd.colorAttachments[0].blendingEnabled = YES;
    hd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    hd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
    hd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
    hd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    hd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    g_halo_pipe = [dev newRenderPipelineStateWithDescriptor:hd error:&err];
    if (!g_halo_pipe) rt_log("world3d: halo pipeline failed: %s", err.localizedDescription.UTF8String);
    for (int i = 0; i < 3; i++) {
        MTLRenderPipelineDescriptor *sp = [MTLRenderPipelineDescriptor new];
        sp.vertexFunction = [lib newFunctionWithName:@"s_vs"];
        sp.fragmentFunction = [lib newFunctionWithName:@"s_fs"];
        MTLRenderPipelineColorAttachmentDescriptor *ca = sp.colorAttachments[0];
        ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (i == 0) { /* alpha */
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        } else if (i == 1) { /* additive */
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = ca.destinationRGBBlendFactor = MTLBlendFactorOne;
        }
        sp.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        g_spr_pipe[i] = [dev newRenderPipelineStateWithDescriptor:sp error:&err];
        if (!g_spr_pipe[i]) rt_log("world3d: sprite pipeline failed: %s", err.localizedDescription.UTF8String);
    }
    MTLSamplerDescriptor *ssd = [MTLSamplerDescriptor new];
    ssd.minFilter = ssd.magFilter = MTLSamplerMinMagFilterLinear;
    ssd.sAddressMode = ssd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_spr_samp = [dev newSamplerStateWithDescriptor:ssd];
    MTLDepthStencilDescriptor *dn = [MTLDepthStencilDescriptor new];
    dn.depthCompareFunction = MTLCompareFunctionLessEqual;
    dn.depthWriteEnabled = NO;
    g_depth_test_nowrite = [dev newDepthStencilStateWithDescriptor:dn];
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionLessEqual;
    dd.depthWriteEnabled = YES;
    g_depth_on = [dev newDepthStencilStateWithDescriptor:dd];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.magFilter = MTLSamplerMinMagFilterLinear;      /* upscaled edges + bilinear: smooth, not grainy */
    sd.minFilter = MTLSamplerMinMagFilterLinear;      /* trilinear when small */
    sd.mipFilter = MTLSamplerMipFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_samp = [dev newSamplerStateWithDescriptor:sd];
    return 1;
}

/* (Re)build the tile texture array when the level's style changes: tiles
 * upscaled (Scale2x, GTA2_UPSCALE) and mipmapped so distant surfaces don't
 * shimmer. Replacement textures would slot in here, ahead of the upscaler. */
static int g_tiles_scale;

static int ensure_tiles(void)
{
    const char *path = world_style_path();
    if (!path) return 0;
    int f = upscale_factor();
    if (g_tiles && !strcmp(path, g_tiles_for) && f == g_tiles_scale) return 1;
    WorldStyle st;
    if (!world_style_load(path, &st)) return 0;
    int px = TILE_PX * f;
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2DArray;
    td.pixelFormat = MTLPixelFormatBGRA8Unorm;
    td.width = td.height = (NSUInteger)px;
    td.arrayLength = (NSUInteger)st.ntiles;
    td.mipmapLevelCount = (NSUInteger)floor(log2(px)) + 1;
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    g_tiles = [presenter_device() newTextureWithDescriptor:td];
    for (int t = 0; t < st.ntiles; t++) {
        const uint32_t *src = st.rgba + (size_t)t * TILE_PX * TILE_PX;
        uint32_t *up = f > 1 ? upscale_pixels(src, TILE_PX, TILE_PX, f) : NULL;
        [g_tiles replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)px, (NSUInteger)px)
                   mipmapLevel:0
                         slice:(NSUInteger)t
                     withBytes:up ? up : src
                   bytesPerRow:(NSUInteger)px * 4
                 bytesPerImage:(NSUInteger)px * px * 4];
        free(up);
    }
    static id<MTLCommandQueue> q;
    if (!q) q = [presenter_device() newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl generateMipmapsForTexture:g_tiles];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    snprintf(g_tiles_for, sizeof g_tiles_for, "%s", path);
    g_tiles_scale = f;
    rt_log("world3d: %d tiles at %dx%d (upscale x%d)", st.ntiles, px, px, f);
    world_style_free(&st);
    return 1;
}

/* GTA2_DEBUG_BLOCKS="x,y,z;x,y,z": log what the mesh builder emits for those blocks */
static void debug_blocks(const WorldMap *m)
{
    static int done;
    const char *e = getenv("GTA2_DEBUG_BLOCKS");
    if (done || !e) return;
    done = 1;
    char *copy = strdup(e), *save = NULL;
    for (char *t = strtok_r(copy, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        int x, y, z;
        if (sscanf(t, "%d,%d,%d", &x, &y, &z) != 3) continue;
        const GBlock *b = world_map_block(m, x, y, z);
        if (!b) { rt_log("dbg block %d,%d,%d: empty", x, y, z); continue; }
        int before = g_nmesh;
        g_bm = m;
        build_block(x, y, z, b);
        rt_log("dbg block %d,%d,%d slope=%u lid=%04X L%04X R%04X T%04X B%04X -> %d verts", x, y, z, b->slope >> 2,
               b->lid, b->left, b->right, b->top, b->bottom, g_nmesh - before);
        for (int i = before; i < g_nmesh; i += 3)
            rt_log("   tri (%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f) tile %.0f back %.0f", g_mesh[i].x, g_mesh[i].y,
                   g_mesh[i].z, g_mesh[i + 1].x, g_mesh[i + 1].y, g_mesh[i + 1].z, g_mesh[i + 2].x, g_mesh[i + 2].y,
                   g_mesh[i + 2].z, g_mesh[i].layer, g_mesh[i].layer2);
        g_nmesh = before;
    }
    free(copy);
}

void world3d_draw(id<MTLRenderCommandEncoder> enc, const float fit[4])
{
    static int env;
    if (!env++) g_halos = getenv("GTA2_NO_HALOS") == NULL;
    if (!world3d_enabled() || !g_cam.valid || !setup() || !ensure_tiles()) return;
    WorldMap m;
    if (!world_map_live(&m)) return;
    debug_blocks(&m);

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
        int32_t emissive;
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
    static int emis = -1;
    if (emis < 0) emis = getenv("GTA2_NO_EMISSIVE") == NULL;
    u.emissive = emis;
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

    /* sprites: depth-tested against the world, not written; game draw order */
    if (g_nsb && g_spr_pipe[0]) {
        id<MTLBuffer> sb = [presenter_device() newBufferWithBytes:g_sv
                                                           length:sizeof *g_sv * (size_t)g_nsv
                                                          options:MTLResourceStorageModeShared];
        [enc setDepthStencilState:g_depth_test_nowrite];
        [enc setVertexBuffer:sb offset:0 atIndex:0];
        [enc setVertexBytes:&u length:sizeof u atIndex:1];
        [enc setFragmentBytes:&u length:sizeof u atIndex:1];
        [enc setFragmentBuffer:lb offset:0 atIndex:2];
        [enc setFragmentSamplerState:g_spr_samp atIndex:0];
        for (int i = 0; i < g_nsb; i++) {
            SBatch *b = &g_sb[i];
            id<MTLTexture> t = render_texture_object(b->tex);
            if (!t) continue;
            int mode = b->blend;
            [enc setRenderPipelineState:g_spr_pipe[mode]];
            [enc setFragmentTexture:t atIndex:0];
            [enc setFragmentBytes:&mode length:sizeof mode atIndex:3];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:(NSUInteger)b->first vertexCount:(NSUInteger)b->count];
        }
    }

    /* light halos (dim scenes only; their strength fades in with darkness) */
    if (g_halo_pipe && u.nlights > 0 && u.ambient < 255.0f && g_halos) {
        [enc setRenderPipelineState:g_halo_pipe];
        [enc setDepthStencilState:g_depth_test_nowrite];
        [enc setVertexBytes:&u length:sizeof u atIndex:1];
        [enc setVertexBuffer:lb offset:0 atIndex:2];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:(NSUInteger)u.nlights];
    }
}

