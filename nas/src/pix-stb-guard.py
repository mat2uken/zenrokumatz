#!/usr/bin/env python3
"""Recover a Linux STB whose stream has stopped.

The X96Max boxes occasionally wedge inside the Amlogic MPEG2 decoder driver:
the decoder stops producing frames, the driver's own timeout calls
``amvdec_stop``, and the DOS-bus read inside it never returns. The kernel then
livelocks in an interrupt handler (the previous boot's console log ends with
``ISR_Long___ERR. irq:19 tout-tin:1090 ms`` and a stack through
``amvdec_stop -> codec_reg_read -> codecio_read_dosbus``), the box stops
answering ARP and stays off the network until it is power cycled. The manager
can only restart the recorder, which cannot reach a wedged kernel, so on
2026-11-05 a wedged box stayed unrecorded for six hours.

This guard watches the manager's node list and recovers a node that has been
unreachable or stalled for longer than its timeout. Recovery runs over ssh:
first it restarts the capture supervisor (S97), which keeps the kernel, the
USB tuner state and the card relay intact — that is what brought
192.168.99.109 back on 2026-11-19, after app-only recovery had left it
unrecorded for 25 hours. A node that does not come back this way is reported
for a physical check. A normal restart takes under a minute, so the timeouts
sit well clear of it. The guard never touches a node that is merely waiting
for a session, and it backs off after an attempt so a box that cannot be
recovered does not get kicked in a loop.

A capture can also lose its audio PES stream and keep encoding video: the
recording then looks healthy while the audio track stops growing, which showed
up as a 939 second audio/video duration mismatch on 2026-11-09. The guard
reads the staged fragment the recorder writes on this host with ffprobe, and
after two consecutive empty probes restarts the capture over ssh.
"""

import argparse
import glob
import json
import subprocess
import sys
import time
import urllib.request

MANAGER = 'http://127.0.0.1:40776/api/nodes'
DEFAULT_TIMEOUT = 180
DEFAULT_COOLDOWN = 900

# A capture can declare an audio track and still send no audio packets: the
# recording then has video and silence. Restarting the capture rebuilds the
# audio path, which is what brought 192.168.99.209 back on 2026-11-09 after a
# morning of silent recordings.
AUDIO_RESTART_HOSTS = {'192.168.99.209', '192.168.99.56',
                       '192.168.99.123'}
AUDIO_CHECK_MIN_BYTES = 40 * 1024 * 1024
AUDIO_PROBE_SECONDS = 20
AUDIO_STRIKES = 2
AUDIO_FFPROBE = '/home/ku/work/TVerRec/bin/ffprobe'

SSH_KEY = '/home/ku/.ssh/id_ed25519_guard'
SSH_TIMEOUT = 25
# Boxes that run the Buildroot image answer ssh but expose no remote shell
# other than it; their stream supervisor can exit on its own. Restarting S97
# keeps the kernel, the USB tuner state and the card relay intact.
SSH_HOSTS = {'192.168.99.109', '192.168.99.123', '192.168.99.56'}
STREAM_INIT = '/etc/init.d/S97pix-stream'
STREAM_STATUS_URL = 'http://{host}:40774/v1/status'
# Recovery is only real when fragments arrive again. The box answers its status
# endpoint from the streamer process alone, so an HTTP 200 proves nothing about
# the capture: on 2026-11-23 a box reported capturing=true while totalPushed
# stayed at 34531 for thirteen hours.
STREAM_VERIFY_SECONDS = 40
STREAM_MIN_FRAGMENTS = 3
STREAM_SAMPLE_SECONDS = 5


def read_nodes(url=MANAGER, timeout=20.0):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read().decode('utf-8'))


def unreachable(node):
    """The manager could not read /v1/status from the box."""
    return bool(node.get('stbError'))


