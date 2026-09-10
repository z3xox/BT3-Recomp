#!/usr/bin/env bash
# End-user packaging from a build/release/out/stage tree (see build.sh).
#
# Produces, in the image's output dir:
#   BT3-Recomp-x86_64.tar.gz             portable folder + install game.sh
#   BT3-Recomp-x86_64.sha256            sha256 of the tarball
#
# The game ships as a plain portable folder: run `install game.sh` to copy it
# into ~/.local/share/bt3-recomp and register a launcher entry (with icon), or
# run ./Launcher directly from the folder. No self-extracting installer.
#
# Layout assumptions on stage/ (produced by tools/release/entrypoint.sh):
#   Launcher            Qt launcher
#   bt3-runner      the game runner
#   lib/                bundled shared libs (incl. lib/qt6/plugins)
#   data/               game data (BIN/ DATA/ IRX/ SYSTEM.CNF + SLUS_216.78)
#   assets/             sky theme + fonts
#   savedata/BASLUS-21678DBZT3/   placeholder created by the wizard later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RELEASE="$ROOT/build/release"
STAGE="$RELEASE/out/stage"
OUT_DIR="$RELEASE/out"
TREE_NAME="Dragon Ball Budokai Tenkaichi 3 Recompiled"

[[ -d "$STAGE" ]] || { echo "ERROR: $STAGE missing (run tools/release/build.sh first)"; exit 2; }
[[ -x "$STAGE/Launcher" && -x "$STAGE/bt3-runner" && -d "$STAGE/lib" ]] || {
    echo "ERROR: stage incomplete"; exit 2; }

# ---- portable tree ---------------------------------------------------------
TARBALL="$OUT_DIR/BT3-Recomp-x86_64.tar.gz"
TMP_TREE="$OUT_DIR/$TREE_NAME"
rm -rf "$TMP_TREE"; mkdir -p "$TMP_TREE"
cp -a "$STAGE"/Launcher "$STAGE"/bt3-runner "$TMP_TREE"/
cp -a "$STAGE"/lib "$STAGE"/assets "$TMP_TREE"/
[[ -d "$STAGE/data" ]] && cp -a "$STAGE"/data "$TMP_TREE"/
[[ -d "$STAGE/textures" ]] && cp -a "$STAGE"/textures "$TMP_TREE"/
mkdir -p "$TMP_TREE/savedata/BASLUS-21678DBZT3"

cp "$ROOT/tools/release/install-game.sh.in" "$TMP_TREE/install game.sh"
chmod +x "$TMP_TREE/install game.sh"

tar -C "$OUT_DIR" -czf "$TARBALL" "$TREE_NAME/"
rm -rf "$TMP_TREE"

# ---- checksums -------------------------------------------------------------
(
    cd "$OUT_DIR"
    sha256sum "BT3-Recomp-x86_64.tar.gz" > "BT3-Recomp-x86_64.sha256"
)

echo
echo "Release artifact:"
ls -lh "$TARBALL" "$OUT_DIR/BT3-Recomp-x86_64.sha256"