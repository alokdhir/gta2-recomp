#include "render3d/texpack.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "render3d/world_data.h"

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

static int sprite_dir(char *out, size_t n)
{
    static int off = -1;
    if (off < 0) off = getenv("GTA2_NO_PACK") != NULL;
    const char *path = world_style_path();
    if (off || !path) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    snprintf(out, n, "textures/%.*s/sprites", len, base);
    struct stat sb;
    return stat(out, &sb) == 0 && S_ISDIR(sb.st_mode);
}

uint32_t *texpack_sprite(uint64_t hash, int *w, int *h)
{
    char dir[1100], file[1200];
    if (!sprite_dir(dir, sizeof dir)) return NULL;
    snprintf(file, sizeof file, "%s/%016llX.png", dir, (unsigned long long)hash);
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

void texpack_dump(uint64_t hash, const uint32_t *rgba, int w, int h)
{
    static const char *dir = (const char *)-1;
    if (dir == (const char *)-1) {
        dir = getenv("GTA2_DUMP_SPRITES");
        if (dir) mkdir(dir, 0755);
    }
    if (!dir) return;
    char file[1200];
    snprintf(file, sizeof file, "%s/%016llX_%dx%d.png", dir, (unsigned long long)hash, w, h);
    struct stat sb;
    if (stat(file, &sb) == 0) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* 0xAARRGGBB words = BGRA bytes; alpha is 0 or 255 so "first" unpremultiplied is exact */
    CGContextRef ctx = CGBitmapContextCreate((void *)rgba, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!ctx) return;
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)file, (CFIndex)strlen(file), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    if (url) CFRelease(url);
    CGImageRelease(img);
}
