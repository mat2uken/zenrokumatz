#!/bin/bash
# flash-ramboot-over-ssh.sh - docs/ram-rootfs.md の起動媒体更新を SSH で実施する。
#
# NAS (192.168.99.80) で実行する。STB には sftp-server が無いため転送は
# `ssh cat >` を使う。手順と停止条件は docs/ram-rootfs.md:
#   1. 接続先が RAM rootfs（root=/rootfs）で動いていること
#   2. 起動媒体の照合（個体識別コマンドが 0 を返すこと）
#   3. 起動用 FAT 区画が未マウントであること
#   4. 旧ファイルを RAM へ退避してから更新する（元の退避を上書きしない）
#   5. RAM 側と FAT 側の SHA256 が一致すること。sync 後にアンマウント
# 再起動はこのスクリプトでは行わない（別途ユーザー確認のうえ実施する）。
#
# usage: flash-ramboot-over-ssh.sh <stb-ip> <export-dir> <dtb-name> <identity-check-cmd>
#   <identity-check-cmd> は STB 内で実行し、媒体が対象機の起動媒体だと
#   確認できるコマンド（終了コード 0 が合格）。
#   例: test "$(cat /sys/class/mmc_host/mmc0/mmc0:0001/cid)" = 0254...
set -euo pipefail

STB=${1:?usage: flash-ramboot-over-ssh.sh <stb-ip> <export-dir> <dtb-name> <identity-check-cmd>}
EXPORT_DIR=${2:?export directory with SHA256SUMS}
DTB=${3:?dtb file name for this unit}
IDENTITY_CHECK=${4:?media identity check command run on the STB}

KEY=/home/ku/.ssh/id_ed25519_guard
SSH_OPTS=(-i "$KEY" -o BatchMode=yes -o StrictHostKeyChecking=accept-new)
FILES=(Image "$DTB" rootfs.cpio.uboot)
WORK=/tmp/ramboot-flash.$$

run() { ssh "${SSH_OPTS[@]}" "root@$STB" "$@"; }
put() { ssh "${SSH_OPTS[@]}" "root@$STB" "cat > '$WORK/$2'" < "$1"; }

hash_of() { awk -v f="$1" '$2=="./"f || $2==f {print $1}' "$EXPORT_DIR/SHA256SUMS"; }

fail() { echo "ABORT: $*" >&2; exit 1; }

[ -f "$EXPORT_DIR/SHA256SUMS" ] || fail "no SHA256SUMS in $EXPORT_DIR"
for f in "${FILES[@]}"; do
    [ -f "$EXPORT_DIR/$f" ] || fail "missing $f in $EXPORT_DIR"
    [ -n "$(hash_of "$f")" ] || fail "$f is not listed in $EXPORT_DIR/SHA256SUMS"
done

echo "== 1. target is RAM rootfs =="
ROOT_FS=$(run 'awk "\$2==\"/\" {print \$3}" /proc/mounts')
[ "$ROOT_FS" = "rootfs" ] || fail "root is not rootfs: $ROOT_FS"
run 'cat /proc/cmdline; cat /proc/swaps'

echo "== 2. boot media identity =="
run "$IDENTITY_CHECK" || fail "media identity check failed on $STB"

run "mkdir -p $WORK"
trap 'run "rm -rf $WORK" 2>/dev/null || true' EXIT

echo "== 3. transfer + verify in RAM =="
for f in "${FILES[@]}"; do
    put "$EXPORT_DIR/$f" "$f"
done
printf '%s  %s\n' "$(hash_of "$DTB")" "$DTB" > /tmp/flash-manifest.$$
for f in Image rootfs.cpio.uboot; do
    printf '%s  %s\n' "$(hash_of "$f")" "$f" >> /tmp/flash-manifest.$$
done
scp -q -o BatchMode=yes /tmp/flash-manifest.$$ "root@$STB:$WORK/SHA256SUMS.new" 2>/dev/null \
    || ssh "${SSH_OPTS[@]}" "root@$STB" "cat > '$WORK/SHA256SUMS.new'" < /tmp/flash-manifest.$$
rm -f /tmp/flash-manifest.$$
run "cd $WORK && sha256sum -c SHA256SUMS.new" || fail "SHA256 mismatch in RAM transfer"

echo "== 4. mount boot FAT (must be unmounted beforehand) =="
BOOT_PART=$(run 'blkid | grep -E "X96BOOT|X96RAMBOOT" | cut -d: -f1 | head -1')
[ -n "$BOOT_PART" ] || fail "no X96BOOT/X96RAMBOOT partition found"
if run "mount | grep -F '$BOOT_PART'"; then fail "$BOOT_PART is mounted"; fi
run "mkdir -p $WORK/boot && mount $BOOT_PART $WORK/boot"

echo "== 5. backup old files, then write =="
run "mkdir -p $WORK/backup"
CHANGED=0
for f in "${FILES[@]}"; do
    if run "test -f $WORK/boot/$f"; then
        run "cp $WORK/boot/$f $WORK/backup/$f"
        OLD_HASH=$(run "sha256sum $WORK/backup/$f | awk '{print \$1}'")
        NEW_HASH=$(hash_of "$f")
        if [ "$OLD_HASH" = "$NEW_HASH" ]; then
            echo "  $f: unchanged, not touched"
            continue
        fi
    fi
    CHANGED=1
    echo "  $f: writing"
    ssh "${SSH_OPTS[@]}" "root@$STB" "cat > '$WORK/boot/$f'" < "$EXPORT_DIR/$f"
done
[ "$CHANGED" = 1 ] || { echo "nothing to write; all files already current"; run "umount $WORK/boot"; exit 0; }

echo "== 6. verify on the FAT, sync, unmount =="
run "cd $WORK/boot && sha256sum -c $WORK/SHA256SUMS.new" || fail "FAT verify failed"
run "sync && umount $WORK/boot" || fail "unmount failed"
echo "FLASH-OK: $STB updated ($BOOT_PART). Reboot is a separate, user-confirmed step."
