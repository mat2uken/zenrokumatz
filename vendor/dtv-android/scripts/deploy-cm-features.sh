#!/bin/bash
# Installs the CM feature sideband on the NAS and switches the worker mode.
#
# Nothing runs unless --apply is given: without it the script only prints the
# steps and copies the sources to a tmpfs scratch directory for inspection.
#
#   scripts/deploy-cm-features.sh            # show what would change
#   scripts/deploy-cm-features.sh --apply    # build and install
#
# Prerequisites: ssh key access to ku@192.168.99.80 and a checkout of this
# repository. Restarting the recorder interrupts the recording for a few
# seconds, so run it when that is acceptable.
set -euo pipefail

nas=${NAS_HOST:-ku@192.168.99.80}
root=$(cd "$(dirname "$0")/.." && pwd)
mode=plan
if [[ ${1:-} == "--apply" ]]; then
    mode=apply
fi

build_dir=/dev/shm/pix-stream-build-feature
install_bin=/home/ku/pix-stream-20261101/pix_stream_recorder
worker_root=/home/ku/pix-cm
release_id=${RELEASE_ID:-$(date +%Y%m%d)-feature-01}

echo "mode: $mode"
echo "sources: $root/nas_src"
echo "build:   $nas:$build_dir"
echo "binary:  $install_bin"
echo "release: $worker_root/releases/$release_id"

if [[ $mode == plan ]]; then
    echo "plan only: rerun with --apply to build and install"
    exit 0
fi

echo "--- copying sources"
ssh "$nas" "rm -rf $build_dir && mkdir -p $build_dir"
scp -q "$root"/nas_src/pix_stream_recorder.c "$root"/nas_src/cm_queue.c \
    "$root"/nas_src/cm_queue.h "$root"/nas_src/feature_sidecar.c \
    "$root"/nas_src/feature_sidecar.h "$root"/nas_src/cJSON.h "$nas:$build_dir/"

echo "--- building"
ssh "$nas" "cd $build_dir && gcc -O2 -Wall -I. -o pix_stream_recorder \
    pix_stream_recorder.c cm_queue.c feature_sidecar.c -l:libcjson.so.1 -lpthread"

echo "--- installing the receiver next to the running binary"
ssh "$nas" "set -e
    test -x $install_bin
    cp $install_bin $install_bin.before-feature
    install -m 0755 $build_dir/pix_stream_recorder $install_bin.new
    mv -f $install_bin.new $install_bin
    $install_bin --help >/dev/null 2>&1 || true
    ls -l $install_bin $install_bin.before-feature"

echo "--- publishing the CM worker release"
# The decoders do not change, so the new release is the running one with the
# updated Python modules. cm-cut and cm-verify keep their exact binaries.
ssh "$nas" "set -e
    test -d $worker_root/current
    rm -rf $worker_root/releases/$release_id
    cp -aL $worker_root/current $worker_root/releases/$release_id"
scp -q "$root"/experiments/cm-generic/cm_service.py \
    "$root"/experiments/cm-generic/cm_features.py \
    "$root"/experiments/cm-generic/cm_rules.py \
    "$root"/experiments/cm-generic/cm_scenario.py \
    "$root"/experiments/cm-generic/test_cm_features.py \
    "$nas:$worker_root/releases/$release_id/"
ssh "$nas" "cd $worker_root/releases/$release_id && python3 -m unittest test_cm_features"
ssh "$nas" "ln -sfn $worker_root/releases/$release_id $worker_root/current"

echo "--- switching the worker mode to shadow"
ssh "$nas" "python3 - <<'PY'
import json
from pathlib import Path
path = Path('$worker_root/config.json')
config = json.loads(path.read_text())
config['feature_mode'] = 'shadow'
path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + '\n')
print('feature_mode =', config['feature_mode'])
PY"

echo "--- restarting services"
ssh "$nas" "sudo systemctl restart pix-stream-manager.service && \
    sudo systemctl restart pix-cm-worker.service"

cat <<'TEXT'

Done. Next:
  1. Confirm the STB is recording and /v1/status shows capturing=true.
  2. Let one programme finish, then read state/jobs/<id>/result.json:
     feature_mode=shadow, feature_reason=ok, feature_shadow.within_tolerance.
  3. Only after several matching programmes, set feature_mode=apply.
TEXT
