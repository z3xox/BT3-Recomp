#!/usr/bin/env bash
# Runs inside the baseline container: configures, builds, and bundles the
# deploy stage. Invoked by tools/release/build.sh with:
#   $1 = repo source root (mounted read-only)
#   $2 = runner build dir  (mounted rw, persistent _deps/cache)
#   $3 = launcher build dir (mounted rw)
#   $4 = output dir         (mounted rw; receives stage/ and floor-report)
set -euo pipefail

SRC="$1"; RUNNER_BUILD="$2"; LAUNCH_BUILD="$3"; OUT="$4"
JOBS="${BT3_RELEASE_JOBS:-$(nproc)}"
export HOME="${HOME:-/w/runner}"

# ccache lives inside the persistent runner build dir, so it survives across
# build.sh runs (and is dropped together with --reuse-deps=off cleaning _deps).
# mold makes the 96 MB runner link near-instant.
export CCACHE_DIR="${CCACHE_DIR:-$RUNNER_BUILD/.ccache}"
export CCACHE_MAXSIZE=8G

log() { echo "== $*"; }

mkdir -p "$OUT/stage/lib" "$OUT/stage/savedata/BASLUS-21678DBZT3" "$OUT/stage/logs"

# ---- 0. generate the player tables from the ISO (optional) ------------------
# A fresh clone has no game code: games/bt3/setup.py --gen-only extracts the
# ISO, builds the recompiler, and installs runner + overlay sources into the
# (rw-mounted) source tree. PS2X_BUILD_DIR keeps recompiler objects inside the
# build workspace instead of the host's build/. Steps 1-6 only; the actual
# runner/launcher build below reuses those generated sources.
if [[ -n "${PS2X_ISO:-}" ]]; then
    log "generating runner + overlay sources from ISO"
    export PS2X_BUILD_DIR="$RUNNER_BUILD/gen"
    # Deterministic from-zero regeneration (nukes any stale cache/toolchain
    # leftovers from a previous half-run).
    rm -rf "$PS2X_BUILD_DIR"
    # The recompiler is built with g++: toml11's std::source_location detection
    # misfires under clang-14 (Ubuntu 22.04 broke the runner links clean). The
    # runner/launcher below keep clang (GCC 11 ICE on ps2_gs_memory.h).
    CC=gcc CXX=g++ python3 "$SRC/games/bt3/setup.py" "$PS2X_ISO" --gen-only --jobs "$JOBS"
fi

# GCC 11/12 ICE (tsubst_copy) on ps2_gs_memory.h class-type NTTPs; clang is
# the baseline container compiler (still glibc 2.35).
export CC=clang CXX=clang++

# ---- 1. runner (bt3-runner), mirrors setup.py's configure ---------------
if [[ ! -f "$RUNNER_BUILD/CMakeCache.txt" ]]; then
    log "configuring runner (Release)"
    cmake -S "$SRC" -B "$RUNNER_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
# (Re)apply fast-build plumbing so later rebuilds reuse ccache + mold.
cmake -S "$SRC" -B "$RUNNER_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=mold"
log "building ps2EntryRunner (-j$JOBS)"
cmake --build "$RUNNER_BUILD" --target ps2EntryRunner -j"$JOBS"

RUNNER="$RUNNER_BUILD/ps2xRuntime/ps2EntryRunner"
[[ -f "$RUNNER" ]] || { log "ERROR: no $RUNNER"; exit 1; }

# ---- 2. Qt launcher ---------------------------------------------------------
if [[ ! -f "$LAUNCH_BUILD/CMakeCache.txt" ]]; then
    log "configuring launcher (Qt6)"
    cmake -S "$SRC/ps2xRuntime/src/launcher" -B "$LAUNCH_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
cmake -S "$SRC/ps2xRuntime/src/launcher" -B "$LAUNCH_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=mold"
log "building Launcher (-j$JOBS)"
cmake --build "$LAUNCH_BUILD" -j"$JOBS"

LAUNCHER="$LAUNCH_BUILD/Launcher"
[[ -f "$LAUNCHER" ]] || { log "ERROR: no $LAUNCHER"; exit 1; }

# ---- 3. bundle shared libraries (transitive closure, minus C/C++ core) ------
BELL=$(cat <<'EOF'
libc.so.* libm.so.* libmvec.so.* libpthread.so.* libdl.so.* librt.so.*
libutil.so.* libresolv.so.* libanl.so.* libnss*.so.* libthread_db.so.*
libBrokenLocale.so.* libcrypt.so.* libnsl.so.* libstdc++.so.* libgcc_s.so.*
EOF
)
is_core() {
    local base="${1##*/}"
    for pat in $BELL; do [[ "$base" == $pat ]] && return 0; done
    return 1
}

