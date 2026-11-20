# 3代目 X96Max_P Linux 化 - 再開手順と現状

作成日: 2026-11-19（第 11 ラウンド時点で中断）

## 目的（達成できていないもの）

3代目を Linux で起動し、DT295 で SBS（ch17）を録画し、2代目と同じく NAS に保存して CM 解析・カットまで通す。

## 到達点

| 項目 | 状態 | 根拠 |
|---|---|---|
| Linux RAM 起動 | **達成** | `x96max-p3 login:`、`rootfs`（RAM）、swap なし、例外 0 |
| Ethernet | **達成** | 外部 RTL8211F、1Gbps/Full、DHCP `192.168.99.56`、NAS から疎通 |
| DT295 受信（ch17=SBS） | **達成** | `locked:1`、`tables_ready:1`、5 秒で 9.9MB、`service=35864` |
| USB を Linux で有効化 | **未達** | `dwc3_meson_g12a_usb2_init_phy` でカーネル例外 |
| 録画・NAS 保存・CM カット | **未達** | 上記が未解決のため |

## 中断理由

**SD カードを Mac に接続してイメージを書き込む操作が 11 ラウンド実施されなかった。** 実機検証には媒体の書き換えが必須で、ソフトウェア側では代替できない。

## 再開に必要な手順

1. 3代目の SD カードを抜いて Mac に接続する。
2. イメージを書き込む（sudo のためユーザーのターミナルで実行）。
   ```sh
   cd ~/Desktop/work/X96MaxLinuxTSEnc
   /usr/bin/python3 scripts/write-sd.py --image output/ramboot-unit3/sdcard.img --device /dev/disk10 --erase
   ```
   期待する出力: `Read-back SHA-256: 470acd52…` または起動ファイル照合の成功。macOS が FAT に `.fseventsd` を書いても、起動ファイルが一致すれば合格する（第 5 ラウンドで修正済み）。
3. SD を戻して電源投入。`192.168.99.56` は Android で起動するので、U-Boot へ戻す必要がある。
   ```sh
   /usr/bin/python3 tools/uboot_boot_linux.py --dev /dev/cu.usbserial-01025FC4 \
       --ramboot --unit 3 --dtb meson-g12a-x96-max-p-usb.dtb --wait 240 --trace 45
   ```
4. 例外の有無を確認する。第 4 ラウンド以降、`wait-carrier` と SSH 鍵が入っているため、次回からは Ethernet 経由で `ssh root@192.168.99.56` が使える。
5. 起動できたら受け入れ検査を回す。
   ```sh
   /usr/bin/python3 tools/check-unit3-acceptance.py --host 192.168.99.56 --channel 17
   ```

## DTB の選択肢

| ファイル | SHA-256（先頭） | 用途 |
|---|---|---|
| `meson-g12a-x96-max-p-nousb.dtb` | `a5690cdf` | USB 全無効。Linux 起動確認用（実績あり） |
| `meson-g12a-x96-max-p-usb.dtb` | `682d4f2c` | **本命候補**。glue 削除、`snps,dwc3` 直接、USB2×2＋USB3 PHY |
| `meson-g12a-x96-max-p-ephy-4g.dtb` | `b031dc16` | Ethernet 用（外部 PHY、4GiB）。USB 有効で例外 |

## 判明している USB の障害

vendor の device tree（Android 実機から取得）と mainline の差分。

- 実機は `dwc3@ff500000` を **glue なしのトップレベル platform デバイス**として持つ。mainline は `usb@ffe09000` を親とする階層を作る。
- 実機の `0xffe09000` は **USB2 PHY の制御ブロック**（`usb2phy@ffe09000`）。mainline はこれを「glue」と誤認し、`regmap_update_bits` で触れて例外になる。
- 実機の `usb2phy@ffe09000` は **`pll-setting-1`〜`8`、`u2-ctrl-*-shift`、`version=2`** など独自プロパティを持つ。mainline の `amlogic,g12a-usb2-phy` はこれらを解釈せず固定値で初期化する。
- Android では `06b8:103b`（DT295）が `/sys/bus/usb/devices/1-2` に接続され、`/dev/bus/usb/001/002` も存在する。**ハードウェアは正常。**

詳細は `30-usb-phy-findings.md`。

## 次の一手（USB が動かなかった場合）

候補 DTB が失敗したら、**案 B** に進む。

- `dwc3-meson-g12a.c` の `dwc3_meson_g12a_usb2_init` が u2p regmap を触らないようにするパッチ（glue の独自初期化を回避）。
- 併せて `phy-meson-g12a-usb2.c` の PHY 初期化にベンダーの PLL 値（`pll-setting-1..8`）を取り込む。

案 C（ベンダーの `amlogic-new-usb2-v2` 移植）は大掛かりなので最終手段。

## 誤診の記録（繰り返さない）

`devmem` で USB レジスタが Bus error になったのは、dwc3 を無効化した DTB で起動した際に `clk: Disabling unused clocks` が `g12a_usb_general` を止めた（enable_count=0）副作用。**周辺レジスタを測る前に、そのノードが有効でクロックが供給されていることを確かめる。**

## 未着手の課題

- 起動時 DHCP がリンク確立前に走る問題 → `wait-carrier` を追加済み（イメージに同梱、実機未検証）
- eMMC が Linux で `-EILSEQ` により初期化失敗（RAM 起動のため運用には影響なし）
- `pix-usb-reset` による再起動時の無応答事象（未解明）
- NAS のチャンネル名が ch17＝テレビ静岡に固定される（NAS 側の対応表。録画成立後にサービス名で確認する）

## 追記（round 13–16）: SD を使わない起動経路（TFTP）

SD カードの抜き差しを避けたい場合のために、TFTP 起動を実装した。

### 実装したこと

- `tools/uboot_boot_linux.py` の分岐順の欠陥を修正。unit 3 が `--tftp-server` を指定しても SD 経路に落ちていた（`if unit == 3:` が `elif server:` より先にあった）。ネットワーク起動を最優先の分岐へ移動し、unit 3 の既定 IP を `192.168.99.56` にした。
- `work/start-tftp.sh` を追加。`output/ramboot-unit3/` の起動ファイルを `work/tftpboot/` へ複製し、`sudo /usr/libexec/tftpd -i 192.168.99.25 -s …` で配信する。
- テスト追加（unit 3 のネットワーク起動が mmc に触れないこと）。全 225 テスト通過。

### 実際の手順

```sh
# 1. サーバ（要 sudo、ターミナルを開いたまま）
cd ~/Desktop/work/X96MaxLinuxTSEnc
./work/start-tftp.sh

# 2. 別のターミナルで起動（3代目は Android なら自動で再起動する）
/usr/bin/python3 tools/uboot_boot_linux.py --dev /dev/cu.usbserial-01025FC4 \
    --ramboot --unit 3 --tftp-server 192.168.99.25 \
    --dtb meson-g12a-x96-max-p-usb.dtb --wait 240 --trace 45
```

U-Boot 側で使われる設定は `ipaddr=192.168.99.56`、`serverip=192.168.99.25`。

### 残る制約

