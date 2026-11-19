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
