# 2代目: 内蔵カードの PC/SC 化（段4）

2026-11-20。設計の段4。CFW を入れた DT295 で、**内蔵 B-CAS カードから
ATR を取ることに成功**した。設計書が最大のリスクとしていた点の決着。

## 手順と結果

### 1. CFW を入れて ARM コアを起こす

CFW 単体では主 FR のカード経路（`0x50`〜`0x57`）は動くが、**ARM 経路は
ARM イメージを載せないと動かない**。

```sh
frc1_load install /usr/share/cfw/frc1-dt295.frc
arm_probe hold  --dev dt295      # ARM を止める
arm_probe image /tmp/armcore.bin --dev dt295   # ITCM 0 へ 6412 バイト
arm_probe run   --dev dt295      # ARM_RUN、'ARM1' を待つ
```

成功時の表示:

```
mailbox  alive 41524d31 ('ARM1')  loops 0000ca2a
         fault 00000000  lr 00000000
```

`armcore.bin` は公開リリース `v0.0.1` のもの（sha256 `f7229bec...5cd`、
SHA256SUMS と一致）。

### 2. ATR

主 FR 経路（`CFW_CARD_DEV=dt295` のみ）:

```
$ card_tool atr
ATR after 33 ms
ATR 3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
protocols 8002  F 372  D 2  IFSC 124  CWI 5  BWI 4  specific mode
S(IFS 251) answered in 8 ms
```

ARM 経路（`CFW_CARD_ARM=0`）でも**同じ ATR**:

```
ATR after 41 ms
ATR 3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
S(IFS 251) answered in 8 ms
```

**カードの有無も見える**: `CARD_STATUS` で `CARD_EVENT 00000400`
（bit10 = カードあり）。

## 設計書の「極性の矛盾」は誤りだった

設計書 §4.2 で「`docs/card.md` は ARM の ON 引数 0 を極性に使わないと書き、
`svc_card.c` は `pol = q->arg0 ? CARD_PIN_POL : 0u` と読む。DT295 の極性は 1
なので ATR が来ない計算になる」と書き、これを段4 の関門にしていた。

**実機で試したら逆だった。**

| ON の引数 | 結果 |
|---|---|
| **0** | **ATR 取得成功（13 バイト）** |
| 1 | E_TIMEOUT（応答なし） |

つまり **0 が正しい**。`card_link.h` が 0 を渡すのは正しく、文書もコードも
そのままでよい。私の読み違いで、実機が優先する。

（`polarity_test.c` を書いて両方の値を試した。この道具は残しておく価値がある。）

## 現在できていること / いないこと

| 項目 | 状態 |
|---|---|
| 主 FR 経路で ATR | **できる** |
| ARM 経路で ATR | **できる**（ただし ARM イメージのロードが要る） |
| カードの挿抜検出 | できる（`CARD_EVENT` bit10） |
| PC/SC のリーダーとして見えるか | **未着手**（IFD ハンドラと pcscd の設定） |
| `bcasd` からの利用 | 未着手 |
| 録画中の同時利用 | 未着手 |

## 次の段階

1. IFD ハンドラ（`libifd_m2tv.so`）を Linux 向けにビルドし、pcscd に読ませる。
   DT295 は CCID クラスを名乗らないので、ドライバを手動で登録する作業が要る。
2. `bcasd` がそのリーダーを掴めることを確認（`reader.N` 指定）。
3. `torne-b25` から APDU が通ることを確認。
4. そこまで通れば段2（生 TS を外部複合）が成立する。

## ガードとの競合を解いた（2026-11-20）

### 原因の特定

`192.168.99.20` から約4分周期で SSH が来て S97 を起動し直していた。
`/v1/status` が 200 を返さないのを検知して復旧する外部ガード
（`nas/src/pix-stb-guard.py` の `restart_stream`）。

**当初これを「ガードが S97 を再起動している」と推測したが、確認手段として
dropbear を止めるという操作を選び、SSH を失った。** ログを見れば数秒で
分かることだった（`dropbear[...]: Pubkey auth succeeded ... from 192.168.99.20`）。
推測を確かめる操作は、可逆で影響の小さいものから選ぶべきだった。

### 対応: 既存の `enabled=false` を使う

ガードは `node['enabled'] is False` のノードを**すべての復旧経路から除外する**
（`decide` / `decide_stalled` / 音声監視 / アプリ再起動のいずれも先頭で弾く）。
これは「意図的に止めた機体を蹴らない」ための既存機能で、ガードに手を入れる必要がない。

```sh
curl -X POST "http://192.168.99.80:40776/api/nodes/192.168.99.123-BS09_0/stop"
# -> {"ok":true}   enabled=false, state=stopped
```

