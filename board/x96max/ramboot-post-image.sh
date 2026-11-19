#!/bin/sh
set -eu
# Buildroot fs/cpio supplies /init (mount devtmpfs then exec BusyBox init).
# Nothing in this boot image requires a persistent /dev/root mount.

# The per-unit DTS list decides which DTBs exist. Ship exactly those that were
# built: unit 3 also has diagnostic DTBs while its USB layout is settled, while
# units 1 and 2 must keep the same image they had before.
dtbs=""
extra=""
for dtb in meson-g12a-x96-max-p.dtb meson-g12a-x96-max-p-ephy-4g.dtb \
           meson-g12a-x96-max-p-nousb.dtb meson-g12a-x96-max-p-usb.dtb; do
    [ -f "$BINARIES_DIR/$dtb" ] || continue
    dtbs="$dtbs $dtb"
    case "$dtb" in
        meson-g12a-x96-max-p.dtb|meson-g12a-x96-max-p-ephy-4g.dtb) ;;
        *) extra="$extra, \"$dtb\"" ;;
    esac
done
[ -n "$dtbs" ] || { echo "ramboot-post-image: no DTBs were built" >&2; exit 1; }

# genimage takes a literal file list, so derive this build's own config from
# the shared template instead of hardcoding every unit's extras into it.
cfg="$BUILD_DIR/genimage-ramboot.cfg"
sed -e "s|\"meson-g12a-x96-max-p-ephy-4g.dtb\"|\"meson-g12a-x96-max-p-ephy-4g.dtb\"$extra|" \
    "$BR2_EXTERNAL_X96MAX_PATH/board/x96max/ramboot-genimage.cfg" > "$cfg"

"${BUILD_DIR}/../host/bin/genimage" \
    --rootpath "$TARGET_DIR" \
    --tmppath "$BUILD_DIR/genimage-ramboot.tmp" \
    --inputpath "$BINARIES_DIR" \
    --outputpath "$BINARIES_DIR" \
    --config "$cfg"

# shellcheck disable=SC2086
(cd "$BINARIES_DIR" && sha256sum Image $dtbs rootfs.cpio.gz rootfs.cpio.uboot ramboot.img > SHA256SUMS)
