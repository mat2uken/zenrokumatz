#!/bin/sh
# apply-vendor-patches.sh - vendor/pix-userland にローカルのパッチを当てる
#
#   scripts/apply-vendor-patches.sh <pix-userland のコピー先>
#
# ベンダー固定コミット (sources.lock.json) をそのまま保ち、変更は
# board/x96max/patches/pix-userland/ のパッチだけで持つ。ビルド
# (package/pix-tuner/pix-tuner.mk) とホストテストが同じ手順を使う。
#
# 何度実行してもよい。当たっていれば飛ばし、当たらなければ失敗する。
set -eu

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
    echo "usage: $0 <pix-userland-directory>" >&2
    exit 2
fi
target=$1
patches=$(cd "$(dirname "$0")/.." && pwd)/board/x96max/patches/pix-userland

for patch_file in "$patches"/*.patch; do
    [ -f "$patch_file" ] || continue
    if patch -p1 -N --dry-run -d "$target" -i "$patch_file" >/dev/null 2>&1; then
        echo "applying $(basename "$patch_file")"
        patch -p1 -N -d "$target" -i "$patch_file"
    elif patch -p1 -N -R --dry-run -d "$target" -i "$patch_file" >/dev/null 2>&1; then
        echo "already applied: $(basename "$patch_file")"
    else
        echo "does not apply: $patch_file" >&2
        exit 1
    fi
done
