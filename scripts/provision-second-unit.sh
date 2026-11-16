#!/bin/sh
#
# provision-second-unit.sh - 2台目の X96Max_P を立ち上げるときの設定をまとめて行う
#
# 実機（root）で実行する。何度実行しても同じ結果になる。
#
#   provision-second-unit.sh --name X96Max-P2 --channel 17 --tuner pxs1ud \
#       [--bcas local|remote] [--token HEX] [--host 192.168.99.10]
#
# 変更した内容は最後に一覧で出す。再起動はしない（--restart を付けたときだけ）。

set -eu

NAME=""
CHANNEL=""
TUNER=""
BCAS_MODE=""
TOKEN=""
REMOTE_HOST=""
RESTART=0
STREAM_DEFAULTS=/etc/default/pix-stream-linux
BCAS_DEFAULTS=/etc/default/bcas-linux

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --name)        NAME="$2"; shift 2 ;;
        --channel)     CHANNEL="$2"; shift 2 ;;
        --tuner)       TUNER="$2"; shift 2 ;;
        --bcas)        BCAS_MODE="$2"; shift 2 ;;
        --token)       TOKEN="$2"; shift 2 ;;
        --host)        REMOTE_HOST="$2"; shift 2 ;;
        --restart)     RESTART=1; shift ;;
        -h|--help)     usage ;;
        *)             echo "unknown option: $1" >&2; usage ;;
    esac
done

[ -n "$NAME" ] && [ -n "$CHANNEL" ] && [ -n "$TUNER" ] || usage
case "$TUNER" in
    pxs1ud|torne|dt295) ;;
    *) echo "--tuner は pxs1ud / torne / dt295 のいずれかです" >&2; exit 1 ;;
esac
case "$BCAS_MODE" in
    ""|local|remote) ;;
    *) echo "--bcas は local か remote です" >&2; exit 1 ;;
esac
if [ "$TUNER" = "dt295" ] && [ -n "$BCAS_MODE" ] && [ "$BCAS_MODE" != "local" ]; then
    # DT295 はチューナー側で復号するため、外付けカードリーダーの設定は使わない。
    echo "dt295 はチューナー内で復号するため --bcas は不要です" >&2
    exit 1
fi

# 既存の値を書き換える小さな関数。キーが無ければ追記する。
set_key() {
    file="$1"; key="$2"; value="$3"
    if grep -q "^${key}=" "$file" 2>/dev/null; then
        sed -i "s|^${key}=.*|${key}=${value}|" "$file"
    else
        printf '%s=%s\n' "$key" "$value" >> "$file"
    fi
}

changed=""

set_key "$STREAM_DEFAULTS" PIX_DISPLAY "$NAME"
changed="$changed PIX_DISPLAY=$NAME"
set_key "$STREAM_DEFAULTS" PIX_CHANNEL "$CHANNEL"
changed="$changed PIX_CHANNEL=$CHANNEL"
set_key "$STREAM_DEFAULTS" PIX_TUNER "$TUNER"
changed="$changed PIX_TUNER=$TUNER"

if [ -n "$BCAS_MODE" ] && [ "$TUNER" != "dt295" ]; then
    [ -f "$BCAS_DEFAULTS" ] || { echo "$BCAS_DEFAULTS がありません" >&2; exit 1; }
    set_key "$BCAS_DEFAULTS" BCAS_MODE "$BCAS_MODE"
    changed="$changed BCAS_MODE=$BCAS_MODE"
    if [ -n "$REMOTE_HOST" ]; then
        set_key "$BCAS_DEFAULTS" BCAS_REMOTE_HOST "$REMOTE_HOST"
        changed="$changed BCAS_REMOTE_HOST=$REMOTE_HOST"
    fi
    if [ -n "$TOKEN" ]; then
        case "$TOKEN" in
            *[!0-9a-fA-F]*|"") echo "--token は16進のみです" >&2; exit 1 ;;
        esac
        [ ${#TOKEN} -eq 64 ] || { echo "--token は64桁です (${#TOKEN}桁でした)" >&2; exit 1; }
        set_key "$BCAS_DEFAULTS" BCAS_REMOTE_TOKEN "$TOKEN"
        changed="$changed BCAS_REMOTE_TOKEN=(設定済み)"
    fi
fi

# チューナーごとに、起動時に必要なものが揃っているか確かめる。
missing=""
case "$TUNER" in
    pxs1ud)
        [ -r /lib/firmware/isdbt_rio.inp ] || missing="$missing /lib/firmware/isdbt_rio.inp"
        command -v siano-ts >/dev/null 2>&1 || missing="$missing siano-ts"
        ;;
    torne)
        command -v torne-c >/dev/null 2>&1 || missing="$missing torne-c"
        ;;
    dt295)
        [ -r /usr/share/pix-tuner/dt295.assets ] || missing="$missing /usr/share/pix-tuner/dt295.assets"
        command -v pix-ts-legacy >/dev/null 2>&1 || missing="$missing pix-ts-legacy"
        ;;
esac
[ -n "$missing" ] && echo "不足しています:$missing" >&2

echo "設定しました:$changed"
grep -E '^PIX_(DISPLAY|CHANNEL|TUNER)=' "$STREAM_DEFAULTS"
if [ "$TUNER" != "dt295" ]; then
    grep -E '^BCAS_(MODE|REMOTE_HOST)=' "$BCAS_DEFAULTS"
fi

if [ "$RESTART" = "1" ]; then
    /etc/init.d/S97pix-stream stop >/dev/null 2>&1 || true
    sleep 3
    /etc/init.d/S97pix-stream start
    sleep 20
    wget -qO- http://127.0.0.1:40774/v1/status 2>/dev/null | head -c 200
    echo
else
    echo "反映には再起動が必要です: /etc/init.d/S97pix-stream stop; sleep 3; /etc/init.d/S97pix-stream start"
fi
