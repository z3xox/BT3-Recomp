# ITEM SHOP — black screen, then a hang

Screen: **Main Menu → Evolution Z → Item Shop**, BT3 top-level state **`0x32`**.
Reproduces on **Ultimate Battle's submenu** too (`0xd → 0xe → 0xf`).

Entering it blacks out and the process spins. It is a **hang, not a crash**: no
signal is raised, no `SIGSEGV`/`SIGABRT` appears in `strace`, and the C++ stack
depth at the fault is 6 frames — no host stack overflow. The process just stops
making progress and burns a core.

## Cause: a mid-function entry point that was never registered

**The game has a second code region.** `DBZP.BIN` is a wrapped image mapped at
`0x334c00..0x3be71c`, immediately after the main ELF (which ends at `0x334bf8`).
`setup.log` records it:

```
wrapped DBZP.BIN: .text 0x334c00..0x3be71c
```

Every "impossible" target in this bug lands inside that region, not outside the
image as it first appears:

| target | screen | − 0x334c00 | in DBZP? |
|---|---|---|---|
| `0x39aaa0` | Item Shop | `0x65EA0` | yes |
| `0x39aae0` | Item Shop | `0x65EE0` | yes |
| `0x36fbc0` | Ultimate Battle | `0x37FC0` | yes |

The overlay table indexes them as `(addr - 0x334C00) / 4` — 97192 and 57328,
both in range. The functions are recompiled and present in
`ps2xRuntime/src/runner_overlay/overlay_functions.cpp`. Coverage is fine:
**707 DBZP functions recompiled, 153 gap-boundary stitches, 1 missing**
(`f_3376b8`).

Both targets are **mid-function entries** — valid game back-edges into the middle
of an overlay function, which is the failure mode `games/bt3/gen_overlay.py`
already documents and handles via `MID_FUNCTION_ENTRIES`:

| crash | function | span | offset in |
|---|---|---|---|
| `0x39aaa0` | `f_39aa20` | `0x39aa20 .. 0x39aae0` | +0x80 |
| `0x36fbc0` | `f_36fb48` | `0x36fb48 .. 0x36fc00` | +0x78 |

Only two mid-function entries are registered, from elsewhere in the binary:

```
registered mid-function entry 0x341358 -> f_3412e8_0x3412e8 (idx 12758)
registered mid-function entry 0x34c0b0 -> f_34bf60_0x34bf60 (idx 23852)
```

So the sequence is:

1. The game jumps to `0x39aaa0` / `0x36fbc0` — a loop back-edge, legitimate.
2. That address is in the **middle** of a recompiled function, not at its entry.
3. The dispatcher only resolves registered entry points, so it finds nothing.
4. `missingFunction` returns control, the guest retries the same PC, and the
   runtime spins. 268,165 iterations on Item Shop, 300,037 on Ultimate Battle.

`isCodeAddress()` also answers `no` for these addresses, because
`registerCodeRegion()` is called from exactly one place
(`ps2_runtime.cpp:1941`) and only for the main ELF's `PF_X` LOAD segments. That
is a second, independent gap: **the DBZP region is never registered as code
either.** Registering it would not fix the hang on its own, but the `codeRegion=no`
in the error message is misleading for anyone reading it.

`0x39aaa0` is `sll $a3, $s1, 5`, in the middle of a straight-line argument-setup
sequence — so entering there without the earlier setup is also semantically wrong,
which is what a loop back-edge into that point would look like if the loop body
were mis-split.

## The fix

**Applied and verified — both screens work.** Commit `e3493e7`.

```python
MID_FUNCTION_ENTRIES = (0x341358, 0x34c0b0, 0x39aaa0, 0x36fbc0)
```

in `games/bt3/gen_overlay.py`. The generator confirms all four with their table
indices:

```
registered mid-function entry 0x39aaa0 -> f_39aa20_0x39aa20 (idx 104360)
registered mid-function entry 0x36fbc0 -> f_36fb48_0x36fb48 (idx 60400)
```

Both new addresses already have their instruction line in
`overlay_functions.cpp` (`// 0x39aaa0: 0x113940` at line 300658, `// 0x36fbc0:
0x3c04003b` at line 176585), so the generator finds them and does not trip its
own guard:

```python
i = next((n for n, l in enumerate(lines) if instr.match(l)), None)
if i is None:
    raise SystemExit("gen_overlay: no instruction line for {lo}; ...")
```

This is the same mechanism that fixed `0x34c0b0` when it was hit live on
2026-09-05, and one list entry cleared both screens.

