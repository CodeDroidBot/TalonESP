#!/usr/bin/env python3
"""
bootanim_conv.py — Convert an animated GIF (or folder of PNGs) into
raw RGB565 frames for the ESP32 boot animation player.

Usage:
    python bootanim_conv.py anim.gif /path/to/sd/boot --fps 12
    python bootanim_conv.py frames/ /path/to/sd/boot --fps 12 --size 128x128

Output:
    <out>/config.txt
    <out>/frame_000.bin  (raw RGB565 little-endian, w*h*2 bytes)
    <out>/frame_001.bin
    ...
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not installed. Run: pip install pillow")
    sys.exit(1)


def rgb565_le(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return struct.pack('<H', v)


def load_from_gif(path):
    im = Image.open(path)
    frames = []
    try:
        while True:
            frames.append(im.convert('RGBA').copy())
            im.seek(im.tell() + 1)
    except EOFError:
        pass
    return frames


def load_from_dir(path):
    exts = ('.png', '.jpg', '.jpeg', '.bmp')
    files = sorted([p for p in Path(path).iterdir() if p.suffix.lower() in exts])
    return [Image.open(p).convert('RGBA') for p in files]


def composite(frame_rgba, bg_rgb):
    bg = Image.new('RGBA', frame_rgba.size, bg_rgb + (255,))
    return Image.alpha_composite(bg, frame_rgba).convert('RGB')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('output_dir')
    ap.add_argument('--size', default='128x128',
                    help='Frame size WxH, max 240x320 (default 128x128)')
    ap.add_argument('--fps', type=int, default=12)
    ap.add_argument('--bg', default='000000',
                    help='Background as RRGGBB hex (default 000000)')
    ap.add_argument('--max-frames', type=int, default=120)
    args = ap.parse_args()

    w, h = map(int, args.size.lower().split('x'))
    if w > 240 or h > 320:
        print(f"ERROR: {w}x{h} exceeds 240x320")
        return 1

    bg_rgb = tuple(int(args.bg[i:i+2], 16) for i in (0, 2, 4))

    p = Path(args.input)
    if p.is_dir():
        frames = load_from_dir(p)
    elif p.is_file():
        frames = load_from_gif(p)
    else:
        print(f"ERROR: not found: {p}")
        return 1

    if not frames:
        print("ERROR: no frames loaded")
        return 1

    if len(frames) > args.max_frames:
        print(f"Truncating {len(frames)} -> {args.max_frames} frames")
        frames = frames[:args.max_frames]

    print(f"Frames: {len(frames)}, size: {w}x{h}, fps: {args.fps}")

    out = Path(args.output_dir)
    out.mkdir(parents=True, exist_ok=True)

    for i, frame in enumerate(frames):
        img = composite(frame, bg_rgb).resize((w, h), Image.LANCZOS)
        data = bytearray()
        px = img.load()
        for y in range(h):
            for x in range(w):
                r, g, b = px[x, y]
                data += rgb565_le(r, g, b)
        with open(out / f"frame_{i:03d}.bin", 'wb') as f:
            f.write(data)
        if (i + 1) % 10 == 0 or i == len(frames) - 1:
            print(f"  {i+1}/{len(frames)} frames written")

    with open(out / "config.txt", 'w') as f:
        f.write("# ESP32 boot animation config\n")
        f.write(f"width={w}\n")
        f.write(f"height={h}\n")
        f.write(f"frames={len(frames)}\n")
        f.write(f"fps={args.fps}\n")

    total = sum((out / f"frame_{i:03d}.bin").stat().st_size for i in range(len(frames)))
    print(f"\nDone: {len(frames)} frames, {total // 1024} KB total")
    print(f"Copy '{out}' to the SD card root as '/boot/'")
    return 0


if __name__ == '__main__':
    sys.exit(main())