#!/system/bin/sh
# Capture agent: turns the tuner and transcoder on and off on demand.
#
# The streamer owns the HTTP server and writes what a reader wants into
# /dev/shm/pix/capture.req. This agent reconciles the actual processes with that
# request, so the STB stays idle until the NAS asks for a channel and returns to
# idle once the reader has been gone long enough.
#
# Nothing is written to the flash: the request file, the fifo and the logs all
# live on tmpfs.

set -u

# The DT295 USB tuner re-enumerates whenever the root hub runtime-suspends.
# Keep every root hub powered; the tuner's own port is already "on".
for hub in /sys/bus/usb/devices/usb*/power/control; do
    [ -w "$hub" ] && echo on > "$hub" 2>/dev/null || true
done

APP=${PIX_APP:-/data/local/tmp/pix-stream}
# tmpfs root: /dev/shm on the box. Overridable so the decision logic can be
# exercised on a host (tests/test_dt295_agent.sh).
SHM=${PIX_SHM:-/dev/shm}
TUNE=$SHM/torne
CARD_RELAY=$APP/cardrelay
BCAS_CONF=$APP/bcas.conf
REMOTE_CARD=0
[ -f "$BCAS_CONF" ] && [ -x "$CARD_RELAY" ] && REMOTE_CARD=1
REQ=$SHM/pix/capture.req
SOCK_PORT=40775
PIX_BITRATE=${PIX_BITRATE:-5000000}
PIX_DST_W=${PIX_DST_W:-1920}
PIX_DST_H=${PIX_DST_H:-1080}
PIX_CODEC=${PIX_CODEC:-hevc}
PIX_RATE_MODE=${PIX_RATE_MODE:-vbr}
PIX_GOP_SEC=${PIX_GOP_SEC:-5}
# Optional adaptive sharpen in the GL bridge (0 disables it).
PIX_SHARPEN=${PIX_SHARPEN:-0}
# Optional encoded-frame ceiling. A box that cannot hold the broadcast rate
# sets this so decoded excess frames are dropped instead of backing up the fifo.
PIX_FPS_LIMIT=${PIX_FPS_LIMIT:-0}
# Adaptive frame ceiling: start at target, lower to floor under pressure.
PIX_FPS_TARGET=${PIX_FPS_TARGET:-0}
PIX_FPS_FLOOR=${PIX_FPS_FLOOR:-25}
PIX_ADAPTIVE_FPS=${PIX_ADAPTIVE_FPS:-0}
# Optional B-picture ceiling for encoders that cannot take post-decode drops.
PIX_B_FPS=${PIX_B_FPS:-0}
# Set to 1 to remux the broadcast video instead of encoding it on the VPU.
PIX_VIDEO_COPY=${PIX_VIDEO_COPY:-0}
PIX_NO_AVC_FALLBACK=${PIX_NO_AVC_FALLBACK:-0}
# RAM ring in front of the demux, in megabytes. The tuner produces faster than
# the encoder can always consume, and on the SMB400 any back pressure reaches
# the DMX ring, which answers by dropping transport packets (continuity errors
# and truncated AAC frames). The ring absorbs the stalls instead. 16 MB is
# about ten seconds of the multiplex, the same order as the torne queue.
PIX_RING_MB=${PIX_RING_MB:-16}
# torne-c's receive queue. 8MiB is only 3.6s of the multiplex, so a stall
# longer than that ends the session with receive_queue_full and costs a tuner
# restart, which is a 5-10 second hole in the recording. 32MiB buys 15s, and
# the transcoder pays the backlog back by dropping B pictures instead of
# pushing the stall down to the tuner.
PIX_TUNER_BUFFER_MIB=${PIX_TUNER_BUFFER_MIB:-32}
# torne-c's --buffer-mib accepts 1..32. A larger value makes it exit with
# rc=2 before it tunes, which looks exactly like a dead tuner: the agent
# restarts it every 20 s and the node stays stalled. Clamp here so a
# per-box experiment cannot take the recording down.
case "$PIX_TUNER_BUFFER_MIB" in
    ''|*[!0-9]*) PIX_TUNER_BUFFER_MIB=32 ;;
    *)
        [ "$PIX_TUNER_BUFFER_MIB" -lt 1 ] && PIX_TUNER_BUFFER_MIB=1
        [ "$PIX_TUNER_BUFFER_MIB" -gt 32 ] && PIX_TUNER_BUFFER_MIB=32
        ;;
esac

# Tuner backend. "torne" keeps the PS3 torne pair (torne-c + torne-b25);
# "dt295" drives a PIX-DT295 through pix-ts-legacy, which descrambles inside
# the tuner and can transcode on board. The optional config file holds the
# device-specific parts (assets path and the transcode register values), which
# are not kept in this repository.
PIX_TUNER=${PIX_TUNER:-torne}
DT295_CONF=${DT295_CONF:-/data/local/tmp/pix-dt295.conf}
if [ -r "$DT295_CONF" ]; then . "$DT295_CONF"; fi
DT295_ASSETS=${DT295_ASSETS:-$APP/assets}
DT295_SYSTEM=${DT295_SYSTEM:-terrestrial}
DT295_TRANSCODE=${DT295_TRANSCODE:-0}

# DT295 health. pix-ts-legacy only starts from firmware state 0x00 (cold) or
# 0x10 (resident, unauthenticated). Every other state is a dead end that looks
# exactly like a stalled tuner, and the old loop answered it by restarting
# every 15 seconds forever. dt295-state (tools/dt295_state_main.c) reads that
# word and turns it into a decision; without the binary the agent keeps the old
# behaviour and only loses the reason in the log.
DT295_STATE_BIN=${DT295_STATE_BIN:-$APP/dt295-state}
# pix-ts-legacy uploads the idle image on SIGTERM before it exits, which is what
# returns the device to 0x10. Give that time before the next start.
DT295_STOP_WAIT=${DT295_STOP_WAIT:-25}
# Optional operator recovery (a switched hub, a smart plug). It is only run
# when the state is confirmed tainted, at most once per DT295_RECOVER_EVERY
# seconds, and the result is always verified by probing again.
DT295_RECOVER_CMD=${DT295_RECOVER_CMD:-}
DT295_RECOVER_EVERY=${DT295_RECOVER_EVERY:-600}
# Empty for the daemon. "decide" sets a suffix so a simulated decision cannot
# overwrite the state file or the evidence bundle of the running agent.
DT295_STATE_SUFFIX=${DT295_STATE_SUFFIX:-}

