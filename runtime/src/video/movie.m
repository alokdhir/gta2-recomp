/* Intro movie: the game's Bink movies, converted once at build time to MP4
 * (scripts/setup.sh, with ffmpeg) and played here with AVFoundation over the
 * game's view. See movie.h. */
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>

#include "video/movie.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#include "winapi/imports.h"
#include "winapi/user32_api.h"

int movie_play(const char *bik_path)
{
    if (getenv("GTA2_HEADLESS")) return 0;
    /* "data\movie\intro.bik" -> data/movie/intro.mp4 (file lookups are case-insensitive
     * in kernel32_file, but AVFoundation needs the real name) */
    char path[1024];
    snprintf(path, sizeof path, "%s", bik_path);
    for (char *c = path; *c; c++) if (*c == '\\') *c = '/';
    char *dot = strrchr(path, '.');
    if (!dot) return 0;
    strcpy(dot, ".mp4");
    NSString *want = @(path), *found = nil;
    NSString *dir = want.stringByDeletingLastPathComponent, *real = @".";
    for (NSString *part in dir.pathComponents) { /* resolve each component without regard to case */
        NSString *hit = nil;
        for (NSString *e in [NSFileManager.defaultManager contentsOfDirectoryAtPath:real error:nil])
            if ([e caseInsensitiveCompare:part] == NSOrderedSame) { hit = e; break; }
        if (!hit) break;
        real = [real stringByAppendingPathComponent:hit];
    }
    for (NSString *e in [NSFileManager.defaultManager contentsOfDirectoryAtPath:real error:nil])
        if ([e caseInsensitiveCompare:want.lastPathComponent] == NSOrderedSame)
            found = [real stringByAppendingPathComponent:e];
    if (!found) {
        rt_log("movie: %s not found (run scripts/setup.sh to convert the Bink movies)", path);
        return 0;
    }

    CALayer *host = (__bridge CALayer *)user32_metal_layer(0);
    if (!host) return 0;
    AVPlayer *player = [AVPlayer playerWithURL:[NSURL fileURLWithPath:found]];
    AVPlayerLayer *layer = [AVPlayerLayer playerLayerWithPlayer:player];
    layer.videoGravity = AVLayerVideoGravityResizeAspect;
    layer.backgroundColor = NSColor.blackColor.CGColor;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.frame = host.bounds;
    layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [host addSublayer:layer];
    [CATransaction commit];

    __block int ended = 0;
    id obs = [NSNotificationCenter.defaultCenter addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                                                             object:player.currentItem
                                                              queue:nil
                                                         usingBlock:^(NSNotification *n) { (void)n; ended = 1; }];
    rt_log("movie: playing %s", found.UTF8String);
    [player play];

    user32_swallow_keys(1);
    unsigned presses = user32_presses();
    int skipped = 0;
    while (!ended) {
        user32_pump();
        if (user32_presses() != presses) { skipped = 1; break; }
        if (player.currentItem.status == AVPlayerItemStatusFailed) {
            rt_log("movie: cannot play: %s", player.currentItem.error.localizedDescription.UTF8String);
            break;
        }
        usleep(10000);
    }
    [player pause];
    [NSNotificationCenter.defaultCenter removeObserver:obs];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [layer removeFromSuperlayer];
    [CATransaction commit];
    /* let the skip key's release go by before the game sees keys again */
    for (int i = 0; i < 15; i++) { user32_pump(); usleep(10000); }
    user32_swallow_keys(0);
    rt_log("movie: %s", skipped ? "skipped" : "finished");
    return 1;
}
