#!/usr/bin/env python3
"""Make the white background of an image transparent while keeping the
creature solid, even where dark, soft-edged parts (a shadow, smoke, a hole the
creature comes out of) fade into the white.

How it works:
  1. "Colour to alpha" against white (like GIMP's tool): each pixel is taken as
     some colour blended over white, and the white is taken back out. Pure white
     becomes fully transparent, pale smoke partly transparent. Anything at least
     --solid opaque is made fully opaque, so dark areas stay solid; the colours
     are solved again so the image over white looks exactly as before.
  2. The creature is protected: pixels with at least --chroma of colour (the
     difference between their strongest and weakest channel) are the creature,
     cleaned of specks, with small gaps closed and enclosed pale highlights
     filled in. They keep their original colour, fully opaque. Enclosed areas
     that are near-white and at least 3 pixels across (holes in leaves,
     the gap between an arm and the body) are background after all.

This suits art whose creature is coloured and whose background effects are
grey or black. For an all-grey creature, use --chroma 256 to skip step 2
(everything then goes through colour to alpha).

Usage: make_transparent.py IMAGE [-o OUTPUT] [--chroma N] [--solid A]
  Writes IMAGE-transparent.png next to IMAGE unless -o is given; never
  overwrites IMAGE itself. Needs numpy and Pillow.

Example (used for Wallmaster.png, 2026-10-04, with the defaults):
  tools/scripts/make_transparent.py Wallmaster.png
"""
import argparse
import pathlib
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def opened(mask, size):
    """`mask` (bool) without the parts narrower than `size` pixels."""
    image = Image.fromarray((mask * 255).astype(np.uint8))
    image = image.filter(ImageFilter.MinFilter(size)).filter(ImageFilter.MaxFilter(size))
    return np.asarray(image) > 0


def grown(mask, size):
    image = Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(size))
    return np.asarray(image) > 0


def creature_mask(rgb, chroma_min):
    """True where the pixel belongs to the (coloured) creature."""
    chroma = rgb.max(2) - rgb.min(2)
    coloured = chroma >= chroma_min
    mask = Image.fromarray((coloured * 255).astype(np.uint8))
    mask = mask.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MaxFilter(3))  # drop specks
    mask = mask.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))  # close small gaps
    # Fill enclosed holes (pale highlights): flood the outside from the
    # corners; whatever it doesn't reach is the creature.
    outside = Image.eval(mask, lambda v: 255 - v)
    w, h = outside.size
    for seed in [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]:
        if outside.getpixel(seed) == 255:
            ImageDraw.floodfill(outside, seed, 128)
    creature = np.asarray(outside) != 128

    # Enclosed near-white areas at least 3 pixels across are holes in the
    # creature, not highlights: background, with their soft rims (the
    # colourless pixels next to them).
    white = creature & ~coloured & (rgb.min(2) >= 235)
    holes = opened(white, 3)
    holes = grown(holes, 3) & ~coloured
    return creature & ~holes


def make_transparent(image, chroma_min=22, solid=0.7):
    rgba = np.asarray(image.convert("RGBA")).astype(float)
    rgb = rgba[..., :3]

    # The least alpha that explains each pixel as a colour over white,
    # scaled so that `solid` and above become fully opaque.
    alpha = ((255 - rgb) / 255).max(2)
    alpha = np.minimum(1, alpha / solid)
    safe = np.where(alpha > 0, alpha, 1)
    colour = np.clip((rgb - (1 - alpha[..., None]) * 255) / safe[..., None], 0, 255)

    creature = creature_mask(rgb, chroma_min)
    out = np.empty_like(rgba)
    out[..., :3] = np.where(creature[..., None], rgb, colour)
    out[..., 3] = np.where(creature, 255, alpha * 255) * (rgba[..., 3] / 255)
    result = np.round(out).astype(np.uint8)

    # Over white the result should look like the original.
    a = result[..., 3:] / 255
    difference = np.abs(result[..., :3] * a + 255 * (1 - a) - rgb).max()
    return Image.fromarray(result, "RGBA"), creature.sum(), difference


def main():
    parser = argparse.ArgumentParser(
        description="Make an image's white background transparent, keeping the creature solid.")
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("-o", "--output", type=pathlib.Path,
                        help="output file (default: IMAGE-transparent.png)")
    parser.add_argument("--chroma", type=int, default=22,
                        help="least colour a pixel needs to count as the creature (default 22; 256: none)")
    parser.add_argument("--solid", type=float, default=0.7,
                        help="opacity from which a pixel is made fully solid (default 0.7)")
    args = parser.parse_args()

    output = args.output or args.image.with_name(args.image.stem + "-transparent.png")
    if output.resolve() == args.image.resolve():
        sys.exit("Refusing to overwrite the original; choose another -o.")
    if not 0 < args.solid <= 1:
        sys.exit("--solid must be above 0 and at most 1.")

    result, creature, difference = make_transparent(Image.open(args.image), args.chroma, args.solid)
    result.save(output)
    alpha = np.asarray(result)[..., 3]
    total = alpha.size
    print(f"{output}: {creature / total:.0%} creature, "
          f"{(alpha == 0).sum() / total:.0%} fully transparent, "
          f"{((alpha > 0) & (alpha < 255)).sum() / total:.0%} partly; "
          f"over white it differs from the original by at most {difference:.1f} levels")


if __name__ == "__main__":
    main()