def decide(now, node, state, timeout, cooldown):
    """Return 'reboot', 'wait' or None for one node, updating state in place.

    Kept free of I/O so the decision can be exercised without rebooting a box.
    """
    key = node.get('id') or node.get('host')
    if not key:
        return None
    # A box that was switched off on purpose must not be kicked every cycle.
    if node.get('enabled') is False:
        state.pop(key, None)
        return None
    if not unreachable(node):
        state.pop(key, None)
        return None
    entry = state.setdefault(key, {'since': now, 'last_attempt': 0.0})
    if now - entry['since'] < timeout:
        return 'wait'
    # 0.0 means "never attempted"; a cooldown only applies after a real attempt.
    if entry['last_attempt'] and now - entry['last_attempt'] < cooldown:
        return 'wait'
    entry['last_attempt'] = now
    return 'reboot'


def stalled(node):
    """The recorder is connected but the box has produced no new fragment."""
    return node.get('state') == 'stalled' and not node.get('stbError')


def decide_stalled(now, node, state, timeout, cooldown, min_fragments=1):
    """Return 'reboot', 'wait' or None for a node that stopped producing data.

    A node can flip between ``stalled`` and ``recording`` while the USB tuner
    resets. Clearing the stall clock on that brief ``recording`` state makes
    the 900 s threshold unreachable, so recovery is only accepted when
    ``pushed`` actually advanced. Otherwise the original stall clock is kept.
    """
    key = node.get('id') or node.get('host')
    if not key:
        return None
    if node.get('enabled') is False:
        state.pop('stalled:' + key, None)
        return None
    frozen = state.get('stalled:' + key)
    pushed = node.get('pushed')
    if not stalled(node):
        if frozen is None:
            return None
        if (pushed is not None and frozen.get('pushed') is not None
                and pushed - frozen['pushed'] >= min_fragments):
            state.pop('stalled:' + key, None)
            return None
    entry = state.setdefault('stalled:' + key, {
        'since': now, 'last_attempt': 0.0, 'pushed': pushed})
    if pushed is not None and entry.get('pushed') is not None \
            and pushed - entry['pushed'] >= min_fragments:
        entry['since'] = now
        entry['pushed'] = pushed
    if entry.get('pushed') is None:
        entry['pushed'] = pushed
    if now - entry['since'] < timeout:
        return 'wait'
    if entry['last_attempt'] and now - entry['last_attempt'] < cooldown:
        return 'wait'
    entry['last_attempt'] = now
    return 'reboot'


