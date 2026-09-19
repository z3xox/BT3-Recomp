# Contributors & roles

BT3-Recomp — a statically recompiled, native PC port of *Dragon Ball Z: Budokai
Tenkaichi 3* (PS2, USA, SLUS-21678), built on
[PS2Recomp](https://github.com/ran-j/PS2Recomp).

## Developers

| Dev | Role | Areas |
| --- | --- | --- |
| **z3xox** | Owner / Lead developer | Recompiler (`ps2xRecomp`), runtime EE/GS/VU1/scheduler, OpenGL + paraLLEl-GS renderer, game overrides, game generators, docs |
| **RexxColder** | **Support** / Collaborator | **Optimization** (perf/async, batching), Qt6 launcher + install wizard + ISO9660, input & gamepads, build/release (floor gate, packaging), deploy layout, game data (AFS/AFL), docs |
| **valenvivaldi** | Collaborator | macOS arm64 port, packaging, audio |

## Third-party

| Author | Contribution | License |
| --- | --- | --- |
| **ran-j** | [PS2Recomp](https://github.com/ran-j/PS2Recomp) — static recompiler (upstream) | GPL-3.0 |
| **ViveTheModder** | NTSC-U AFS file lists (`PZS3US1.AFL`/`PZS3US2.AFL`) | Apache-2.0 |
| **Arntzen Software** | [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) — GS in Vulkan compute | LGPL-3.0-or-later |

## License

This repository is GPL-3.0 (see `LICENSE`). *Dragon Ball Z: Budokai Tenkaichi 3*
© Spike / Bandai Namco. This project is not affiliated with or endorsed by them;
it distributes no game content — the game is recompiled at build time from the
user's own disc image.
