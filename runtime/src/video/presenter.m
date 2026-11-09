/* Metal presenter. See presenter.h. */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "video/presenter.h"
#include "win32/imports.h"

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLRenderPipelineState> g_blit;
static id<MTLSamplerState> g_nearest;
static id<MTLTexture> g_fbtex;
static id<MTLTexture> g_depth; /* depth buffer for the 3D world, sized to the drawable */
static CAMetalLayer *g_layer;
static int g_w, g_h;
static unsigned g_frame_no;
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
    }
    g_layer = (__bridge CAMetalLayer *)metal_layer;
    g_layer.device = g_dev;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_capture_path = getenv("GTA2_CAPTURE");
    g_capture_frame = getenv("GTA2_CAPTURE_FRAME") ? (unsigned)atoi(getenv("GTA2_CAPTURE_FRAME")) : 120;
    g_layer.framebufferOnly = g_capture_path == NULL;
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
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
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

        if (scene) scene(enc, ctx);
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
        [enc endEncoding];
        int capture = g_capture_path && ++g_frame_no == g_capture_frame;
        [cb presentDrawable:drawable];
        [cb commit];
        if (capture) {
            [cb waitUntilCompleted];
            write_png(drawable.texture, g_capture_path);
        }
    }
}

void presenter_shutdown(void)
{
    g_layer = nil;
}
