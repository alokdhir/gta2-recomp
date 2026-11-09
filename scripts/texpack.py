#!/usr/bin/env python3
"""Build an upscaled texture pack for the 3D renderer from a GTA2 style (.sty).

Two stages around an external upscaler (run between them, e.g. Real-ESRGAN):

  texpack.py prep   <style.sty> <work_dir>
      writes <work_dir>/in/NNNN.png: each tile tiled 3x3 (so the upscaler sees
      its wrap-around neighbours and the result tiles seamlessly), with colour
      bled into transparent pixels so edges do not pick up a dark fringe.

  realesrgan-ncnn-vulkan -i <work_dir>/in -o <work_dir>/out -n realesrgan-x4plus -s 4

  texpack.py finish <style.sty> <work_dir> <pack_dir>
      crops the centre tile of each result to 256x256, restores the alpha mask
      (smoothly upscaled original mask, thresholded like the renderer's alpha
      test) and writes <pack_dir>/tiles/NNNN.png.

The runtime loads <game_dir>/textures/<style name>/tiles/NNNN.png when present.
Needs Pillow and numpy (build-time only).
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

TILE = 64
OUT = 256


def chunks(path):
    d = open(path, 'rb').read()
    off, ch = 6, {}
    while off + 8 <= len(d):
        n = struct.unpack_from('<I', d, off + 4)[0]
        ch[d[off:off + 4]] = d[off + 8:off + 8 + n]
        off += 8 + n
    return ch


def tiles(path):
    ch = chunks(path)
    tile, ppal, palx = ch[b'TILE'], ch[b'PPAL'], ch[b'PALX']
    for t in range(len(tile) // (TILE * TILE)):
        phys = struct.unpack_from('<H', palx, 2 * t)[0]
        page, slot = phys // 64, phys % 64
        src = np.frombuffer(tile, np.uint8, 65536, (t // 16) * 65536).reshape(256, 256)
        col, row = t % 4, (t // 4) % 4
        idx = src[row * TILE:(row + 1) * TILE, col * TILE:(col + 1) * TILE]
        pal = np.frombuffer(ppal, np.uint8, 65536, page * 65536).reshape(256, 64, 4)[:, slot, :]
        bgr = pal[idx]
        yield t, np.dstack([bgr[..., 2], bgr[..., 1], bgr[..., 0]]), idx > 0


def bleed(rgb, mask):
    """Fill transparent pixels with the average of nearby opaque ones."""
    if mask.all() or not mask.any():
        return rgb
    rgb = rgb.astype(np.float32)
    known = mask.copy()
    for _ in range(TILE):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(mask.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
            cnt += k
        grow = ~known & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        known |= grow
    return rgb.clip(0, 255).astype(np.uint8)


def prep(sty, work):
    os.makedirs(f'{work}/in', exist_ok=True)
    n = 0
    for t, rgb, mask in tiles(sty):
        if not mask.any():
            continue
        Image.fromarray(np.tile(bleed(rgb, mask), (3, 3, 1))).save(f'{work}/in/{t:04d}.png')
        n += 1
    print(f'prep: {n} tiles in {work}/in')


def finish(sty, work, pack):
    os.makedirs(f'{pack}/tiles', exist_ok=True)
    n = missing = 0
    for t, _, mask in tiles(sty):
        p = f'{work}/out/{t:04d}.png'
        if not mask.any():
            continue
        if not os.path.exists(p):
            missing += 1
            continue
        up = Image.open(p).convert('RGB')
        s = up.width // 3
        rgb = up.crop((s, s, 2 * s, 2 * s)).resize((OUT, OUT), Image.LANCZOS)
        a = Image.fromarray((mask * 255).astype(np.uint8)).resize((OUT, OUT), Image.BILINEAR)
        a = a.point(lambda v: 255 if v >= 128 else 0)
        img = rgb.convert('RGBA')
        img.putalpha(a)
        img.save(f'{pack}/tiles/{t:04d}.png')
        n += 1
    print(f'finish: {n} tiles to {pack}/tiles ({missing} missing)')


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'prep':
        prep(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 5 and sys.argv[1] == 'finish':
        finish(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)
