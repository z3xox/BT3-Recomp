#!/usr/bin/env python3
"""Rewrite the boot memory-card popup's text by remapping cells in the PS2 font atlas.

The popup's text is not a string. The game stores glyph INDICES, one draw per glyph, and the atlas
is a plain ASCII-ordered font where `index = ascii - 32`. The game therefore knows nothing about the
letters -- it only knows which cell to sample. Replace the shape in a cell and the popup says
something else, with no change to the game, its data files, or the save format.

The remap copies whole cells from the original atlas, so the replacement is the console's own
rasterisation: same hinting, same 4 alpha levels, no font dependency, nothing to tune.

See docs/MEMORY-CARD-POPUP.md for the measurements this is built on.

    ./atlas_text.py --message "HELLO" --out ../Textures/<identity>.png
    ./atlas_text.py --preview            # render the current message, no files written
"""

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

# --- the atlas, as measured -------------------------------------------------------------------
IDENTITY = "1dd4c76113969303-6dfa844c9490b8ed-00001e54"   # <tex0hash>-<cluthash>-<bits>
ATLAS_W, ATLAS_H = 512, 128
COLS, ROWS = 25, 6
CELL = 20
FIRST_ASCII = 32          # index 0 is the space cell
LAST_ASCII = 126          # index 94

# The four lines the popup draws, and how many cells each one gets. The positions are fixed by the
# draw stream, so a replacement has to fit the existing cell count per line -- a longer line is not
# a rendering problem, it is a different message array.
LINE_CELLS = (18, 27, 32, 43)

# The message is NOT transcribed here on purpose. Hand-typing it is how "controler" turned into
# "controller" and "(PS2)..." into "(PS2)." the wrong way round: the ghosting of the tripled glyphs
# makes the rebuilt image read like proper English, and the eye fills in the missing letters. The
# index array is the only authority, so the text is decoded from it.


def cell_box(index: int) -> tuple[int, int, int, int]:
    """Texel rect of a glyph cell. index = row * COLS + col."""
    row, col = divmod(index, COLS)
    x, y = col * CELL, row * CELL
    return x, y, x + CELL, y + CELL


def ascii_index(ch: str) -> int:
    """Cell holding `ch`, or -1 when the atlas has no glyph for it."""
    code = ord(ch)
    if FIRST_ASCII <= code <= LAST_ASCII:
        return code - FIRST_ASCII
    return -1


def load_atlas(path: Path) -> Image.Image:
    im = Image.open(path).convert("RGBA")
    if im.size != (ATLAS_W, ATLAS_H):
        raise SystemExit(
            f"atlas is {im.size[0]}x{im.size[1]}, expected {ATLAS_W}x{ATLAS_H} -- "
            "the grid maths below assume the measured 25x6 cells of 20px"
        )
    return im


def build_message(lines: list[str]) -> list[str]:
    """The message as index arrays, one per draw line, padded to the original cell counts."""
    if len(lines) != len(LINE_CELLS):
        raise SystemExit(f"expected {len(LINE_CELLS)} lines, got {len(lines)}")
    out = []
    for text, budget in zip(lines, LINE_CELLS):
        if len(text) > budget:
            raise SystemExit(
                f'line {len(out) + 1} is {len(text)} characters, the draw stream gives it {budget} '
                f"cells: {text!r}"
            )
        row = []
        for ch in text:
            idx = ascii_index(ch)
            if idx < 0:
                raise SystemExit(f"character {ch!r} (U+{ord(ch):04X}) is not in the atlas")
            row.append(str(idx))
        row += ["0"] * (budget - len(text))       # pad with the space cell
        out.append(" ".join(row))
    return out


def decode_message() -> list[str]:
    """The message as text, decoded from the index array. The authoritative form."""
    out = []
    for cells in cell_pattern():
        out.append("".join(
            " " if cell == 0 else
            (chr(cell + FIRST_ASCII) if FIRST_ASCII <= cell + FIRST_ASCII <= LAST_ASCII else "?")
            for cell in cells
        ))
    return out


def cell_pattern() -> list[list[int]]:
    """The drawn index array, split into lines -- the positions the game fills, in order."""
    lines, at = [], 0
    for budget in LINE_CELLS:
        lines.append(DEFAULT_INDICES[at:at + budget])
        at += budget
    if at != len(DEFAULT_INDICES):
        raise SystemExit(f"LINE_CELLS sums to {at} but the draw stream has {len(DEFAULT_INDICES)} cells")
    return lines