**TFTP サーバの起動にも sudo（root）が必要。** ポート 69 は特権ポートで、macOS では root 以外バインドできない。Python 等のユーザー空間実装でも同じ。`launchctl` での `com.apple.tftpd` 起動も root が必要。

つまり SD 書き込み（`sudo dd`）と TFTP 配信（`sudo tftpd`）の**どちらも、ユーザーがターミナルで一度 sudo を実行する必要がある**。この工程を AI 側から代替する手段はない。

### U-Boot の現状値（実機で確認）

```
ipaddr=10.18.9.97      serverip=192.168.99.8    ethaddr=90:0e:b3:24:81:7e
tftpboot コマンドは利用可能
```

## 追記（round 20–21）: 機体が無応答、他機とネットワークは正常

2026-11-19 20:00 頃の実測。

| 対象 | 状態 |
|---|---|
| 1代目 `192.168.99.109` | UP |
| 2代目 `192.168.99.123` | UP |
| NAS `192.168.99.80` | UP |
| NAS ホスト `192.168.99.10` | UP |
| Mac `en8` | `192.168.99.25` で接続 |
| **3代目 `192.168.99.56`** | **down** |
| 3代目シリアル | 8 秒間で受信 0 バイト（起動ログすら流れない） |

シリアルアダプタ（CP2104、`/dev/cu.usbserial-01025FC4`）は Mac に接続され、デバイスは生存している。したがって**3代目は電源が入っていないか、起動していない**。

ネットワーク全体の障害ではない（他機は全て応答）。3代目固有の停止。

### 再開の前提

**3代目の電源投入（または入れ直し）が唯一の必要作業。** 起動すれば U-Boot のプロンプトが出るか、Android が立ち上がるので、そこから作業を続けられる。

### この間に積んだもの（すべてコミット済み、`716618f`）

- USB 障害の原因解明（vendor DT との照合）
- DTB 候補 3 種（`-nousb` で Linux 起動実績、`-p-usb` が本命）
- YMODEM 転送ツール（SD も sudo も不要。U-Boot がヘッダに ACK を返すことは実機確認済み）
- 受け入れ検査ツール、225 テスト
- ビルド設定の欠陥 3 件の修正

## 追記（round 22–24）: 電源投入待ちで停滞中

2026-11-19 20:00 以降、3代目 `192.168.99.56` は応答しない。シリアルも自発出力 0 バイト。他機（1代目 .109、2代目 .123）と NAS は正常。

**3代目の電源投入が、再開に必要な唯一の操作。** 遠隔電源の仕組み（スマートプラグ等）は、この LAN 構成には無い。

### 停滞中に確定した設計上の結論

1. 候補 DTB `meson-g12a-x96-max-p-usb.dtb` の phandle は完全に解決している
   （`0x54`→`phy@36000`、`0x55`→`phy@3a000`、`0x06`→`phy@46000`）。
   vendor の `usb-phy = <usb2phy usb3phy>` と等価で、mainline の `phys`/`phy-names` 形式。
2. YMODEM（`loady_send.py`）は SD も sudo も不要な唯一の搬入経路。実機で
   U-Boot がヘッダに ACK（0x06）を返すことまでは確認済み。データブロックの
   途中で機体が停止したため、`probe_ymodem.py` で応答の実パターンを測って
   から詰める方針にした。
3. macOS の termios は 230400 bps まで。CP2104 自体は 2Mbps 対応だが API 制約。
   77MB（Image＋initramfs）の転送は 230400bps で約 56 分。

### 再開したときの最初の一歩

```sh
# 3代目の電源を入れ、U-Boot か Android が起動したのを確認してから
/usr/bin/python3 tools/probe_ymodem.py --dev /dev/cu.usbserial-01025FC4 \
    --file output/ramboot-unit3/meson-g12a-x96-max-p-usb.dtb --address 0x08000000
```

計測の結果、`C` と ACK の出方を見て `loady_send.py` を確定させる。DTB（50KB）
なら 230400bps で数秒。それが通れば DTB を `0x08000000` に置き、カーネルと
initramfs は SD から読む（候補 DTB だけ差し替える）ことで、媒体を書かずに
USB の検証ができる。

## 追記（round 25–27）: YMODEM 転送が成功。DTB を実機へ搬入できた

**sudo も SD カードも使わずに、DTB を U-Boot へ送り込むことに成功した。**

```
$ python3 tools/loady_send.py --dev /dev/cu.usbserial-01025FC4 --baud 115200 \
      --file output/ramboot-unit3/meson-g12a-x96-max-p-usb.dtb --address 0x08000000
sending header (meson-g12a-x96-max-p-usb.dtb, 50529 bytes)
sent 50529 bytes
```

U-Boot 側で検証:

```
fdt addr 0x08000000; fdt header
  magic: 0xd00dfeed
  totalsize: 0xc561 (50529)      ← 送信量と一致
crc32 0x08000000 0xC561
  ==> 7c0115d4                   ← ホスト側 zlib.crc32 と一致
```

### 成功の条件（重要）

1. **ボーレートは 115200 のまま使う。** `loady` に 230400 等を渡すと U-Boot は
   `## Switch baudrate to N bps` を出して切り替えを待ち、その間は 'C' を返さない。
   115200 なら即座に `## Ready for binary (ymodem) download to ...` と 'C' が出る。
2. **ポートは開いたまま使う。** 閉じて開き直すと、U-Boot が送る 1 個の 'C' を失う。
3. **書き込みは再試行する。** ポートは `O_NONBLOCK` なので、カーネルバッファが
   満杯になると `BlockingIOError` になる。最初の実装はこれで 79% 地点で落ちた。
   `write_all()` で書き切るまで再試行するようにして解決した。
4. **ACK だけで判定しない。** ブロック間の応答が 'C' のことがある。ヘッダへの
   `0x06`（ACK）を確認し、その後は書き込み完了で進める。

### 所要時間

50KB で約 3〜5 分（115200bps、1 ブロックあたり約 2.6 秒の応答待ちを含む）。
Image（41MB）と initramfs（35MB）を送る場合は数時間かかるため、**DTB だけを
差し替え、カーネルと initramfs は SD から読む**運用が現実的。

### 転送中の注意

転送が中断すると U-Boot は YMODEM 受信モードに残る。`CAN`（0x18）を数回送ってから
`ESC` でプロンプトに戻る。機体が無応答になった場合は、USB シリアルケーブルの
機体側コネクタが緩んでいることがある（実際に一度発生し、挿し直しで復旧した）。

### 補助経路: ADB

シリアルが落ちても、Android が起動していれば ADB で操作できる。

```
adb connect 192.168.99.56:5555
adb shell su -c "id"      → uid=0(root)
/dev/block/env            → U-Boot 環境の保存領域
```

ただし `/dev/block/env` への書き込みは起動設定を壊す危険があるため、シリアルが
使える状況では行わない。

## 追記（round 40）: DT295 接続後にやること（GXL DTB 構成）

### 現在の Linux の状態

GXL DTB で起動済み。USB ホストコントローラは正常、`VCC_5V` は enabled。
SSH 可（`root@192.168.99.56`、鍵は `~/.ssh/id_ed25519`）。