Note: `ps2xRuntime/src/runner_overlay/overlay_functions.cpp` is **generated** and
untracked — `setup.py` regenerates it from `gen_overlay.py` plus the three CSVs, so
the fix is only the one-line change to `MID_FUNCTION_ENTRIES`.

## What the symptom looks like

```
[bt3state] 0x32 UNKNOWN (was 0x30 EVOLUTION_Z)
[badjump] out-of-code target 0x39aaa0 ; last valid fn entered = 0x39aae0 ; prev = 0x10d8b0
[badjump] sp=0x1effe40  [sp-16..sp)= 0039ab14 00000000 0039ab04 00000000  tid=1 ra=0x39ab14
[badjump-bt] C++ stack frames=6
Error: No exact recompiled function for guest PC 0x39aaa0
  tableBase=0x100008 tableEnd=0x2bf69c codeRegion=no
```

That last line repeats **268,165 times** in one session.

`codeRegion=no` is the load-bearing part: `PS2Memory::isCodeAddress()` walks
`m_codeRegions`, which are registered **only from the ELF's `PF_X` LOAD segments**
(`ps2_runtime.cpp:1941`). The highest one ends at `0x2c33c0`, matching
`tableEnd=0x2bf69c`. So `0x39aaa0` is roughly 0xDD000 past the end of the last
LOAD segment — it is not code, there is nothing to look up, and the runtime falls
back to `missingFunction`, which returns control and lands in the same place again.

`codeRegion=no` is also the `first` symptom worth trusting: the runtime knows it
is out of bounds before anything else happens.

## The dispatch trace

`formatDispatchHistory()` (last 64 dispatches) shows two excursions into
`0x39xxxx`, and they behave differently:

```
...
43  0x10be0c     valid .text
44  0x39c77c     FIRST jump out of image
...
54  0x126628     guest RECOVERS, back into .text
55  0x100878
56  0x102208
57  0x1006e8
58  0x2ab238
59  0x2af0f0
60  0x2ab138
61  0x2aea38
62  0x10d8b0     valid .text
63  0x39aae0     SECOND excursion
64  0x39aaa0     and this one hangs
```

**The first bad jump is `0x10be0c` → `0x39c77c`, and the guest recovered from
it.** The 268k-iteration loop is the second excursion. The fault is therefore not
deterministic at the first occurrence; it depends on state.

### 0x10be0c is a mid-function entry

```
0x10bdf0: lw    $v0, 4($s1)
0x10bdf4: nop
0x10bdf8: move  $a0, $s0
0x10bdfc: daddu $a1, $s3, $zero
0x10be00: addiu $s0, $s0, 1
0x10be04: jal   0x0010c178
0x10be08: addu  $a0, $v0, $a0
0x10be0c: lhu   $v0, 22($s1)     <- the recorded dispatch PC
0x10be10: sltu  $v0, $s0, $v0
0x10be14: bne   $v0, $zero, 0x10bdf0
```

A clean counted loop. `0x10be0c` is the loop body, **not the entry point of any
function** — the dispatcher entered mid-function. That is precisely the failure
mode `games/bt3/gen_overlay.py` documents, and the one `MID_FUNCTION_ENTRIES`
exists to solve.

## Register state at the fault

```
ra=0039ab14   gp=00304270   sp=01effe40   at=30000000
s5=s6=s7=003c0000   a3=01010101   t2=80808080   t0=92a09c92
```

`ra` is invalid, and it equals `[sp-16]`. The stack holds
`0x000000000039ab14` and `0x000000000039ab04` sixteen bytes apart — two 64-bit
values, not saved return addresses. The game's own code uses 64-bit GPRs
(`daddu` in the prologue at `0x10d8b0`), so that is legitimate data in those slots.

## Refuted — do not re-tread