def check(message: list[str], auto: bool = False) -> tuple[dict[int, str], list[str]]:
    """cell -> the character it must show. Refuses anything the draw stream cannot express.

    The game addresses the alphabet by INDEX, so a cell drawn twice can only ever show one letter:
    the second write simply overwrites the first. That is a hard constraint on any replacement --
    wherever the original reuses a cell, the replacement must reuse the same letter, or the two
    positions fight and the text comes out garbled. And a position drawn from cell 0 is the space
    cell, so it cannot carry a letter at all, which also means the replacement cannot be longer than
    the run of non-space cells before the next space.

    Both are checked here rather than discovered on screen. A positional remap without this check
    "works" and produces nonsense: writing H,O,L,A into positions 0..3 of line 1 lands H and L in
    cell 45, which the game draws twice, so it reads H O H L.
    """
    assign: dict[int, str] = {}
    where: dict[int, list[tuple[int, int]]] = {}
    problems: list[str] = []

    for row, (text, cells) in enumerate(zip(message, cell_pattern())):
        padded = text.ljust(len(cells))[:len(cells)]
        for col, (ch, cell) in enumerate(zip(padded, cells)):
            where.setdefault(cell, []).append((row, col))
            if cell == 0:
                if ch != " ":
                    problems.append(
                        f"line {row + 1} pos {col + 1}: the game draws the SPACE cell here, "
                        f"so {ch!r} cannot be shown"
                    )
                continue
            if ch == " ":
                # A letter cell can be blanked: copy the space cell's pixels over it and the glyph
                # quad samples empty texels, which under the Fs/Fd blend draw nothing. That is what
                # makes short text possible at all -- the draw stream always issues 120 glyph quads,
                # so a 9-character title is 9 letters and 9 blanked cells, not 9 quads.
                assign[cell] = " "
                continue
            prev = assign.get(cell)
            if prev is None:
                assign[cell] = ch
            elif prev != ch:
                others = ", ".join(f"line {r + 1} pos {c + 1}" for r, c in where[cell][:-1])
                problems.append(
                    f"cell {cell} is drawn at {others} and at line {row + 1} pos {col + 1}: it "
                    f"would have to show both {prev!r} and {ch!r}. One cell, one letter."
                )
    if problems and not auto:
        raise SystemExit(
            "the draw stream cannot express this text:\n  "
            + "\n  ".join(problems)
            + "\n\nMatch the shape of the original message, or pass --auto to resolve the "
            "conflicts by first-come (see what it changes with --preview).\n"
            "The shape each line must have:\n"
            + "\n".join(
                "  " + " ".join(
                    f"{sum(1 for c in cells[:i + 1] if c == 0) and '.' or '#'}"
                    for i, _ in enumerate(cells))
                for cells in cell_pattern())
        )
    if problems:
        notes = ["--auto resolved these conflicts; the listed positions will NOT read as asked:"]
        notes += ["  " + p for p in problems]
        problems = notes
    return assign, problems


def remap(atlas: Image.Image, assign: dict[int, str]) -> Image.Image:
    """Copy each source cell's pixels into the cell the draw stream samples.

    Whole cells, not just alpha: the original's RGB is flat (128,128,128) and the shape is entirely
    in the alpha channel, so copying RGBA preserves both the colour the draw blends and the 4 alpha
    levels the console antialiases with.
    """
    out = atlas.copy()
    # Read every source out of the pristine atlas before writing any of them back: a cell is very
    # often both a source and a destination, since the replacement reuses the same alphabet.
    # A cell assigned " " is blanked by copying the space cell (index 0), which is already empty in
    # the original atlas -- so the result is a genuinely blank cell, not a transparent one.
    patches = [(cell, atlas.crop(cell_box(ascii_index(ch) if ch != " " else 0)))
               for cell, ch in assign.items()]
    for cell, patch in patches:
        if cell == 0:
            continue                            # never fill the space cell
        out.paste(patch, cell_box(cell)[:2])
    return out


# The index arrays the game actually draws, measured from the draw stream.
DEFAULT_INDICES = [
    45, 37, 45, 47, 50, 57, 0, 35, 33, 50, 36, 0, 83, 76, 79, 84, 0, 17,
    35, 72, 69, 67, 75, 73, 78, 71, 0, 77, 69, 77, 79, 82, 89, 0, 67, 65, 82, 68, 0, 8, 48, 51,
    18, 9, 14,
    36, 79, 0, 78, 79, 84, 0, 82, 69, 77, 79, 86, 69, 0, 77, 69, 77, 79, 82, 89, 0, 67, 65, 82,
    68, 0, 8, 48, 51, 18, 9, 12,
    67, 79, 78, 84, 82, 79, 76, 69, 82, 12, 0, 82, 69, 83, 69, 84, 12, 0, 79, 82, 0, 83, 87, 73,
    84, 67, 72, 0, 79, 70, 0, 84, 72, 69, 0, 67, 79, 78, 83, 79, 76, 69, 14,
]


