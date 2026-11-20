# 2代目: 内部カードでの外部複合が成立（段2・段4 完了）

2026-11-21。CFW の生 TS を、**DT295 の内蔵 B-CAS カード**で複合することに成功した。

## 結果

```
pix-cfw-ts（生TS）-> torne-b25（内蔵カード / bcasd / CFW リーダー）
```

```
TS_STATS stage=clear packets=106382 tei=0 cc_errors=0 duplicate_cc=0 scrambled=0
B25_STATS card_commands=6 failed=0
```

- **106,382 パケットを複合**（`scrambled=0`）
- TEI 0、連続性の誤り 0、重複 CC 0
- カードのやり取りは 6 回、**失敗 0**
- 復号後の TS は `mpeg2video` + `aac` + `arib_caption` として読め、ffmpeg でデコードできる

**純正ファームでは原理的に不可能だった構成**（生 TS は出せず、内蔵カードは
外部から見えなかった）が、CFW + IFD ハンドラ + bcasd で通った。

## 経路

```
pix-cfw-ts                 CFW から生 TS（複合前）
   |                       4.87 MB/s、スクランブル 82%
   v
torne-b25                  ARIB MULTI2 の複合
   |                       /dev/shm/torne/*.sock 経由
   v
cardrelay  ->  bcasd       524 バイトフレーム
   |                       pcscd の CFW リーダー
   v
libifd_m2tv.so  ->  CFW カード枠（0x50-0x57）
   |                       DT295 の内蔵 B-CAS カード
   v
（クリア TS）
```

## 途中で直した誤り（自分で作った混乱）

### 1. APDU が通らないと誤診した

`apdu_test.c` で `{90 30 00 00 00}` を **そのまま CARD_TX へ送っていた**が、
T=1 のカードは `NAD PCB LEN data LRC` の枠を期待する。
枠を作らずに送っていたので応答が来ず、それを「ファームウェアの不具合」と
誤って判断しかけた。

`t1_transceive`（ハンドラが ATR 後に使うのと同じ経路）に変えたら即座に通った。

### 2. 「カードが 262 バイト応答している」と誤報した

受信報告のフィールドオフセットを間違えて読み、`got=262` と解釈していた。
正しくは `got=0`（応答ゼロ）。測定道具の誤りを事実として報告してしまった。

### 3. 「初回 CARD_ON が無視される」は実在した（これは本物）

冷起動後の最初の `CARD_ON` は stage 4 のまま ATR を返さず、
直後の 2 回目で成功する。8 秒待っても同じなので待ち時間の問題ではない
（`docs/card.md` の「分かっていないこと」に記載のある挙動）。

ハンドラの `power_up` に **1 回だけの再試行**を入れて吸収した。
これで pcscd から見て安定した（`excl_test` が 4 回連続成功）。

### 4. リモートカードは 1 クライアント専有

`192.168.99.10` の B-CAS は `SCARD_SHARE_EXCLUSIVE` で掴まれるため、
1台しか同時に使えない。1代目が録画中は2代目から使えない
（1代目自身で2本目を試しても同じく失敗することを確認）。
**段2をリモートカードで通す案はこれで不可**。内蔵カードが正解だった。

## 再現手順

```sh
# 1. CFW を入れる（cold から）
P=/sys/bus/usb/devices/usb1/1-0:1.0/usb1-port1
echo 1 > $P/disable; sleep 3; echo 0 > $P/disable; sleep 8
frc1_load install /usr/share/cfw/frc1-dt295.frc

# 2. IFD ハンドラを置き、pcscd を起動
#    /usr/lib/pcsc/drivers/ifd-cfw-m2tv.bundle/{Contents/Info.plist,Contents/Linux/libifd_m2tv.so}
#    Info.plist: CFWAllowAPDU=true（B-CAS に必須）
pcscd -f &

# 3. bcasd と cardrelay
bcasd --config /etc/bcas/bcasd.conf &
cardrelay --config /etc/bcas/cardrelay.conf &

# 4. 生 TS を複合
pix-cfw-ts | torne-b25 --card-socket /dev/shm/torne/card.sock
```

## 残り

- ランチャー（`pix-stream-simple`）への組み込みと、`S97` からの起動
- ガードとの運用（`enabled=false` の扱い）
- 連続運用の確認（長時間）

## 動作する構成（実測で確定した条件）

同じハードで「通る」「通らない」が分かれたので、確定した条件を残す。

### 1. bcasd の RESET を本物にする（これが最大の原因だった）

`bcasd` は接続済みのカードに対して RESET を**何もしていなかった**。
`torne-b25` は RESET → `90 30` の順で初期化するので、2 回目以降は
前のやり取りが残ったままになり、CFW のカード経路が次のブロックを拒否する。

→ `SCardReconnect(..., SCARD_RESET_CARD)` を入れた（`package/bcasd/src/bcasd.c`）。
これで 3 回連続の初期化が通る。

### 2. ゾンビプロセスが USB を握ると何も動かない

`pix-cfw-grab` や `pix-cfw-ts` が残っていると、
`cannot claim interface 0: LIBUSB_ERROR_BUSY (a driver for this device is loaded)`
で取得が失敗する。**入れ直す前に必ず落とす。**

```sh
for p in $(ps | awk '/pix-cfw|torne-b25/ && !/awk/ {print $1}'); do kill -KILL $p; done
```

### 3. USB を電源切り直したら pcscd も入れ直す

`frc1_load install` のために `usb1-port1/disable` を叩くと、pcscd は
リーダーを見失って `Reader is unavailable.` を返す。pcscd を再起動する。

### 4. カードと TS は同時に使える（実測）

TS を流しながら `90 30` を投げても通った。EP0 を取り合うが、
`pix-cfw-grab` の 1 枠 32KiB とカードの短い枠なら実用上問題ない。

## 実測値

```
$ torne-b25 --card-socket /dev/shm/torne/card.sock < final-raw.ts
TS_STATS stage=clear packets=106382 tei=0 duplicate_cc=1 scrambled=0
B25_STATS card_commands=7 failed=0
```

- **106,382 パケットを複合**（`scrambled=0`）
- TEI 0、`failed=0`
- 45 秒の連続パイプラインで約 202 MB が流れた