# The SMB400 is its own tuner: smb400-tuner-oem.sh powers the OEM frontend,
# waits for the lock, hands ACAS over to b21dec and writes the already
# descrambled transport stream on stdout. There is no torne pair, no card
# socket and no separate descrambler stage on this box.
SMB400_TUNER=${SMB400_TUNER:-/data/local/tmp/smb400-tuner-oem.sh}

# Per-box encoder tuning, written at deploy time (deploy-pipeline.sh
# encode-conf). A box whose encoder cannot sustain 1080p30 can run a lighter
# profile without touching the shared defaults.
[ -f "$APP/encode.conf" ] && . "$APP/encode.conf"

BITRATE=$PIX_BITRATE
WIDTH=$PIX_DST_W
HEIGHT=$PIX_DST_H
CODEC=$PIX_CODEC
RATE_MODE=$PIX_RATE_MODE
GOP_SEC=$PIX_GOP_SEC

log() {
    echo "$(date +%H:%M:%S) $*" >>"$SHM/capture-agent.log"
}

# Only one agent may run: two of them race and start duplicate tuners.
# The pid file survives a reboot while pids are reused, so the command line is
# checked as well; otherwise a stale file silently disables the agent.
#
# The guard and the setup below belong to the daemon. A query (state, evidence,
# decide) must keep working while the daemon runs, and must never create or
# sweep anything.
if [ $# -eq 0 ]; then
    PIDFILE=$APP/agent.pid
    if [ -f "$PIDFILE" ]; then
        old=$(cat "$PIDFILE" 2>/dev/null || true)
        if [ -n "$old" ] && [ "$old" != "$$" ] && kill -0 "$old" 2>/dev/null \
           && grep -q "capture-agent" "/proc/$old/cmdline" 2>/dev/null; then
            exit 0
        fi
    fi
    echo $$ > "$PIDFILE"

    mkdir -p "$SHM/pix" "$TUNE"
    chmod 755 "$SHM" "$TUNE" 2>/dev/null || true
    # The transcoder runs as an ordinary app uid and mirrors its session state
    # into $SHM/pix; the agent itself only needs the fifo.
    chmod 777 "$SHM/pix" 2>/dev/null || true
fi
# toybox on this Android build does not link every applet into /system/bin,
# and `mkfifo` is one of the missing ones. Asking toybox explicitly works; the
# plain name is kept first so a box that has the applet uses it.
make_fifo() {
    mkfifo "$1" 2>/dev/null || toybox mkfifo "$1" 2>/dev/null \
        || /system/bin/toybox mkfifo "$1" 2>/dev/null \
        || mknod "$1" p 2>/dev/null
}

# The tuner writes with a plain shell redirection (`> clear.fifo`). If the path
# is missing at that moment the shell creates a *regular file* instead of a
# fifo, the transcoder can no longer open it (permission denied, root-owned)
# and the stream silently fills RAM. Re-create it whenever it is not a fifo.
ensure_fifo() {
    for f in "$TUNE/raw.fifo" "$TUNE/clear.fifo"; do
        if [ ! -p "$f" ]; then
            rm -f "$f"
            make_fifo "$f" || log "cannot create fifo $f"
        fi
        chmod 666 "$f" 2>/dev/null || true
    done
}
[ $# -eq 0 ] && ensure_fifo

# Kill fifo holders left behind by a previous agent. They keep the write end of
# clear.fifo open, so the transcoder never sees end of file and never returns
# to re-initialise its codecs between sessions.
sweep_holders() {
    for p in /proc/[0-9]*; do
        [ "$(cat "$p/comm" 2>/dev/null)" = "sleep" ] || continue
        case "$(readlink "$p/fd/1" 2>/dev/null)" in
            "$TUNE/clear.fifo") kill -9 "${p#/proc/}" 2>/dev/null ;;
        esac
    done
}

[ $# -eq 0 ] && sweep_holders

# Kills every process whose command line matches the pattern.
#
# awk is not on every box: the PIX-SMB400's toybox has no awk applet, so
# `ps -ef | awk '{print $2}'` returned nothing and every pid lookup failed
# (the tuner looked dead right after it started and was restarted in a loop).
pid_column() {
    ps -ef 2>/dev/null | grep -E "$1" | grep -v grep \
        | while read -r _uid pid _rest; do echo "$pid"; done
}

kill_all() {
    for p in $(pid_column "$1"); do
        kill -9 "$p" 2>/dev/null
    done
}

tuner_pids() {
    if [ "$PIX_TUNER" = smb400 ]; then
        pid_column 'smb400-tuner-oem|tuner-stream-ng|tuner-stream-bs|b21dec'
        return
    fi
    pid_column 'torne-c|torne-b25|cardrelay|pix-ts-legacy'
}

# Both halves are needed. With a local reader torne-c also serves the card
# socket; with a remote card cardrelay owns it instead.
tuner_alive() {
    if [ "$PIX_TUNER" = dt295 ]; then
        pidof pix-ts-legacy >/dev/null 2>&1
        return $?
    fi
    if [ "$PIX_TUNER" = smb400 ]; then
        # The wrapper exits when the tuner stops, so a live decryptor or a
        # live tuner stream is the signal that the frontend is still owned.
        [ -n "$(tuner_pids)" ]
        return $?
    fi
    pidof torne-c >/dev/null 2>&1 && pidof torne-b25 >/dev/null 2>&1 || return 1
    [ "$REMOTE_CARD" = 0 ] || pidof cardrelay >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
# DT-295 health

# Prints cold / unauthenticated / idle / receiver / uninitialized / unknown, or
# nothing when the probe is not installed. Cheap enough to call on a transition
# but not once a second: it opens the device node.
dt295_state() {
    [ "$PIX_TUNER" = dt295 ] || return 0
    # Simulation hook for "capture-agent.sh decide" and the host test: it lets
    # an operator ask what the agent would do for a given state without a
    # device in that state.
    if [ -n "${DT295_FAKE_STATE:-}" ]; then
        echo "$DT295_FAKE_STATE"
        return 0
    fi
    [ -x "$DT295_STATE_BIN" ] || return 0
    "$DT295_STATE_BIN" --quiet 2>/dev/null
}

dt295_state_detail() {
    [ -x "$DT295_STATE_BIN" ] || return 0
    "$DT295_STATE_BIN" --format kv --full 2>/dev/null
}

# Writes what the agent knows about the device for on-box inspection:
#   /dev/shm/pix/dt295.state
# state=(probe) action=start|retry|replug|busy|absent failures=N
dt295_write_status() {
    mkdir -p "$SHM/pix" 2>/dev/null
    {
        echo "state=${1:-unknown}"
        echo "action=${2:-none}"
        echo "failures=${3:-0}"
        echo "fault=${dt295_fault:-0}"
        echo "probe=$DT295_STATE_BIN"
        echo "updated=$(date +%s)"
    } >"$SHM/pix/dt295.state$DT295_STATE_SUFFIX" 2>/dev/null
}

# Logs at most once a minute: the loop calls this every second while it holds a
# restart back.
dt295_say() {
    now=$1
    shift
    [ "$now" -lt "${dt295_last_log:-0}" ] && return 0
    log "$*"
    dt295_last_log=$((now + 60))
}

# Collects the state, the logs and the session files once, when the agent
# decides it cannot recover on its own. The evidence is what makes a remote
# "recording stopped" report actionable instead of a guess.
dt295_evidence() {
    mkdir -p "$SHM/pix" 2>/dev/null
    out="$SHM/pix/dt295-evidence$DT295_STATE_SUFFIX.txt"
    {
        echo "== dt295 evidence: $(date)${DT295_STATE_SUFFIX:+ (simulated:$DT295_STATE_SUFFIX)} =="
        echo "probe: $(dt295_state_detail)"
        echo "pix-ts-legacy: $(pidof pix-ts-legacy 2>/dev/null)"
        echo "agent pid: $(cat "$APP/agent.pid" 2>/dev/null)  query pid: $$  channel: ${current_channel:-0}"
        echo
        echo "-- live.conf --"
        cat "$APP/live.conf" 2>/dev/null
        echo "-- device conf --"
        grep -E '^(PIX_TUNER|DT295_)' "$DT295_CONF" 2>/dev/null
        echo "-- status --"
        cat "$SHM/pix/dt295.state$DT295_STATE_SUFFIX" 2>/dev/null
        echo "-- fifo --"
        ls -l "$TUNE/clear.fifo" 2>/dev/null
        echo "-- agent log (40) --"
        tail -n 40 "$SHM/capture-agent.log" 2>/dev/null
        echo "-- tuner log (20) --"
        tail -n 20 "$TUNE/dt295.log" 2>/dev/null
    } >"$out" 2>&1
    log "dt295: wrote $out"
}

# Runs DT295_RECOVER_CMD once per DT295_RECOVER_EVERY seconds, then re-probes.
# Nothing is assumed about the command: a power cycle either brings the device
# back to cold or it does not, and the probe says which.
dt295_try_recover() {
    now=$1
    state=$2
    [ -n "$DT295_RECOVER_CMD" ] || return 0
    if [ -n "${dt295_recover_at:-}" ] \
       && [ "$now" -lt $((dt295_recover_at + DT295_RECOVER_EVERY)) ]; then
        return 0
    fi
    dt295_recover_at=$now
    log "dt295: running DT295_RECOVER_CMD (state=$state)"
    sh -c "$DT295_RECOVER_CMD" >>"$SHM/capture-agent.log" 2>&1
    sleep 5
    log "dt295: recovery command finished, state=$(dt295_state)"
}

# Decides whether the restart loop may start the tuner again. The other tuners
# keep the old unconditional behaviour.
#
#   0  restart now
#   1  hold (backoff, taint, or no device)
dt295_restart_allowed() {
    [ "$PIX_TUNER" = dt295 ] || return 0
    now=$(date +%s)

    if [ "${dt295_fault:-0}" = 1 ]; then
        # The latch only clears when the device is provably startable again, so
        # a re-plug is detected without any operator action.
        state=$(dt295_state)
        case "$state" in
            cold|unauthenticated)
                log "dt295: device is startable again (state=$state); clearing the fault latch"
                dt295_fault=0
                dt295_failures=0
                dt295_next_try=0
                dt295_write_status "$state" start 0
                return 0
                ;;
            absent|"")
                dt295_say "$now" "dt295: waiting for the device to re-enumerate"
                return 1
                ;;
            *)
                dt295_say "$now" "dt295: still state=$state; holding the restart (cold boot for 0x11, owner cleanup for receiver states)"
                dt295_try_recover "$now" "$state"
                return 1
                ;;
        esac
    fi

    if [ "$now" -lt "${dt295_next_try:-0}" ]; then
        dt295_say "$now" "dt295: holding the restart for $((dt295_next_try - now))s"
        return 1
    fi

    state=$(dt295_state)
    case "$state" in
        absent)
            # The probe answered, so the device really is not on the bus. There
            # is nothing to start; retry cheaply until it enumerates again.
            dt295_say "$now" "dt295: no device on the bus; waiting for it to enumerate"
            dt295_write_status "$state" absent 0
            return 1
            ;;
        idle)
            # 0x11: the resident firmware is already running, so pix-ts-legacy
            # refuses to boot it and dt295_shutdown needs an authenticated
            # session it no longer has. Only a cold boot clears this.
            dt295_fault=1
            dt295_failures=$(( ${dt295_failures:-0} + 1 ))
            log "dt295: device is idle (0x11): the previous session never ran dt295_shutdown; a start needs a cold boot, holding the restart"
            dt295_write_status "$state" replug "$dt295_failures"
            dt295_evidence
            dt295_try_recover "$now" "$state"
            return 1
            ;;
        receiver)
            # Streaming with no capture of ours alive: another owner holds it.
            dt295_fault=1
            log "dt295: device is streaming but no pix-ts-legacy of ours is alive; holding the restart"
            dt295_write_status "$state" busy 0
            dt295_evidence
            return 1
            ;;
    esac

    # A start that never reaches two minutes of service is a failure, not a
    # channel change. Back off exponentially (15 s .. 300 s) so a broken assets
    # bundle, a bad register profile or a dead transponder cannot turn into a
    # restart every 15 seconds. Two minutes is well past the tune, the PMT and
    # the first key renewal, so a tuner that survives it is treated as healthy.
    lived=$((now - ${started_at:-0}))
    if [ "$lived" -lt 120 ]; then
        dt295_failures=$(( ${dt295_failures:-0} + 1 ))
    else
        dt295_failures=0
    fi
    if [ "$dt295_failures" -gt 0 ]; then
        delay=$((15 * (1 << (dt295_failures - 1))))
        [ "$delay" -gt 300 ] && delay=300
        dt295_next_try=$((now + delay))
        log "dt295: the tuner lived ${lived}s (failure $dt295_failures); next attempt in ${delay}s"
        dt295_write_status "${state:-unknown}" retry "$dt295_failures"
    else
        dt295_next_try=0
        dt295_write_status "${state:-unknown}" start 0
    fi
    return 0
}

