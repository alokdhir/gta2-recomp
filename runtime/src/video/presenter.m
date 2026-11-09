/* Metal presenter. See presenter.h. */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "video/presenter.h"
#include "render3d/world3d.h"
#include "win32/imports.h"

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLRenderPipelineState> g_blit;
static id<MTLSamplerState> g_nearest;
static id<MTLTexture> g_fbtex;
static id<MTLTexture> g_depth; /* depth buffer for the 3D world, sized to the drawable */
/* bloom: scene rendered offscreen, bright parts blurred at half size, added back */
static id<MTLTexture> g_scene, g_bloomA, g_bloomB;
static id<MTLRenderPipelineState> g_bright, g_blurH, g_blurV, g_compose;
static id<MTLSamplerState> g_linear;
static int g_bloom_on = -1; /* GTA2_NO_BLOOM=1 to compare */
static int g_phase = PHASE_ALL;
int presenter_phase(void) { return g_phase; }
static CAMetalLayer *g_layer;
static int g_w, g_h;
static unsigned g_frame_no;
static volatile int g_ab_request;
void presenter_request_ab(void) { g_ab_request = 1; }
static const char *g_capture_path;
static unsigned g_capture_frame;

/* GTA2_CAPTURE=out.png [GTA2_CAPTURE_FRAME=n]: save frame n (default 120) as a PNG. */
static void write_png(id<MTLTexture> t, const char *path)
{
    size_t w = t.width, h = t.height;
    void *px = malloc(w * h * 4);
    [t getBytes:px bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, w * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    CGImageDestinationAddImage(dst, img, NULL);
    CGImageDestinationFinalize(dst);
    CFRelease(dst); CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    free(px);
    rt_log("presenter: captured frame %u to %s", g_frame_no, path);
}

static float g_fit[4] = { -1, -1, 1, 1 };
static float g_gamma[4] = { 1, 1, 1, 0 }; /* float3 in the shader: 16 bytes */

static NSString *const kShaders =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 pos [[position]]; float2 uv; };\n"
     "vertex VOut blit_vs(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]]) {\n"
     "    float2 c = float2(vid & 1, vid >> 1);\n"
     "    VOut o;\n"
     "    o.pos = float4(mix(rect.xy, rect.zw, c), 0, 1);\n"
     "    o.uv = float2(c.x, 1 - c.y);\n"
     "    return o;\n"
     "}\n"
     "fragment float4 blit_fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
     "                        sampler s [[sampler(0)]], constant float3 &gamma [[buffer(0)]],\n"
     "                        constant int &key [[buffer(1)]]) {\n"
     "    float3 c = t.sample(s, in.uv).rgb;\n"
     "    if (key != 0 && all(c == 0)) discard_fragment(); /* overlay on a 3D frame: black is clear */\n"
     "    return float4(pow(c, 1.0 / gamma), 1);\n"
     "}\n"
     /* bloom: fullscreen triangle */
     "struct FOut { float4 pos [[position]]; float2 uv; };\n"
     "vertex FOut full_vs(uint vid [[vertex_id]]) {\n"
     "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "    FOut o; o.pos = float4(p * 2 - 1, 0, 1); o.uv = float2(p.x, 1 - p.y); return o;\n"
     "}\n"
     "fragment float4 bright_fs(FOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    float3 c = t.sample(s, in.uv).rgb;\n"
     "    float l = max(c.r, max(c.g, c.b));\n"
     "    float k = smoothstep(0.55, 0.95, l);\n"   /* soft threshold */
     "    return float4(c * k, 1);\n"
     "}\n"
     "constant float W[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };\n"
     "fragment float4 blur_fs(FOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],\n"
     "                        constant float2 &dir [[buffer(0)]]) {\n"
     "    float3 c = t.sample(s, in.uv).rgb * W[0];\n"
     "    for (int i = 1; i < 5; i++) {\n"
     "        float2 o = dir * (float(i) * 1.5);\n"
     "        c += (t.sample(s, in.uv + o).rgb + t.sample(s, in.uv - o).rgb) * W[i];\n"
     "    }\n"
     "    return float4(c, 1);\n"
     "}\n"
     "fragment float4 compose_fs(FOut in [[stage_in]], texture2d<float> scene [[texture(0)]],\n"
     "                           texture2d<float> bloom [[texture(1)]], sampler s [[sampler(0)]]) {\n"
     "    float3 c = scene.sample(s, in.uv).rgb + bloom.sample(s, in.uv).rgb * 0.85;\n"
     "    return float4(c, 1);\n"
     "}\n";

