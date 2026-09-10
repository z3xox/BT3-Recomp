#!/usr/bin/env bash
# Build BT3-Recomp from the USA ISO and assemble the final deployable tree.
#
#   ./build_and_deploy.sh                          # interactive (asks ISO + output)
#   ./build_and_deploy.sh --iso PATH --output DIR  # non-interactive
#   ./build_and_deploy.sh --skip-setup --output DIR  # reuse work/, just rebuild + deploy
#
# Deploys a runnable portable tree into DIR/, ready to play. No self-extracting
# installer: run `install game.sh` inside the folder to install the game on the
# host (copies it to ~/.local/share/bt3-recomp and registers a launcher entry).
#   DIR/  Launcher   bt3-runner   data/   savedata/   assets/   logs/   install game.sh
set -euo pipefail

# ---- config ---------------------------------------------------------------------
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO_DEFAULT="/home/rexx/Descargas/Roms/PS2/DragonBall Z - Budokai Tenkaichi 3.iso"
JOBS="${BT3_JOBS:-$(nproc)}"
GAME="Dragon Ball - Budokai Tenkaichi 3"

# ---- arg parsing ----------------------------------------------------------------
ISO=""
OUT=""
SKIP_SETUP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iso)   ISO="$2"; shift 2 ;;
        --output) OUT="$2"; shift 2 ;;
        --skip-setup) SKIP_SETUP=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ "$OUT" == "" ]]; then
    read -r -p "Deploy output directory: " OUT
fi
if [[ "$OUT" == "" ]]; then
    echo "ERROR: no output directory given" >&2; exit 2
fi
OUT="$(realpath -m "$OUT")"

# ---- build ----------------------------------------------------------------------
if [[ "$SKIP_SETUP" == "1" ]]; then
    python3 "$ROOT/games/bt3/setup.py" --skip-setup --deploy "$OUT" --jobs "$JOBS"
else
    if [[ "$ISO" == "" ]]; then
        read -r -p "BT3 ISO path [$ISO_DEFAULT]: " ISO
        ISO="${ISO:-$ISO_DEFAULT}"
    fi
    if [[ ! -f "$ISO" ]]; then
        echo "ERROR: ISO not found: $ISO" >&2; exit 2
    fi
    python3 "$ROOT/games/bt3/setup.py" "$ISO" --deploy "$OUT" --jobs "$JOBS"
fi

RUNNER="$OUT/ps2EntryRunner"
if [[ ! -f "$RUNNER" ]]; then
    echo "ERROR: build/deploy did not produce $RUNNER" >&2; exit 2
fi

# ---- rename runner to what the launcher expects ----------------------------------
# The launcher boots "$appDir/bt3-runner" directly (plain-runner mode); the
# tarball built by tools/release/package.sh uses the same name.
cp -v "$RUNNER" "$OUT/bt3-runner" | sed 's/^/  /'
rm -f "$OUT/ps2EntryRunner"

# ---- bundle the runner's shared libraries into OUT/lib ----------------------------
echo "== bundling shared libs"
mkdir -p "$OUT/lib"
mapfile -t LIBS < <(ldd "$RUNNER" 2>/dev/null |
    awk '/=> \//{print $3} /^\//{print $1}' | sort -u)
count=0
for lib in "${LIBS[@]}"; do
    base="$(basename "$lib")"
    if [[ "$base" == ld-linux* ]]; then
        continue  # the kernel maps the interpreter itself; not needed in lib/
    fi
    # NEVER bundle the C/C++ runtime core (glibc/libstdc++): forcing the
    # build-host glibc onto an arbitrary target causes GLIBC_PRIVATE symbol
    # clashes at load. The target system provides these; the runner's minimum
    # glibc floor still applies (build on an older baseline to widen it).
    case "$base" in
        libc.so.*|libm.so.*|libmvec.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libutil.so.*|libresolv.so.*|libnss*.so.*|libanl.so.*|libthread_db.so.*|libBrokenLocale.so.*|libcrypt.so.*|libnsl.so.*|libstdc++.so.*|libgcc_s.so.*)
            continue ;;
    esac
    if [[ -f "$lib" && ! -f "$OUT/lib/$base" ]]; then
        cp "$lib" "$OUT/lib/$base"
        count=$((count + 1))
    fi
done
echo "  collected $count shared libs"

# ---- Qt launcher (bt3-launcher) ---------------------------------------------------
echo "== building Qt launcher"
LAUNCH_DIR="$ROOT/ps2xRuntime/src/launcher"
LAUNCH_BUILD="$LAUNCH_DIR/build"
if [[ -f /usr/lib/cmake/Qt6/Qt6Config.cmake ]]; then
    cmake -S "$LAUNCH_DIR" -B "$LAUNCH_BUILD" -DCMAKE_BUILD_TYPE=Release > "$OUT/launcher_cmake.log" 2>&1
    cmake --build "$LAUNCH_BUILD" -j"$JOBS" >> "$OUT/launcher_cmake.log" 2>&1
    cp -v "$LAUNCH_BUILD/Launcher" "$OUT/Launcher" | sed 's/^/  /'
    chmod +x "$OUT/Launcher"
    cp -rv "$LAUNCH_BUILD/assets" "$OUT/" 2>/dev/null | sed 's/^/  /'
    if [[ -f "$LAUNCH_DIR/assets/background.png" ]]; then
        cp -v "$LAUNCH_DIR/assets/background.png" "$OUT/assets/" | sed 's/^/  /'
    fi
else
    echo "  (Qt6 not found -- skipping launcher)"
fi

mkdir -p "$OUT/logs"
if [[ ! -d "$OUT/savedata/BASLUS-21678DBZT3" ]]; then
    mkdir -p "$OUT/savedata/BASLUS-21678DBZT3"
fi

# ---- portable installer script ---------------------------------------------------
cp "$ROOT/tools/release/install-game.sh.in" "$OUT/install game.sh"
chmod +x "$OUT/install game.sh"

echo
echo "Deploy ready:"
echo "  $OUT/Launcher"
echo "  $OUT/data   $OUT/savedata   $OUT/assets   $OUT/logs"
echo "Portable:  \"$OUT/Launcher\"  (or ./install game.sh to install on the host)"