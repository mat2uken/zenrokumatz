#!/bin/bash
set -eu
NDK=${ANDROID_NDK_HOME:-/Users/kenichim/Library/Android/sdk/ndk/28.2.13676358}
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi28-clang"
STRIP="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip"

mkdir -p bin
"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
    -D_FILE_OFFSET_BITS=64 \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I c_src \
    c_src/fmp4_writer.c c_src/ram_spool.c c_src/stb_http.c c_src/fmp4_sink.c c_src/mdns_announce.c c_src/stb_main.c \
    -pthread -o bin/stb_streamer

"$STRIP" bin/stb_streamer

# 時刻同期は単体で動く小さな道具。ウォッチドッグが定期的に呼ぶ。
"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
    -D_FILE_OFFSET_BITS=64 \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    c_src/timesync.c -o bin/timesync

"$STRIP" bin/timesync

# su シム: 1台目 (SuperSU) と 2台目 (Amlogic) の su 呼び出し規約の差を吸収する。
# 配備は tools/install-su-shim.sh。
"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    c_src/su_shim.c -o bin/su_shim

# カード応答の確認用。B-CAS の初期化結果(0x2100)を直接見る。
"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tools/cardprobe.c -o bin/cardprobe

"$STRIP" bin/cardprobe

# ネットワーク越しの B-CAS カードをローカルの UNIX ソケットへ橋渡しする。
# bcas.conf がある端末だけがこれを使う。
"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tools/cardrelay.c -o bin/cardrelay

"$STRIP" bin/cardrelay
"$STRIP" bin/su_shim
ls -lh bin/stb_streamer bin/timesync bin/su_shim bin/cardprobe bin/cardrelay
file bin/stb_streamer bin/timesync bin/su_shim bin/cardrelay
