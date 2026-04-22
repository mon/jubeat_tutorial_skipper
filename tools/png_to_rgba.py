"""Convert a PNG to a raw RGBA8 blob + a tiny header with its dimensions.

Usage: png_to_rgba.py <input.png> <output_dir>

Derives a name prefix from the input filename stem. For input `foo.png`,
writes into <output_dir>:
  foo.rgba      - width*height*4 bytes, top-to-bottom, RGBA8.
  foo_dims.h    - constexpr FOO_W / FOO_H / FOO_RGBA_SIZE.
"""
import sys
from pathlib import Path
from PIL import Image


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    src = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    stem = src.stem
    upper = stem.upper()

    img = Image.open(src).convert("RGBA")
    rgba = img.tobytes()
    assert len(rgba) == img.width * img.height * 4

    (out_dir / f"{stem}.rgba").write_bytes(rgba)
    (out_dir / f"{stem}_dims.h").write_text(
        f"// Generated from {src.name} by tools/png_to_rgba.py. Do not edit.\n"
        f"#pragma once\n"
        f"#include <stddef.h>\n"
        f"inline constexpr unsigned {upper}_W = {img.width};\n"
        f"inline constexpr unsigned {upper}_H = {img.height};\n"
        f"inline constexpr size_t   {upper}_RGBA_SIZE = {len(rgba)};\n"
    )


if __name__ == "__main__":
    main()