DT295 未接続のため、`pix-stream` は次の流れで停止している:

```
simple: dt295: three quick failures in a row
simple: dt295: attempting guarded USB controller recovery
pix-usb-reset: reset budget exhausted/cooling down (max 3 per boot, 300s apart)
simple: dt295: recovery refused or failed; stopping (manual intervention required)
```

**これは正しい安全動作**。リセットは起動あたり 3 回・300 秒間隔に制限されており、
上限に達すると停止する。USB を壊さない設計。

### DT295 を挿したあとの手順

```sh
# 1. 認識を確認
ssh root@192.168.99.56 'lsusb | grep 06b8'      # Pixela Corp. PIX-DT295

# 2. サービスを再起動（リセット回数の上限に達しているため必須）
ssh root@192.168.99.56 '/etc/init.d/S97pix-stream start'

# 3. 配信を確認
ssh root@192.168.99.56 'wget -qO- http://127.0.0.1:40774/v1/status'

# 4. 段階判定
python3 tools/check-unit3-acceptance.py --host 192.168.99.56 --channel 17
```

`pix-usb-reset.state` に `1 31`（回数と時刻）が記録されていた。上限に達した
状態を解除したい場合は、サービスを再起動するのが最も簡単（状態ファイルは
起動ごとに初期化される想定）。

### 設定の確認（実機で確認済み）

```
PIX_TUNER=dt295
PIX_SYSTEM=terrestrial
PIX_CHANNEL=17
```

`pix-usb-reset` の対象は `_PIX_USB_GLUE=ffe09000.usb`。GXL 構成ではこのノードが
glue なので整合している。

## 追記（round 41）: NAS ノードを enabled にした

### 見つけた問題

3代目の NAS ノードが `enabled: false` だった。この状態では NAS が録画を開始しない。
登録しただけで有効化されていなかった（round 20 で登録したが、その後の確認で
`enabled` を見落としていた）。

### 対応

`/api/nodes` は既存 ID への POST を 409 で拒否し、PATCH/PUT は 404 だったため、
DELETE → POST で入れ替えた。

```sh
curl -X DELETE http://192.168.99.80:40776/api/nodes/192.168.99.56-17
curl -X POST -H 'Content-Type: application/json' \
  -d '{"host":"192.168.99.56","port":40774,"display":"X96Max-P3","channel":"17",
       "enabled":true,"codec":"avc","width":1440,"height":1080,
       "bitrate":6000000,"gopSec":2}' \
  http://192.168.99.80:40776/api/nodes
```

結果:

```
enabled: True | state: error | ch: 17 | codec: avc
size: 1440x1080 | bitrate: 6000000 | gopSec: 2
```

画質は2代目（`192.168.99.123-BS09_0`）と同値。`state: error` は STB が
応答しないため（`stbError: unreachable`）。配信が始まれば `recording` になる。

### 現在の到達点（round 41）

| 項目 | 状態 |
|---|---|
| Linux RAM 起動（GXL DTB） | 達成。SSH 可 |
| USB ホストコントローラ | 達成。xhci 稼働、例外 0 |
| VBUS (VCC_5V) | enabled |
| 録画設定（ch17/地上波/DT295） | 達成。実機で確認 |
| NAS ノード | **enabled: true、画質は2代目と同等** |
| DT295 の認識 | **未達（物理未接続）** |
| 録画・NAS 保存・CM カット | 未達 |

**DT295 を挿してサービスを再起動するだけで、録画の検証に入れる状態。**

## 追記（round 80）: rootfs 更新の準備が完了（実機検証待ち）

### 書き込むファイル（検証済み）

`output/ramboot-unit3/rootfs.cpio.uboot` = `a230c50bab24ba92…`

中身を直接検証した:

- uImage ヘッダ magic `27051956`（OK）
- 本体は gzip（`1f8b`）
- 展開すると 111,633,920 バイト
- **`wait_for_tuner` が含まれる**（`b"wait_for_tuner" in raw` が True）
- `rootfs.cpio.gz` 内の `usr/bin/pix-stream-simple` にも 2 箇所

### SD の更新方法（Image を据え置く）

SD 上の `Image` は `b3099f99…`（DT295 認識に成功した実績のある版）。
最新の `Image` は `6a9b20e4…`（PPC 修正版、未検証）なので、**Image は更新しない**。

```sh
cd ~/Desktop/work/X96MaxLinuxTSEnc
/usr/bin/python3 scripts/write-sd.py \
  --image output/ramboot-unit3/sdcard.img \
  --device /dev/diskN \
  --update-boot --only rootfs.cpio.uboot
```

`--only` は FAT レイアウト検証の後に選択名を検証する。Image の必須チェックは
`--only` 指定時に外れるので、rootfs 単独更新が可能。

### 次の起動で確認すること

1. 起動直後、`wait_for_tuner` が DT295 を待つログ（`tuner: appeared after Ns`）
2. `/etc/init.d/S97pix-stream` 起動後も機体が無応答にならないこと
3. `/v1/status` が応答し、フラグメントが増えること
4. NAS 側で `state` が `recording`、`stb.status` が `ok` になること
5. 録画ファイルが NAS に保存されること

### 停止した場合の復旧

機体が無応答になったら電源再投入。SD はそのままでよい。

## 追記（round 89）: 現時点の到達点と再開手順（最新）

### 到達点

| 項目 | 状態 | 根拠 |
|---|---|---|
| Linux RAM 起動 | **達成** | `skip-glue-init` で例外が消え、`x96max-p3 login:` まで到達（round 73） |
| DT295 認識 | **達成** | `lsusb` に `06b8:103b Pixela Corp. PIX-DT295`、`/sys/.../1-1/product = PIX-DT295` |
| 停止の防止 | **実装** | `wait_for_tuner`（既定は無期限待機） |
| NAS 登録 | **達成** | `/api/discovered` に `name=X96Max-P3, channel=17, known=true` |
| NAS 画質設定 | **達成** | codec/解像度/bitrate/rateMode/gopSec が2代目と完全一致 |
| CM スキップ | **実装** | `encode_with_features` + `--feature-*`。ffmpeg 構成は Mac で動作確認 |
| **録画の成立** | **未達** | 機体が未接続のため実機確認できず |

### 再開に必要なもの

1. **SD カードを Mac へ**（`/dev/diskN` を `diskutil list external physical` で確認）
2. **3代目の電源**

### 書き込み（Image は据え置く）

```sh
cd ~/Desktop/work/X96MaxLinuxTSEnc
cp output/ramboot-unit3/ramboot.img output/ramboot-unit3/sdcard.img
H=$(shasum -a 256 output/ramboot-unit3/ramboot.img | cut -d' ' -f1)
grep -vE "  (sdcard\.img|rootfs\.cpio\.gz|ramboot\.img)$" output/ramboot-unit3/SHA256SUMS > /tmp/s
echo "$H  sdcard.img" >> /tmp/s && echo "$H  ramboot.img" >> /tmp/s
cp /tmp/s output/ramboot-unit3/SHA256SUMS
/usr/bin/python3 scripts/write-sd.py --image output/ramboot-unit3/sdcard.img \
  --device /dev/diskN --update-boot
```

