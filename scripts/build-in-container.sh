#!/bin/bash
set -euo pipefail
profile=${X96MAX_PROFILE:-disk}
unit=${X96MAX_UNIT:-2}
# Also validate direct container invocations before downloads or build changes.
case "$unit" in
    1|2) ;;
    *) echo "unknown X96MAX_UNIT: $unit (expected 1 or 2)" >&2; exit 1 ;;
esac
case "$profile" in
    disk|ramboot) ;;
    *) echo "unknown X96MAX_PROFILE: $profile" >&2; exit 1 ;;
esac
version=2026.08.1
sha=34fb7bea8a213ffde39cc02e61703391560f26e0850f03138c722d0b09880f2b
cd /build
if [[ ! -d buildroot-$version ]]; then
    curl -fL "https://buildroot.org/downloads/buildroot-$version.tar.xz" -o "buildroot-$version.tar.xz"
    echo "$sha  buildroot-$version.tar.xz" | sha256sum -c -
    tar -xJf "buildroot-$version.tar.xz"
fi
kernel_archive="/build/buildroot-$version/dl/linux/linux-6.18.62.tar.xz"
if [[ ! -f "$kernel_archive" ]]; then
    mkdir -p "$(dirname "$kernel_archive")"
    curl -fL https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.62.tar.xz -o "$kernel_archive"
fi
echo "490a7aedf1b355c565d3a6c05333ccccb5da82064eca46b590448a7099d7d651  $kernel_archive" | sha256sum -c -
make -C "buildroot-$version" O=/build/output BR2_EXTERNAL=/project x96max_defconfig
if [[ "$profile" == ramboot ]]; then
    (cd /build/output && /build/buildroot-$version/support/kconfig/merge_config.sh -m \
        .config /project/board/x96max/ramboot.fragment \
        "/project/board/x96max/ramboot-unit${unit}.fragment")
    make -C "buildroot-$version" O=/build/output BR2_EXTERNAL=/project olddefconfig
fi
make -C "buildroot-$version" O=/build/output BR2_EXTERNAL=/project -j"${JOBS:-6}" "$@"
if [[ $# == 0 ]]; then
    if [[ "$profile" == ramboot ]]; then
        cp /build/output/images/{Image,meson-g12a-x96-max-p.dtb,meson-g12a-x96-max-p-ephy-4g.dtb,rootfs.cpio.gz,rootfs.cpio.uboot,ramboot.img,SHA256SUMS} /export/
        if [[ "$unit" == 1 ]]; then
            cp /project/board/x96max/ramboot-unit1-uboot.txt /export/
        else
            cp /project/board/x96max/ramboot-uboot.txt /export/
        fi
    else
        cp /build/output/images/{Image,meson-g12a-x96-max-p.dtb,rootfs.ext4,sdcard.img,SHA256SUMS} /export/
    fi
    cp /build/output/.config /export/buildroot.config
    cp /build/output/build/linux-6.18.62/.config /export/linux.config
    /build/output/host/bin/aarch64-buildroot-linux-gnu-gcc --version > /export/toolchain.txt
fi
