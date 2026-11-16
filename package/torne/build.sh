#!/bin/sh
set -eu
src=$1
build=$src/out
mkdir -p "$build/include"
cat > "$build/include/arib25_api.h" <<'EOF'
#define ARIB25_API_EXPORT
#define ARIB25_API_NO_EXPORT
EOF
common="-O2 -ffunction-sections -fdata-sections -pthread"
includes="-I$src/src/torne -I$src/third_party/torne -I$src/third_party/arib25 -I$build/include"
for name in torne_c ts_stats card_server bcas_direct card_client torne_b25; do
    $CC $CFLAGS $common -std=gnu11 -D_FILE_OFFSET_BITS=64 $includes $USB_CFLAGS \
        -c "$src/src/torne/$name.c" -o "$build/$name.o"
done
for name in torne_bridge torne_dib torne_card; do
    $CC $CFLAGS $common -std=gnu11 $includes \
        -c "$src/third_party/torne/$name.c" -o "$build/$name.o"
done
for name in arib_std_b25 ts_section_parser; do
    $CC $CFLAGS $common -std=gnu11 $includes \
        -c "$src/third_party/arib25/$name.c" -o "$build/$name.o"
done
$CXX $CXXFLAGS $common -std=c++98 $includes \
    -c "$src/third_party/arib25/multi2.cc" -o "$build/multi2.o"
$CC $LDFLAGS -pthread -Wl,--gc-sections \
    "$build/torne_c.o" "$build/ts_stats.o" "$build/card_server.o" \
    "$build/torne_bridge.o" "$build/torne_dib.o" "$build/torne_card.o" \
    $USB_LIBS -lm -ldl -o "$build/torne-c"
$CXX $LDFLAGS -pthread -Wl,--gc-sections \
    "$build/torne_b25.o" "$build/ts_stats.o" "$build/bcas_direct.o" "$build/card_client.o" \
    "$build/arib_std_b25.o" "$build/ts_section_parser.o" "$build/multi2.o" \
    -lm -ldl -o "$build/torne-b25"