**注意**: 最新の `Image` は `6a9b20e4`（PPC 修正版、未検証）。SD 上の実績値は
`b3099f99`（DT295 認識に成功）。PPC は必須でなかった可能性が高いので、
`--only rootfs.cpio.uboot` で rootfs だけ更新するのが安全。

### 起動と確認

```sh
# U-Boot へ（tools/uboot_boot_linux.py の catch_prompt を使う）
# 起動後
ssh root@192.168.99.56 'lsusb | grep 06b8'
ssh root@192.168.99.56 'tail -5 /tmp/pix-simple.log'      # tuner: appeared after Ns
ssh root@192.168.99.56 'wget -qO- http://127.0.0.1:40774/v1/status'
python3 tools/check-unit3-acceptance.py --host 192.168.99.56 --channel 17
curl -s http://192.168.99.80:40776/api/nodes | python3 -m json.tool  # state=recording
```

### 既知の落とし穴

- **`pix-stream` を DT295 認識前に起動しない。** 3 回失敗すると `dt295_recover`
  → `pix_usb_reset` で機体が無応答になる。`wait_for_tuner` が既定で待つように
  したが、起動順序には注意。
- **SD の FAT 区画はコンソールユーザー所有**なので、`--update-boot` なら sudo 不要。
- **YMODEM は 115200bps 固定。** 上げると U-Boot が切り替え待ちで止まる。
- **`pix-usb-audit` は起動メディアの指紋監査**で、DT295 とは無関係。
- **PORTSC3 は USB3 ポート。** DT295 は `1-1`（USB2 ポート1）で認識される。
- **CM の安全装置**: 解析が 3 回失敗すると `PIX_FEATURES=0` にして録画を守る。

## 訂正（round 96）: SD 上の Image には skip-glue-init が入っていない

### 誤り

round 95 の再開手順に「SD 上の `b3099f99` は DT295 認識に成功した実績があり、
`--only rootfs.cpio.uboot` で rootfs だけ更新するのが安全」と書いた。**これは誤り。**

### 実際

`b3099f99` は round 73 で「例外は出ず Linux が起動し、DT295 が `1-1` に
列挙された」版だが、**その後 `skip-glue-init` の修正を経て作られた
`Image`（`6a9b20e4`）とは別物**。round 96 に `b3099f99` で起動したところ、
`usb_init_glue+0x38` で synchronous external abort が出た。

```
[1.310848] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.319435] Internal error: synchronous external abort
[1.360551] pc : regmap_mmio_read32le+0x8/0x20
```

つまり `b3099f99` は **glue の書き込みを回避する前の版**。

### 確認方法（今後のために）

`Image` は非圧縮のカーネルなので、文字列を直接確認できる:

```sh
python3 -c "
d=open('output/ramboot-unit3/Image','rb').read()
print('skip-glue-init:', b'amlogic,skip-glue-init' in d)
print('port-power-control:', b'amlogic,port-power-control' in d)"
```

最新の `Image`（`6a9b20e4`）は両方 True。**`b3099f99` は False。**

### 正しい手順

**Image と rootfs の両方を更新する**（`--only` を付けない）。

```sh
/usr/bin/python3 scripts/write-sd.py --image output/ramboot-unit3/sdcard.img \
  --device /dev/diskN --update-boot
```

SD 上の gxl.dtb（`e0cc1dbf`）は最新と同じなので、実質 Image と rootfs の更新になる。

## 追記（round 96）: NAS 上の TFTP サーバで SD 不要の起動が可能になった

### 背景

SD カードを Mac と機体の間で抜き差しする往復が非効率という指摘を受けて、
TFTP 起動を再検討した。round 13–16 では「TFTP サーバの起動に sudo が必要で
AI 側から代替できない」として断念していたが、**NAS（`192.168.99.80`）では
`sudo -n` がパスワードなしで通る**ことが分かり、この制約が解消した。

### 実測した NAS の条件

```
$ ssh ku@192.168.99.80 'sudo -n true' → 成功（パスワード不要）
uname -a: Linux sweedmaster02 6.8.0-147-generic #147-Ubuntu x86_64
dnsmasq 2.91 が導入済み、既存サービスは inactive
ポート 69 は空き
インターフェイス: enp11s0 = 192.168.99.80（enp0s25 も 192.168.99.52）
```

### 構築したもの

**配信ディレクトリ**: `/srv/x96tftp`（root 所有、644）

```
Image                  41,757,184  bytes
rootfs.cpio.uboot      35,195,682  bytes
meson-g12a-x96-max-p-gxl.dtb  51,207 bytes
```

`/home/ku` 配下だとディレクトリが 750 で `dnsmasq`（`nobody` 実行）が
アクセスできず `Permission denied` になったため、`/srv` へ移した。

**設定**: `/etc/x96-tftp.conf`

```
port=0
interface=enp11s0
bind-interfaces
enable-tftp
tftp-root=/srv/x96tftp
```

`port=0` で DNS を無効化し、既存の DNS/DHCP に一切影響しない。DHCP も無効。

**起動**:

```sh
sudo dnsmasq --conf-file=/etc/x96-tftp.conf
```

### 検証（Mac から取得）

macOS の `tftp` コマンドは転送が途中で切れる（1468 バイトで停止）ため、
**`curl` を使う**:

```sh
curl -o /tmp/Image "tftp://192.168.99.80/Image"
```

| ファイル | サイズ | 所要時間 | SHA-256 照合 |
|---|---|---|---|
| `Image` | 41,757,184 | **59 秒** | `6a9b20e4…` 一致 |
| `rootfs.cpio.uboot` | 35,195,682 | **49 秒** | `ef4964aa…` 一致 |
| `gxl.dtb` | 51,207 | 瞬時 | `e0cc1dbf…` 一致 |

**Gigabit で約 0.7MB/s。77MB の一式が 2 分弱で配信できる。**

### U-Boot 側の使い方

`tools/uboot_boot_linux.py` に `--tftp-server` が実装済み:

```sh
/usr/bin/python3 tools/uboot_boot_linux.py --dev /dev/cu.usbserial-01025FC4 \
    --ramboot --unit 3 --tftp-server 192.168.99.80 \
    --dtb meson-g12a-x96-max-p-gxl.dtb --wait 240 --trace 60
```

U-Boot 側の設定は `ipaddr=192.168.99.56`、`serverip=192.168.99.80`。

**未検証**: U-Boot からの実際の `tftpboot` 取得。round 13–16 の実測では
`ipaddr=10.18.9.97, serverip=192.168.99.8` という既定値で、`tftpboot` コマンド
自体は利用可能だった。今回の構成で実際に取得できるかは次の起動で確認する。

## 追記（round 96 後半）: U-Boot から TFTP は使えない（実機で確認）

### サーバ側は正常

NAS（`192.168.99.80`）の dnsmasq TFTP は動作しており、Mac の `curl` から
3 ファイルすべてをハッシュ一致で取得できた（Image 59 秒、rootfs 49 秒）。

### U-Boot 側で失敗

```
setenv ipaddr 192.168.99.56
setenv serverip 192.168.99.80
tftpboot 0x08000000 meson-g12a-x96-max-p-gxl.dtb
```

