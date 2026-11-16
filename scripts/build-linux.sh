#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
context=colima-x96max-linux
if ! docker --context "$context" info >/dev/null 2>&1; then
    colima start x96max-linux --activate=false --arch aarch64 --cpu 6 --memory 8 --disk 32 \
        --vm-type vz --mount-type virtiofs
fi
mkdir -p "$root/output" "$root/work/build"
docker --context "$context" build -t x96max-builder:bookworm -f "$root/scripts/Dockerfile" "$root/scripts"
docker --context "$context" run --rm --user root -v x96max-build:/build \
    x96max-builder:bookworm sh -c 'mkdir -p /build/cache; chown builder:builder /build /build/cache'
docker --context "$context" run --rm --name x96max-build \
    -v x96max-build:/build -v "$root:/project:ro" -v "$root/output:/export" \
    -e JOBS="${JOBS:-6}" x96max-builder:bookworm \
    bash /project/scripts/build-in-container.sh "$@"