int presenter_init(void *metal_layer, int width, int height)
{
    if (!g_dev) {
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) rt_fatal("no Metal device");
        g_queue = [g_dev newCommandQueue];
        NSError *err = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:kShaders options:nil error:&err];
        if (!lib) rt_fatal("Metal shader compile failed: %s", err.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"blit_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"blit_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.colorAttachments[0].blendingEnabled = NO;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        g_blit = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_blit) rt_fatal("Metal pipeline failed: %s", err.localizedDescription.UTF8String);
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        g_nearest = [g_dev newSamplerStateWithDescriptor:sd];
        MTLSamplerDescriptor *ld = [MTLSamplerDescriptor new];
        ld.minFilter = ld.magFilter = MTLSamplerMinMagFilterLinear;
        ld.sAddressMode = ld.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_linear = [g_dev newSamplerStateWithDescriptor:ld];
        NSString *frags[4] = { @"bright_fs", @"blur_fs", @"blur_fs", @"compose_fs" };
        id<MTLRenderPipelineState> __strong *outs[4] = { &g_bright, &g_blurH, &g_blurV, &g_compose };
        for (int i = 0; i < 4; i++) {
            MTLRenderPipelineDescriptor *bd = [MTLRenderPipelineDescriptor new];
            bd.vertexFunction = [lib newFunctionWithName:@"full_vs"];
            bd.fragmentFunction = [lib newFunctionWithName:frags[i]];
            bd.colorAttachments[0].pixelFormat = i == 3 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA16Float;
            *outs[i] = [g_dev newRenderPipelineStateWithDescriptor:bd error:&err];
            if (!*outs[i]) rt_log("presenter: bloom pipeline failed: %s", err.localizedDescription.UTF8String);
        }
    }
    g_layer = (__bridge CAMetalLayer *)metal_layer;
    g_layer.device = g_dev;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_capture_path = getenv("GTA2_CAPTURE");
    g_capture_frame = getenv("GTA2_CAPTURE_FRAME") ? (unsigned)atoi(getenv("GTA2_CAPTURE_FRAME")) : 120;
    g_layer.framebufferOnly = g_capture_path == NULL && getenv("GTA2_AB") == NULL;
    g_layer.displaySyncEnabled = YES;

    if (width != g_w || height != g_h || !g_fbtex) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatB5G6R5Unorm
                                         width:(NSUInteger)width
                                        height:(NSUInteger)height
                                     mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        g_fbtex = [g_dev newTextureWithDescriptor:td];
        g_w = width;
        g_h = height;
    }
    rt_log("presenter: %dx%d on %s", width, height, g_dev.name.UTF8String);
    return 1;
}

void presenter_fit_rect(float out[4]) { memcpy(out, g_fit, sizeof g_fit); }
id<MTLDevice> presenter_device(void) { return g_dev; }
int presenter_width(void) { return g_w; }
int presenter_height(void) { return g_h; }

void presenter_set_gamma(float r, float g, float b)
{
    g_gamma[0] = r > 0 ? r : 1;
    g_gamma[1] = g > 0 ? g : 1;
    g_gamma[2] = b > 0 ? b : 1;
}

