#!/bin/bash
# test-dt295-host.sh - DT295 のホストテスト (実機不要)
#
#   scripts/test-dt295-host.sh            # すべて
#   scripts/test-dt295-host.sh recovery   # firmware 復旧のテストだけ
#   scripts/test-dt295-host.sh logo       # 局ロゴ抽出のテストだけ
#
# ベンダー固定コミットにパッチを当てたコピーを作り、dt295_io を差し替えた
# モックデバイスで動かす。実機の DT295 は使わない。
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$root/work/dt295-host
only=${1:-all}
cc=${CC:-cc}

rm -rf "$work"
mkdir -p "$work"
cp -R "$root/vendor/pix-userland" "$work/pix-userland"
"$root/scripts/apply-vendor-patches.sh" "$work/pix-userland"

common_flags=(-std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter
              -D_POSIX_C_SOURCE=200809L
              -I"$work/pix-userland/include" -I"$work/pix-userland/src")
core=("$work/pix-userland/src/dt295.c" "$work/pix-userland/src/dt295_crypto.c"
      "$work/pix-userland/src/crypto.c" "$work/pix-userland/src/transport.c"
      "$work/pix-userland/src/dt295_routes.c" "$work/pix-userland/src/platform.c")

rc=0
if [[ $only == all || $only == recovery ]]; then
    echo "--- recovery"
    "$cc" "${common_flags[@]}" -DDT295_TESTING \
        "$root/tests/test_dt295_recovery.c" "${core[@]}" \
        -o "$work/test_recovery" -lm -lpthread
    "$work/test_recovery" "$work/pix-userland/firmware/dt295.bin" || rc=1
fi

if [[ $only == all || $only == logo ]]; then
    echo "--- logo"
    logo_src=("$root/package/pix-tuner/logo/ts_section.c"
              "$root/package/pix-tuner/logo/logo_carousel.c"
              "$root/package/pix-tuner/logo/logo_cdt.c"
              "$root/package/pix-tuner/logo/logo_store.c")
    "$cc" "${common_flags[@]}" \
        -I"$root/package/pix-tuner/logo" -I"$root/package/stb-streamer/src" \
        "$root/tests/test_dt295_logo.c" "${logo_src[@]}" \
        "$root/package/stb-streamer/src/arib_text.c" \
        -o "$work/test_logo" || rc=1
    "$work/test_logo" "$work/logo-sample.ts" "$work/logo-dummy.ts" || rc=1

    # The same TS through the real binary: the files the NAS will fetch.
    "$cc" "${common_flags[@]}" \
        -I"$root/package/pix-tuner/logo" -I"$root/package/stb-streamer/src" \
        "$root/package/pix-tuner/logo/pix_dt295_logo.c" "${logo_src[@]}" \
        "$root/package/stb-streamer/src/arib_text.c" \
        -o "$work/pix-dt295-logo" || rc=1
    rm -rf "$work/logos"
    "$work/pix-dt295-logo" --out "$work/logos" --wait 5 --system bs --tsid 0x40f1 \
        < "$work/logo-sample.ts" || rc=1
    for expected in 400211.png 400212.png index.json; do
        if [[ ! -s "$work/logos/$expected" ]]; then
            echo "missing output: $expected" >&2
            rc=1
        fi
    done
    if ! grep -q '"id": 400211' "$work/logos/index.json"; then
        echo "index.json does not list the service" >&2
        rc=1
    fi

    # Only a transparent placeholder: nothing is written, and the exit code
    # tells the fetch script to try again later.
    rm -rf "$work/logos-dummy"
    if "$work/pix-dt295-logo" --out "$work/logos-dummy" --wait 5 --quiet \
        < "$work/logo-dummy.ts"; then
        echo "transparent placeholder was accepted" >&2
        rc=1
    else
        status=$?
        if [[ $status -ne 3 ]]; then
            echo "expected exit 3 for no logo data, got $status" >&2
            rc=1
        fi
    fi
    if compgen -G "$work/logos-dummy/*.png" > /dev/null; then
        echo "a transparent placeholder was written" >&2
        rc=1
    fi

    # The passive tap must build: it is what runs inside the recording pipeline.
    "$cc" "${common_flags[@]}" -D_GNU_SOURCE \
        -I"$root/package/pix-tuner/logo" -I"$root/package/stb-streamer/src" \
        -o "$work/eit_watch" "$root/package/stb-streamer/src/eit_watch.c" \
        "$root/package/pix-tuner/logo/logo_cdt.c" \
        "$root/package/pix-tuner/logo/ts_section.c" \
        "$root/package/pix-tuner/logo/logo_store.c" \
        "$root/package/stb-streamer/src/arib_text.c" || rc=1
fi

exit $rc
