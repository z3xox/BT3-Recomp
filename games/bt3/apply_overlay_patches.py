#!/usr/bin/env python3
"""Post-generation source patches for the BT3 overlay tree (DBZP.BIN → overlay_functions.cpp).

Run after gen_overlay.py regenerates ps2xRuntime/src/runner_overlay/:

    apply_overlay_patches.py <ps2xRuntime_dir>

Same idea as apply_patches.py, but targets the overlay tree, which gen_overlay.py
writes to directly (ps2xRuntime/src/runner_overlay/) and which apply_patches.py never
sees (it only patches the `runner` tree from the main config.toml recomp pass — see
setup.py step 5 vs. step "generating overlay sources from BIN/DBZP.BIN"). Without this
step, any hand-edit to overlay_functions.cpp is silently discarded the next time
gen_overlay.py runs (it's gitignored/generated) — see modding-docs/lessons.md, Lección 17.

Patches are idempotent (marker string checked first) and the script fails loudly if an
anchor is missing, since that means DBZP.BIN or the generator changed and the patch
needs review.
"""
import sys
from pathlib import Path

PATCHES = [
    {
        # Need <cstdio>/<cstdlib> for std::fprintf/std::getenv used by the patches below.
        # Not already included in this generated TU (unlike the `runner` tree files,
        # which get per-patch extra_include via apply_patches.py).
        "file": "overlay_functions.cpp",
        "marker": "#include <cstdio>\n#include <cstdlib>\n",
        "anchor": '#include <stdexcept>\n#include "ps2_overlay_functions.h"',
        "replacement": (
            '#include <stdexcept>\n#include <cstdio>\n#include <cstdlib>\n'
            '#include "ps2_overlay_functions.h"'
        ),
    },
    {
        # [bt3 patch: reveal-hidden-entry] The main-menu entry builder (overlay fn at
        # PS2 addr 0x334ca0, loop at label_335578) skips whichever entry index equals
        # $t0. $t0 is hardcoded to 4 at 0x335568, which hides the "Network Battle" plate
        # (a ghost menu with no backend — modding-docs/lessons.md, Lecciones 13-14).
        #
        # This is ON by default (user chose to test it live): $t0 becomes unreachable
        # (0xFF) so the beql at label_335578 never matches -> no entry is skipped -> the
        # "Network Battle" ghost plate renders. Equivalent to the verified PCSX2 cheat
        # `00335568 000000FF`. Can be turned off per-run with
        # PS2X_REVEAL_HIDDEN_MENU_ENTRY=0 (or n/N).
        "file": "overlay_functions.cpp",
        "marker": "[bt3 patch: reveal-hidden-entry]",
        "anchor": (
            "    // 0x335568: 0x24080004  addiu       $t0, $zero, 0x4\n"
            "    ctx->pc = 0x335568u;\n"
            "    SET_GPR_S32(ctx, 8, (int32_t)ADD32(GPR_U32(ctx, 0), 4));"
        ),
        "replacement": (
"    // 0x335568: 0x24080004  addiu       $t0, $zero, 0x4\n"
            "    // [bt3 patch: reveal-hidden-entry] Original skip-index=4 hides the \"Network Battle\"\n"
            "    // main-menu entry (loop below skips $s1==$t0). Default ON per user request:\n"
            "    // $t0 becomes unreachable (0xFF), so the beql at label_335578 never matches and\n"
            "    // the ghost \"Network Battle\" plate renders (equiv. to the verified PCSX2 cheat\n"
            "    // 00335568 000000FF, just at the source level, reversible with the env var below).\n"
            "    ctx->pc = 0x335568u;\n"
            "    {\n"
            "        static const bool s_revealHidden = [](){\n"
            '            const char *v = std::getenv("PS2X_REVEAL_HIDDEN_MENU_ENTRY");\n'
            "            return !(v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N'));\n"
            "        }();\n"
            "        static bool s_loggedOnce = false;\n"
            "        if (s_revealHidden && !s_loggedOnce) {\n"
            "            s_loggedOnce = true;\n"
            '            std::fprintf(stderr, "[reveal-hidden-entry] ARMED: skip-index disabled, entry 4 "\n'
            '                                  "(\\"Network Battle\\") will render. Its AFS texture/text data "\n'
            "                                  \"were never confirmed to exist -- watch for \"\n"
            "                                  \"'[sceCdRead] unresolved request' or '[reveal-hidden-entry] \"\n"
            '                                  "entry4 *\' in this log while the main menu is on screen.\\n");\n'
            "        }\n"
            "        SET_GPR_S32(ctx, 8, (int32_t)ADD32(GPR_U32(ctx, 0), s_revealHidden ? 0xFFu : 4u));\n"
            "    }"
        ),
    },
    {
        # [bt3 patch: reveal-hidden-entry-log] Diagnostic for the concern "what if the
        # revealed entry's AFS data is missing and it breaks the game": logs the first
        # time each entry index reaches the plate-build step without being skipped, so a
        # PS2X_MENUHEX=1 sceCdRead trace can be correlated against this specific loop
        # iteration, and the existing generic "[sceCdRead] unresolved request" logger
        # (CD.cpp) can be matched against the right entry index. Entry idx=4 only ever
        # reaches this point when PS2X_REVEAL_HIDDEN_MENU_ENTRY=1 is set.
        "file": "overlay_functions.cpp",
        "marker": "[bt3 patch: reveal-hidden-entry-log]",
        "anchor": (
            "            ctx->pc = 0x3355C0u;\n"
            "            goto label_3355c0;\n"
            "        }\n"
            "    }\n"
            "    ctx->pc = 0x335580u;"
        ),
        "replacement": (
            "            ctx->pc = 0x3355C0u;\n"
            "            goto label_3355c0;\n"
            "        }\n"
            "        // [bt3 patch: reveal-hidden-entry-log] Not skipped this iteration. Log once per\n"
            "        // entry-index the first time it reaches here without being skipped, so a\n"
            "        // PS2X_MENUHEX=1 trace of surrounding sceCdRead calls can be correlated against\n"
            "        // this specific loop iteration. entry index 4 only reaches this point at all\n"
            "        // when PS2X_REVEAL_HIDDEN_MENU_ENTRY=1 is set (see the $t0 patch above).\n"
            "        {\n"
            "            static bool s_seen[16] = {};\n"
            "            const uint32_t idx = GPR_U32(ctx, 17);\n"
            "            if (idx < 16 && !s_seen[idx]) {\n"
            "                s_seen[idx] = true;\n"
            '                std::fprintf(stderr, "[reveal-hidden-entry] entry idx=%u reached plate-build "\n'
            '                                      "(not skipped). If this is idx=4 and its texture/text is "\n'
            '                                      "actually missing in PZS3US1.AFS, expect a "\n'
            "                                      \"'[sceCdRead] unresolved request' shortly after in this \"\n"
            '                                      "log (run with PS2X_MENUHEX=1 for a full read trace).\\n",\n'
            "                                      idx);\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "    ctx->pc = 0x335580u;"
        ),
    },
]


def apply(runtime_dir: Path) -> int:
    failures = 0
    overlay_dir = runtime_dir / "src" / "runner_overlay"
    for patch in PATCHES:
        path = overlay_dir / patch["file"]
        if not path.is_file():
            print(f"ERROR: {path} not found", file=sys.stderr)
            failures += 1
            continue
        text = path.read_text()
        if patch["marker"] in text:
            print(f"skip (already patched): {patch['file']} ({patch['marker'].strip()!r})")
            continue
        anchor = patch["anchor"]
        idx = text.find(anchor)
        if idx < 0:
            print(f"ERROR: anchor not found for patch {patch['marker']!r} in {patch['file']}", file=sys.stderr)
            failures += 1
            continue
        text = text[:idx] + patch["replacement"] + text[idx + len(anchor):]
        path.write_text(text)
        print(f"patched: {patch['file']} ({patch['marker'].strip()})")
    return failures


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(1 if apply(Path(sys.argv[1])) else 0)
