/* Metal side of the renderer: GPU textures and the per-frame draw list. */
#import <Metal/Metal.h>

#include "render/render.h"
#include "video/video.h"
#include "render3d/world3d.h"
#include "win32/imports.h"

#define MAX_TEXTURES 16384

static id<MTLTexture> g_textures[MAX_TEXTURES];
static int g_free_ids[MAX_TEXTURES];
static int g_nfree, g_next_id;
static id<MTLTexture> g_white;
static id<MTLRenderPipelineState> g_pipe[3]; /* indexed by RBlend */
static id<MTLSamplerState> g_samp[2];        /* 0 linear, 1 nearest */
static id<MTLDepthStencilState> g_nodepth;   /* 2D: never tested, never written */

typedef struct {
    int first, count, tex;
    RBlend blend;
    int point;
} Batch;

static RVert *g_verts;
static int g_nverts, g_cap_verts;
static Batch *g_batches;
static int g_nbatches, g_cap_batches;
static float g_screen[2] = { 640, 480 };

static NSString *const kShaders =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct RVert { packed_float2 pos; packed_float2 uv; uint argb; };\n"
     "struct U { float4 fit; float2 screen; };\n"
     "struct VOut { float4 pos [[position]]; float2 uv; float4 col; };\n"
     "vertex VOut r_vs(uint vid [[vertex_id]], const device RVert *v [[buffer(0)]],\n"
     "                 constant U &u [[buffer(1)]]) {\n"
     "    RVert in = v[vid];\n"
     "    float2 t = float2(in.pos) / u.screen;\n"
     "    VOut o;\n"
     "    o.pos = float4(mix(u.fit.x, u.fit.z, t.x), mix(u.fit.w, u.fit.y, t.y), 0, 1);\n"
     "    o.uv = float2(in.uv);\n"
     "    uint c = in.argb;\n"
     "    o.col = float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0;\n"
     "    return o;\n"
     "}\n"
     "fragment float4 r_fs_alpha(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    float4 c = t.sample(s, in.uv);\n"
     "    if (c.a < 0.5) discard_fragment();\n"
     "    return float4(c.rgb * in.col.rgb, in.col.a);\n"
     "}\n"
     "fragment float4 r_fs_add(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    float4 c = t.sample(s, in.uv);\n"
     "    return float4(c.rgb * in.col.rgb * c.a * in.col.a, 1);\n"
     "}\n"
     "fragment float4 r_fs_opaque(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    return float4(t.sample(s, in.uv).rgb * in.col.rgb, 1);\n"
     "}\n";

static void setup(void)
{
    id<MTLDevice> dev = presenter_device();
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kShaders options:nil error:&err];
    if (!lib) rt_fatal("renderer shader compile failed: %s", err.localizedDescription.UTF8String);
    NSString *frag[3] = { @"r_fs_alpha", @"r_fs_add", @"r_fs_opaque" };
    for (int i = 0; i < 3; i++) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"r_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:frag[i]];
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
        ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        if (i == RB_ALPHA) {
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        } else if (i == RB_ADDITIVE) {
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
            ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
        }
        g_pipe[i] = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_pipe[i]) rt_fatal("renderer pipeline failed: %s", err.localizedDescription.UTF8String);
    }
    for (int i = 0; i < 2; i++) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = i ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_samp[i] = [dev newSamplerStateWithDescriptor:sd];
    }
    MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionAlways;
    dsd.depthWriteEnabled = NO;
    g_nodepth = [dev newDepthStencilStateWithDescriptor:dsd];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:1 height:1 mipmapped:NO];
    g_white = [dev newTextureWithDescriptor:td];
    uint32_t w = 0xFFFFFFFFu;
    [g_white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&w bytesPerRow:4];
}

/* ---- textures ---------------------------------------------------------------- */

