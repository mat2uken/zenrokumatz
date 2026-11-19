#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
context=colima-x96max-linux
profile=${X96MAX_PROFILE:-disk}
unit=${X96MAX_UNIT:-2}
# Reject configuration mistakes before probing or starting Docker/Colima.
case "$unit" in
    1|2|3) ;;
    *) echo "unknown X96MAX_UNIT: $unit (expected 1, 2 or 3)" >&2; exit 1 ;;
esac
case "$profile" in
    disk) export_dir="$root/output" ;;
    ramboot)
        # Unit 2 keeps its original names so the existing scripts and identity
        # remain valid; units 1 and 3 always carry their number.
        if [[ "$unit" == 1 ]]; then
            export_dir="$root/output/ramboot-unit1"
            identity_dir="$root/work/ramboot-identity-unit1"
        elif [[ "$unit" == 3 ]]; then
            export_dir="$root/output/ramboot-unit3"
            identity_dir="$root/work/ramboot-identity-unit3"
        else
            export_dir="$root/output/ramboot"
            identity_dir="$root/work/ramboot-identity"
        fi
        ;;
    *) echo "unknown X96MAX_PROFILE: $profile" >&2; exit 1 ;;
esac
if ! docker --context "$context" info >/dev/null 2>&1; then
    colima start x96max-linux --activate=false --arch aarch64 --cpu 6 --memory 8 --disk 32 \
        --vm-type vz --mount-type virtiofs
fi
mkdir -p "$export_dir" "$root/work/build"
identity_mount=()
if [[ "$profile" == ramboot ]]; then
    mkdir -p "$identity_dir"
    # The boot archive embeds this unit's private identity; never share it.
    chmod 700 "$identity_dir" "$export_dir"
    identity_mount=(-v "$identity_dir:/identity")
fi
docker --context "$context" build -t x96max-builder:bookworm -f "$root/scripts/Dockerfile" "$root/scripts"
docker --context "$context" run --rm --user root -v x96max-build:/build \
    x96max-builder:bookworm sh -c 'mkdir -p /build/cache; chown builder:builder /build /build/cache'
docker --context "$context" run --rm --name x96max-build \
    -v x96max-build:/build -v "$root:/project:ro" -v "$export_dir:/export" \
    "${identity_mount[@]}" -e X96MAX_PROFILE="$profile" -e X96MAX_UNIT="$unit" -e JOBS="${JOBS:-6}" x96max-builder:bookworm \
    bash /project/scripts/build-in-container.sh "$@"