### 実測

| 条件 | S97 の再起動 |
|---|---|
| `enabled=true` で S97 を停止 | **230 秒後に復活**（再現済み） |
| `enabled=false` で S97 を停止 | **6分間まったく復活せず** |
| `enabled=false` で CFW 導入後 8分放置 | **CFW もカードも維持** |

### 復旧するとき

```sh
curl -X POST "http://192.168.99.80:40776/api/nodes/192.168.99.123-BS09_0/start"
```

`enabled=false` の間は**録画が止まる**（`stop_node` が録画プロセスを止める）。
CFW の作業中だけ使い、終わったら必ず `start` に戻すこと。

## カードの持ち主は排他（実測）

段4 の運用で必ず踏む条件。実機で確認した。

| 状況 | 結果 |
|---|---|
| 主 FR 経路で ATR → `CARD_OFF` した後 | ARM 経路で **ATR が取れる** |
| 主 FR 経路がカードに電源を入れている間 | ARM 経路は **`E_BUSY (another core has it)`** |

設計書に書いた `card_mine()` / `card_owner()` の排他が、そのまま実機で出る。
**同時に両方の経路を使うことはできない。** IFD ハンドラを作るときは、
どちらか一方に固定する（設計では ARM 経路を推奨していたが、主 FR 経路でも
ATR が取れるので、pcscd の実装しやすさで選べばよい）。

## pcscd のリーダーとして動いた（2026-11-20）

### バンドルのビルドと配置

```sh
# クロスビルド（Buildroot の sysroot を使う）
$CC -std=c11 -O2 -Wall -Wextra -DCHIPIO_HAVE_LIBUSB -D_POSIX_C_SOURCE=200809L \
    -I$SYS/usr/include/libusb-1.0 -I/src/sample -fPIC -shared -pthread \
    -o libifd_m2tv.so ifd_m2tv.c -lusb-1.0 -ldl
```

警告 0 でビルドできた（macOS 固有部分は `#ifdef __APPLE__` で分離済み）。

配置は PC/SC のドライバ置き場。このイメージの pcscd は
`usbdropdir=/usr/lib/pcsc/drivers` でビルドされている（`strings /usr/sbin/pcscd` で確認）。

```
/usr/lib/pcsc/drivers/ifd-cfw-m2tv.bundle/Contents/Info.plist
/usr/lib/pcsc/drivers/ifd-cfw-m2tv.bundle/Contents/Linux/libifd_m2tv.so
```

`Info.plist` は `sample/ifd/Makefile` と同じく `CFBundleExecutable` を
`libifd_m2tv.so` に書き換える。

### 結果

`pcscd` を起動するとリーダーとして認識された。

```
$ pcsc_atr
reader: PIX-DT295 CFW card slot (WD10043976) 00 00
  card present (state 00060022)
  protocol T=1  ATR 3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
  disconnect: Command successful.
```

**PC/SC 経由で T=1 プロトコルまで確立できた。** 段4 の目的（内蔵カードを
PC/SC のリーダーとして見せる）は達成。

### 既知の不安定さ（CFW 側の問題）

**毎回 1 回目の電源投入が失敗し、2 回目で成功する。**

```
power action 500
power up: no ATR within 3000 ms (stage 4)     <- 1回目は失敗
power action 500
ATR 3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99   <- 2回目は成功
T=1 up: F 372 D 2 IFSC 124 IFSD 251 CWI 5 BWI 4
```

これは CFW の文書が「分かっていないこと」に挙げている
「CFW を入れた直後の最初の `CARD_ON` が答えないことがある（2 回目からは通る）」
と同じ挙動。**pcscd やアプリ側から見ると「接続に失敗した」ように見える**ので、
実運用では再試行が要る。

対応方針（未実装）:
- `bcasd` は接続失敗時に再試行する実装になっているか確認する
- ならなければ、pcscd を再起動するか、アプリ側で 1 回リトライする

### 現在できていること

| 項目 | 状態 |
|---|---|
| pcscd がリーダーとして認識 | **できる** |
| カードの在否 | できる |
| ATR / T=1 の確立 | **できる**（2 回目の試行で） |
| APDU | 未確認（`CFWAllowAPDU` が false のため、まだ許可していない） |
| `bcasd` からの利用 | 未着手 |

## 次の段階（段2 へ）

APDU を通すには `Info.plist` の `CFWAllowAPDU` を `true` にする。
ただしコメントに「macOS はカードを見つけると自分で APDU を送るため既定は off」と
ある。Linux の pcscd は自分からは送らないので、`true` にして問題ないはずだが、
**確かめてからにする**。