int render_texture_create(int width, int height)
{
    int tid = g_nfree ? g_free_ids[--g_nfree] : g_next_id++;
    if (tid >= MAX_TEXTURES) rt_fatal("renderer: out of texture ids");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:(NSUInteger)width
                                                                                 height:(NSUInteger)height
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    g_textures[tid] = [presenter_device() newTextureWithDescriptor:td];
    return tid;
}

void render_texture_upload(int tid, int width, int height, const uint32_t *argb)
{
    id<MTLTexture> t = g_textures[tid];
    if ((int)t.width != width || (int)t.height != height) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:(NSUInteger)width
                                                                                     height:(NSUInteger)height
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        t = g_textures[tid] = [presenter_device() newTextureWithDescriptor:td];
    }
    /* ARGB words in memory are B,G,R,A bytes: exactly BGRA8 */
    [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
         mipmapLevel:0
           withBytes:argb
         bytesPerRow:(NSUInteger)width * 4];
}

void render_texture_free(int tid)
{
    if (tid < 0 || tid >= MAX_TEXTURES || !g_textures[tid]) return;
    g_textures[tid] = nil;
    g_free_ids[g_nfree++] = tid;
}

id<MTLTexture> render_texture_object(int tid)
{
    return (tid >= 0 && tid < MAX_TEXTURES) ? g_textures[tid] : nil;
}

/* ---- frame ------------------------------------------------------------------------ */

void render_begin_frame(void)
{
    g_nverts = 0;
    g_nbatches = 0;
}

void render_set_screen(float width, float height)
{
    g_screen[0] = width;
    g_screen[1] = height;
}

void render_triangles(const RVert *v, int count, int tex, RBlend blend, int point)
{
    if (count <= 0) return;
    if (g_nverts + count > g_cap_verts) {
        g_cap_verts = (g_nverts + count) * 2 + 4096;
        g_verts = realloc(g_verts, sizeof *g_verts * (size_t)g_cap_verts);
    }
    memcpy(g_verts + g_nverts, v, sizeof *v * (size_t)count);
    Batch *last = g_nbatches ? &g_batches[g_nbatches - 1] : NULL;
    if (last && last->tex == tex && last->blend == blend && last->point == point &&
        last->first + last->count == g_nverts) {
        last->count += count;
    } else {
        if (g_nbatches == g_cap_batches) {
            g_cap_batches = g_cap_batches * 2 + 256;
            g_batches = realloc(g_batches, sizeof *g_batches * (size_t)g_cap_batches);
        }
        g_batches[g_nbatches++] = (Batch){ g_nverts, count, tex, blend, point };
    }
    g_nverts += count;
}

static void draw_scene(id<MTLRenderCommandEncoder> enc, void *ctx)
{
    (void)ctx;
    struct { float fit[4]; float screen[2]; float pad[2]; } u;
    presenter_fit_rect(u.fit);
    world3d_draw(enc, u.fit);
    [enc setDepthStencilState:g_nodepth];
    if (!g_nbatches) return;
    u.screen[0] = g_screen[0];
    u.screen[1] = g_screen[1];
    id<MTLBuffer> vb = [presenter_device() newBufferWithBytes:g_verts
                                                       length:sizeof *g_verts * (size_t)g_nverts
                                                      options:MTLResourceStorageModeShared];
    [enc setVertexBuffer:vb offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:1];
    int cur_blend = -1;
    for (int i = 0; i < g_nbatches; i++) {
        Batch *b = &g_batches[i];
        if ((int)b->blend != cur_blend) {
            [enc setRenderPipelineState:g_pipe[b->blend]];
            cur_blend = (int)b->blend;
        }
        id<MTLTexture> t = (b->tex >= 0 && g_textures[b->tex]) ? g_textures[b->tex] : g_white;
        [enc setFragmentTexture:t atIndex:0];
        [enc setFragmentSamplerState:g_samp[b->point ? 1 : 0] atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:(NSUInteger)b->first vertexCount:(NSUInteger)b->count];
    }
}

void render_install(void)
{
    if (!g_pipe[0]) setup();
    video_set_scene(draw_scene, NULL);
}
