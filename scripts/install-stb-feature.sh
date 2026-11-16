#!/bin/bash
# Installs the feature-aware stb_streamer and launcher on the Linux STB.
#
#   scripts/install-stb-feature.sh            # show the plan
#   scripts/install-stb-feature.sh --apply    # copy and restart the pipeline
#
# The restart stops the current recording for a few seconds. The previous
# binary and launcher are kept next to the new ones with a .before-feature
# suffix.
set -euo pipefail

stb=${STB_HOST:-root@192.168.99.109}
root=$(cd "$(dirname "$0")/.." && pwd)
binary=${STB_BINARY:-$root/output/stb_streamer}
launcher=$root/board/x96max/overlay/usr/bin/pix-stream-linux
defaults=$root/board/x96max/overlay/etc/default/pix-stream-linux
mode=plan
if [[ ${1:-} == "--apply" ]]; then
    mode=apply
fi

echo "mode:   $mode"
echo "stb:    $stb"
echo "binary: $binary"
echo "files:  /usr/bin/stb_streamer /usr/bin/pix-stream-linux /etc/default/pix-stream-linux"

if [[ $mode == plan ]]; then
    echo "plan only: rerun with --apply to install"
    exit 0
fi

test -x "$binary" || { echo "missing binary: $binary" >&2; exit 1; }

echo "--- checking the box is reachable"
ssh "$stb" 'uname -m; ls -l /usr/bin/stb_streamer'

echo "--- copying (old files kept as *.before-feature)"
scp -O -q "$binary" "$stb:/tmp/stb_streamer.new"
scp -O -q "$launcher" "$stb:/tmp/pix-stream-linux.new"
scp -O -q "$defaults" "$stb:/tmp/pix-stream-linux.defaults.new"
ssh "$stb" 'set -e
    cp /usr/bin/stb_streamer /usr/bin/stb_streamer.before-feature
    cp /usr/bin/pix-stream-linux /usr/bin/pix-stream-linux.before-feature
    cp /etc/default/pix-stream-linux /etc/default/pix-stream-linux.before-feature
    install -m 0755 /tmp/stb_streamer.new /usr/bin/stb_streamer
    install -m 0755 /tmp/pix-stream-linux.new /usr/bin/pix-stream-linux
    install -m 0644 /tmp/pix-stream-linux.defaults.new /etc/default/pix-stream-linux
    rm -f /tmp/stb_streamer.new /tmp/pix-stream-linux.new /tmp/pix-stream-linux.defaults.new
    /usr/bin/stb_streamer --help 2>&1 | head -2 || true'

echo "--- enabling features and restarting the pipeline"
ssh "$stb" 'set -e
    sed -i "s/^PIX_FEATURES=.*/PIX_FEATURES=1/" /etc/default/pix-stream-linux
    grep "^PIX_FEATURES" /etc/default/pix-stream-linux
    /etc/init.d/S97pix-stream restart
    sleep 5
    curl -s --max-time 5 http://127.0.0.1:40774/v1/status || true'

cat <<'TEXT'

Done. Next:
  1. On the NAS, start the recorder for this box so it asks for features=v1.
  2. Confirm /dev/shm/pix/feat-v.log grows while recording.
  3. Roll back with: cp /usr/bin/stb_streamer.before-feature /usr/bin/stb_streamer
     and /etc/init.d/S97pix-stream restart.
TEXT