def ssh(host, command, timeout=SSH_TIMEOUT):
    """Run one command on a Buildroot box. Never raises on a dead host."""
    try:
        return subprocess.run(
            ['ssh', '-i', SSH_KEY, '-o', 'BatchMode=yes',
             '-o', f'ConnectTimeout={max(5, timeout // 3)}',
             '-o', 'StrictHostKeyChecking=accept-new',
             f'root@{host}', command],
            capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None


def stream_pushed(host, timeout=6):
    """The fragment counter from the box, or None when it does not answer."""
    try:
        with urllib.request.urlopen(STREAM_STATUS_URL.format(host=host),
                                    timeout=timeout) as response:
            return json.loads(response.read().decode('utf-8')).get('totalPushed')
    except Exception:
        return None


def advance(previous, current, minimum=STREAM_MIN_FRAGMENTS):
    """(advanced, baseline) for one sample of the fragment counter.

    A restarted streamer begins again at zero, so a drop is a new run and only
    replaces the baseline. Kept free of I/O so the judgement can be tested.
    """
    if current is None:
        return False, previous
    if previous is None or current < previous:
        return False, current
    return current - previous >= minimum, previous


def wait_for_fragments(host, seconds=STREAM_VERIFY_SECONDS):
    """True once the box has produced new fragments since this call started."""
    baseline = stream_pushed(host)
    deadline = time.time() + seconds
    while time.time() < deadline:
        time.sleep(STREAM_SAMPLE_SECONDS)
        current = stream_pushed(host)
        if current is None:
            continue
        if baseline is None:
            baseline = current
            continue
        moved, baseline = advance(baseline, current)
        if moved:
            return True
    return False


def restart_stream(host, log=print):
    """Restart the capture over ssh and prove it by new fragments.

    S97 lifts a fence left by a dead process itself (pix-stream-unfence), so
    this only asks for a restart and then insists on evidence. The status
    endpoint is served by the streamer alone and answers while the media path
    is dead, so only a moving fragment counter proves the capture is back.
    """
    restarted = ssh(host, f'{STREAM_INIT} restart')
    if restarted is None:
        log(f'{host}: ssh unreachable')
        return False
    text = (restarted.stdout or '') + (restarted.stderr or '')
    if restarted.returncode == 0 and wait_for_fragments(host):
        log(f'{host}: capture restarted over ssh')
        return True
    # restart is stop then start: a stop that failed leaves the start undone,
    # so ask for it once before reporting the box as needing a physical check.
    started = ssh(host, f'{STREAM_INIT} start')
    started_text = '' if started is None else (started.stdout or '') + (started.stderr or '')
    if started is not None and started.returncode == 0 and wait_for_fragments(host):
        log(f'{host}: capture started over ssh')
        return True
    detail = (text.strip() or started_text.strip())[:140]
    log(f'{host}: no fragments after the restart: {detail}')
    return False


def recorder_pid(host):
    """The recorder process watching this box, taken from its command line."""
    try:
        result = subprocess.run(['ps', '-eo', 'pid,args'], capture_output=True,
                                text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in result.stdout.splitlines():
        if 'pix_stream_recorder' not in line or '--stb-ip ' + host not in line:
            continue
        try:
            return int(line.split()[0])
        except (IndexError, ValueError):
            return None
    return None


def staged_fragment(pid):
    """Newest staged fragment that recorder is writing, or None."""
    names = glob.glob('/dev/shm/pix_stream/staging_%d_*.fmp4' % pid)
    if not names:
        return None
    try:
        return max(names, key=lambda name: __import__('os').stat(name).st_mtime)
    except OSError:
        return None


def audio_packet_count(path, seconds=AUDIO_PROBE_SECONDS):
    """Audio packets in the first `seconds`; None when the probe fails."""
    probe = AUDIO_FFPROBE if __import__('os').path.exists(AUDIO_FFPROBE) else 'ffprobe'
    try:
        result = subprocess.run(
            [probe, '-v', 'error', '-select_streams', 'a',
             '-read_intervals', '%%+%d' % seconds,
             '-show_entries', 'packet=pts_time', '-of', 'csv=p=0', path],
            capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return sum(1 for line in result.stdout.splitlines() if line.strip())


def watch_audio(nodes, state, now, args, log=print):
    """Restart the capture when the live fragment carries no audio.

    A fragment is only judged once it holds enough data to contain audio, and
    only after two consecutive empty probes, so a capture that has just started
    is left alone.
    """
    restarted = 0
    for node in nodes:
        host = node.get('host')
        if host not in AUDIO_RESTART_HOSTS or node.get('enabled') is False:
            continue
        key = 'audio:' + (node.get('id') or host)
        entry = state.get(key) or {}
        if now - entry.get('last_attempt', 0) < args.restart_cooldown:
            continue
        pid = recorder_pid(host)
        fragment = staged_fragment(pid) if pid else None
        if fragment is None:
            state.pop(key, None)
            continue
        try:
            size = __import__('os').stat(fragment).st_size
            mtime = __import__('os').stat(fragment).st_mtime
        except OSError:
            continue
        if size < AUDIO_CHECK_MIN_BYTES or now - mtime > 120:
            state.pop(key, None)
            continue
        count = audio_packet_count(fragment)
        if count is None or count > 0:
            state.pop(key, None)
            continue
        entry['strikes'] = entry.get('strikes', 0) + 1
        state[key] = entry
        if entry['strikes'] < AUDIO_STRIKES:
            log(f'{node.get("id") or host}: fragment has no audio packets '
                f'({entry["strikes"]}/{AUDIO_STRIKES})')
            continue
        log(f'{node.get("id") or host}: capture carries no audio, restarting capture')
        if args.dry_run:
            continue
        entry['last_attempt'] = now
        entry['strikes'] = 0
        state[key] = entry
        if host in SSH_HOSTS and restart_stream(host, log):
            restarted += 1
        else:
            log(f'{node.get("id") or host}: no ssh recovery path, needs a physical check')
    return restarted


def load_state(path):
    try:
        with open(path, encoding='utf-8') as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return {}


def save_state(path, state):
    """Best effort: a read-only directory must not end the watch loop."""
    try:
        with open(path, 'w', encoding='utf-8') as handle:
            json.dump(state, handle, indent=2, sort_keys=True)
    except OSError as error:
        print(f'cannot save state to {path}: {error}', flush=True)


def cycle(state, args, now=None, log=print):
    """One pass over the node list. Returns the number of recoveries requested."""
    now = time.time() if now is None else now
    try:
        nodes = read_nodes(args.manager)
    except (OSError, ValueError) as error:
        log(f'manager unavailable: {error}')
        return 0
    reboots = 0
    # A silent capture looks healthy to every other check.
    watch_audio(nodes, state, now, args, log)
    # Every box at once is a network or manager problem, not four wedged
    # kernels. Restarting everything would turn a switch failure into four
    # interrupted recordings, so leave that case to a human.
    if nodes and all(unreachable(node) for node in nodes):
        log(f'all {len(nodes)} nodes unreachable: treating as a network fault, no action')
        return 0
    for node in nodes:
        action = decide(now, node, state, args.timeout, args.cooldown)
        if action == 'wait':
            key = node.get('id') or node.get('host')
            waited = int(now - state[key]['since'])
            if waited % 300 < args.interval:      # about once per five minutes
                log(f'{key}: unreachable for {waited}s (threshold {args.timeout}s)')
            continue
        if action != 'reboot':
            continue
        key = node.get('id') or node.get('host')
        log(f'{key}: unreachable for {int(now - state[key]["since"])}s, recovering')
        if args.dry_run:
            log(f'{key}: dry run, not restarting')
            continue
        host = node['host']
        if host in SSH_HOSTS and restart_stream(host, log):
            continue
        log(f'{key}: no ssh recovery path, needs a physical check')
    # A box that is reachable but has stopped producing fragments is a hung
    # encoder or capture agent, not a network fault. Restart it after a longer
    # threshold; a normal programme boundary does not stall for this long.
    if nodes and all(stalled(node) for node in nodes):
        log(f'all {len(nodes)} nodes stalled: treating as a network fault, no action')
        return reboots
    for node in nodes:
        action = decide_stalled(now, node, state, args.stalled_timeout, args.stalled_cooldown)
        if action == 'wait':
            key = node.get('id') or node.get('host')
            waited = int(now - state['stalled:' + key]['since'])
            if waited % 300 < args.interval:
                log(f'{key}: stalled for {waited}s (threshold {args.stalled_timeout}s)')
            continue
        if action != 'reboot':
            continue
        key = node.get('id') or node.get('host')
        log(f'{key}: stalled for {int(now - state["stalled:" + key]["since"])}s, recovering')
        if args.dry_run:
            log(f'{key}: dry run, not restarting')
            continue
        host = node['host']
        if host in SSH_HOSTS and restart_stream(host, log):
            continue
        log(f'{key}: no ssh recovery path, needs a physical check')
    return reboots


SELF_TEST_NODES = [
    {'id': 'a', 'host': '192.0.2.10', 'stbError': 'unreachable'},
    {'id': 'b', 'host': '192.0.2.11'},
    {'id': 'c', 'host': '192.0.2.12', 'state': 'stalled'},
    {'id': 'd', 'host': '192.0.2.13', 'enabled': False, 'stbError': 'unreachable'},
]


def self_test():
    """Decision table, without touching the network or a real box."""
    state = {}
    assert decide(0, SELF_TEST_NODES[0], state, 180, 900) == 'wait'
    assert decide(179, SELF_TEST_NODES[0], state, 180, 900) == 'wait'
    assert decide(180, SELF_TEST_NODES[0], state, 180, 900) == 'reboot'
    assert decide(181, SELF_TEST_NODES[0], state, 180, 900) == 'wait'      # cooldown
    assert decide(180 + 899, SELF_TEST_NODES[0], state, 180, 900) == 'wait'
    assert decide(180 + 900, SELF_TEST_NODES[0], state, 180, 900) == 'reboot'
    assert decide(400, SELF_TEST_NODES[1], state, 180, 900) is None       # reachable
    assert decide(400, SELF_TEST_NODES[2], state, 180, 900) is None       # no stbError: still answering
    assert decide(400, SELF_TEST_NODES[3], state, 180, 900) is None       # stopped on purpose
    assert decide(401, SELF_TEST_NODES[1], state, 180, 900) is None
    assert 'a' in state and 'b' not in state and 'd' not in state
    assert decide_stalled(0, SELF_TEST_NODES[2], state, 900, 3600) == 'wait'
    assert decide_stalled(900, SELF_TEST_NODES[2], state, 900, 3600) == 'reboot'
    assert decide_stalled(901, SELF_TEST_NODES[2], state, 900, 3600) == 'wait'
    assert decide_stalled(1000, SELF_TEST_NODES[1], state, 900, 3600) is None
    # A stalled node that briefly flips to recording without new fragments
    # keeps its original stall clock; a real pushed increment clears it.
    flaky = {'id': 'e', 'host': '192.0.2.14', 'state': 'stalled', 'pushed': 10}
    assert decide_stalled(0, flaky, state, 900, 3600) == 'wait'
    assert decide_stalled(901, dict(flaky, state='recording'),
                          state, 900, 3600) == 'reboot'
    assert decide_stalled(902, dict(flaky, state='recording', pushed=11),
                          state, 900, 3600) is None
    state.pop('a')                                                        # recovery clears
    assert decide(2000, SELF_TEST_NODES[1], state, 180, 900) is None
    # A restart is only real when fragments arrive; a restarted streamer
    # starts again at zero, which replaces the baseline instead of failing.
    assert advance(None, 5) == (False, 5)
    assert advance(10, 12) == (False, 10)
    assert advance(10, 13) == (True, 10)
    assert advance(10, 2) == (False, 2)
    assert advance(10, None) == (False, 10)
    print('self-test ok')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--manager', default=MANAGER)
    parser.add_argument('--state', default='/home/ku/pix-stream-20261101/pix-stb-guard.json')
    parser.add_argument('--timeout', type=int, default=DEFAULT_TIMEOUT,
                        help='seconds a node must be unreachable before a restart')
    parser.add_argument('--cooldown', type=int, default=DEFAULT_COOLDOWN,
                        help='seconds before another restart of the same node')
    parser.add_argument('--stalled-timeout', type=int, default=900,
                        help='seconds a node must produce no fragments before a restart')
    parser.add_argument('--stalled-cooldown', type=int, default=3600,
                        help='seconds before another restart of a stalled node')
    parser.add_argument('--interval', type=int, default=30)
    parser.add_argument('--restart-cooldown', type=int, default=120,
                        help='seconds before another capture restart for silent audio')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0

    state = load_state(args.state)
    while True:
        cycle(state, args)
        save_state(args.state, state)
        if args.once:
            return 0
        time.sleep(args.interval)


if __name__ == '__main__':
    sys.exit(main())
