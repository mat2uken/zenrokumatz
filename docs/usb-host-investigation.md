# X96Max_P の USB ホストが mainline Linux で動かない件：調査引き継ぎ

2026年11月10日のセッション終了時点の記録。PX-S1UD と USB カードリーダーが本体の USB ポートで認識されない問題について、ここまでに分かったこと、試して駄目だったこと、次に疑うべき点をまとめる。

前提: Android（vendor カーネル 4.9）では USB が動いていた。したがってハードウェア故障ではなく、mainline カーネル 6.18 と DTS の組み合わせに原因がある可能性が高い。安易にハードウェア故障と結論づけない。

---

## 1. 環境

| 項目 | 内容 |
| --- | --- |
| 本体 | X96Max_P、Amlogic G12A（S905X2 系）、RAM 2 GiB |
| カーネル | Linux 6.18.62（Buildroot 2026.08.1 でビルド） |
| 起動 | eMMC の既存 U-Boot 2015.01 から SD 上の Linux を手動起動 |
| 接続 | UART `/dev/cu.usbserial-0001` 115200 8N1、root（パスワード空）、SSH は鍵認証で `root@192.168.99.109` |
| 作業先 | `/Users/kenichim/Desktop/work/X96MaxLinuxTSEnc` |
| DTS | `board/x96max/dts/amlogic/meson-g12a-x96-max-p.dts` |
| ビルド | `./scripts/build-linux.sh linux-reconfigure` → `./scripts/build-linux.sh` |

注意: `board/x96max/patches/linux/` のパッチは展開時にしか適用されない。パッチや DTS を変えたら必ず `linux-reconfigure` から通すこと。通常ビルドだけでは古い成果物が残る（実際にこれで一度はまった）。

---

## 2. 症状

- `lsusb` は root hub 2件のみ。PX-S1UD（`3275:0080`）もカードリーダーも現れない
- USB キーボード・マウスを挿しても認識されない（デバイス種別を問わず全滅）
- `siano-ts --list` も空
- カーネルログに USB 接続・切断のイベントが一切出ない

---

## 3. ハードウェア構成の把握（実測）

### USB コントローラの配置

G12A は USB を 2 系統持つ。DTS は `meson-g12-common.dtsi` で次のように定義されている。

```
usb@ffe09000 (親, compatible="amlogic,meson-g12a-usb-ctrl", dr_mode="otg")
  ├── usb@ff400000 (dwc2, dr_mode="peripheral")  ← USB2.0 ポート
  └── usb@ff500000 (dwc3, dr_mode="host")        ← USB3.0 ポート
```

ボード DTS（`meson-g12a-x96-max.dts`）は `&usb { dr_mode = "host" }` で親だけを上書きしている。子の dwc2 は `peripheral` のまま残る。

### 実機の状態

| 対象 | レジスタ | 値 | 解釈 |
| --- | --- | --- | --- |
| dwc2 | GHWCFG2 `0xff400048` | `0x2288D854` | op_mode(bits25:22)=0x8 = `NO_SRP_CAPABLE_DEVICE`。ハードウェア的にデバイス専用 |
| dwc2 | GUSBCFG `0xff40000c` | `0x00001400` | ForceDevMode(bit30)=0, ForceHostMode(bit29)=0。モードは GHWCFG2 の固定値 |
| dwc3 | GSNPSID `0xff50c120` | `0x5533320A` | dwc3 コアは応答する（Version 3.30a） |
| dwc3 | GCTL `0xff50c100` | `0x00000001` | PrtCapDir(bits13:12)=0 |
| dwc3 | GUSB2PHYCFG0 `0xff50c200` | `0x00102500` | PHYSoftRst(bit30)=0（当初は 1 だった。手動でクリアした） |
| dwc3 | GUSB3PIPECTL0 `0xff50c2c0` | `0x010C0002` | SuspendSS(bit17)=0（当初は 1。手動でクリアした） |
| dwc3 | DCTL/DEVTEN/DSTS `0xff50c704/708/70c` | すべて `0` | デバイスモード側のレジスタが未初期化 |
| xhci | USBCMD `0xff500020` | `0x00000005` | Run(bit0)=1, INTx=1, HSEE=1 |
| xhci | CONFIG `0xff500058` | `0x00000040` | MaxSlotsEn=64 |
| xhci | DCBAAP `0xff500050` | `0x04B8F000` | 設定済み |
| xhci | PORTSC1 `0xff500400` | `0x00000000` | PP=0, CCS=0 |
| xhci | PORTSC2 `0xff500410` | `0x00000000` | PP=0, CCS=0 |
| xhci | PORTSC3 `0xff500420` | `0x0A0002A0` | PP=1, CCS=0, PLS=5(RxDetect), WPR=1 |

