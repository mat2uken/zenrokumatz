#!/bin/sh
# Runs on Buildroot's per-filesystem staging tree, never the shared target.
set -eu
root=$1
[ "$root" = "$BUILD_DIR/buildroot-fs/cpio/target" ] || {
    echo "ramboot-finalize: unexpected staging tree: $root" >&2; exit 1;
}
# Overlay rsync is additive: a renamed init script can survive in the cached
# Buildroot target and otherwise reappear in every new archive. Retire only
# this known obsolete service, never --delete the package-populated /etc tree.
obsolete="$root/etc/init.d/S95pix-stream"
if [ -e "$obsolete" ] || [ -L "$obsolete" ]; then
    [ -f "$obsolete" ] && [ ! -L "$obsolete" ] || {
        echo "ramboot-finalize: unexpected obsolete init path: $obsolete" >&2; exit 1;
    }
    unlink "$obsolete"
fi
key=/identity/dropbear_ed25519_host_key
if [ ! -s "$key" ]; then
    umask 077
    "$root/lib/ld-linux-aarch64.so.1" --library-path "$root/lib:$root/usr/lib" \
        "$root/usr/bin/dropbearkey" -t ed25519 -f "$key"
fi
if [ -L "$root/etc/dropbear" ]; then
    # Replace exactly this staging symlink, never follow it into host /var/run.
    unlink "$root/etc/dropbear"
fi
install -d -m 0700 "$root/etc/dropbear"
install -m 0600 "$key" "$root/etc/dropbear/dropbear_ed25519_host_key"
chown 0:0 "$root/etc/dropbear" "$root/etc/dropbear/dropbear_ed25519_host_key"
# Explicit private-input whitelist. These never enter the shared Buildroot target
# or source overlays; optional inputs are present only for the selected identity.
for path in etc/default/bcas-linux etc/network/interfaces root/.ssh/authorized_keys; do
    if [ -f "/identity/$path" ]; then
        case "$path" in
            etc/default/bcas-linux) mode=0600 ;;
            etc/network/interfaces) mode=0644 ;;
            root/.ssh/authorized_keys)
                mode=0600
                install -d -m 0700 "$root/root/.ssh"
                chown 0:0 "$root/root/.ssh"
                ;;
        esac
        install -m "$mode" "/identity/$path" "$root/$path"
        # This hook executes under fakeroot, so the archive records uid/gid 0.
        chown 0:0 "$root/$path"
    fi
done
# Preserve optional per-unit logo cache in this CPIO only. Do not follow source
# or staging symlinks, and allow only flat regular JSON/PNG cache members.
logos=/identity/var/lib/pix/logos
for path in /identity/var /identity/var/lib /identity/var/lib/pix "$logos"; do
    if [ -L "$path" ] || { [ -e "$path" ] && [ ! -d "$path" ]; }; then
        echo "ramboot-finalize: invalid logo cache directory: $path" >&2; exit 1
    fi
done
if [ -d "$logos" ]; then
    for path in "$root/var" "$root/var/lib" "$root/var/lib/pix" "$root/var/lib/pix/logos"; do
        # The standard skeleton may redirect /var/lib into /tmp, which a
        # runtime tmpfs would hide. Materialize only that known staging link.
        if [ "$path" = "$root/var/lib" ] && [ -L "$path" ] && [ "$(readlink "$path")" = ../tmp ]; then
            unlink "$path"
            install -d -m 0755 "$path"
            chown 0:0 "$path"
        fi
        if [ -L "$path" ] || { [ -e "$path" ] && [ ! -d "$path" ]; }; then
            echo "ramboot-finalize: invalid staging logo directory: $path" >&2; exit 1
        fi
    done
    install -d -m 0700 "$root/var/lib/pix/logos"
    chown 0:0 "$root/var/lib/pix/logos"
    for source in "$logos"/* "$logos"/.[!.]* "$logos"/..?*; do
        [ -e "$source" ] || [ -L "$source" ] || continue
        if [ -L "$source" ] || [ ! -f "$source" ]; then
            echo "ramboot-finalize: logo cache member must be a regular file: $source" >&2; exit 1
        fi
        case "${source##*/}" in
            *.json|*.png) ;;
            *) echo "ramboot-finalize: unexpected logo cache file type: $source" >&2; exit 1 ;;
        esac
        destination="$root/var/lib/pix/logos/${source##*/}"
        if [ -L "$destination" ] || { [ -e "$destination" ] && [ ! -f "$destination" ]; }; then
            echo "ramboot-finalize: invalid staging logo member: $destination" >&2; exit 1
        fi
        install -m 0600 "$source" "$destination"
        chown 0:0 "$destination"
    done
fi
# initramfs root is already writable and cannot be remounted like ext4.
sed -i '\|^::sysinit:/bin/mount -o remount,rw /$|d' "$root/etc/inittab"
# Make runtime root semantics explicit; no disk is an implicit root candidate.
sed -i '\|^/dev/root[[:space:]]|d' "$root/etc/fstab"
