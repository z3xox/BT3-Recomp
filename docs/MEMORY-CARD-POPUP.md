# The boot memory-card popup

The first frame the game draws is the memory-card prompt — the box that says `MEMORY CARD slot 1`.
This file records what that popup is made of, how the text is composed, what is wrong with it, and
how to replace the text by rewriting the font atlas.

Everything here was measured from a live run; the commands that produced each fact are named so any
line can be re-derived rather than trusted.

---

## 1. What the popup actually is

Not a BIOS message. The BIOS theory was the first one and it was wrong on the evidence:

- The ELF has no game UI strings. `strings -n 6 SLUS_216.78` yields 405 phrases and every one of them
  belongs to the SCE SDK or to CRI (`E0040701:Illigal format(not AFS).`, `ADXF/PS2EE Ver.7.44`,
  `DVCI: "%s" found.`). Not one belongs to the game. Menu text ("Character Customize", "Item Shop")
  ships as painted art, and so does this.
- `sceFontKit` is never called: `sceeFontLoadFont` / `sceeFontGenerateString` have **0** hits across
  every instrumented run, confirmed repeatedly. The SDK font is not involved.
- There is no BIOS ROM in the deploy and no BIOS emulation. `PS2_BIOS_BASE` (`0x1FC00000`) exists
  only as an address range that the memory model special-cases; nothing loads or executes a BIOS.
- The game **uploads the font itself** and **draws every glyph itself**. Measured below.

So the popup is the game's own screen, and its text is the game's own data.

## 2. The font atlas

| | |
|---|---|
| identity | `1dd4c76113969303-6dfa844c9490b8ed-00001e54` |
| decoded size | **512 × 128** |
| format | **PSMCT32** (psm 20) |
| uploaded to | `tbp0 = 10760` = **`0x2A0800`** |
| grid | **25 columns × 6 rows**, cell **20 × 20**, pitch exactly 20 |
| cells used | 150 (indices `0`..`149`) |

The grid was measured off the dumped PNG's alpha channel, not assumed: 25 inked column runs with a
start-to-start pitch of 20, and 6 inked row runs.

**The glyphs live in the ALPHA channel.** The RGB of the whole atlas is flat `(128,128,128)` — four
distinct colours in the entire image: `(128,128,128,0)`, `(…,125)`, `(…,193)`, `(…,249)`. The shape
is entirely in alpha, and the draw blends it in with `abe=1, bm=0x44` (Fs and Fd).

This is why the atlas looked empty in the texture dumps and why it was twice reported as "not there".
`PS2X_TEXPNGD` writes RGBA, and a flat-grey RGB composited onto a background hides every glyph. The
file was correct the whole time; the viewer was wrong. To see it, render the alpha channel as
luminance, or use the "flat RGB + varying alpha → show alpha" rule.

## 3. How the text is composed — and why there is no string to find

**The game does not store the sentence. It stores glyph indices, one draw per glyph.**

The atlas is a plain ASCII-ordered font:

```
index = ASCII code − 32
```

| index | 0 | 17 | 33 | 35 | 36 | 37 | 45 | 47 | 50 | 57 | 76 | 79 | 83 | 84 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| glyph | *space* | `1` | `A` | `C` | `D` | `E` | `M` | `O` | `R` | `Y` | `l` | `o` | `s` | `t` |

This is why the `PS2X_MCSTR` probe reports **0 hits** and that is not a contradiction. `MCSTR` scans
RDRAM for `"MEMORY CARD"`, `"memory card"`, `"slot 1"`, `"(PS2)"`. The bytes in RAM are the indices,
so the word `MEMORY` is stored as `0x2D 0x25 0x2D 0x2F 0x30 0x39` (`-%-/09`), offset by 0x20. An
ASCII needle can never match it. (The first version of that probe was also 32 MB × 7 needles per
heartbeat — ~7 GB/s of `memcmp` on the boot path, which starved the game to `prims/sec=0`. It is now
a 1 MB window per heartbeat with overlap, full coverage every 32 beats.)

## 4. The text, recovered from the draw stream

The popup's four lines were reconstructed by reading the draw log, not by OCR:

```
MEMORY CARD slot 1

Checking memory card (PS2)...
Do not remove memory card (PS2),
controler, reset, or switch of the console.
```

**The last line is misspelled in the game and must stay that way:** `controler` with one L, and `of`
with one F. Line 4 is 43 cells and `controller ... switch off` would be 45, so the corrected spelling
does not fit the draw stream. Reading the rebuilt popup instead of the draw stream is exactly what
hides this — every glyph is stamped three times 2 px down, and the ghosting visually completes the
missing letters. Trust the index array.