void presenter_frame(const void *fb, uint32_t pitch, PresenterSceneFn scene, void *ctx)
{
    if (!g_layer) return;
    @autoreleasepool {
        CGFloat scale = g_layer.contentsScale;
        CGSize sz = g_layer.bounds.size;
        CGSize px = CGSizeMake(MAX(1, sz.width * scale), MAX(1, sz.height * scale));
        if (!CGSizeEqualToSize(g_layer.drawableSize, px)) g_layer.drawableSize = px;

        if (fb)
            [g_fbtex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)g_w, (NSUInteger)g_h)
                       mipmapLevel:0
                         withBytes:fb
                       bytesPerRow:pitch];

        id<CAMetalDrawable> drawable = [g_layer nextDrawable];
        if (!drawable) return;
        if (g_bloom_on < 0) g_bloom_on = getenv("GTA2_NO_BLOOM") == NULL;
        int bloom = g_bloom_on && world3d_enabled() && g_compose;
        NSUInteger dw = drawable.texture.width, dh = drawable.texture.height;
        if (bloom && (!g_scene || g_scene.width != dw || g_scene.height != dh)) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:dw height:dh mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            g_scene = [g_dev newTextureWithDescriptor:td];
            MTLTextureDescriptor *hd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                          width:dw / 2 height:dh / 2 mipmapped:NO];
            hd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            hd.storageMode = MTLStorageModePrivate;
            g_bloomA = [g_dev newTextureWithDescriptor:hd];
            g_bloomB = [g_dev newTextureWithDescriptor:hd];
        }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = bloom ? g_scene : drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (!g_depth || g_depth.width != drawable.texture.width || g_depth.height != drawable.texture.height) {
            MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                          width:drawable.texture.width
                                                                                         height:drawable.texture.height
                                                                                      mipmapped:NO];
            dd.usage = MTLTextureUsageRenderTarget;
            dd.storageMode = MTLStorageModePrivate;
            g_depth = [g_dev newTextureWithDescriptor:dd];
        }
        rp.depthAttachment.texture = g_depth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;

        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

        /* Aspect-fit the game's resolution into the drawable (letterbox) */
        double ga = (double)g_w / g_h, da = px.width / px.height;
        float sx = da > ga ? (float)(ga / da) : 1.0f, sy = da > ga ? 1.0f : (float)(da / ga);
        float rect[4] = { -sx, -sy, sx, sy };
        memcpy(g_fit, rect, sizeof rect);

        g_phase = bloom ? PHASE_WORLD : PHASE_ALL;
        if (scene) scene(enc, ctx);
        if (!bloom) {
            if (fb) {
                [enc setRenderPipelineState:g_blit];
                [enc setVertexBytes:rect length:sizeof rect atIndex:0];
                [enc setFragmentTexture:g_fbtex atIndex:0];
                [enc setFragmentSamplerState:g_nearest atIndex:0];
                [enc setFragmentBytes:g_gamma length:sizeof(float) * 4 atIndex:0];
                int key = scene != NULL;
                [enc setFragmentBytes:&key length:sizeof key atIndex:1];
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
        }
        [enc endEncoding];
        if (bloom) {
            /* bright pass (half size), blur H, blur V, compose onto the drawable */
            id<MTLTexture> src[4] = { g_scene, g_bloomA, g_bloomB, g_scene };
            id<MTLTexture> dst[4] = { g_bloomA, g_bloomB, g_bloomA, drawable.texture };
            id<MTLRenderPipelineState> pipe[4] = { g_bright, g_blurH, g_blurV, g_compose };
            for (int i = 0; i < 4; i++) {
                MTLRenderPassDescriptor *pp = [MTLRenderPassDescriptor renderPassDescriptor];
                pp.colorAttachments[0].texture = dst[i];
                pp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                pp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> pe = [cb renderCommandEncoderWithDescriptor:pp];
                [pe setRenderPipelineState:pipe[i]];
                [pe setFragmentTexture:src[i] atIndex:0];
                if (i == 3) [pe setFragmentTexture:g_bloomA atIndex:1];
                [pe setFragmentSamplerState:g_linear atIndex:0];
                if (i == 1 || i == 2) {
                    float dir[2] = { i == 1 ? 1.0f / (float)(dw / 2) : 0, i == 2 ? 1.0f / (float)(dh / 2) : 0 };
                    [pe setFragmentBytes:dir length:sizeof dir atIndex:0];
                }
                [pe drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [pe endEncoding];
            }
            /* overlays (HUD, 2D surface) on top of the bloomed image, crisp */
            MTLRenderPassDescriptor *op = [MTLRenderPassDescriptor renderPassDescriptor];
            op.colorAttachments[0].texture = drawable.texture;
            op.colorAttachments[0].loadAction = MTLLoadActionLoad;
            op.colorAttachments[0].storeAction = MTLStoreActionStore;
            op.depthAttachment.texture = g_depth;
            op.depthAttachment.loadAction = MTLLoadActionDontCare;
            op.depthAttachment.storeAction = MTLStoreActionDontCare;
            id<MTLRenderCommandEncoder> oe = [cb renderCommandEncoderWithDescriptor:op];
            g_phase = PHASE_OVERLAY;
            if (scene) scene(oe, ctx);
            if (fb) {
                [oe setRenderPipelineState:g_blit];
                [oe setVertexBytes:rect length:sizeof rect atIndex:0];
                [oe setFragmentTexture:g_fbtex atIndex:0];
                [oe setFragmentSamplerState:g_nearest atIndex:0];
                [oe setFragmentBytes:g_gamma length:sizeof(float) * 4 atIndex:0];
                int key = scene != NULL;
                [oe setFragmentBytes:&key length:sizeof key atIndex:1];
                [oe drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            [oe endEncoding];
            g_phase = PHASE_ALL;
        }
        /* GTA2_AB=n: frame n with the original renderer, frame n+2 in 3D, same run */
        static int ab = -2;
        if (ab == -2) ab = getenv("GTA2_AB") ? atoi(getenv("GTA2_AB")) : -1;
        if (g_ab_request) { /* F10: capture original (next frame) then 3D */
            g_ab_request = 0;
            ab = (int)g_frame_no + 2;
            g_layer.framebufferOnly = NO;
        }
        const char *ab_path = NULL;
        if (!g_capture_path) g_frame_no++;
        if (ab > 0 && (int)g_frame_no == ab - 1) world3d_set(0);
        if (ab > 0 && (int)g_frame_no == ab) { ab_path = "ab_original.png"; world3d_set(1); }
        if (ab > 0 && (int)g_frame_no == ab + 2) ab_path = "ab_3d.png";
        int capture = (g_capture_path && ++g_frame_no == g_capture_frame) || ab_path;
        [cb presentDrawable:drawable];
        [cb commit];
        if (capture) {
            [cb waitUntilCompleted];
            char path[1100];
            if (ab_path) snprintf(path, sizeof path, "%s/%s", getenv("GTA2_AB_DIR") ?: ".", ab_path);
            write_png(drawable.texture, ab_path ? path : g_capture_path);
        }
    }
}

void presenter_shutdown(void)
{
    g_layer = nil;
}