declare -A COPIED
bundle_binary() {
    local binary="$1"
    [[ -e "$binary" ]] || return 0
    ldd "$binary" 2>/dev/null | awk '/=> \//{print $3} /^\//{print $1}' | sort -u |
        while read -r lib; do
            local base="${lib##*/}"
            [[ -z "$base" ]] && continue
            if is_core "$lib" || [[ "$base" == ld-linux* ]]; then
                continue
            fi
            local src="$(realpath -m "$lib" 2>/dev/null || echo "$lib")"
            local dst="$(realpath -m "$OUT/stage/lib/$base" 2>/dev/null || echo "$OUT/stage/lib/$base")"
            if [[ -f "$lib" && -z "${COPIED[$base]+_}" && "$src" != "$dst" ]]; then
                cp -L "$lib" "$OUT/stage/lib/$base"
                COPIED[$base]=1
            fi
        done
}

log "bundling runner libs"
bundle_binary "$RUNNER"
log "bundling launcher libs"
bundle_binary "$LAUNCHER"

# Qt platform plugins (dlopened, so ldd of Launcher does not list them).
# Locate the plugins dir: prefer an explicit qtprefix; otherwise scan the
# standard Debian/Ubuntu install roots.
QT_PLUGIN_DIR=""
if [[ -n "${QT_PLUGIN_PATH:-}" ]]; then
    QT_PLUGIN_DIR="$(dirname "$(dirname "$QT_PLUGIN_PATH")")"
fi
if [[ -z "$QT_PLUGIN_DIR" ]]; then
    for cand in /usr/lib/x86_64-linux-gnu/qt6/plugins /usr/lib/qt6/plugins; do
        if [[ -d "$cand/platforms" ]]; then QT_PLUGIN_DIR="$cand"; break; fi
    done
fi
if [[ -n "$QT_PLUGIN_DIR" && -d "$QT_PLUGIN_DIR/platforms" ]]; then
    log "bundling Qt platform plugins from $QT_PLUGIN_DIR"
    mkdir -p "$OUT/stage/lib/qt6/plugins/platforms"
    for plugin in "$QT_PLUGIN_DIR"/platforms/libqxcb.so "$QT_PLUGIN_DIR"/platforms/libqoffscreen.so; do
        [[ -f "$plugin" ]] || continue
        cp -L "$plugin" "$OUT/stage/lib/qt6/plugins/platforms/"
    done
    for plugin in "$OUT/stage"/lib/qt6/plugins/platforms/*.so; do
        [[ -e "$plugin" ]] && bundle_binary "$plugin"
    done
else
    log "WARNING: no Qt platforms/ plugin dir at $QT_PLUGIN_DIR"
fi

# ---- 4. stage layout ---------------------------------------------------------
cp -v "$RUNNER"   "$OUT/stage/bt3-runner" | sed 's/^/  /'
cp -v "$LAUNCHER" "$OUT/stage/Launcher"       | sed 's/^/  /'
if [[ -d "$SRC/ps2xRuntime/src/launcher/assets" ]]; then
    mkdir -p "$OUT/stage/assets"
    cp -rv "$SRC/ps2xRuntime/src/launcher/assets/." "$OUT/stage/assets/" | sed 's/^/  /'
fi

# Game data from the ISO extraction (--gen-only left it in games/bt3/work/).
# Keyed the same way as setup.py's deploy_tree() so the stage is playable.
WORK="$SRC/games/bt3/work"
if [[ -d "$WORK" ]]; then
    log "copying game data (work/ -> stage/data)"
    mkdir -p "$OUT/stage/data"
    for name in BIN DATA IRX SYSTEM.CNF; do
        [[ -e "$WORK/$name" ]] && cp -rv "$WORK/$name" "$OUT/stage/data/" | sed 's/^/  /'
    done
    [[ -f "$WORK/SLUS_216.78" ]] && cp -v "$WORK/SLUS_216.78" "$OUT/stage/data/" | sed 's/^/  /'
fi

chmod +x "$OUT/stage/bt3-runner" "$OUT/stage/Launcher"

log "stage assembled:"
du -sh "$OUT/stage"
find "$OUT/stage" -maxdepth 2 -type f | sed 's/^/  /' | head -40 || true
echo "  libs: $(find "$OUT/stage" -path "*/lib/*" -maxdepth 3 -type f | wc -l)"