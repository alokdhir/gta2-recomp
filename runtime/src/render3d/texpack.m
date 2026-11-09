/* Sprite texture pack: see texpack.h.
 *
 * Nothing here may stall the game thread: the game runs on a fixed clock and
 * makes up for a slow frame by running fast for a moment (visible as a 2x
 * speed burst). So the pack folder is listed once, every image in it is
 * decoded on a background queue as soon as a style is known, and dumps are
 * written in the background too. */
#import <Foundation/Foundation.h>

#include "render3d/texpack.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <os/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "render3d/world_data.h"
#include "winapi/imports.h"

uint64_t texpack_hash(const uint32_t *rgba, int w, int h)
{
    uint64_t x = 0xcbf29ce484222325ull;
    uint32_t dims[2] = { (uint32_t)w, (uint32_t)h };
    const uint8_t *p = (const uint8_t *)dims;
    for (int i = 0; i < 8; i++) x = (x ^ p[i]) * 0x100000001b3ull;
    p = (const uint8_t *)rgba;
    for (size_t i = 0, n = (size_t)w * h * 4; i < n; i++) x = (x ^ p[i]) * 0x100000001b3ull;
    return x;
}

/* ---- decoding ---------------------------------------------------------------- */

typedef struct {
    uint32_t *px;
    int w, h;
    int ready; /* 0 = listed, not decoded yet; 1 = decoded (px may be NULL on failure) */
} PackImage;

static NSMutableDictionary<NSNumber *, NSValue *> *g_index; /* hash -> PackImage* */
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;
static char g_dir[1100], g_for_style[1100];

static uint32_t *decode_png(const char *file, int *w, int *h)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)file, (CFIndex)strlen(file), false);
    CGImageSourceRef src = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
    if (url) CFRelease(url);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    if (src) CFRelease(src);
    if (!img) return NULL;
    int iw = (int)CGImageGetWidth(img), ih = (int)CGImageGetHeight(img);
    uint32_t *px = calloc((size_t)iw * ih, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, (size_t)iw, (size_t)ih, 8, (size_t)iw * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (ctx) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, iw, ih), img);
        CGContextRelease(ctx);
    }
    CGImageRelease(img);
    *w = iw;
    *h = ih;
    return px;
}

static void decode_into(PackImage *pi, uint64_t hash)
{
    char file[1200];
    snprintf(file, sizeof file, "%s/%016llX.png", g_dir, (unsigned long long)hash);
    int w = 0, h = 0;
    uint32_t *px = decode_png(file, &w, &h);
    os_unfair_lock_lock(&g_lock);
    if (!pi->ready) {
        pi->px = px, pi->w = w, pi->h = h, pi->ready = 1;
        px = NULL;
    }
    os_unfair_lock_unlock(&g_lock);
    free(px); /* someone else decoded it first */
}

/* List the style's pack folder and start decoding everything in it. */
static void index_style(void)
{
    static int off = -1;
    if (off < 0) off = getenv("GTA2_NO_PACK") != NULL;
    const char *path = world_style_path();
    if (off || !path || !strcmp(path, g_for_style)) return;
    snprintf(g_for_style, sizeof g_for_style, "%s", path);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    snprintf(g_dir, sizeof g_dir, "textures/%.*s/sprites", len, base);

    NSMutableDictionary *idx = [NSMutableDictionary dictionary];
    NSArray *files = [NSFileManager.defaultManager contentsOfDirectoryAtPath:@(g_dir) error:nil];
    for (NSString *f in files) {
        if (![f hasSuffix:@".png"] || f.length != 20) continue;
        unsigned long long h = strtoull([f substringToIndex:16].UTF8String, NULL, 16);
        PackImage *pi = calloc(1, sizeof *pi);
        idx[@(h)] = [NSValue valueWithPointer:pi];
    }
    os_unfair_lock_lock(&g_lock);
    g_index = idx; /* images of a previous style are leaked deliberately: tiny, and rare */
    os_unfair_lock_unlock(&g_lock);
    rt_log("texpack: %lu sprite replacements in %s, decoding in the background", (unsigned long)idx.count, g_dir);
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    [idx enumerateKeysAndObjectsUsingBlock:^(NSNumber *k, NSValue *v, BOOL *stop) {
        (void)stop;
        PackImage *pi = v.pointerValue;
        uint64_t h = k.unsignedLongLongValue;
        dispatch_async(q, ^{ decode_into(pi, h); });
    }];
}

uint32_t *texpack_sprite(uint64_t hash, int *w, int *h)
{
    index_style();
    os_unfair_lock_lock(&g_lock);
    PackImage *pi = g_index ? [g_index[@(hash)] pointerValue] : NULL;
    int ready = pi && pi->ready;
    os_unfair_lock_unlock(&g_lock);
    if (!pi) return NULL;
    if (!ready) decode_into(pi, hash); /* needed before the background got to it: one small decode */
    if (!pi->px) return NULL;
    uint32_t *copy = malloc((size_t)pi->w * pi->h * 4);
    memcpy(copy, pi->px, (size_t)pi->w * pi->h * 4);
    *w = pi->w;
    *h = pi->h;
    return copy;
}

/* ---- dumping ----------------------------------------------------------------- */

void texpack_dump(uint64_t hash, const uint32_t *rgba, int w, int h, int pal)
{
    static const char *dir = (const char *)-1;
    static NSMutableSet<NSNumber *> *seen;
    if (dir == (const char *)-1) {
        dir = getenv("GTA2_DUMP_SPRITES");
        if (dir) mkdir(dir, 0755);
        seen = [NSMutableSet set];
    }
    if (!dir || [seen containsObject:@(hash)]) return;
    [seen addObject:@(hash)];
    char file[1200];
    snprintf(file, sizeof file, "%s/%016llX_%dx%d_p%d.png", dir, (unsigned long long)hash, w, h, pal);
    size_t n = (size_t)w * h * 4;
    uint32_t *copy = malloc(n);
    memcpy(copy, rgba, n);
    NSString *path = @(file);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
        const char *fp = path.UTF8String;
        struct stat sb;
        if (stat(fp, &sb) == 0) { free(copy); return; }
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        /* 0xAARRGGBB words = BGRA bytes; alpha is 0 or 255 so "premultiplied" is exact */
        CGContextRef ctx = CGBitmapContextCreate(copy, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
                                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(cs);
        if (ctx) {
            CGImageRef img = CGBitmapContextCreateImage(ctx);
            CGContextRelease(ctx);
            CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)fp, (CFIndex)strlen(fp), false);
            CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
            if (dst) {
                CGImageDestinationAddImage(dst, img, NULL);
                CGImageDestinationFinalize(dst);
                CFRelease(dst);
            }
            if (url) CFRelease(url);
            CGImageRelease(img);
        }
        free(copy);
    });
}
