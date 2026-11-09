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

Sprites (cars) are captured from the running game instead, so every colour
remap and damage state the game produces is covered:

  GTA2_DUMP_SPRITES=<dump_dir> GTA2.app ...   (drive around)
  texpack.py sprites-prep <dump_dir>/<style> <work_dir> [--all] [--pack <pack_dir>] [--double]
      picks car-sized sprites (people are too small to gain from AI upscaling;
      --all takes every sprite in the folder, for a hand-picked set of objects),
      pads them and bleeds colour into the transparent area. --pack skips
      sprites the pack already has.
  realesrgan-ncnn-vulkan -i <work_dir>/in -o <work_dir>/out -n realesrgan-x4plus-anime -s 4
  texpack.py sprites-masks <work_dir>                       (optional)
  realesrgan-ncnn-vulkan -i <work_dir>/mask_in -o <work_dir>/mask_out -n realesrgan-x4plus-anime -s 4
      upscales each sprite's outline too, so edges come out smooth instead
      of keeping the original pixel staircase (hearts, icons).
  texpack.py sprites-finish <work_dir> <pack_dir>
      writes <pack_dir>/sprites/<hash>.png at 4x, alpha restored (from
      mask_out when present). sprites-prep --double feeds the upscaler a
      pixel-doubled input: cleaner for small flat graphics such as the gang
      meters, where the model otherwise invents texture.
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


PAD = 8


def is_car(w, h):
    return max(w, h) >= 40 and min(w, h) >= 20


def sprites_prep(dump, work, all_sizes=False, pack=None, double=False):
    os.makedirs(f'{work}/in', exist_ok=True)
    os.makedirs(f'{work}/mask', exist_ok=True)
    done = set(os.listdir(f'{work}/in'))
    if pack and os.path.isdir(f'{pack}/sprites'):
        done |= set(os.listdir(f'{pack}/sprites'))
    n = 0
    for f in sorted(os.listdir(dump)):
        if not f.endswith('.png'):
            continue
        h16, dims = f[:-4].split('_')[:2]
        w, h = map(int, dims.split('x'))
        if (not all_sizes and not is_car(w, h)) or f'{h16}.png' in done:
            continue
        im = np.array(Image.open(f'{dump}/{f}').convert('RGBA'))
        mask = im[..., 3] > 0
        rgb = np.pad(bleed(im[..., :3], mask), ((PAD, PAD), (PAD, PAD), (0, 0)), mode='edge')
        out = Image.fromarray(rgb)
        if double:
            out = out.resize((out.width * 2, out.height * 2), Image.NEAREST)
        out.save(f'{work}/in/{h16}.png')
        np.save(f'{work}/mask/{h16}.npy', mask)
        n += 1
    print(f'sprites-prep: {n} new sprites in {work}/in')


def sprites_masks(work):
    os.makedirs(f'{work}/mask_in', exist_ok=True)
    n = 0
    for f in sorted(os.listdir(f'{work}/mask')):
        a = np.pad((np.load(f'{work}/mask/{f}') * 255).astype(np.uint8), PAD)
        Image.fromarray(np.dstack([a, a, a])).save(f'{work}/mask_in/{f[:-4]}.png')
        n += 1
    print(f'sprites-masks: {n} masks in {work}/mask_in')


def sprites_finish(work, pack):
    os.makedirs(f'{pack}/sprites', exist_ok=True)
    n = 0
    for f in sorted(os.listdir(f'{work}/in')):
        if not f.endswith('.png') or not os.path.exists(f'{work}/out/{f}'):
            continue
        mask = np.load(f'{work}/mask/{f[:-4]}.npy')
        h, w = mask.shape
        up = Image.open(f'{work}/out/{f}').convert('RGB')
        k = up.width // (w + 2 * PAD)
        rgb = up.crop((PAD * k, PAD * k, (PAD + w) * k, (PAD + h) * k))
        if k > 4:  # --double: the model gave 8x
            k = 4
            rgb = rgb.resize((w * k, h * k), Image.LANCZOS)
        mp = f'{work}/mask_out/{f}'
        if os.path.exists(mp):  # upscaled outline
            m = Image.open(mp).convert('L')
            km = m.width // (w + 2 * PAD)
            a = m.crop((PAD * km, PAD * km, (PAD + w) * km, (PAD + h) * km)).resize((w * k, h * k), Image.LANCZOS)
        else:
            a = Image.fromarray((mask * 255).astype(np.uint8)).resize((w * k, h * k), Image.BILINEAR)
        img = rgb.convert('RGBA')
        img.putalpha(a.point(lambda v: 255 if v >= 128 else 0))
        img.save(f'{pack}/sprites/{f}')
        n += 1
    print(f'sprites-finish: {n} sprites in {pack}/sprites')


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'prep':
        prep(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 4 and sys.argv[1] == 'sprites-prep':
        pk = sys.argv[sys.argv.index('--pack') + 1] if '--pack' in sys.argv else None
        sprites_prep(sys.argv[2], sys.argv[3], '--all' in sys.argv, pk, '--double' in sys.argv)
    elif len(sys.argv) >= 3 and sys.argv[1] == 'sprites-masks':
        sprites_masks(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == 'sprites-finish':
        sprites_finish(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 5 and sys.argv[1] == 'finish':
        finish(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)