```
Speed: 1000, full duplex
Using dwmac.ff3f0000 device
TFTP from server 192.168.99.80; our IP address is 192.168.99.56
Filename 'meson-g12a-x96-max-p-gxl.dtb'.
Load address: 0x8000000
Loading: *T T T T T T T
Retry count exceeded; starting again
```

**リンクは 1000/full で確立しているのに、パケットが届かない。**

### 切り分け結果

| 対象 | 結果 |
|---|---|
| `ping 192.168.99.80`（NAS） | **失敗**（`host is not alive`） |
| `ping 192.168.99.52`（NAS 別 NIC） | **失敗** |
| `ping 192.168.99.20`（Mac） | **失敗** |

**どのホストにも ping が通らない。** 特定ホストの問題ではなく、U-Boot の
ネットワークスタック自体が送受信できていない。

### 確認したこと

- `ethaddr = 90:0e:b3:24:81:7e`（設定済み）
- `env default -f -a ethaddr` でリセット後に再設定しても同じ
- NAS 側のファイアウォールは `policy ACCEPT`（遮断していない）
- NAS 側の dnsmasq ログにリクエストの記録なし（到達していない）
- `mii` コマンドは未搭載（PHY レジスタを直接読めない）
- Linux（Android）では同じ NIC で通信できる（`eth0` が `192.168.99.56` を取得）

### 結論

**この U-Boot（2015.01 vendor）の TFTP は、この機体では使えない。**
リンクが上がってもフレームが通らない。考えられる理由:

1. `dwmac.ff3f0000`（G12A の内蔵 MAC）の初期化が U-Boot 側で不完全
2. RTL8211F PHY の設定（速度・デュプレックスは読めても、実際の送受信が
   できていない）— vendor U-Boot がこの PHY 構成に対応していない可能性
3. TX 記述子/DMA の設定漏れ

いずれも vendor U-Boot のバイナリを直す必要があり、現実的でない。

### 代替

**SD カードでの起動に戻す。** ただし Mac 側の書き込みは `--update-boot` で
sudo 不要、1 回 10 秒程度なので、往復の手間は SD の抜き差しのみ。

## 追記（round 96 追加）: SD を使わない他の経路を実機で確認

TFTP が使えないため、代替を実機の U-Boot で調べた。

### U-Boot のコマンド一覧（`help` で確認）

| コマンド | 有無 |
|---|---|
| `fatload` | **あり** |
| `fatls` | **あり** |
| `fatwrite` | **なし** ← FAT への書き込みは不可 |
| `mmc write` | なし |
| `ext4write` | なし |
| `loady`（YMODEM） | **あり** |
| `loadx` | あり |
| `usb` | **あり** |
| `mii` | なし |

**`fatwrite` が無いので、U-Boot から SD の FAT を書き換えることはできない。**

### USB ストレージは使える可能性

```
usb start
(Re)start USB...
USB0:   USB3.0 XHCI init start
Register 3000140 NbrPorts 2
Starting the controller
USB XHCI 1.10
scanning bus 0 for devices... 2 USB Device(s) found
       scanning usb for storage devices... 0 Storage Device(s) found
```

**U-Boot の USB ホストは動作している**（2 デバイス検出 = DT295 と別の何か）。
USB メモリを挿せば `usb storage` として認識され、`fatload usb 0:1 ...` で読める
可能性がある。

**ただし未検証**: USB メモリを挿して `0 Storage Device(s)` が
`1 Storage Device(s)` に変わるか、実際に試す必要がある。

### YMODEM は非現実的

| ファイル | 理想（115200bps） | 実測ペース換算 |
|---|---|---|
| `rootfs.cpio.uboot`（35MB） | 51 分 | 25 時間 |
| `Image`（41.7MB） | 60 分 | 29 時間 |

U-Boot が ACK を返すまでに時間がかかるため、実測ペースでは論外。

### 結論（現時点）

1. **TFTP**: U-Boot のネットワークスタックが送受信できず不可（本ラウンドで確認）
2. **U-Boot からの SD 書き込み**: `fatwrite` が無く不可
3. **YMODEM**: 時間がかかりすぎて不可
4. **USB メモリ**: **可能性あり、要検証**（U-Boot の USB ホストは動作している）

### 次に試すこと

USB メモリ（FAT32、起動ファイルを入れたもの）を 3代目に挿し、U-Boot で
`usb start` → `usb storage` が認識されるか確認する。認識されれば
`fatload usb 0:1 0x01080000 Image` で起動でき、SD の抜き差しが不要になる。

## 追記（round 98）: U-Boot の USB で DT295 が認識されている

```
usb info

1: Hub,  USB Revision 3.0
 - u-boot XHCI Host Controller
 - Class: Hub

2: Vendor specific,  USB Revision 2.0
 - PIXELA CORPORATION PIX-DT295 VK12041874
 - Class: (from Interface) Vendor specific
```

**`usb start` の "2 USB Device(s) found" は root hub と DT295 だった。**
DT295 は U-Boot の段階でも USB で見えている（シリアル番号 `VK12041874`）。

`0 Storage Device(s) found` は USB メモリが挿さっていないため。

### USB メモリ挿入時に期待される動作

USB メモリ（FAT32）を挿して `usb start` すると、
`1 Storage Device(s) found` になり、`fatload usb 0:1 ...` が使えるはず。

**未検証**: 実際の USB メモリでの認識と fatload。

### 準備済みのファイル

`work/usbstick/` に配置済み（SD と同じ最新版）:

| ファイル | サイズ | SHA-256（先頭） |
|---|---|---|
| `Image` | 41,757,184 | `6a9b20e4` |
| `rootfs.cpio.uboot` | 35,195,682 | `ef4964aa` |
| `meson-g12a-x96-max-p-gxl.dtb` | 51,207 | `e0cc1dbf` |

USB メモリを FAT32 でフォーマットし、この 3 ファイルをルートに置けばよい。

## 追記（round 99）: U-Boot の usb サブコマンド（USB メモリ検証用）

```
usb start - start (scan) USB controller
usb reset - reset (rescan) USB controller
usb stop [f] - stop USB [f]=force stop
usb tree - show USB device tree
usb info [dev] - show available USB devices
usb test [dev] [port] [mode] - set USB 2.0 test mode
usb storage - show details of USB storage devices
usb dev [dev] - show or set current USB stor
```

USB メモリを挿した後は:

```
usb reset          # 再スキャン
usb storage        # ストレージとして見えているか
fatls usb 0:1      # FAT の中身
```

で確認できる。`fatls` があるので、`fatload` の前に中身を確かめられる。

### NAS の TFTP サーバの扱い

`x96tftp.service`（dnsmasq、`/srv/x96tftp`）は稼働中だが、**3代目の U-Boot
では使えないと判明している**（本機のネットワークスタックが送受信できない）。

- 停止する場合: `sudo systemctl stop x96tftp && sudo systemctl disable x96tftp`
- 他機（1代目・2代目）は同じ vendor U-Boot 系列なので、同じ理由で使えない
  可能性が高い（未検証。他機は現在停止中でシリアルも3代目に接続されている）。

