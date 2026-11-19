# 3代目 X96Max_P 立ち上げ - Checkpoint（2026-11-19 夜）

## 現在の状態

| 項目 | 状態 |
|---|---|
| Linux 起動（RAM rootfs） | **成功**。例外 0、`x96max-p3 login:`、eth0 1Gbps/Full、DHCP `192.168.99.56` |
| Ethernet | **確定**。外部 RTL8211F（RGMII）。内蔵PHY版 DTB では NO-CARRIER |
| 録画設定 | `PIX_CHANNEL=17`（SBS、service_id 35864）、地上波、DT295 内蔵カード復号 |
| DT295 受信 | **成功**（ch17 で `locked:1`、`tables_ready:1`、5秒で 9.9MB） |
| USB（Linux での dwc3） | **未解決**。`dwc3_meson_g12a_usb2_init_phy` で例外 |
| SD 書き込み | 未実施（SD が実機内）。イメージは `output/ramboot-unit3/ramboot.img` |
| NAS 登録 | 未実施。`192.168.99.56-17` に古いノード（X96Max-Plus2、テレビ静岡）が残存 |

## 機体の識別結果

| 項目 | 値 |
|---|---|
| U-Bootプロンプト | `g12a_u212_v1#` |
| U-Bootの`aml_dt` | `sm1_ac213_4g`（soc=sm1, platform=ac213, variant=4g） |
| MAC | `90:0e:b3:24:81:7e` |
| シリアル | `/dev/cu.usbserial-01025FC4`（CP2104、`01025FCC`） |
| SDカード | 31.9GB、`SL32G` |
| eMMC | U-Bootでは `SDIO Port C` 58.2GiB、Linuxでは初期化失敗（`-EILSEQ`） |

## USB 障害の原因（ベンダー DT との照合で確定）

ベンダー Android の device tree を実機から読み、レジスタ配置を確定した。

```
usb2phy@ffe09000  compatible = amlogic,amlogic-new-usb2-v2
  reg = 0xffe09000(0x80), 0xffd01008(0x100), 0xff636000(0x2000), 0xff63a000(0x2000)
usb3phy@ffe09080  compatible = amlogic,amlogic-new-usb3-v2
  reg = 0xffe09080(0x20), 0xffd01008(0x100)
```

`/proc/iomem`（vendor kernel）でも `ffe09000-ffe0907f : /usb2phy@ffe09000`、`ff500000-ff507fff : /dwc3@ff500000`。

**mainline G12A は `0xffe09000` を「USB glue」として扱うが、実機では USB2 PHY の制御ブロック。** この解釈差が `dwc3_meson_g12a_usb2_init_phy` の例外の原因。

### 誤診の記録

`devmem` で `0xff500000` 等が Bus error になったのは、dwc3 を無効化した DTB で起動したため `clk: Disabling unused clocks` が `g12a_usb_general` を止めた副作用（enable_count=0）。**周辺レジスタを `devmem` で測る前に、そのノードが有効でクロックが供給されていることを確かめる。**

## 次に試すこと（ビルド済み・未検証）

`meson-g12a-x96-max-p-usb.dts`（ハッシュ `77e7d6bc…`）を実機で試す。

- G12A の glue ノードを削除し、`usb@ff500000` を `snps,dwc3` として直接記述
- PHY は既存の `usb2_phy0`（`0xff636000`）と `usb2_phy1`（`0xff63a000`）を参照
- DTB 検証済み：`status="okay"`、phandles 解決済み

これで例外が出なければ、「glue ドライバさえ外せば動く」ことが確定し、DT295 の受信・録画へ進める。出る場合はカーネルパッチ（glue の u2p 書き込みを回避する）が必要。

## 再開手順

1. SD を書き込み（`scripts/write-sd.py`、sudo が必要なためユーザーのターミナル実行）
2. 実機に戻して電源投入
3. `tools/uboot_boot_linux.py --dev /dev/cu.usbserial-01025FC4 --ramboot --unit 3 --dtb meson-g12a-x96-max-p-usb.dtb`
4. 例外の有無を確認。出なければ `tools/check-unit3-acceptance.py --host 192.168.99.56 --channel 17` で段階判定
5. NAS の `192.168.99.56-17`（古いノード）を削除してから登録

## 受け入れ基準

`40-acceptance.md` に、2代目の実測値（avc 1440x1080、6Mbps、vbr、gopSec 2、`totalDropped:0`）を基準として明記した。判定は `tools/check-unit3-acceptance.py` で行う（boot / receipt / stream / nas の4段階、全205テスト通過）。

## 未着手・既知の課題

- 起動時の DHCP がリンク確立前に走り失敗する。`wait-carrier` を overlay に追加済み（未ビルド・未検証）
- eMMC が Linux で `-EILSEQ` により初期化失敗（RAM 起動のため運用には影響なし）
- 3代目の SSH 公開鍵が未配置（シリアルのみ）。`~/.ssh/id_ed25519.pub` が既存機の authorized_keys と一致することを確認済み
- `pix-usb-reset` が録画サービス再起動時に USB をリセットし、機体が無応答になった事象が未解明
- ビルド設定の修正：パッチが `meson-vdec.mod.c` の削除差分を含みクリーンビルドで失敗していた→除去。`BR2_LINUX_KERNEL_INTREE_DTS_NAME` は unit fragment 側へ移動