# The writer subprocesses share the executable name. Only signal the capture
# parent, which stops the receiver before draining and closing its writers.
stop_dt295() {
    dp=''
    if [ -f "$APP/dt295.pid" ]; then
        dp=$(cat "$APP/dt295.pid" 2>/dev/null)
    fi
    rm -f "$APP/dt295.pid"
    [ -n "${dp:-}" ] || return 0
    grep -q 'pix-ts-legacy' "/proc/$dp/cmdline" 2>/dev/null || return 0

    # SIGTERM, never SIGKILL: the capture loop then runs dt295_shutdown(), which
    # uploads the idle image and returns the device to 0x10. A killed capture
    # leaves 0x11 behind and the next start fails until the cable is re-plugged.
    kill "$dp" 2>/dev/null
    [ -x "$DT295_STATE_BIN" ] || return 0

    waited=0
    state=''
    while [ "$waited" -lt "$DT295_STOP_WAIT" ]; do
        state=$(dt295_state)
        case "$state" in
            cold|unauthenticated) break ;;
        esac
        sleep 1
        waited=$((waited + 1))
    done
    case "$state" in
        unauthenticated|cold)
            log "dt295: stopped cleanly after ${waited}s (state=$state)"
            ;;
        idle)
            # The shutdown did not complete: the next start cannot recover from
            # this without a cold boot. Say so now instead of 15 seconds later.
            log "dt295: capture exited but the device stayed idle (0x11); a start needs a cold boot"
            dt295_fault=1
            dt295_write_status "$state" replug "${dt295_failures:-0}"
            ;;
        *)
            log "dt295: capture stopped with the device in state=${state:-unknown} after ${waited}s"
            ;;
    esac
    return 0
}