**判断**: 現時点では使い道が無いため、停止してよい。再開する場合は
`/etc/x96-tftp.conf` と `/srv/x96tftp` が残っていれば
`sudo dnsmasq --conf-file=/etc/x96-tftp.conf` で復帰できる。

## 追記（round 100）: TFTP 不通の原因をレジスタレベルで特定

### 測定したこと

NAS 側で `tcpdump` を回しながら U-Boot から `ping` した結果、**ARP 要求は
NAS から出るが、U-Boot からのパケットは一切出ていない**。MAC アドレスは NAS の
近隣テーブルに載っている（`192.168.99.56 lladdr 90:0e:b3:24:81:7e STALE`）。

### DWMAC のレジスタ（`macreg d`）

**MAC レジスタ**

```
[0x0040] 0x80007e81   ← MAC_CONFIG
```

| ビット | 値 | 意味 |
|---|---|---|
| bit0 RE | **1** | 受信有効 |
| **bit1 TE** | **0** | **送信が無効！** |
| bit14 FES | 1 | 100M モード（リンクは 1000/full なのに） |

**DMA レジスタ**

```
DMA_OP_MODE [0x0000] = 0x1c800
  ST (Start/Stop TX, bit13) = 0     ← 送信停止
  SR (Start/Stop RX, bit 1) = 0     ← 受信停止
```

**PHY レジスタ**（正常）

```
reg0 (BMCR) = 0x5040   オートネゴ有効
reg1 (BMSR) = 0x79ad   Link Status=1, Auto-Neg Done=1
```

### 原因

**U-Boot の DWMAC 初期化が `TE`（送信有効）を立てていない。** さらに DMA の
送受信開始ビットも 0。

手動で `macreg w 0x40 0x80007e83`（TE=1）と `macreg w 0x0 0x1e802`
（ST/SR=1）を書くと値は入るが、**`ping` を実行すると U-Boot が DWMAC を
再初期化して上書きしてしまう**。

### Amlogic 独自のデバッグコマンド（今回発見）

```
phyreg d / r reg / w reg val   ethernet phy register read/write/dump
macreg d / r reg / w reg val   ethernet mac register read/write/dump
ethloop                        loopback test using ethernet test package
```

`mii` コマンドは無いが、これらで PHY/MAC を直接操作できる。

### 次のアプローチ

U-Boot の DWMAC ドライバが `FES=1`（100M）のまま送信を有効化していない。
リンクは 1000/full なので、**PHY の速度設定と MAC の速度設定が食い違っている**。

考えられる対処:

1. **PHY を 100M に固定する**（`phyreg w 0 0x2100` で AN 無効 + 100M full）
   → リンクが 100M になり、MAC の FES=1 と一致する可能性
2. `ethloop` で送受信そのものをテストする
3. U-Boot の DWMAC 初期化コードを追う（バイナリなので困難）

**まず 1 を試す価値がある。** 100M でも TFTP で 77MB を送るのに 10 分程度。

## 追記（round 100 後半）: DWMAC の送信が機能しない（実機で深掘り）

### 試したことと結果

| 試行 | 結果 |
|---|---|
| `phyreg w 0 0x2100`（100M full 固定、AN 無効） | U-Boot が「Waiting for PHY auto negotiation」でタイムアウト |
| `phyreg w 4 0x0180`（100M のみアドバタイズ）→ AN 再開 | AN 完了、`Speed: 1000, full duplex` |
| `macreg w 0x40 0x80007e83`（TE=1） | 値は入るが、`ping`/`ethloop` が DWMAC を再初期化して TE=0 に戻る |
| `macreg w 0x0 0x1e802`（DMA ST/SR=1） | 同上、上書きされる |
| TE=1 にした直後に `ethloop` | **`loopback test failed`** |
| `usb stop` + `ethact` 明示 | 変化なし |
| `gatewayip` 修正 | 変化なし |

### 決定的な事実

```
ethloop
Speed: 1000, full duplex
loopback test failed
```

**`ethloop` は内部ループバックテスト。外部ネットワークと無関係に失敗する。**
つまり MAC の送信経路そのものが機能していない。

### MAC の状態（`ethloop` 後）

```
MAC_CONFIG [0x0040] = 0x80007e81
  RE=1 (受信有効), TE=0 (送信無効), FES=1 (100M)
```

**U-Boot の DWMAC 初期化が終わった状態でも TE=0。** これはドライバが
送信を有効化していないことを意味する。

U-Boot 2015.01（Aug 17 2020 ビルド、crosstool-NG linaro 1.13.1）。

### 結論

**この U-Boot の `dwmac` ドライバは、この機体（X96Max_P、RTL8211F）で
送信を有効化できない。** PHY は正常（Link Status=1、AN Done=1）、MAC アドレスも
正しく設定されているが、`TE` が立たないためフレームが出ない。

`macreg` で手動で TE=1 にしても、`ping`/`ethloop` が毎回 DWMAC を再初期化して
TE=0 に戻す。U-Boot のバイナリを修正しない限り回避できない。

### 可能な残りの道（いずれも重い）

1. **U-Boot をビルドし直す** — vendor のソースが必要。`dwmac` の該当箇所に
   TE=1 を足す。ただし Amlogic の U-Boot ソースは非公開部分が多い。
2. **Linux 側から TFTP** — 目標は「U-Boot から TFTP で起動」だが、Linux が
   起動できるなら SD は既に不要（rootfs が RAM にある）。つまり TFTP の利点は
   「カーネル/rootfs の差し替え」だけであり、Android 側から `/dev/block` 経由で
   SD を書く方が現実的かもしれない（要検証）。
3. **`loadx`（XMODEM）** — `loady` より遅いので不可。

**現時点で U-Boot からの TFTP は不可能。** 原因はレジスタレベルで特定済み。

## 追記（round 101）: Android から SD を書き換える経路が使える（実証済み）

U-Boot の TFTP が使えないため、**Android（vendor Linux）経由で SD の FAT を
書き換える**方式を実機で確立した。

### SD の見え方（Android）

```
# /proc/partitions
179  128  31166976  mmcblk1        ← SD カード（31.9GB）
179  129    131072  mmcblk1p1      ← FAT 区画（134MB）

# マウント
/mnt/media_rw/0708-F8A4            ← 直接読み書きできる（media_rw 所有）

# 空き容量
126M 中 75M 使用、51M 空き
```

### 手順（実証済み）

```sh
# 1. Mac → Android へ転送（25.9 MB/s、41.8MB が 1.5 秒）
adb push output/ramboot-unit3/Image /data/local/tmp/Image.new

# 2. Android 側でハッシュ確認
adb shell 'su -c "sha256sum /data/local/tmp/Image.new"'

# 3. SD の FAT へコピー
adb shell 'su -c "cp /data/local/tmp/Image.new /mnt/media_rw/0708-F8A4/Image; sync"'

# 4. 検証
adb shell 'su -c "sha256sum /mnt/media_rw/0708-F8A4/Image"'
```

### 実績

