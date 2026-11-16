#!/bin/sh
set -eu
"${BUILD_DIR}/../host/bin/genimage" \
    --rootpath "$TARGET_DIR" \
    --tmppath "$BUILD_DIR/genimage.tmp" \
    --inputpath "$BINARIES_DIR" \
    --outputpath "$BINARIES_DIR" \
    --config "$BR2_EXTERNAL_X96MAX_PATH/board/x96max/genimage.cfg"
(cd "$BINARIES_DIR" && sha256sum Image meson-g12a-x96-max-p.dtb rootfs.ext4 sdcard.img > SHA256SUMS)
