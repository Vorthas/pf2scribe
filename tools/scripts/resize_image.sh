#!/bin/bash
# Shrinks images, in place, to at most WIDTH pixels wide (644 by default:
# enough for a sharp 300 dpi print of an image in a left()/right() sidebar).
# Images already narrower are left untouched; transparency is kept.
#
#   resize_image.sh [-w WIDTH] FILE...
#   resize_image.sh samples/Resource/*.png
#   resize_image.sh -w 1950 full-width-art.png
#
# This overwrites the files: keep a copy of the originals first, e.g.
#   cp -r samples/Resource samples/Resource-originals

set -euo pipefail

width=644
if [[ "${1:-}" == "-w" ]]; then
    width="${2:?-w needs a width in pixels}"
    shift 2
fi
if [[ ! "$width" =~ ^[0-9]+$ ]]; then
    echo "Width must be a number of pixels, not '$width'." >&2
    exit 1
fi
if [[ $# -eq 0 ]]; then
    echo "Usage: $(basename "$0") [-w WIDTH] FILE..." >&2
    exit 1
fi

# ImageMagick 7 runs it as "magick mogrify"; version 6 as "mogrify".
if command -v magick >/dev/null; then
    mogrify=(magick mogrify)
elif command -v mogrify >/dev/null; then
    mogrify=(mogrify)
else
    echo "ImageMagick isn't installed (package: imagemagick)." >&2
    if command -v pacman >/dev/null && [[ -t 0 ]]; then
        read -r -p "Install it now with 'sudo pacman -S --needed imagemagick'? [y/N] " answer
        if [[ "$answer" == [yY]* ]]; then
            sudo pacman -S --needed imagemagick
            mogrify=(magick mogrify)
        else
            exit 1
        fi
    else
        echo "Install it with your package manager, e.g.:" >&2
        echo "  Arch: sudo pacman -S imagemagick    Debian/Ubuntu: sudo apt install imagemagick" >&2
        echo "  Fedora: sudo dnf install ImageMagick" >&2
        exit 1
    fi
fi

for file in "$@"; do
    if [[ ! -f "$file" ]]; then
        echo "Skipped (not a file): $file" >&2
        continue
    fi
    before=$(magick identify -format '%wx%h' "$file" 2>/dev/null || identify -format '%wx%h' "$file")
    # "WIDTHx>": shrink to WIDTH wide, keeping the aspect ratio; never enlarge.
    "${mogrify[@]}" -resize "${width}x>" "$file"
    after=$(magick identify -format '%wx%h' "$file" 2>/dev/null || identify -format '%wx%h' "$file")
    if [[ "$before" == "$after" ]]; then
        echo "unchanged  $file ($before)"
    else
        echo "resized    $file ($before -> $after)"
    fi
done