| ファイル | 更新前 | 更新後 |
|---|---|---|
| `Image` | `b3099f99` | **`6a9b20e4`**（ハッシュ一致を SD 上で確認） |
| `rootfs.cpio.uboot` | `ef4964aa` | 変更なし（既に最新） |

**SD カードを Mac に抜き差しせずに、カーネルを差し替えられた。**

### 制約

- 空き容量が 51MB。`Image`（41.8MB）の更新は可能だが、`rootfs`
  （35.2MB）と同時に更新すると溢れる。**1 ファイルずつ、古いものを消してから**
  置き換える（`cp` は上書きなので一度は両方存在する点に注意）。
- Android が起動している必要がある。U-Boot からは SD を書けない。
- `su` が必要（この機体は ADB の `su -c` が通る）。

### これで解決したこと

**SD の抜き差しが不要になった。** 以降のカーネル/rootfs の更新は、
Android を起動 → `adb push` → `cp` で完結する（1 ファイル 2 秒程度）。

## 追記（round 101 後半）: skip-glue-init が不十分だった（usb_post_init も書く）

### 発見

`6a9b20e4` の Image（`skip-glue-init` のみ）で起動したところ、**別の場所で
同じ例外**になった。

```
dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
dwc3-meson-g12a ffe09000.usb: USB3 ports: 0
Internal error: synchronous external abort

Call trace:
 regmap_mmio_read32le
 _regmap_update_bits
 dwc3_meson_g12a_usb_otg_apply_mode+0x120/0x160
 dwc3_meson_gxl_usb_post_init+0x3c/0x50      ← ここ
 dwc3_meson_g12a_probe+0x354/0x590
```

**`usb_init_glue` はスキップできていた**が、その後の `usb_post_init`
（GXL 版）が `usb_otg_apply_mode` を呼び、そこで glue レジスタを書いて例外。

### 修正

`dwc3_meson_gxl_usb_post_init` の先頭にも同じガードを追加した:

```c
static int dwc3_meson_gxl_usb_post_init(struct dwc3_meson_g12a *priv)
{
	int ret;

	/* The glue block is not writable on this board; see usb_init_glue. */
	if (of_property_read_bool(priv->dev->of_node, "amlogic,skip-glue-init"))
		return 0;

	ret = priv->drvdata->set_phy_mode(priv, USB2_OTG_PHY, ...);
```

パッチ 0004 に 2 つ目のハンクとして追加し、クリーンなツリーで
4 パッチすべての適用を確認（`Image` = `6779f086`）。

### 学び

`skip-glue-init` は **glue レジスタを触るすべての経路**に必要。今回で
`usb_init_glue`（USB_R1/USB_R5/usb3_init/otg_apply_mode）と
`usb_post_init`（otg_apply_mode）の 2 箇所を塞いだ。他に `set_phy_mode`
（`dwc3_meson_gxl_set_phy_mode`）も glue を触る可能性があるので、次の起動で
別の場所で例外が出たら同じように塞ぐ。

## 追記（round 103）: 起動ログの欠落を解消（シリアル読み取りの高速化）

### 問題

起動ログが 2,601 バイトしか取れず、文字化けしていた:

```
wralue calculated using timer frequency.. 48.00 BogoMIPS
t>                                                      ← 行の途中で切れる
76] vgaarb: loaded
```

**`abort` の文字すら含まれていなかった。** 例外の全文を読む前に
シリアルのカーネルバッファが溢れて失われていた。

### 修正

`tools/uboot_boot_linux.py` の `Console.pump`:

```python
# 修正前
r, _, _ = select.select([self.fd], [], [], 0.1)
data = os.read(self.fd, 4096)

# 修正後
r, _, _ = select.select([self.fd], [], [], 0.02)   # 0.1 → 0.02
data = os.read(self.fd, 65536)                     # 4KB → 64KB
```

`select` の待ちを 5 倍細かくし、1 回の読み取り量を 16 倍にした。

### 部分的に取れた情報（修正前のログから）

コールトレースが次を示していた:

```
of_platform_bus_create+0x194/0x390
bus_probe_device_deferred
deferred_probe_work_func
```

**deferred probe（後回しにされた probe）で例外が起きている。**
これは「依存が揃うのを待って後から probe される」ドライバ、つまり
dwc3/dwc2 のいずれか。

### 次の確認

修正版の取得で完全なコールトレースを取り、例外の発生源を特定する。
`dwc2` を無効化しても例外が残っているため、`dwc3` の別経路か、
別ドライバの可能性がある。

## 追記（round 104）: シリアル改善後も完全なコールトレースは取れない

### 状況

`select` を 0.02 秒、読み取りを 64KB に改善した後も、取得できたログは
1,615 バイト。文字化けは減ったが、**行の途中で混ざる**現象が残る:

```
alternatives: applying r SCSI generic (bsg) driver version 0.4 loaded
[   [    0.859426] ledtrig-cpu: registered ...
```

末尾は `__bus_probe_device+0x38/0xb0` で切れ、**例外のドライバ名が読めない**。

### 原因

USB-Serial（CP2104）の転送能力が、この kernel の起動ログ（数百 KB）に対して
不足している。ボーレート 115200 では追いつかない。

### 現在の例外の状態

| 段階 | 例外の発生源 |
|---|---|
| 最初 | `usb_init_glue+0x38`（USB_R1） |
| 0002 適用後 | 同上 |
| 0004（hunk1）適用後 | `gxl_usb_post_init` → `otg_apply_mode` |
| 0004（hunk2）適用後 | `dwc2_driver_probe` |
| **`&dwc2 { disabled }` 後** | **不明（`__bus_probe_device` 経由 = deferred probe）** |

### 次の方針

シリアルでの特定を続けるより、**USB 関連ドライバを絞り込んで例外源を
切り分ける**方が確実。

1. `-nousb` DTB（全 USB 無効）で起動し、Linux がユーザー空間まで到達するか
   確認する。到達すれば「USB 以外は正常」が確定する。
2. 通れば、USB を有効に戻す際に、dwc2・dwc3 の PHY の順序を変える
   （`phys` リストの並び、`status` の組み合わせ）。

現在の DTB（`-nousb`）は以前からビルド済みで、SD にも入っている。

## 追記（round 108）: 診断 printk 入りのカーネルをビルドした

### 目的

シリアルが起動ログを落とすため、コールトレースが読めない。代わりに
**短い `pr_err` マーカー**を glue 経路の入口に置き、「どこまで進んで落ちたか」
を 1 行で分かるようにする。

### 追加したマーカー（`0005-dwc3-meson-probe-trace.patch`）

```
DBG usb3_init            dwc3_meson_g12a_usb3_init の入口
DBG gxl_usb_post_init    dwc3_meson_gxl_usb_post_init の入口
DBG otg_apply_mode       dwc3_meson_g12a_usb_otg_apply_mode の入口
```

出力例（期待）:

```
[1.31] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.31] dwc3-meson-g12a ffe09000.usb: USB3 ports: 1
[1.31] DBG gxl_usb_post_init        ← ここまで来た
[1.32] Internal error
```

これで「`usb3_init` に到達したか」「`post_init` か」が分かる。

### ビルド

5 パッチすべて適用OK。`Image` = `76915354`。