as index arrays of 18, 27, 32 and 43 glyphs:

```
line 1  45 37 45 47 50 57  0 35 33 50 36  0 83 76 79 84  0 17
line 2  35 72 69 67 75 73 78 71  0 77 69 77 79 82 89  0 67 65 82 68  0  8 48 51 18  9 14
line 3  36 79  0 78 79 84  0 82 69 77 79 86 69  0 77 69 77 79 82 89  0 67 65 82 68  0  8 48 51 18  9 12
line 4  67 79 78 84 82 79 76 69 82 12  0 82 69 83 69 84 12  0 79 82  0 83 87 73 84 67 72  0 79 70  0 84 72 69  0 67 79 78 83 79 76 69 14
```

`0` is the space cell. The images that back this section are `/tmp/opencode/popup_text.png` (the popup
rebuilt from the draws) and `/tmp/opencode/atlas_index.png` (all 150 cells with their index).

**How it was extracted.** `PS2X_DRAWLOG_LIVE=0` captures the first 3000 emitted draws. `[dlg]` lines
carry `dx`/`dy` (screen rect) and `su`/`sv` (texel range in the atlas). Crop that texel range out of
the dumped atlas and paste it at the screen rect: the popup reassembles. The glyph UVs land exactly
on the 20-texel grid, which is what confirms the atlas grid and the index arithmetic at the same time.

## 5. Who initiates the memory-card conversation

`mcTrace` now prints the caller. The recomp pre-decodes the syscall, so by the time a stub runs the
guest has already entered the SDK's libc wrapper; `$ra` ($31) is the return address in the **caller
of that wrapper**, which is the game code that decided to touch the card.

The SDK wrappers sit together in libc:

| stub | `pc` |
|---|---|
| `sceMcOpen` | `0x002a1e58` |
| `sceMcRead` | `0x002a2208` |
| `sceMcSync` | `0x002a2498` |
| `sceMcGetInfo` | `0x002a25b8` |

and the game drives the whole conversation from **one contiguous function**:

```
GetInfo  ra=0x00116de4   <- first call, starts it
GetInfo  ra=0x00116c74   (x4, the retry)
Sync     ra=0x00116cdc
Open     ra=0x0011859c -> 0x118690 -> 0x118718 -> 0x1187b0
                    -> 0x1187e4 -> 0x118860 -> 0x1188f8
Open     ra=0x00117f2c   (the save file)
Read     ra=0x00117fe8   size=16384
```

The `ra` values walk forward in near-perfect order through `0x116c74`..`0x1188f8`, which is one
function executing its own lines, not a call tree. The save-load module is in `0x116xxx`.

The conversation itself completes normally:

```
GetInfo -> type=2 free=8192 format=1 result=0
Open    '/BASLUS-21678DBZT3/BASLUS-21678DBZT3'  -> cmd=2 result=4
Read    fd=4 dst=0x90f1f8 size=16384            -> cmd=5 result=16384
                                                cmd=3 result=0
```

`type=2` is a formatted card holding a save; the game also opens its own `icon.sys` and `dbzsm.ico`.

## 6. The bug: every glyph is drawn six times

2625 glyph draws for **744 distinct (position, cell) pairs**. Each glyph is emitted:

- at **three vertical offsets 2 px apart** — `y = 146, 148, 150` for line 1, `194/196/198` for line 2,
  `214/216/218`, `234/236/238` — and
- into **both** display buffers (`fbp112` ×1509, `fbp0` ×1116).

3 × 2 = 6. That is the ghosting: every letter is stamped three times 2 px down, which is what makes
the text look doubled in `popup_text.png`.

The popup box has the same defect and `[mcfade]` already flagged it: `body exec=12` is three
rectangles (top border, middle, bottom border) each executed four times, e.g. `y224-226 y224-226
y224-226 y224-226 | y231-233 ×4 | y226-231 ×4`. The box also animates open, `y204-212` → `y194-205`
→ `y173-192` → `y163-185` → `y153-178`, so the growth is the game and the repetition is not.

One root cause, two symptoms: stale draw commands are being re-executed. The draws are emitted once
(`s=10760 p=20 b=44 a=1 k=1 tx=16 512x128`) and then served repeatedly.

## 7. Replacing the text by rewriting the atlas

Because the game addresses the alphabet by index and knows nothing about the letters, **the text can
be replaced without touching the game, its data files, or the save format**: overwrite the shapes in
the atlas cells and the popup says something else.

