#!/bin/sh
set -eu
# Buildroot fs/cpio supplies /init (mount devtmpfs then exec BusyBox init).
# Nothing in this boot image requires a persistent /dev/root mount.

# The per-unit DTS list decides which DTBs exist. Derive it from this build's
# own config (BR2_LINUX_KERNEL_INTREE_DTS_NAME) instead of a hardcoded unit
# list: unit 3 boots an sm1-only DTB that the hardcoded list silently dropped
# from SHA256SUMS and the export (2026-11-22). Diagnostic DTBs (nousb / usb /
# gxl / …) ship while unit 3's USB layout is being settled; units 1 and 2
# keep the same image they had before.
dtbs=""
extra=""
for name in $(sed -n 's/^BR2_LINUX_KERNEL_INTREE_DTS_NAME="\(.*\)"$/\1/p' "$BR2_CONFIG"); do
    dtb="$(basename "$name").dtb"
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
