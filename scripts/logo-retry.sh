#!/bin/sh
# Retries the BS-15 logo fetch on the DT295 box once a night.
#
# The carousel of service 929 was announcing carousels without transmitting any
# module on 2026-11-23, so a single attempt is not enough: this asks again every
# night and stops by itself once the logo directory has content. It runs over
# ssh with the guard key, and every attempt pauses recording for up to 30
# minutes, which is why it is pinned to the quiet hour.
set -u
HOST=192.168.99.123
KEY=/home/ku/.ssh/id_ed25519_guard
SSH="ssh -i $KEY -o BatchMode=yes -o ConnectTimeout=10 root@$HOST"

STATE=/home/ku/pix-cm/logo-retry.count
attempts=$(cat "$STATE" 2>/dev/null || echo 0)
case "$attempts" in ''|*[!0-9]*) attempts=0 ;; esac
if [ "$attempts" -ge 14 ]; then
    echo "$(date '+%F %T') $attempts attempts made without logos; stopping until someone looks"
    exit 0
fi

count=$($SSH 'ls /var/lib/pix/logos/*.png 2>/dev/null | wc -l' 2>/dev/null || echo 0)
case "$count" in ''|*[!0-9]*) count=0 ;; esac
if [ "$count" -gt 0 ]; then
    echo "$(date '+%F %T') logos already present: $count png, nothing to do"
    exit 0
fi

echo "$(date '+%F %T') fetching BS-15 logos (up to 60 min of recording paused)"
# Keep the parser's verdict: "logo modules X/Y in N carousel(s)" separates an
# empty carousel (Y=0, nothing is being transmitted) from a partial one (X<Y).
output=$($SSH 'pix-logo-fetch --force --wait 3600' 2>&1)
status=$?
echo "$output" | grep -E "logo_end|no complete logo data|restarting the pipeline" | tail -3
after=$($SSH 'ls /var/lib/pix/logos/*.png 2>/dev/null | wc -l' 2>/dev/null || echo 0)
echo "$(date '+%F %T') exit=$status png_after=$after"
if [ "${after:-0}" -gt 0 ]; then
    echo "$(date '+%F %T') logos recovered; the timer can be disabled"
    echo 0 > "$STATE"
else
    echo $((attempts + 1)) > "$STATE"
fi
exit 0