The hook already exists. `ps2_texreplace.cpp` indexes `<exeDir>/data/Textures` (or
`PS2X_TEXREPLACE=<dir>`) by filename `<tex0hash>-<cluthash>-<bits>.png` and decodes it to **RGBA8,
alpha included** (`GsDecodeImageRGBA8` → `BT3_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8`). So the file to
drop in is:

```
<exeDir>/data/Textures/1dd4c76113969303-6dfa844c9490b8ed-00001e54.png
```

512 × 128, RGBA. The key is the hash **pair**; the third filename field is not part of the key, which
is deliberate — the same texture appears as `…-00001e53` and `…-00005e53` depending on whether TEXA
was included in the dump, and keying on the full name loads nothing from a working pack.

Rules the replacement has to respect, all measured:

1. **512 × 128 exactly.** 25 × 6 cells of 20 × 20. A different size is rejected or mis-sampled.
2. **The shape goes in alpha.** RGB is flat 128 in the original and the draw blends `Fs`/`Fd`
   (`bm=0x44`). Putting the letter in RGB with a flat alpha will draw a solid 20 × 20 block.
3. **Antialiasing lives in alpha too** — the original uses 4 alpha levels (0/125/193/249), so keep
   soft edges or the text will look harsher than the console's.
4. Cell `(row, col)` → index `row * 25 + col`. Index 0 is the space cell and must stay blank; the
   popup uses it as a word separator.
5. Only the cells the message actually indexes need to be right. The message reuses indices freely —
   line 2's trailing `9 14` and line 3's `9 12` differ in one glyph, so per-cell fidelity matters.

Practical consequence: to change the sentence, remap the ~36 distinct cells the four lines use and
leave the rest of the atlas untouched. `games/bt3/atlas_text.py` does this: it copies whole cells out
of the original atlas, so the replacement is the console's own rasterisation — same hinting, same 4
alpha levels, no font dependency, nothing to tune.

```
./atlas_text.py --pattern                      # the template a replacement has to fit
./atlas_text.py --atlas <dump> --message 'A|B|C|D' --preview out.png --out <Textures>/<id>.png
```

### The constraint that decides what text is possible

The game addresses the alphabet by **index**, so **a cell drawn twice can only ever show one letter** —
the second write simply overwrites the first. The 120 drawn cells are only ~36 distinct indices, and
27 of those are reused. `atlas_text.py --pattern` prints every collision:

```
cell  45 ('M')  line 1 pos 1, line 1 pos 3
cell  67 ('c')  line 2 pos 4, line 2 pos 17, line 3 pos 22, line 4 pos 1, line 4 pos 26, line 4 pos 36
cell  69 ('e')  line 2 pos 3, line 2 pos 11, line 3 pos 9, line 3 pos 13, line 3 pos 16, line 4 pos 8,
                line 4 pos 13, line 4 pos 15, line 4 pos 34, line 4 pos 42
cell  79 ('o')  12 positions
```

So the replacement is a **substitution, not a free rewrite**: wherever the original reuses a cell, the
replacement must reuse the same letter, and a position drawn from cell 0 (the space cell) must stay
blank — which also caps the line length at the run of non-space cells before the next space.

A positional remap without this check "works" and renders garbage. Writing `H,O,L,A` into positions
0..3 of line 1 puts `H` and `L` both in cell 45, which the game draws twice, and the popup reads
`H O H L`. The tool validates before writing and names every conflict:

```
line 1 pos 3: cell 45 is drawn at line 1 pos 1 and at line 1 pos 3: it would have to show
both 'H' and 'L'. One cell, one letter.
```

Two smaller rules, both measured:

- Line lengths are fixed at 18 / 27 / 32 / 43 cells. A longer line is not a rendering problem, it is
  a different message array, and we do not know where that array lives yet.
- If the index array's location in RDRAM is ever found, patching it would lift the reuse constraint
  and allow arbitrary text. The needle is the 0x20-subtracted byte sequence (§3).

## 8. Replacing the text: the draw-stream override

The atlas substitution above works, and it is enough for a cipher — but §7's constraint is
fatal for real text. Not because a cell *cannot* be blanked (it can: copy the space cell over it),
but because 23 of the 35 cells are drawn in more than one place, and a cell can hold only one glyph.
The mapping is therefore global across all four lines at once, and almost no new text survives it:

```
$ ./atlas_text.py --atlas <dump> --message 'MEMORY CARD slot 1|Wait what?|...' --auto
  cell 69 is drawn at line 2 pos 3, line 2 pos 11, line 3 pos 9, line 3 pos 13, line 3 pos 16
        and at line 4 pos 8: it would have to show both ' ' and 'd'. One cell, one letter.
  ... 8 more
```

`--auto` resolves the conflicts and produces `MEMORY CARD 1 / C a / D h /`. Proof that the
substitution path is exhausted, not that the text was badly chosen.

### Why no data patch can fix this

The index array is not in the ELF (searched as bytes, as `u16` little-endian, and as ASCII), and not
in any of the **68 826** files under the deploy's `data/` tree. It is not in RDRAM as ASCII either,
because the bytes are indices (§3). **The game generates it in code.** There is no table to patch,
so the text cannot be lengthened or re-worded by editing data.

### The override

`PS2X_MCTEXT='line 1|line 2|line 3|line 4'` rewrites the popup's text at draw time. This is the path
that carries arbitrary text; the atlas substitution above is kept only because it is the right tool
for a cipher-shaped replacement and documents the constraint that forced this design.

`DrawCmd` carries the atlas sample rect as texels — `su0, sv0, su1, sv1` — separately from the
screen rect `dx0, dy0, dx1, dy1`. Rewriting **the UV per draw** instead of overwriting the atlas
removes the sharing entirely: each draw becomes independent, so two slots that happen to sample the
same cell can show different letters. It also leaves the atlas alone, so nothing has to be regenerated
per language and `data/Textures` stays empty.

A popup glyph draw is identified without ambiguity, from the measurements in §2 and §4:

| field | value |
|---|---|
| `srcTbp0` | `10760` (`0x2A08`) |
| `srcPsm` | `20` (PSMCT32) |
| `srcTexW` × `srcTexH` | `512` × `128` |
| `abe` | `1` |

**Identifying the slot.** Within one line the game emits the slots in message order, and thanks to
the tripled draws (§6) each slot arrives as a **run of consecutive draws with the same index**. The
triplication multiplies the run's *length* — 9 per slot in one run, 6 in another — but never splits
it, so counting **runs rather than draws** is correct regardless of how the multiplier lands. The
run count modulo the 120-slot message is the flat slot index, and the measured index arrays above act
as a dictionary: when the observed index disagrees with the expected slot, the parser walks forward
to the next occurrence and adopts that, which makes it self-correcting.

An earlier version tried to identify the slot geometrically — the four rows sit at y = 146, 194, 214
and 234, each drawn at y, y+2 and y+4, so a row spans a 6 px band — and it was **worse**: the box
animates, those coordinates came from one run, and the bands did not hold. One line's letters landed
on another line's baseline. Run counting is the version that works; the geometric version is recorded
here only so it is not re-derived.

**Placing the text.** The obvious mapping — character *n* of the line to slot *n* — is right, and it
is the only rule needed. An intermediate version sorted slots into "letter" and "gap" buckets and let
the game's own word breaks place the replacement's spaces, which put the break for `Think` after `Th`
because line 3's original breaks after `Do`. The distinction is unnecessary: **any slot can show any
cell**, so a 1:1 walk is both simpler and correct, and the spaces land where *this* text has them.

Each line is then laid out from the **measured ink width** of every glyph (table in the source), a
pitch of the glyph's own width plus one pixel of tracking, a six pixel word gap at the replacement's
own spaces, centred on the 512 px display. The UV window stays the fixed 13×18 the game uses, so
glyphs keep the console's exact appearance instead of stretching. Slots past the end of a line, and
slots that would land on a space, sample cell 0 — the atlas's blank cell — and collapse on **both**
axes, because a zero-width source rect alone still leaves the screen rect the game gave it and a
degenerate sample rasterizes as a stray mark at the old x.

### Reading the message back

`mctext-msg` censors the message the game draws, and it has to be read **before** the UV is
rewritten — reading it afterwards returns our own cell and logs our replacement text back at us.

The three passes do **not** decode identically: the same frame has been observed as both `CHECKING`
and `,HECKING`, and `RESET` as `RESUT`. A cell is drawn once correctly and twice carrying a
neighbour's index, so any single pass is a coin flip per slot. **Majority vote per slot over the
passes** recovers the index array — the fixed thing — instead of trusting whichever pass got logged.
If the slot numbering were still wrong the disagreement would appear as a *stable per-slot split*
rather than as noise, which is what makes the vote diagnostic and not just a filter.

