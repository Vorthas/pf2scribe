#!/usr/bin/env python3
"""Pull web fonts embedded as base64 data: URLs out of a saved Scribe HTML
export, e.g. the Taroca headline font.

Fonts Scribe loads from Adobe Fonts (Sabon, Good Pro, Gin) are only linked by
URL, not embedded, so they can't be extracted this way.

Usage: extract_embedded_fonts.py export.htm [output_dir]

Check each font's license before bundling or redistributing it; the name-table
license text is printed when it can be read. Converting woff/woff2 to ttf needs
`pip install fonttools brotli`.
"""
import base64
import pathlib
import re
import sys

FONT_FACE = re.compile(r"@font-face\s*\{(.*?)\}", re.S)
FAMILY = re.compile(r"font-family:\s*\\?['\"]?([^;'\"\\]+)")
DATA = re.compile(
    r"data:font/(woff2|woff|ttf|otf|truetype|opentype);base64,([A-Za-z0-9+/=]+)"
)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    html = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    out = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "fonts")
    out.mkdir(parents=True, exist_ok=True)

    found = 0
    for face in FONT_FACE.finditer(html):
        data = DATA.search(face.group(1))
        family = FAMILY.search(face.group(1))
        if not data or not family:
            continue
        found += 1
        ext = {"truetype": "ttf", "opentype": "otf"}.get(data.group(1), data.group(1))
        name = family.group(1).strip().replace(" ", "-")
        path = out / f"{name}.{ext}"
        b64 = data.group(2)
        path.write_bytes(base64.b64decode(b64 + "=" * (-len(b64) % 4)))
        print(f"wrote {path}")

        try:
            from fontTools.ttLib import TTFont

            font = TTFont(path)
            for record in font["name"].names:
                if record.nameID in (0, 13, 14):
                    print(f"  name {record.nameID}: {record.toUnicode()[:200]}")
            if ext in ("woff", "woff2"):
                font.flavor = None
                ttf = path.with_suffix(".ttf")
                font.save(ttf)
                print(f"  converted to {ttf}")
        except Exception as error:  # missing fontTools/brotli, odd files
            print(f"  could not inspect/convert: {error}")

    if not found:
        print("no embedded fonts found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
