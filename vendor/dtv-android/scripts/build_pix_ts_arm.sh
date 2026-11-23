#!/bin/bash
#
# build_pix_ts_arm.sh - build pix-ts-legacy (PIX DT-295) for the STB.
#
# The DT-295 support lives in the reference implementation (pix-userland). This
# script builds the legacy CLI for Android/armv7a with the on-board transcoder
# hooks enabled and applies the one patch Android needs (there is no writable
# /tmp, so the recording lock picks TMPDIR or /data/local/tmp instead).
#
# Usage:
#   PIX_USERLAND_DIR=~/work/pix-userland ./scripts/build_pix_ts_arm.sh
#
# Environment:
#   PIX_USERLAND_DIR  checkout of the dt295/transcode-hooks branch (required)
#   ANDROID_NDK_HOME  NDK root (default: the SDK copy below)
#   OUT               output directory (default: bin)
#
set -eu

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIX_DIR=${PIX_USERLAND_DIR:?set PIX_USERLAND_DIR to the pix-userland checkout}
NDK=${ANDROID_NDK_HOME:-/Users/kenichim/Library/Android/sdk/ndk/27.0.12077973}
OUT=${OUT:-$ROOT_DIR/bin}
API=${ANDROID_API:-24}

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin"
CC="$TOOLCHAIN/armv7a-linux-androideabi$API-clang"
[ -x "$CC" ] || { echo "Android compiler missing: $CC" >&2; exit 1; }
# libusb's configure calls ar/ranlib by name, so the NDK tools must be on PATH.
export PATH="$TOOLCHAIN:$PATH"

WORK=${WORK:-/tmp/pix-ts-build}
mkdir -p "$WORK/prefix" "$OUT"

# libusb is linked statically; the release tarball is only fetched once.
if [ ! -f "$WORK/prefix/lib/libusb-1.0.a" ]; then
    if [ ! -d "$WORK/libusb-1.0.30" ]; then
        ( cd "$WORK" && curl -sL -o libusb.tar.bz2 \
            https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2 \
            && tar -xjf libusb.tar.bz2 )
    fi
    ( cd "$WORK/libusb-1.0.30" \
        && CC="$CC" ./configure --host=arm-linux-androideabi --prefix="$WORK/prefix" \
               --disable-udev --disable-shared --enable-static AR=llvm-ar RANLIB=llvm-ranlib \
        && make -j4 && make install )
fi

# The patch is idempotent: it is skipped once the source already carries it.
if ! grep -q 'lock_dirs\[\]' "$PIX_DIR/src/dt295_capture.c"; then
    ( cd "$PIX_DIR" && patch -p1 < "$ROOT_DIR/tools/patches/pix-userland-android-tmpdir.patch" )
fi

( cd "$PIX_DIR" && make clean >/dev/null 2>&1 || true
  make -j"${JOBS:-6}" pix-ts-legacy CC="$CC" PKG_CONFIG=false \
      USB_CFLAGS="-I$WORK/prefix/include/libusb-1.0" \
      USB_LIBS="$WORK/prefix/lib/libusb-1.0.a -llog" \
      CFLAGS="-O2 -g -DDT295_CAPTURE_TESTING" )

install -m 755 "$PIX_DIR/pix-ts-legacy" "$OUT/pix-ts-legacy"
ls -lh "$OUT/pix-ts-legacy"
file "$OUT/pix-ts-legacy"
