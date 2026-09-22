"""Console output for setup.py -- a plain, line-oriented layout.

No full-screen TUI: that kept fighting the legacy Windows console (buffer vs window size, ANSI codes
counted as columns, HANDLE truncation, ESC[H/ESC[2J ignored). The information is printed as discrete
lines, which is stable everywhere:

    --------------------------------------------------------------
      BT3-Recomp
      build setup
    --------------------------------------------------------------
      [1/3] Platform ................. OK  (windows amd64)
      ...
      -- [2/4] dependencies (Windows) -----------------------------
      [1/9] Visual Studio Build Tools ... OK
      ...
      x FAILED: Ninja
          get it here : https://github.com/ninja-build/ninja/releases   (ninja-win.zip)
          drop it here: drag the file here, or copy it into build\\deps-inbox

Colours are optional (``--no-color`` / ``NO_COLOR``); box-drawing degrades to ASCII when the console
encoding is not UTF-8.
"""

from __future__ import annotations

import os
import re
import shutil
import sys

STATUS = {   # status -> label
    "ok": "OK", "running": "RUNNING", "fail": "FAILED", "miss": "MISSING",
    "skip": "SKIP", "wait": "waiting", "done": "done",
}

G_UTF8 = dict(tl="┌", tr="┐", bl="└", br="┘", h="─", v="│", full="█", empty="░", cross="✗", dot="·")
G_ASCII = dict(tl="+", tr="+", bl="+", br="+", h="-", v="|", full="#", empty="-", cross="x", dot=".")


class Caps:
    def __init__(self, force_plain: bool = False, color: bool | None = None):
        self.tty = bool(sys.stdout.isatty()) and not force_plain
        enc = (getattr(sys.stdout, "encoding", "") or "").lower()
        self.utf8 = "utf" in enc
        if color is None:
            color = self.tty and os.environ.get("NO_COLOR") is None
        self.color = bool(color)
        size = shutil.get_terminal_size((100, 30))
        self.width, self.height = size.columns, size.lines


class G:
    """Glyph set as attributes (box drawing in UTF-8, ASCII otherwise)."""

    def __init__(self, utf8: bool):
        self.__dict__.update(G_UTF8 if utf8 else G_ASCII)


def _strip(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", s)


def splash_box(title: str, lines: list[str]) -> None:
    """Print a boxed splash panel (the developer welcome / pre-stage)."""
    g = G("utf" in (getattr(sys.stdout, "encoding", "") or "").lower())
    inner = max([len(title)] + [len(l) for l in lines]) + 4
    rule = g.h * inner
    print(g.tl + rule + g.tr)
    print(g.v + title.center(inner) + g.v)
    print(g.tl + rule + g.tr)
    for l in lines:
        print(g.v + ("  " + l).ljust(inner) + g.v)
    print(g.bl + rule + g.br)


class View:
    """Plain console view. Writes discrete lines; the file log is owned by the caller."""

    def __init__(self, log=None, caps: Caps | None = None, level: int = 3):
        self.log = log
        self.caps = caps or Caps()
        self.g = G(self.caps.utf8)
        self.level = level
        self.tui = False   # kept for API compatibility: there is no full-screen mode any more
        self._head = False
        self.summary: list[tuple[str, str]] = []

    # -- primitives ------------------------------------------------------------------------------
    def _c(self, code: str, s: str) -> str:
        return f"{code}{s}\x1b[0m" if self.caps.color else s

    def _w(self, s: str) -> None:
        sys.stdout.write(s)
        sys.stdout.flush()

    @staticmethod
    def _field(name: str, label: str, width: int = 56) -> str:
        used = len(str(name))
        return f"{name} " + "." * max(1, width - used - len(label)) + f" {label}"

    # -- lifecycle -------------------------------------------------------------------------------
    def start(self) -> None:
        pass

    def close(self) -> None:
        pass

    def finish(self, lines: list[str]) -> None:
        for ln in lines:
            self._w(ln + "\n")

    def prompt(self, text: str) -> str:
        return input(text)

    def splash(self, lines: list[str]) -> None:
        for ln in lines:
            self._w(ln + "\n")

    # -- state -----------------------------------------------------------------------------------
    def header(self, title: str, subtitle: str = "") -> None:
        rule = self.g.h * 70
        self._w(f"{rule}\n  {title}\n  {subtitle}\n{rule}\n")

    def stage(self, n: int, total: int, name: str) -> None:
        self._w(f"\n-- [{n}/{total}] {name} " + self.g.h * max(0, 62 - len(name)) + "\n")

    def item(self, i: int, total: int, name: str, status: str = "ok", detail: str | None = None) -> None:
        lab = STATUS.get(status, status)
        line = f"  [{i}/{total}] " + self._field(name, lab)
        if detail:
            line += f"  ({detail})"
        self._w(line + "\n")

    def item_plain(self, name: str, status: str, detail: str | None = None) -> None:
        lab = STATUS.get(status, status)
        line = "  " + self._field(name, lab, 58)
        if detail:
            line += f"  ({detail})"
        self._w(line + "\n")

    def summary_row(self, k: str, v: str) -> None:
        self.summary.append((k, v))

    def progress(self, pct: int, label: str = "", eta: str = "") -> None:
        pass   # the plain layout shows per-step lines instead of a live bar

    def status(self, status: str, name: str | None = None) -> None:
        pass

    def note(self, line: str) -> None:
        self._w("  " + line + "\n")

    def actions(self, keys: str = "") -> None:
        pass

    def feed(self, line: str) -> None:
        pass   # no live bar to drive in plain mode

    def failed(self, name: str, url: str = "", filename: str = "", drop_hint: str = "") -> None:
        self._w(f"  {self.g.cross} FAILED: {name}\n")
        if url:
            self._w(f"      get it here : {url}" + (f"   ({filename})" if filename else "") + "\n")
        if drop_hint:
            self._w(f"      drop it here: {drop_hint}\n")