# The wrapper owns the OEM frontend and the ACAS pairing: stopping it releases
# both, so no separate stop command is needed. SIGTERM (not SIGKILL) so the
# wrapper's trap can tear the chroot pipeline down and dmx_deinit() runs.
stop_smb400() {
    if [ -f "$APP/smb400.pid" ]; then
        sp=$(cat "$APP/smb400.pid" 2>/dev/null)
        if [ -n "$sp" ] && kill -0 "$sp" 2>/dev/null; then
            kill "$sp" 2>/dev/null
        fi
        rm -f "$APP/smb400.pid"
    fi
    for p in $(tuner_pids); do
        kill "$p" 2>/dev/null
    done
}

# The holder keeps the write end of clear.fifo open so the transcoder can open
# the fifo before the tuner produces anything. It must be killed when the
# session ends: while it lives the fifo never reports end of file, so the
# transcoder never returns and never re-initialises its codecs. Leftovers used
# to pile up one per start (five were found on the second unit).
#
# holder.pid alone is not enough: an agent restart leaves the old sleep running
# with no pid file pointing at it, and the reader then never sees EOF. Find the
# holders from /proc so a leftover can be adopted or killed.
fifo_holder_pids() {
    # One ls for every fd; a per-fd readlink loop takes a minute on this box
    # (about 1800 fds) and blocks the agent's once-a-second loop. sed is used
    # instead of awk because toybox on the SMB400 has no awk applet.
    for hp in $(ls -l /proc/[0-9]*/fd/* 2>/dev/null \
                | grep -F " -> $TUNE/clear.fifo" \
                | sed 's|/proc/\([0-9][0-9]*\)/fd/.*|\1|' \
                | sort -u); do
        [ "$(cat "/proc/$hp/comm" 2>/dev/null)" = "sleep" ] && echo "$hp"
    done
}

release_holder() {
    if [ -f "$APP/holder.pid" ]; then
        kill "$(cat "$APP/holder.pid" 2>/dev/null)" 2>/dev/null
        rm -f "$APP/holder.pid"
    fi
    # Kill every leftover holder, not just the recorded one: an orphan from an
    # earlier agent still keeps the reader from seeing end of file.
    for hp in $(fifo_holder_pids); do
        kill "$hp" 2>/dev/null
    done
}

# Keeps the write end of clear.fifo open so the transcoder can open the fifo
# before the tuner produces anything. Exactly one is kept for the agent's whole
# life: killing and recreating it on every tuner restart made the transcoder see
# end of file, drop its session, and kill the tuner again -- a loop that kept
# both boxes restarting once a minute.
ensure_holder() {
    if [ -f "$APP/holder.pid" ]; then
        hp=$(cat "$APP/holder.pid" 2>/dev/null)
        if [ -n "$hp" ] && kill -0 "$hp" 2>/dev/null; then
            return 0
        fi
    fi
    # Adopt a holder that survived an agent restart instead of adding another.
    hp=$(fifo_holder_pids | head -n 1)
    if [ -n "$hp" ]; then
        echo "$hp" > "$APP/holder.pid"
        return 0
    fi
    ( sleep 86400 ) >"$TUNE/clear.fifo" 2>/dev/null &
    echo $! > "$APP/holder.pid"
}

# Waits until the app has closed its read end of the fifo. Creating the next
# writer while the old reader still has it open can lose the EOF, and then the
# channel/PES state stays stale.
wait_for_fifo_reader_close() {
    reader_pid=$1
    limit=$2
    waited=0
    while [ -d "/proc/$reader_pid/fd" ]; do
        found=0
        for fd in /proc/"$reader_pid"/fd/*; do
            [ "$(readlink "$fd" 2>/dev/null)" = "$TUNE/clear.fifo" ] && found=1
        done
        [ "$found" = 0 ] && return 0
        [ "$waited" -ge "$limit" ] && return 1
        sleep 1
        waited=$((waited + 1))
    done
    return 0
}

stop_capture() {
    # Always drop the holder and stop leftover tuner/card processes, even when
    # this agent thinks it is idle. A previous run may have left them behind,
    # and the next start would then fail on a busy card socket.
    release_holder
    [ "$current_channel" = 0 ] || log "stopping capture on channel $current_channel"
    # Only the tuner is stopped. The reader then sees end of file, the streamer
    # releases the codecs and waits for the next session. Force-stopping the app
    # would leave the single instance hardware decoder allocated.
    kill_all 'torne-c'
    kill_all 'torne-b25'
    kill_all 'cardrelay'
    stop_dt295
    stop_smb400
    # Wait until they really are gone before a new one can start.
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -z "$(tuner_pids)" ] && break
        sleep 1
    done
    # A leftover card socket makes the next start fail with EADDRINUSE.
    rm -f "$TUNE/card.sock"
    current_channel=0
}

# Format of the running session. Needed to restart the tuner with the same
# settings when it drops, instead of falling back to the box defaults.
current_codec=""
current_width=""
current_height=""
current_bitrate=""
current_mode=""
current_gop=""

# Starts the tuner and descrambler. In remote mode cardrelay owns the local
# card socket and torne-c only tunes; otherwise torne-c serves the card itself.
start_tuner_pair() {
    channel=$1
    rm -f "$TUNE/card.sock"
    if [ "$REMOTE_CARD" = 1 ]; then
        "$CARD_RELAY" --config "$BCAS_CONF" >>"$TUNE/cardrelay.log" 2>&1 &
        "$APP/torne-c" --channel "$channel" --buffer-mib "$PIX_TUNER_BUFFER_MIB" \
            </dev/null >"$TUNE/raw.fifo" 2>>"$TUNE/torne.log" &
    else
        "$APP/torne-c" --channel "$channel" --buffer-mib "$PIX_TUNER_BUFFER_MIB" \
            --card-socket "$TUNE/card.sock" \
            </dev/null >"$TUNE/raw.fifo" 2>>"$TUNE/torne.log" &
    fi
    "$APP/torne-b25" --card-socket "$TUNE/card.sock" \
        <"$TUNE/raw.fifo" >"$TUNE/clear.fifo" 2>>"$TUNE/b25.log" &
}

# Starts the DT-295 for one channel.
#
# pix-ts-legacy writes the finished transport stream: the tuner already removed
# the B-CAS scrambling, so torne-b25 and the card socket are not used. With
# DT295_TRANSCODE=1 the on-board H.264 encoder is enabled through the two
# register hooks and the stream carries H.264 instead of MPEG-2.
start_dt295() {
    channel=$1

    # Only two firmware states accept a start. Starting from 0x11 (or from a
    # receiver state nobody of ours owns) ends in a doomed process, a log line
    # 15 seconds later and another attempt: the restart loop that made a
    # tainted device look dead. Refuse here and say which state was seen.
    if [ -x "$DT295_STATE_BIN" ]; then
        state=$(dt295_state)
        case "$state" in
            idle)
                log "dt295: refusing to start on $channel: device is idle (0x11); a start needs a cold boot (re-plug the USB)"
                dt295_fault=1
                dt295_write_status "$state" replug "${dt295_failures:-0}"
                return 1
                ;;
            receiver)
                log "dt295: refusing to start on $channel: the receiver is already active"
                dt295_fault=1
                dt295_write_status "$state" busy 0
                return 1
                ;;
            uninitialized|unknown)
                log "dt295: device state is $state; starting anyway so pix-ts-legacy can report it"
                ;;
        esac
    fi

    log "dt295: starting on $channel, device state=${state:-unknown}"
    set -- --model dt295 --assets "$DT295_ASSETS"
    # The channel comes from the NAS: "17" is terrestrial (T17), "BS15_0" or
    # "CS..." is a satellite transponder, passed to pix-ts-legacy as-is.
    case "$channel" in
        BS*)
            set -- "$@" -s bs -c "$channel"
            ;;
        CS*)
            set -- "$@" -s cs -c "$channel"
            ;;
        *)
            set -- "$@" -c "T$channel"
            ;;
    esac
    if [ -f "$SHM/pix/epg.dump" ]; then
        # Guide scan: keep a copy of the transport stream this session already
        # produces. The DT-295 receives the satellite here, so a guide that only
        # walked the SMB400 channels would never see BS11.
        mkdir -p "$SHM/pix/epg" 2>/dev/null || true
        "$APP/pix-ts-legacy" "$@" -t - -o - \
            </dev/null 2>>"$TUNE/dt295.log" \
            | tee "$TUNE/clear.fifo" "$SHM/pix/epg/$channel.ts" >/dev/null &
    elif [ "$DT295_TRANSCODE" = 1 ]; then
        DT295_TOLERATE_PSI=1 DT295_XREG="$DT295_XREG" DT295_XREG2="$DT295_XREG2" \
            "$APP/pix-ts-legacy" "$@" -t - -o - \
            </dev/null >"$TUNE/clear.fifo" 2>>"$TUNE/dt295.log" &
    else
        "$APP/pix-ts-legacy" "$@" -t - -o - \
            </dev/null >"$TUNE/clear.fifo" 2>>"$TUNE/dt295.log" &
    fi
    echo $! > "$APP/dt295.pid"
    dt295_write_status "${state:-unknown}" start 0
}

# Starts the SMB400 internal tuner for one channel.
#
# The wrapper prints the descrambled transport stream on stdout, so it is the
# writer of clear.fifo itself: no torne-b25 stage and no card socket. Channels
# arrive in the NAS notation -- "20" is a terrestrial physical channel and
# "BS15_0" is a satellite transponder.
start_smb400() {
    channel=$1
    # A stale frontend handle from the previous session makes the next Tune
    # return 4 (no stream); the wrapper drops the saved tuning state itself
    # only for BS, so drop it here for every band.
    rm -f /data/local/tmp/.tuner_state
    # The wrapper's stdout is redirected onto the fifo. If the path is missing
    # the shell creates a regular file instead and the stream piles up in RAM
    # (a 300MB one was found after a retune). Guarantee the fifo before the
    # redirection is set up.
    if [ ! -p "$TUNE/clear.fifo" ]; then
        rm -f "$TUNE/clear.fifo"
        make_fifo "$TUNE/clear.fifo" || { log "cannot create $TUNE/clear.fifo"; return 1; }
        chmod 666 "$TUNE/clear.fifo" 2>/dev/null || true
    fi
    # Only bytes written from now on say whether this attempt authenticated.
    acas_log_offset=$(wc -c < "$TUNE/smb400.log" 2>/dev/null | tr -d ' ')
    [ -n "$acas_log_offset" ] || acas_log_offset=0
    # A guide scan copies the same transport stream the recording uses. It must
    # not open the tuner itself: the frontend and the ACAS handshake only come up
    # through this session, and a second wrapper ends with "b21dec: SetData
    # err=-1" and no bytes at all. Teeing the running stream keeps one tuner.
    if [ -f "$SHM/pix/epg.dump" ]; then
        mkdir -p "$SHM/pix/epg" 2>/dev/null || true
        "$SMB400_TUNER" "$channel" \
            </dev/null 2>>"$TUNE/smb400.log" \
            | tee "$TUNE/clear.fifo" "$SHM/pix/epg/$channel.ts" >/dev/null &
    else
        "$SMB400_TUNER" "$channel" \
            </dev/null >"$TUNE/clear.fifo" 2>>"$TUNE/smb400.log" &
    fi
    echo $! > "$APP/smb400.pid"
}

# True once b21dec has authenticated against the ACAS chip during this tune.
# Until it does, the tuner produces scrambled bytes and the transcoder finds no
# stream at all, so a failed authentication is worth one more attempt.
acas_ready() {
    tail -c "+$(( ${acas_log_offset:-0} + 1 ))" "$TUNE/smb400.log" 2>/dev/null \
        | grep -q "ACAS conventional init OK"
}

start_tuner() {
    case "$PIX_TUNER" in
        dt295)  start_dt295 "$1" ;;
        smb400) start_smb400 "$1" ;;
        *)      start_tuner_pair "$1" ;;
    esac
}

# Restarts just the tuner pair. The transcoder session, live.conf and the fifo
# holder are left alone: a dropped tuner is not a reason to restart the session.
restart_tuner() {
    channel=$1
    log "restarting the tuner on channel $channel"
    kill_all 'torne-c'
    kill_all 'torne-b25'
    kill_all 'cardrelay'
    stop_dt295
    stop_smb400
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -z "$(tuner_pids)" ] && break
        sleep 1
    done
    start_tuner "$channel"
    started_at=$(date +%s)
}

start_capture() {
    channel=$1
    want_codec=${2:-}
    want_w=${3:-}
    want_h=${4:-}
    want_bitrate=${5:-}
    want_mode=${6:-}
    want_gop=${7:-}

    # The reader may ask for a format. Without one the box keeps its own
    # defaults from encode.conf / the built-ins.
    eff_codec=${want_codec:-$CODEC}
    eff_w=${want_w:-$WIDTH}
    eff_h=${want_h:-$HEIGHT}
    eff_bitrate=${want_bitrate:-$BITRATE}
    eff_mode=${want_mode:-$RATE_MODE}
    eff_gop=${want_gop:-$GOP_SEC}
    # Some NAS requests carry a smaller per-service size (for example SBS3's
    # 1280x720). A box that must keep its native 1440x1080 can opt out of the
    # request with PIX_FORCE_DST=1 in encode.conf. The SBS3 partial on
    # 2026-11-06 was captured as 1280x720 and failed to decode, so .57 uses
    # this switch to keep the broadcast-native size.
    if [ "${PIX_FORCE_DST:-0}" = 1 ]; then
        eff_w=$WIDTH
        eff_h=$HEIGHT
    fi
    if [ "$PIX_TUNER" = dt295 ] && [ "$DT295_TRANSCODE" = 1 ]; then
        eff_codec=avc
    fi

    # Same channel and same format means there is nothing to restart.
    live_ts=$(sed -n 's/^tsPath=//p' "$APP/live.conf" 2>/dev/null | head -1)
    if [ "$channel" = "$current_channel" ] \
       && [ "$eff_codec" = "$current_codec" ] \
       && [ "$eff_w" = "$current_width" ] \
       && [ "$eff_h" = "$current_height" ] \
       && [ "$eff_bitrate" = "$current_bitrate" ] \
       && [ "$eff_mode" = "$current_mode" ] \
       && [ "$eff_gop" = "$current_gop" ] \
       && [ "$live_ts" = "$TUNE/clear.fifo" ]; then
        return 0
    fi
    # Wait for the old Java session to close its FIFO before creating a new
    # writer. Otherwise EOF can be missed and channel/PES state stays stale.
    old_reader_pid=''
    if [ "$current_channel" != 0 ] || tuner_alive; then
        set -- $(pidof com.example.dtvandroid 2>/dev/null)
        old_reader_pid=${1:-}
    fi
    stop_capture
    # The wait must be bounded and must not turn into a permanent refusal:
    # a reader that reopens the fifo blocks inside open() until a writer
    # appears, and the kernel already owns a descriptor for that pending
    # open. On the SMB400 that made the "reader still holds clear.fifo"
    # check fail forever, so a format change stopped the session and nothing
    # ever started again. Proceed after the limit and log it instead.
    fifo_wait=30
    [ "$PIX_TUNER" = smb400 ] && fifo_wait=5
    if [ -n "$old_reader_pid" ] && ! wait_for_fifo_reader_close "$old_reader_pid" "$fifo_wait"; then
        log "old Java session still holds clear.fifo after ${fifo_wait}s; starting anyway"
    fi

    log "starting capture on channel $channel ($eff_codec ${eff_w}x${eff_h} ${eff_bitrate}bps $eff_mode gop=${eff_gop}s)"
    cat > "$APP/live.conf" << EOF
tsPath=$TUNE/clear.fifo
sinkPort=$SOCK_PORT
dstW=$eff_w
dstH=$eff_h
codec=$eff_codec
bitrate=$eff_bitrate
bitrateMode=$eff_mode
gopSec=$eff_gop
maxSeconds=0
sharpen=$PIX_SHARPEN
bufferMb=$PIX_RING_MB
fields=${PIX_FIELDS:-auto}
ptsMode=${PIX_PTS_MODE:-source}
deqMs=${PIX_DEQ_MS:-0}
nv21=${PIX_NV21:-0}
decoderMime=${PIX_DECODER_MIME:-video/mpeg2}
pesDepth=${PIX_PES_DEPTH:-1}
decOutMs=${PIX_DEC_OUT_MS:-10000}
fpsLimit=${PIX_FPS_LIMIT:-0}
fpsTarget=${PIX_FPS_TARGET:-0}
fpsFloor=${PIX_FPS_FLOOR:-25}
adaptiveFps=${PIX_ADAPTIVE_FPS:-0}
bFps=${PIX_B_FPS:-0}
noAvcFallback=${PIX_NO_AVC_FALLBACK:-0}
dumpTs=${PIX_DUMP_TS:-}
dumpBytes=${PIX_DUMP_BYTES:-8388608}
EOF
    # The DT-295 already emits H.264, so the session must remux instead of
    # decoding MPEG-2 and encoding on the VPU.
    if [ "$PIX_TUNER" = dt295 ] && [ "$DT295_TRANSCODE" = 1 ]; then
        echo "videoCodec=copy" >> "$APP/live.conf"
    fi
    if [ "${PIX_VIDEO_COPY:-0}" = 1 ]; then
        echo "videoCodec=copy" >> "$APP/live.conf"
    fi
    # A decoder that contradicts the stream leaves the app decoding nothing:
    # the tuner then restarts in a loop and the recording never starts. The
    # setting used to live in a per-box file, so check what was written rather
    # than what was meant to be written.
    session_decoder=$(sed -n 's/^decoderMime=//p' "$APP/live.conf" 2>/dev/null | head -1)
    if [ -n "$session_decoder" ]; then
        case "$eff_codec:$session_decoder" in
            hevc:video/hevc|avc:video/avc|*:video/mpeg2) ;;
            *)
                log "decoderMime $session_decoder contradicts codec $eff_codec; removing it"
                grep -v '^decoderMime=' "$APP/live.conf" > "$APP/live.conf.tmp" && \
                    mv "$APP/live.conf.tmp" "$APP/live.conf"
                ;;
        esac
    fi
    chmod 644 "$APP/live.conf"
    cp "$APP/live.conf" /data/local/tmp/live.conf 2>/dev/null || true

    # Ending the previous session is what makes the transcoder reload
    # live.conf, so only do it when something actually changed.
    release_holder
    sleep 1
    ensure_holder

    if [ -z "$(pidof com.example.dtvandroid)" ]; then
        am start -n com.example.dtvandroid/.MainActivity >/dev/null 2>&1
    fi

    # Give the transcoder time to configure the codecs before the tuner writes.
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        if grep -q "Java client connected" "$SHM/streamer.log" 2>/dev/null; then
            break
        fi
        sleep 1
    done
    sleep 2

    # A socket left behind by a previous tuner makes the next start fail with
    # EADDRINUSE (ERROR stage=card_socket status=98) even though nothing is
    # listening on it any more. stop_capture only cleans up when it thinks a
    # capture is running, so an idle restart leaves the file behind.
    rm -f "$TUNE/card.sock"

    # The reader may have removed it, and the tuner's redirection would then
    # create a regular file instead of a fifo.
    ensure_fifo

    start_tuner "$channel"

    current_channel=$channel
    current_codec=$eff_codec
    current_width=$eff_w
    current_height=$eff_h
    current_bitrate=$eff_bitrate
    current_mode=$eff_mode
    current_gop=$eff_gop
    started_at=$(date +%s)
    acas_retry=0
    log "capture started on channel $channel"
}

current_channel=0
started_at=0
acas_retry=0
# DT295 restart policy: a taint latch, a failure count and the next allowed try.
dt295_fault=0
dt295_failures=0
dt295_next_try=0
dt295_last_log=0
dt295_recover_at=''

# Manual entry points. The init service starts this script with no arguments,
# so anything else is an operator asking a question on the box.
case "${1:-}" in
    state|--state)
        echo "probe: $(dt295_state_detail)"
        echo "pix-ts-legacy: $(pidof pix-ts-legacy 2>/dev/null)"
        echo "status file: $(cat "$SHM/pix/dt295.state" 2>/dev/null | tr '\n' ' ')"
        case "$(dt295_state)" in
            cold|unauthenticated) exit 0 ;;
            idle) exit 2 ;;
            receiver) exit 3 ;;
            absent|"") exit 4 ;;
            *) exit 7 ;;
        esac
        ;;
    evidence|--evidence)
        # Keep the previous bundle: the one that matters is usually the fault
        # snapshot, and asking for a new one must not destroy it.
        if [ -f "$SHM/pix/dt295-evidence.txt" ]; then
            cp "$SHM/pix/dt295-evidence.txt" "$SHM/pix/dt295-evidence.prev.txt" 2>/dev/null
        fi
        dt295_evidence
        cat "$SHM/pix/dt295-evidence.txt" 2>/dev/null
        exit 0
        ;;
    decide|--decide)
        # Answers "what would the agent do if the device were in state X?".
        # The state list may hold several comma separated values; each attempt
        # consumes the next one, which is how a re-plug (idle,cold) is
        # simulated without a device.
        states=${2:-}
        lived=${3:-30}
        repeat=${4:-1}
        if [ -z "$states" ]; then
            echo "usage: capture-agent.sh decide <state[,state...]> [lived_seconds] [attempts]" >&2
            echo "       states: cold unauthenticated idle receiver absent uninitialized unknown" >&2
            exit 2
        fi
        PIX_TUNER=dt295
        # Keep the simulation out of the daemon's files: the log would read as a
        # real fault, and the state file would claim a device state that never
        # happened.
        DT295_STATE_SUFFIX=.decide
        log() {
            echo "$(date +%H:%M:%S) decide: $*" >>"$SHM/pix/dt295-decide.log"
        }
        i=0
        while [ "$i" -lt "$repeat" ]; do
            case "$states" in
                *,*) one=${states%%,*}; states=${states#*,} ;;
                *)   one=$states ;;
            esac
            DT295_FAKE_STATE=$one
            started_at=$(( $(date +%s) - lived ))
            if dt295_restart_allowed; then
                echo "state=$one lived=${lived}s attempt=$((i + 1)) decision=restart"
            else
                echo "state=$one lived=${lived}s attempt=$((i + 1)) decision=hold"
            fi
            i=$((i + 1))
        done
        if [ -f "$SHM/pix/dt295.state.decide" ]; then
            echo "status: $(tr '\n' ' ' <"$SHM/pix/dt295.state.decide")"
        fi
        exit 0
        ;;
    help|--help|-h)
        echo "usage: capture-agent.sh            run the agent (init service)"
        echo "       capture-agent.sh state      probe the DT-295 and exit"
        echo "       capture-agent.sh evidence   write the evidence bundle and print it"
        echo "       capture-agent.sh decide <state[,state...]> [lived] [attempts]"
        echo "                                   show the restart decision for a state"
        echo "                                   (simulation: writes dt295.state.decide,"
        echo "                                    dt295-decide.log and dt295-evidence.decide.txt)"
        exit 0
        ;;
esac
log "capture agent started (card=$([ "$REMOTE_CARD" = 1 ] && echo remote || echo local))"

# ---------------------------------------------------------------------------
# Programme guide scan.
#
# The tuner, the ACAS handshake and the decryptor only come up together inside
# this agent's own start sequence. A separate process cannot reproduce it: a
# fresh wrapper ends with "b21dec: SetData err=-1" and no transport stream. So
# the guide scan runs here, retuning channel by channel and keeping a few
# seconds of transport stream for each. The NAS feeds those files to mirakc,
# which owns the ARIB decoding, and the archive pulls the guide over HTTP.
if [ "${1:-}" = "--epg-scan" ]; then
    out_dir=${2:-/data/local/tmp/epg}
    channels=${3:-13,14,15,16,17,18,19,20}
    seconds=${4:-12}
    mkdir -p "$out_dir"
    ensure_fifo
    log "epg-scan: channels=$channels seconds=$seconds out=$out_dir"
    old_ifs=$IFS
    for channel in $(echo "$channels" | tr ',' ' '); do
        IFS=$old_ifs
        log "epg-scan: tuning $channel"
        EPG_DUMP_FILE="$out_dir/$channel.ts"
        start_tuner "$channel"
        started_at=$(date +%s)
        waited=0
        while [ "$waited" -lt 20 ]; do
            acas_ready && break
            sleep 1
            waited=$((waited + 1))
        done
        log "epg-scan: acas_ready_after=$waited channel=$channel"
        sleep "$seconds"
        size=$(wc -c < "$out_dir/$channel.ts" 2>/dev/null | tr -d ' ')
        log "epg-scan: channel=$channel bytes=${size:-0}"
        kill_all 'smb400-tuner-oem'
        kill_all 'tuner-stream-ng'
        kill_all 'tuner-stream-bs'
        kill_all 'b21dec'
        sleep 2
    done
    IFS=$old_ifs
    log "epg-scan: done"
    exit 0
fi

while :; do
    want_state=""
    want_channel=0
    if [ -f "$REQ" ]; then
        read -r want_state want_channel want_codec want_w want_h want_bitrate \
            want_mode want_gop < "$REQ" 2>/dev/null || true
    fi

    case "${want_state:-}" in
        start)
            if [ -n "${want_channel:-}" ] && [ "$want_channel" != "0" ]; then
                start_capture "$want_channel" "${want_codec:-}" \
                    "${want_w:-}" "${want_h:-}" "${want_bitrate:-}" \
                    "${want_mode:-}" "${want_gop:-}"
            fi
            ;;
        stop)
            stop_capture
            ;;
        *)
            ;;
    esac

    # If the tuner disappeared while a channel is wanted, bring it back once.
    # A freshly started tuner is given time to register before the check runs.
    # For the DT295 the decision is deferred to dt295_restart_allowed(), which
    # probes the firmware state and applies the backoff.
    if [ "$current_channel" != 0 ] && [ $(( $(date +%s) - ${started_at:-0} )) -gt 15 ] \
       && ! tuner_alive; then
        if dt295_restart_allowed; then
            log "tuner disappeared, restarting channel $current_channel"
            sleep 3
            restart_tuner "$current_channel"
        fi
    fi

    # The ACAS handshake is the one part of the SMB400 pipeline that depends on
    # the OEM stack's timing: when the box has just booted, the first tune can
    # end up with a decryptor that never authenticates and silently passes
    # scrambled bytes through. Retry the tune before the reader gives up.
    if [ "$PIX_TUNER" = smb400 ] && [ "$current_channel" != 0 ] \
       && [ $(( $(date +%s) - ${started_at:-0} )) -gt 20 ] \
       && [ "${acas_retry:-0}" -lt 2 ] && ! acas_ready; then
        acas_retry=$((acas_retry + 1))
        log "ACAS did not authenticate; retuning (attempt $acas_retry)"
        restart_tuner "$current_channel"
    fi

    sleep 1
done