def preview(atlas: Image.Image, out: Path, scale: int = 3) -> None:
    """Render what the game will actually show: the drawn index array, sampled from `atlas`.

    Deliberately NOT indexed by the message's characters. After a remap, cell 45 holds a different
    letter, so looking up "the cell for H" would show the untouched original and hide the whole
    point -- the game addresses the alphabet by index and knows nothing about the letters. Only
    sampling by the drawn index shows what lands on screen.
    """
    pad = 8
    lh = CELL * scale + 6
    lines = cell_pattern()
    # Width from the longest line, not from the atlas: 43 cells at 3x is 2580px, so a 512px canvas
    # silently crops the preview and hides the end of every line after the first few characters.
    width = max(len(cells) for cells in lines) * CELL * scale + pad * 2
    canvas = Image.new("RGBA", (width, pad * 2 + lh * len(lines)), (0, 0, 0, 255))
    for row, cells in enumerate(lines):
        y = pad + row * lh
        for col, cell in enumerate(cells):
            if cell == 0:
                continue
            glyph = atlas.crop(cell_box(cell)).split()[3]
            big = glyph.resize((CELL * scale, CELL * scale), Image.NEAREST)
            canvas.paste(Image.merge("RGB", (big, big, big)), (pad + col * CELL * scale, y))
    canvas.convert("RGB").save(out)
    print(f"preview -> {out}  ({canvas.width}x{canvas.height})")


def show_pattern() -> None:
    """The template any replacement has to fit: which drawn positions share a cell."""
    for row, cells in enumerate(cell_pattern()):
        letters = []
        for cell in cells:
            if cell == 0:
                letters.append(".")
            else:
                letters.append(chr(cell + FIRST_ASCII) if FIRST_ASCII <= cell + FIRST_ASCII <= LAST_ASCII else "?")
        print(f"line {row + 1}: {''.join(letters)}")
        print(f"        {' '.join(str(c) for c in cells)}")
    # Which positions are forced to carry the same letter.
    shared: dict[int, list[tuple[int, int]]] = {}
    for r, cells in enumerate(cell_pattern()):
        for c, cell in enumerate(cells):
            if cell:
                shared.setdefault(cell, []).append((r, c))
    forced = {k: v for k, v in shared.items() if len(v) > 1}
    print("\ncells drawn more than once -- these positions must show the SAME letter:")
    for cell, pos in sorted(forced.items()):
        where = ", ".join(f"line {r + 1} pos {c + 1}" for r, c in pos)
        shown = chr(cell + FIRST_ASCII) if FIRST_ASCII <= cell + FIRST_ASCII <= LAST_ASCII else "?"
        print(f"  cell {cell:3d} ({shown!r})  {where}")
    print("\n'.' above is a position the game draws from the SPACE cell: it must stay blank.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--atlas", type=Path,
                    help=f"the dumped atlas PNG, e.g. {IDENTITY}_512x128.png")
    ap.add_argument("--message", help="the replacement text; '|' separates the four lines")
    ap.add_argument("--message-file", type=Path,
                    help="read the replacement text from a file, one line per popup row")
    ap.add_argument("--out", type=Path, help=f"output PNG (default: <Textures>/{IDENTITY}.png)")
    ap.add_argument("--preview", type=Path, help="also write a PNG preview of the result here")
    ap.add_argument("--pattern", action="store_true",
                    help="print the template a replacement must fit, then exit")
    ap.add_argument("--dry-run", action="store_true", help="validate and report, write nothing")
    ap.add_argument("--auto", action="store_true",
                    help="resolve one-cell-one-glyph conflicts by first come instead of refusing")
    args = ap.parse_args()

    if args.pattern:
        show_pattern()
        return 0

    if not args.atlas:
        ap.error("--atlas is required unless --pattern is given")
    atlas = load_atlas(args.atlas)

    if args.message and args.message_file:
        ap.error("--message and --message-file are mutually exclusive")
    if args.message_file:
        # One line per popup row. Trailing blank lines are dropped, interior ones are kept -- a blank
        # row is a legitimate thing to want, and silently eating it would move every row below it up.
        lines = args.message_file.read_text(encoding="utf-8").splitlines()
        while lines and not lines[-1].strip():
            lines.pop()
    elif args.message:
        lines = args.message.split("|")
    else:
        lines = decode_message()

    for text, idx_line in zip(lines, build_message(lines)):
        print(f"{text!r}")
        print(f"    {idx_line}")

    # Raises with the exact conflicts before anything is written -- a remap that the draw stream
    # cannot express looks like it worked and then renders as garbage.
    assign, notes = check(lines, auto=args.auto)
    for n in notes:
        print(n)
    print(f"\n{len(assign)} cells to rewrite")

    if args.dry_run:
        print("dry run: nothing written")
        return 0

    result = remap(atlas, assign)

    if args.preview:
        preview(result, args.preview)

    out = args.out
    if out is None:
        print(f"\n--out not given: pass --out <Textures>/{IDENTITY}.png to write it")
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    result.save(out)
    print(f"\natlas -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