### 一時的なパッチであること

`0005` はデバッグ専用。原因が判明したら削除する。

## 追記（round 109）: 診断マーカーを 4 箇所に拡張（最終形）

### 追加したマーカー

```
DBG usb_init_glue node=<name> skip=<0|1>   glue の入口。ノード名と判定結果
DBG usb3_init                              usb3_init の入口
DBG gxl_usb_post_init                      post_init の入口
DBG otg_apply_mode                         otg_apply_mode の入口
```

`usb_init_glue` のマーカーが重要。**`skip=1` なら glue は即 return
しているので、例外は別の場所**。`skip=0` ならプロパティが届いていない。

`node=` で glue ノードの名前も出るので、`priv->dev->of_node` が
`/soc/usb@ffe09000` を指しているか確認できる。

### 期待される出力と判定

```
[1.31] dwc3-meson-g12a ffe09000.usb: USB3 ports: 1
[1.31] DBG usb_init_glue node=/soc/usb@ffe09000 skip=1    ← 正常に読めている
[1.31] DBG gxl_usb_post_init                              ← ここへ進んだ
[1.32] Internal error                                     ← post_init 内で落ちた
```

| 観察 | 意味 |
|---|---|
| `skip=1` で `usb3_init` が出ない | glue は正しくスキップ。例外は post_init 以降 |
| `skip=1` でも `usb3_init` が出る | 別の呼び出し経路がある |
| `skip=0` | プロパティが届いていない（DTS か of_node の問題） |
| `node=(null)` | `priv->dev->of_node` が無い |

### ビルド

5 パッチ適用OK。`Image` = `8de56a3b`。4 マーカーすべてがバイナリに含まれる
ことを文字列検索で確認済み。

### 状態

**機体が停止中のため、書き込みと起動ができていない。**
Android か Linux が起動すれば、SD へ書いて（両方の経路が確立済み）検証できる。

## 追記（round 111）: 診断マーカー4つが確実に入った Image を確定

### 問題

`0005` に `usb_init_glue` のマーカーを追記したが、**ビルド時に4つ目のハンクが
適用されず 3 つしか入らなかった**（`patch` が部分適用）。Image の文字列検索で
`DBG usb_init_glue` が False だったことで発見。

### 対処

`usb_init_glue` のマーカーを **別パッチ `0006-dwc3-meson-glue-trace.patch`**
として分離した。先行パッチが同じファイルを書き換えているため、追記ではなく
独立したハンクにした方が確実に当たる。

### 確定した Image

`Image` = **`3d48ee0e`**（6 パッチ適用）。**4 マーカーすべてがバイナリに含まれる
ことを文字列検索で確認済み**:

```
DBG usb_init_glue          True
DBG usb3_init              True
DBG gxl_usb_post_init      True
DBG otg_apply_mode         True
```

### 期待される出力

```
[1.31] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.31] dwc3-meson-g12a ffe09000.usb: USB3 ports: 1
[1.31] DBG usb_init_glue node=/soc/usb@ffe09000 skip=1
[1.31] DBG gxl_usb_post_init
[1.32] Internal error
```

`skip=1` なら glue は正しくスキップされ、例外は `post_init` 以降。
`skip=0` ならプロパティが届いていない。

### ツール

`tools/install-boot.py` で「SD 書き込み → 起動 → DBG 行表示」を 1 コマンドに
まとめた（`1e5a976`）。Android と Linux の両経路を自動判別する。

### 状態

**機体が停止中のため未検証。** 電源が入れば
`tools/install-boot.py --dtb meson-g12a-x96-max-p-gxl.dtb` の 1 回で診断まで進む。

## 追記（round 113）: 実機が 11 ラウンド連続で応答なし（現状の停止点）

### 確認したこと

| 確認 | 結果 |
|---|---|
| `/dev/cu.usbserial-01025FC4` | **存在する**（22:43 更新、USB 接続は生きている） |
| シリアル応答 | **なし**（0 バイト） |
| Android (5555) | 不通 |
| Linux SSH (22) | 不通 |

**シリアルデバイスは Mac に認識されているが、機体から何も返らない。**
機体の電源が入っていないか、起動に失敗して停止している。

### 準備完了しているもの（電源が入れば即実行できる）

| 項目 | 状態 |
|---|---|
| `Image` = `3d48ee0e` | 4 つの `DBG` マーカー入り（文字列検索で確認済み） |
| `rootfs.cpio.uboot` = `ef4964aa` | チューナー待機＋CM 特徴量 |
| `gxl.dtb` | `skip-glue-init`、`dwc2` 無効、PPC |
| `tools/install-boot.py` | 書き込み→検証→起動→DBG 表示を 1 コマンド |
| テスト | 268 件通過 |

### 実行するコマンド（電源投入後）

```sh
cd ~/Desktop/work/X96MaxLinuxTSEnc
python3 tools/install-boot.py --dtb meson-g12a-x96-max-p-gxl.dtb
```

`DBG usb_init_glue node=... skip=...` の 1 行で、以下が確定する:

- `skip=1` → glue はスキップ済み。例外は別の場所（`usb3_init` の外側）
- `skip=0` → プロパティが届いていない（DTB の取り違え or `of_node` の問題）

### 静的解析の結論（実機確認待ち）

`gxl_set_phy_mode` は glue を触らない。`usb3_init` と `otg_apply_mode` は触るが、
両方 `skip-glue-init` のガードの内側にある。したがって `skip=0` が最有力。

## 追記（round 119）: dwc2 を有効に戻した DTB を用意（未検証）

### 変更

`board/x96max/dts/amlogic/meson-g12a-x96-max-p-gxl.dts`:

```dts
&dwc2 {
	status = "okay";      // disabled から戻した
};
```

`gxl.dtb` = `30d2605d`。`dtc` で `usb@ff400000` の `status = "okay"` を確認済み。

### 根拠（round 118 の観察）

round 37 の成功ログでは **dwc2 が PHY を初期化してから xhci が
`0xff500000` をマップ**していた:

```
[1.865] dwc2 ff400000.usb: 初期化
[1.892] xhci-hcd: io mem 0xff500000    ← 成功
```

現在は dwc2 を無効化しているため、その前提が失われている可能性がある。
dwc2 の以前の例外（round 106）は glue 経由だったが、今は
`skip-glue-init` で抑えているので再発しない見込み。

### 検証コマンド（電源投入後）

```sh
cd ~/Desktop/work/X96MaxLinuxTSEnc
python3 tools/install-boot.py --dtb meson-g12a-x96-max-p-gxl.dtb --trace 60
```

### 判定

| 観察 | 意味 |
|---|---|
| `xhci-hcd: io mem 0xff500000` が出る | **解決**。dwc2 の PHY 初期化が前提だった |
| また `dwc3_core_probe+0x96c` | dwc2 は無関係。別の原因を探す |
| `dwc2_driver_probe` で落ちる | glue 経由の例外が再発。dwc2 は無効に戻す |

### 現在の Image

`Image` = `29ecb5ec`（7 パッチ、4 マーカー入り）。
dwc3 の clocks/resets も入っている（round 115 の変更、効果は未確認）。
