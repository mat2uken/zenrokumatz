#!/bin/bash
# Builds the NAS side programs from nas/src and optionally installs them.
#
#   nas/build.sh            # build only, into nas/out
#   nas/build.sh --apply    # build, install on the NAS and restart the services
#
# The NAS is x86_64 Debian. It needs libcjson-dev for the headers
# (/usr/include/cjson/cJSON.h) and links against libcjson.so.1.
#
# Installing stops the recorder for a few seconds while the manager restarts,
# which ends the segment that is being written. Run it when that is acceptable;
# the recorder continues the same programme as the next part, so the programme
# is not split across folders.
set -euo pipefail

root=$(cd "$(dirname "$0")" && pwd)
out="$root/out"
src="$root/src"
nas=${NAS_HOST:-ku@192.168.99.80}
install_dir=${NAS_INSTALL_DIR:-/home/ku/pix-stream-20261101}
mode=build
if [[ ${1:-} == "--apply" ]]; then
    mode=apply
fi

# cJSON is Debian's; -I/usr/include/cjson is where libcjson-dev puts it.
cflags=(-O2 -Wall -I/usr/include/cjson)
mkdir -p "$out"

build() {
    local name=$1; shift
    local sources=()
    local s
    for s in "$@"; do
        sources+=("$src/$s")
    done
    gcc "${cflags[@]}" -o "$out/$name" "$src/$name.c" "${sources[@]}" -lcjson -lpthread
    echo "built $name"
}

build pix_stream_recorder fmp4_to_mp4.c cm_queue.c
build pix_stream_manager cm_queue.c feature_sidecar.c
python3 -m py_compile "$src/pix-stb-guard.py"
echo "checked pix-stb-guard.py"
ls -l "$out"

if [[ $mode == build ]]; then
    echo
    echo "build only: rerun with --apply to install on $nas"
    exit 0
fi

# Copy sources to the NAS and rebuild there, so the compiler and the libc that
# run the binaries are the same ones that linked them.
remote_build=${REMOTE_BUILD_DIR:-/dev/shm/pix-nas-build}
echo "--- building on $nas:$remote_build"
ssh "$nas" "rm -rf $remote_build && mkdir -p $remote_build"
scp -q "$src"/*.c "$src"/*.h "$nas:$remote_build/"

ssh "$nas" "set -e
    cd $remote_build
    gcc -O2 -Wall -I/usr/include/cjson -o pix_stream_recorder \
        pix_stream_recorder.c fmp4_to_mp4.c cm_queue.c -lcjson -lpthread
    gcc -O2 -Wall -I/usr/include/cjson -o pix_stream_manager \
        pix_stream_manager.c cm_queue.c feature_sidecar.c -lcjson -lpthread
    ls -l pix_stream_recorder pix_stream_manager"

stamp=$(date +%Y%m%d-%H%M)
echo "--- installing (backups tagged .before-$stamp)"
ssh "$nas" "set -e
    cd $install_dir
    for name in pix_stream_recorder pix_stream_manager; do
        cp -p \$name \$name.before-$stamp
        install -m 0755 $remote_build/\$name \$name.new
        mv -f \$name.new \$name
    done
    cp -p pix-stb-guard.py pix-stb-guard.py.before-$stamp
    install -m 0755 /dev/stdin pix-stb-guard.py <<'PYEOF'
$(cat "$src/pix-stb-guard.py")
PYEOF
    python3 -m py_compile pix-stb-guard.py && echo 'guard compiles'
    ls -l pix_stream_recorder pix_stream_manager pix-stb-guard.py"

echo "--- restarting services"
# The manager owns the recorders, so restarting it reloads both binaries.
ssh "$nas" "sudo systemctl restart pix-stream-manager.service pix-stb-guard.service"
sleep 12
ssh "$nas" "systemctl is-active pix-stream-manager pix-stb-guard; \
    curl -s http://127.0.0.1:40776/api/nodes | python3 -c \"
import json,sys
for n in json.load(sys.stdin):
    print(' ', n['id'], n.get('state'))
\""

echo
echo "installed; previous binaries are kept as *.before-$stamp"