重要: レジスタの正しいオフセット

- xhci のケーパビリティは `0xff500000`。`CAPLENGTH=0x20` なので、オペレーショナルレジスタは `0xff500020` から。`USBCMD` は `0xff500020`、`CONFIG` は `0xff500058`（`0x20+0x38`）。当初 `0xff500070` を読んで誤診した。
- dwc3 のグローバルレジスタは `0xff500000 + 0xc100`（`GCTL` は `0xff50c100`）。当初 `0xff5000c100` を読んで Bus error になり誤診した。
- GLUE レジスタは `0xffe09080`（`G12A_GLUE_OFFSET=0x80`）。`USB_R1` は `0xffe09084`。当初 `0xffe09000` を読んで誤診した。

### 割り込みの実測

```
16: 0 0 0 0  GICv2 63 Level  ff400000.usb, ff400000.usb
17: 3 0 0 0  GICv2 62 Level  xhci-hcd:usb1
```

xhci の割り込みは 3 回で止まっている。 `USBCMD.Run=1` なのに割り込みが増えない。これはコントローラのレジスタ値は設定されているが、ハードウェアが実際には動作していないことを示す。

---

## 4. 試して駄目だったこと

| 試したこと | 結果 |
| --- | --- |
| DTS で `&dwc2 { dr_mode = "host" }` | 実機 DT は `host` になったが、ドライバが `Configuration mismatch. dr_mode forced to device` で戻す。GHWCFG2 がデバイス専用のため原理的に不可 |
| xhci を unbind/bind | バスは再登録されるが `USBCMD=0x5` のまま。割り込みは増えない |
| `usb_role` を device→host に切り替え | `switching to Host Mode` のログは出るが、ポート状態は変化なし |
| PORTSC1/2 に PP(bit9) を書く | `0x00000000` のまま。書込みが反映されない |
| PORTSC3 に PR(bit4) を書く | `0x0A0002A0` のまま。書込みが反映されない |
| GLUE `USB_R1` の `PORT_POWER_CONTROL_PRESENT`(bit17) を立てる | 既にドライバが設定済み（`0xFF020000`）。書込みは反映されないが、必要なら既に立っている |
| `GUSB3PIPECTL0` の SuspendSS(bit17) をクリア | 書込みは成功（`0x010E0002`→`0x010C0002`）。ポート検出は変わらず |
| `GUSB2PHYCFG0/1` の PHYSoftRst(bit30) をクリア | 書込みは成功（`0x40102500`→`0x00102500`）。ポート検出は変わらず |
| PX-S1UD を USB3.0 → USB2.0 へ移動 | dwc2 はデバイス専用なので認識せず |
| USB キーボード・マウスを接続 | 認識せず（デバイス種別の問題ではない） |

書込み可否の非対称性が最大の手がかり: dwc3 のグローバルレジスタ（`0xff50c2xx`）は書けるが、xhci のポートレジスタ（`0xff5004xx`）は書けない。同じコントローラのレジスタなのに挙動が違う。

---

## 5. 現在の DTS の状態

`board/x96max/dts/amlogic/meson-g12a-x96-max-p.dts` の末尾に以下を追加済み（ビルド・DTB 生成済み、実機への反映は未実施）。

```dts
&dwc2 {
	dr_mode = "host";
};

/*
 * snps,dis_u2_susphy_quirk keeps the USB2 PHY in reset on this board:
 * both GUSB2PHYCFG registers stay at 0x40102500 (PHYSoftRst=1) and the xHCI
 * port registers never report a connect.
 */
&dwc3 {
	/delete-property/ snps,dis_u2_susphy_quirk;
};
```

`snps,dis_u2_susphy_quirk` の削除は未検証。実機の DTB を差し替えて再起動し、`GUSB2PHYCFG0` の PHYSoftRst が 0 のまま維持されるか、xhci の割り込みが増えるか、`lsusb` にデバイスが現れるかを確認する必要がある。

なお、手動で PHYSoftRst をクリアしても改善しなかったので、このクワークが単独の原因である可能性は低い。

---

## 6. 次に疑うべき点（調査の優先順）

### 6.1 vendor カーネル（Android 4.9）の DTS との差分

Android の DT には次のノードがあった（UART ログの aliases から確認）。

```
dwc2_a@ff400000
dwc3@ff500000
usb2phy@ffe09000
usb3phy@ffe09080
```