| theory | how it died |
|---|---|
| **the loader's fixup (`FUN_0010a028`) with a stale base overwrites a saved `$ra`** | the *symptom* was right — the address corruption is real — but the mechanism was wrong. The target sequence (`base`, `base+1`, `base+2`, `base+3`, `base+4`, `base+6`, doubled 64-bit reads) looked like a fixup walking a table. It is not: those are **the guest executing DBZP code**, and the "corrupted `$ra`" is a mid-function entry the dispatcher cannot resolve. The `[fixupguard]` widening below is therefore **not** the fix and was not applied. |
| the targets are outside the loaded image | they are outside the **main ELF**, but inside `DBZP.BIN` (`0x334c00..0x3be71c`). `readelf` on `SLUS_216.78` alone cannot show this; `setup.log`'s `wrapped DBZP.BIN` line does. |
| a 64-bit store overwrites a 32-bit saved `$ra` | **measured false.** `PS2X_BADSTORE=1` records every 64-bit guest store with the PC that issued it. All of them come from one PC and land in `0x12000000..0x120000d0` — a buffer, nowhere near the stack at `0x1effe40`. Zero stores touch the stack. |
| a register-indirect jump computed the bad target | the whole call tree — `0x10d8b0` plus its five callees `0x2a9acc`, `0x10ea48`, `0x10eb30`, `0x10f0b0`, `0x10f1e8` — contains **only** `jal` to fixed targets and `jr $ra`. No `jr $reg` anywhere. The target comes from a branch back-edge in DBZP code. |
| DVP overlay loaded at `0x334C00` (the third LOAD's `p_paddr`, and `gen_overlay.py`'s overlay base) | the address is right — it is the DBZP image base — but **my decoder was wrong**. `jal`'s target is `((PC+4) & 0xF0000000) \| (imm26 << 2)`; I had computed `(PC+4) + (imm26<<2)`, which turned `jal 0x002a9acc` into a bogus `0x3b73b0`. No DVP overlay is involved. |
| `sceSifSetDma:DTX_MISS_DUMP` | DTX is CRI's **audio** driver; its destinations are small RAM addresses, not VRAM. |
| menu jump to reach the screen | forces state from outside; the game's own transition never runs |

## About the `[fixupguard]` gap (real, but not this bug)

`[fixupguard]` (`game_overrides.cpp:3920`) rejects a fixup when

```cpp
cnt > 1024u || entBase < a1m || entBase - a1m > 0x200000u || (a1 & 0x1FFFFFFFu) >= 0x2000000u
```

Every condition is **relative to `a1`**, so a wildly out-of-image `a1` satisfies all
of them; the only absolute bound, `0x2000000`, is just the 32 MB of EE RAM. A
recorded call with `a1=0x99ea80` passes all four and executes.

That is a genuine hole worth closing on its own — but it is **not** what causes
this hang, and it was not applied. Recorded for the next time the loader misbehaves.

## A second screen does the same thing

Ultimate Battle's submenu (`0xd → 0xe → 0xf`) reproduces the hang, and the two
crashes are **not** the same jump:

| | Item Shop | Ultimate Battle submenu |
|---|---|---|
| out-of-code target | `0x39aaa0` | `0x36fbc0` |
| last valid fn | `0x39aae0` (invalid) | `0x10f0b0` (**valid** `.text`) |
| prev | `0x10d8b0` | `0x25e190` |

Both targets are past the end of the image. `0x10f0b0` is one of the five callees
already found in the Item Shop call tree, so the two crashes share a function even
though the bad target differs. `entOff=0x20` is constant across all six recorded
fixup calls on both screens.

That is consistent with one defect reached by two paths: the loader runs, the
base is stale, and where the resulting writes land differs per screen.

## Still open

**`0x10be0c`** appears as a dispatch target in the trace and is also a mid-function
address — the body of the counted loop at `0x10bdf0..0x10be14`. It resolved
normally here, so it may be benign, but it is the same class of address and worth
adding to the same list if the first attempt does not clear both screens.

**Registering the DBZP code region.** `0x334c00..0x3be71c` is still not passed to
`registerCodeRegion()`, so `isCodeAddress()` answers `no` for it and the error
message blames the wrong thing. Not required for the hang, but it removes a
red herring for whoever reads the next one.

## Tooling added (uncommitted)

| flag | what it does |
|---|---|
| `PS2X_BADSTORE=1` | ring buffer of the last 64 x 64-bit guest stores, each tagged with the guest PC that issued it; printed at the bad jump by `ps2xDumpBadStores()` |
| `[badregs]` | the full 32-register file at the first eight bad jumps. The out-of-code target can only come from a register, so this is what identifies the instruction at fault |

Pre-existing and directly relevant: `[fixupring]` (last 16 fixup calls),
`[fixupprobe]` (first 40, with header words), `[fixupguard]` (the reject path),
`PS2X_FIXUPGUARD=0` to disable it. `[badjump]` prints the out-of-code target, the
last valid dispatch, the stack slots, and the C++ frame count.

Note: the `pc=` in `[badstore]` is the most recent **branch** PC, published from
`dispatchGuestBranch`, not the store instruction itself. It narrows the caller,
not the faulting instruction.