The fade, the box that animates open, and the Fs/Fd blend are all still the game's; only the glyph
each quad samples — and where it sits — is ours. Because the override places each draw itself, the
ghosting disappears from the text as a side effect: it *was* the repeated draws, and now each glyph is
emitted once. The box border still shows it, since that geometry is left alone.

## 9. Dead ends

Recorded so they are not re-tread.

- **`strings` on the ELF.** The first pass claimed 0 strings, which was a pipeline error, not a fact:
  `-n 6` piped through `grep` returned nothing while `-n 4` returns 6283. There are 1826 ASCII runs
  ≥ 5 chars. All 405 real phrases are SDK/CRI. The game's own text is not in the ELF.
- **The prompt string in RAM.** `PS2X_MCSTR` = 0 hits, and always will be: the bytes are indices, not
  characters (§3). Do not re-run it expecting a hit; if the indices' location is ever needed, the
  needle is the 0x20-subtracted sequence.
- **The prompt string in the data files.** Not in the seven paks the game reads
  (`resident_system`, `resident_battle_param`, `resident_effect`, `resident_chara_param`,
  `SE_System`, `Init_US`, `SaveLoad_US`), 0 hits in each. Not in the atlas either, which only ever
  holds single characters.
- **`SaveLoad_US.cpak` is compressed.** It looks like noise because it is a length-coded stream, not
  because the format is exotic. The game decompresses it through the CRI ADX stack with **zero**
  `E0xxxxx` errors, so decompression is not a problem. `CRI_ADXI.IRX` runs natively on the IOP
  (`base 0xdf000`, 305 recompiled functions in `ps2xRuntime/src/iop_native/cri_adxi/`), and the same
  stack is linked into the ELF. There is no public ADXI decoder and none is needed.
- **The `.pak`/`.cpak` container format.** Not publicly documented — the only tool that ever
  documented it, the CTE-Plugin, is marked "[Not released]". Do not attempt standalone decoding. The
  guest is the only parser that exists; read the content through the guest instead
  (`PS2X_FILEDUMP` writes the bytes the game actually reads, reassembled per file).
- **`sceFontKit`.** Never called. Measured three times, 0 hits. Stop looking.
- **The BIOS.** There is no BIOS ROM in the deploy and none is emulated. The MC prompt is not the
  firmware's; the game draws it.
- **`PS2X_GSMASK=1`.** Broke the whole game and did not fix the Evolution Z menu. Unrelated to this.

## 9. Tooling notes

Probes used, and the two mistakes worth not repeating.

- `PS2X_MCLOG=1 PS2X_MCLOGMAX=0` — the whole save conversation, now with `pc`/`ra`/`sp` per call.
- `PS2X_DRAWLOG=1 PS2X_DRAWLOG_LIVE=0` — the first 3000 emitted draws. **Use the counter, not
  `DRAWLOG_AT`**: the time window is consumed before the popup starts and reports nothing.
- `PS2X_TEXPNGD=<dir>` — complete texture dump, no cap, no dedupe. **F9 re-arms** and wipes the
  directory, which is how you scope a dump to one screen.
- `PS2X_FILEDUMP=<dir> PS2X_FILEDUMP_ONLY=<substr>` — the bytes the guest reads, per file.
- `PS2X_MCSTR=1` — scans RDRAM for the prompt words. Keep the windowed version.
- `PS2X_MCFADE=1` — per-frame census of the popup's fade and body draws. This is what surfaced the
  12-executes-3-rectangles repetition and the box animation.

**Logging must be a census, not a stream.** Seven probes printed unconditionally and one boot
produced 6470 of their lines against the ~1600 that answer a question — the 23-line memory-card
conversation was unreadable inside the flood. They are now keyed on a set of everything already
logged, so a thing that recurs prints once and is counted, with the totals reported at exit
(`events → distinct logged, repeats`) and `matched nothing` for a probe that never fired. Two traps
inside that: comparing against the *previous* line only kills consecutive repeats, and the
cross-module call targets come back in a rotating order, so it logged all 2486 of them; and a key must
not contain anything that only goes up (`pc`, `ra`, the DMA counters), or every window is "new".

**A zsh trap that invalidated a whole run's results:** inside a `\` continuation, a `#` starts a
comment that swallows the following lines. Sectioning a `setsid env \ …` with `## ---- headers ----`
left only the last 5 of 45 flags reaching the process, and every probe that was not one of those 5
reported "0 hits" because it was never armed. Flags go in an array now, with the comments outside the
command, and the launcher prints the flag count and the count that actually arrived in
`/proc/<pid>/environ`.