mainline の DTS には `usb2phy@ffe09000` / `usb3phy@ffe09080` に相当する独立した PHY ノードが無い。mainline は `phy@36000`（usb2_phy0）、`phy@3a000`（usb2_phy1）、`phy@46000`（usb3_pcie_phy）を使う。vendor は GLUE 領域の PHY レジスタを直接叩いている。

調べること: vendor の DTS で `usb2phy@ffe09000` がどう定義され、ドライバが何をしているか。mainline の `dwc3-meson-g12a.c` の `dwc3_meson_g12a_usb2_init()` と処理が一致しているか。

### 6.2 U-Boot が残すハードウェア状態

U-Boot は起動時に `usb start` 相当の初期化を行う（`recovery_from_udisk` などで `usb start 0` を実行する bootcmd がある）。U-Boot が dwc3 をデバイスモードで初期化したまま Linux に渡している可能性がある。

調べること: U-Boot のプロンプトで `usb start` を実行した後に Linux を起動し、挙動が変わるか。あるいは U-Boot の環境変数 `usb_burning` などが影響していないか。

### 6.3 `dr_mode = "otg"` とロール切り替え

親ノードが `otg` のため、dwc3-meson-g12a は `otg_phy_mode` をロール切り替えで決める。`usb_role` の切り替えで `switching to Host Mode` は出るが、xhci の再初期化が走っていない可能性がある。

調べること: `dwc3_meson_g12a_usb_otg_apply_mode()` がホストモードで何をするか。`xhci` の再登録が必要なのに、dwc3 のロール切り替えが xhci を再初期化していないのではないか。

### 6.4 電源（VBUS）供給

PORTSC3 は PP=1 だが CCS=0。VBUS が実際に 5V 出ているかは未測定。テスターで USB ポートの VBUS-GND を測れば確定する。

調べること: 実測。`VCC_5V` レギュレータは 5000mV で有効だが、ポートまでの経路にスイッチ（GPIO 制御）がある可能性。

### 6.5 mainline の既知の問題

G12A の USB ホストは mainline で広く使われている（Odroid N2、VIM3 など）。同じ DTS 構造で動いている実績がある。X96Max 固有の何か（GLUE の初期値、基板の配線）が原因の可能性。

調べること: 同じ G12A で動いている他ボード（`meson-g12a-u200.dts`、`meson-g12b-odroid-n2.dts`）との DTS 差分。特に `u200` は内蔵 PHY を使っており参考になる。

---

## 7. 参考: 既に動作している部分

USB 以外は動作している。目標「TS → H.264/AAC → MP4」のうち、USB 以外の部分は完成済み。

| 機能 | 状態 | 実測値 |
| --- | --- | --- |
| MPEG-2 HW デコード（VDEC） | 動作 | PSNR 66.8 dB（SW デコード比較） |
| H.264 HW エンコード（HCodec） | 動作 | PSNR 43.1 dB、6.6 Mbps、1080p29.97 |
| 1080i → 1080p デインターレース | 動作 | 実時間の 1.04 倍（bwdif） |
| A/V MP4 出力 | 動作 | H.264 110 フレーム + AAC 198 フレーム |
| NAS への HTTP pull 配信 | 動作 | 43 MB 受信成功（stb_streamer） |
| `siano-ts`（PX-S1UD 制御） | ビルド・配備済み | aarch64、`/usr/bin/siano-ts` |
| ファームウェア `isdbt_rio.inp` | 配備済み | SHA-256 `05452064…`（参考リポジトリの期待値と一致） |

USB が解決すれば、`siano-ts` で TS を取得し、既存のパイプラインへ流すだけで目標に到達する見込み。

---

## 8. 参照すべきファイル

| ファイル | 内容 |
| --- | --- |
| `board/x96max/dts/amlogic/meson-g12a-x96-max-p.dts` | この個体向け DTS |
| `board/x96max/linux.fragment` | カーネル設定のフラグメント |
| `board/x96max/patches/linux/0001-vdec-meson-sm1-codecs.patch` | VDEC ドライバの修正 |
| `work/sources/siano-userland/` | PX-S1UD の参考実装 |
| `work/serial-20261110/` | UART 操作スクリプト（`uart.py`、`boot-now.py`） |
| `docs/handoff-20261110.md` | 全体的な引継ぎ |

---

## 9. 調査依頼時に伝えるとよいこと

- Android では動いていたので、ハードウェア故障ではない可能性が高い
- レジスタのオフセットを間違えると誤診する（上記 3 章の表を参照）
- 「xhci は正常」と早合点しない。割り込みが増えないことが動作していない証拠
- dwc3 のレジスタは書けるが xhci のポートレジスタは書けない、という非対称性が鍵
- vendor カーネル（4.9）の DTS と mainline の DTS の差分が最も有望な調査対象
