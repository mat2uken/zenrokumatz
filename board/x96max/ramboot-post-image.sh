#!/bin/sh
set -eu
# Buildroot fs/cpio supplies /init (mount devtmpfs then exec BusyBox init).
# Nothing in this boot image requires a persistent /dev/root mount.
"${BUILD_DIR}/../host/bin/genimage" \
    --rootpath "$TARGET_DIR" \
    --tmppath "$BUILD_DIR/genimage-ramboot.tmp" \
    --inputpath "$BINARIES_DIR" \
    --outputpath "$BINARIES_DIR" \
    --config "$BR2_EXTERNAL_X96MAX_PATH/board/x96max/ramboot-genimage.cfg"
(cd "$BINARIES_DIR" && sha256sum Image meson-g12a-x96-max-p.dtb meson-g12a-x96-max-p-ephy-4g.dtb rootfs.cpio.gz rootfs.cpio.uboot ramboot.img > SHA256SUMS)
