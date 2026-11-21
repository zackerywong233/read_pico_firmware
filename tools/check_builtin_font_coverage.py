#!/usr/bin/env python3
"""Check that the generated UI font covers every scanned UI character."""

from pathlib import Path

import gen_builtin_font
from fontTools.ttLib import TTFont

root = Path(__file__).resolve().parents[1]
font_path = root / "main/assets/builtin.ttf"
main = root / "main"
texts = []
for path in [*main.rglob("*.c"), *main.rglob("*.h"), *main.rglob("*.md"), *main.rglob("*.txt")]:
    data = path.read_text(encoding="utf-8", errors="ignore")
    if path.suffix in gen_builtin_font.TEXT_SUFFIXES:
        texts.append(data)
    else:
        texts.extend(gen_builtin_font.unescape(match.group(0))
                     for match in gen_builtin_font.STRING_RE.finditer(data))
required = gen_builtin_font.unique_text(
    gen_builtin_font.ascii_printable(),
    gen_builtin_font.load_preset(gen_builtin_font.CHARSET),
    "".join(texts),
)
font = TTFont(font_path)
covered = set().union(*(table.cmap.keys() for table in font["cmap"].tables))
missing = sorted({ord(char) for char in required} - covered)
font.close()

size = font_path.stat().st_size
if missing:
    print("Missing code points:", " ".join(f"U+{code:04X}" for code in missing))
    raise SystemExit(1)
if size > gen_builtin_font.MAX_BYTES:
    print(f"Font is too large: {size} > {gen_builtin_font.MAX_BYTES}")
    raise SystemExit(1)
print(f"Font coverage: 100% ({len(required)} characters), {size} bytes")
