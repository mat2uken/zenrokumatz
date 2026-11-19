# 3代目 X96Max_P の USB 構成（ベンダー DT との照合で確定）

作成日: 2026-11-19

## 結論

この基板の USB レジスタマップは **G12A mainline の想定と `0xffe09000` の解釈だけが違う**。mainline はこの領域を「USB glue」として扱うが、ベンダー DT では **USB2 PHY の制御レジスタブロック（0x80 バイト）** である。

### ベンダー DT（Android、実機から取得）

`/sys/firmware/devicetree/base` の実データ。

```
usb2phy@ffe09000   compatible = amlogic,amlogic-new-usb2-v2
  reg = 0xffe09000 (0x80), 0xffd01008 (0x100), 0xff636000 (0x2000), 0xff63a000 (0x2000)

usb3phy@ffe09080   compatible = amlogic,amlogic-new-usb3-v2
  reg = 0xffe09080 (0x20), 0xffd01008 (0x100)
  clocks = <pcie_refpll>
```

`/proc/iomem`（Android、vendor kernel 4.9）でも一致：

```
ff500000-ff507fff : /dwc3@ff500000
ff50c100-ff5fffff : /dwc3@ff500000
ffe09000-ffe0907f : /usb2phy@ffe09000
ffe09080-ffe0909f : /usb3phy@ffe09080
```

**`usb@ffe09000`（glue）というノードは存在しない。** `dwc3@ff500000` は単独で存在する。

### mainline G12A DTS との差分

| アドレス | mainline（`meson-g12-common.dtsi`） | 実機（ベンダー DT） |
|---|---|---|
| `0xffe09000` | `usb@ffe09000` = **glue**（サイズ 0xa0） | **usb2phy 制御レジスタ**（0x80） |
| `0xffe09080` | glue 内の `USB_R1`（`G12A_GLUE_OFFSET=0x80`） | **usb3phy 制御レジスタ**（0x20） |
| `0xff636000` | `usb2_phy0`（一致） | usb2phy 本体（一致） |
| `0xff63a000` | `usb2_phy1`（一致） | usb2phy 本体（一致） |
| `0xff500000` | `dwc3` 子ノード | `dwc3@ff500000` が単独で存在 |

## 例外の機構

1. `dwc3_meson_g12a_probe` が `usb@ffe09000` を glue として ioremap する
2. `clk_bulk_prepare_enable` → `reset_control_reset` → `usb_init`
3. `usb_init` → `dwc3_meson_g12a_usb2_init` → `usb2_init_phy(priv, 0)`
4. `u2p_regmap[0]` = `base + 0*U2P_REG_SIZE(0x20)` = **`0xffe09000`**
5. `regmap_update_bits(u2p_regmap[0], U2P_R0, ...)` → `regmap_mmio_read32le`
6. **実機の `0xffe09000` は PHY 制御レジスタであり、glue と同じ扱いで read-modify-write すると例外**

つまりアドレス計算は論理的に正しいが、**そのアドレスが指すハードウェアの意味が違う**。

## 誤診の記録（同じ轍を踏まないため）

| 誤った結論 | 実際 |
|---|---|
| `0xff500000`/`0xffe09000` の Bus error = 基板に USB が無い | dwc3 を無効化したことで `clk: Disabling unused clocks` が `g12a_usb_general` を止めた副作用（enable_count=0）。クロックが止まればレジスタは応答しない |
| `0xffe09000` を読んで無応答 = 領域が未実装 | クロック停止状態で測っていた |

**教訓：`devmem` で周辺レジスタを測る前に、そのノードが有効でクロックが供給されていることを確かめる。**

## 必要な対策（未実施）

mainline の `dwc3-meson-g12a` は G12A の glue 配置を前提としているため、この基板では次のいずれかが要る。

1. **glue レジスタを書かない経路で動かす**：`usb2_init_phy` が触る u2p レジスタをこの基板の配置に合わせる。
2. **ベンダーの `amlogic-new-usb2-v2` 相当の初期化を移植する**。
3. **U-Boot が初期化した USB をそのまま使う**（Linux 側でリセットしない）。U-Boot では DT295 が認識されていた実績がある。

現実的には 3 が最も早い可能性があるが、mainline の probe が reset をかけるため DTS だけでは制御できない。カーネルパッチが必要。

## 参考：eMMC は Linux で読めない

```
mmc1: error -84 whilst initialising MMC card
mmc1: Failed to initialize a non-removable card
```

U-Boot では 58.2GiB の `SDIO Port C`、Android も eMMC から起動。Linux 6.18 では `-EILSEQ`。RAM 起動のため運用には影響しないが、同じ基板差の現れとみられる。

## 追記（round 4）: ベンダー専用ドライバの存在確認

Android（vendor kernel 4.9）の `/sys/bus/platform/drivers/` に次がある。

```
amlogic-new-usb2  amlogic-new-usb2-v2
amlogic-new-usb3  amlogic-new-usb3-v2
amlogic-usb2      amlogic-usb3
```

vendor DT の compatible `amlogic,amlogic-new-usb2-v2` / `amlogic-new-usb3-v2` はこれらに対応する。**mainline の `dwc3-meson-g12a` とは別系統。**

## mainline の PHY ドライバが触る範囲

`drivers/phy/amlogic/phy-meson-g12a-usb2.c` は `PHY_CTRL_R0`(0x0)〜 の連続ブロックだけを regmap 経由で操作する（`phy_meson_g12a_usb2_init`）。対応 compatible は `amlogic,g12a-usb2-phy` と `amlogic,a1-usb2-phy` のみ。

したがって DTS の `usb2_phy0`（`phy@36000` = `0xff636000`）は mainline ドライバの期待と一致する。**`0xffe09000` には触れない。**

**結論：mainline の PHY ドライバはそのまま使える。問題は glue が `0xffe09000` を glue として書き込むことだけ。**

## 回避策の評価

| 案 | 内容 | 見通し |
|---|---|---|
| A（採用・検証待ち） | glue ノードを削除し `snps,dwc3` を直接記述。PHY は既存の `usb2_phy0/1` | glue を完全に外せるので最も単純。カーネルパッチ不要 |
| B | glue の u2p 書き込みだけを無効化するカーネルパッチ | 確実だがパッチ保守が必要 |
| C | ベンダーの `amlogic-new-usb2-v2` を移植 | 大掛かり。既存2台の構成から外れる |

A が通らなければ B に進む。C は最終手段。

## 追記（round 6）: vendor DT の USB 構成を完全取得

Android の `/sys/firmware/devicetree/base` を走査して確定した。

### ノード構成（トップレベル）

```
dwc3@ff500000     compatible = "snps,dwc3"
                  reg = 0xff500000(0x100000)
                  status = "okay"
                  usb-phy = <0x12 0x13>          ← mainline に無い独自プロパティ
usb2phy@ffe09000  phy-reg / phy-reg-size          ← mainline に無い独自プロパティ
usb3phy@ffe09080  phy-reg / phy-reg-size
                  usb2-phy-reg / usb2-phy-reg-size
dwc2_a@ff400000   phy-interface / phy-reg / phy-reg-size / usb-fifo
```

`__symbols__` には `dwc3` / `usb2_phy_v2` / `usb3_phy_v2` が存在する。

### これが意味すること

**vendor の USB スタックは mainline と同じ `snps,dwc3` という compatible を使いながら、`usb-phy` という独自プロパティで PHY を結び、PHY 側も `phy-reg` で自前管理している。**

mainline の `snps,dwc3` ドライバ（`drivers/usb/dwc3/core.c`）は `phys` / `phy-names` しか見ないため、vendor の `usb-phy` は無視される。すると PHY が `phy_init` されず、コントローラは動かない。

つまり **mainline でこの基板を動かすには、vendor の `usb-phy` 流儀を mainline の `phys`/`phy-names` に読み替えた DTS を書く**必要がある。ノード構成自体は流用できる（dwc3 が独立し、PHY が別ノード）。

### 候補 DTB（`-p-usb`）との関係

`meson-g12a-x96-max-p-usb.dts` は、まさにこの読み替えを行ったもの:

- glue を削除 → vendor と同じく `dwc3` を独立させる
- `snps,dwc3` に `phys = <&usb2_phy0>, <&usb2_phy1>` と `phy-names` を付ける → mainline 流
- PHY は mainline の `amlogic,g12a-usb2-phy` ドライバが `phy@36000`/`phy@3a000`（= `0xff636000`/`0xff63a000`）を扱う

**リスク**: vendor の `usb2phy@ffe09000` は `phy-reg` で `0xffe09000` も PHY の一部として持つ。mainline ドライバは `0xff636000` だけを見るので、`0xffe09000`（USB2 PHY 制御）の初期化が抜ける可能性がある。実機で `lsusb` に DT295 が出るかで判定する。

## 追記（round 6 後半）: phandle の解決

```
__symbols__
  dwc3         : /dwc3@ff500000
  usb2_phy_v2  : /usb2phy@ffe09000
  usb3_phy_v2  : /usb3phy@ffe09080

dwc3@ff500000: usb-phy = <0x12 0x13>   # → usb2phy@ffe09000, usb3phy@ffe09080
```

vendor の dwc3 は **USB2 PHY と USB3 PHY の両方**に `usb-phy` で結ばれている。

### 候補 DTB の不足

`meson-g12a-x96-max-p-usb.dts` は `usb2_phy0` / `usb2_phy1` だけを参照しており、**USB3 PHY が抜けている**。DT295 は USB2（high-speed）デバイスなので最初の動作確認には支障しない見込みだが、SuperSpeed ポートを使う場合は追加が要る。実機で `lsusb` に DT295 が出るかを先に確かめる。

### mainline で使える PHY ドライバ

`amlogic,g12a-usb2-phy` は `phy@36000`（= `0xff636000`）と `phy@3a000`（= `0xff63a000`）を扱う。vendor DT の `usb2phy@ffe09000` も同じ 2 つの領域を `reg` に含むので、**物理的には mainline ドライバで駆動できる見込み**。ただし vendor は `0xffe09000` も PHY の一部として初期化するため、そこを省くことの影響は実機で確認する。

## 追記（round 9）: 稼働中の vendor kernel から観察した構造

Android（vendor kernel 4.9.113、稼働中）の sysfs。

```
/sys/bus/usb/devices/
  1-0:1.0 -> .../platform/ff500000.dwc3/xhci-hcd.0.auto/usb1/1-0:1.0
  1-2     -> .../platform/ff500000.dwc3/xhci-hcd.0.auto/usb1/1-2
  usb1, usb2 -> .../platform/ff500000.dwc3/xhci-hcd.0.auto/...

1-2/idVendor  = 06b8
1-2/idProduct = 103b        ← PIX-DT295 が接続認識されている
```

```
/sys/bus/platform/devices/
  ff400000.dwc2_a      (USB2.0、デバイス側)
  ff500000.dwc3        (USB3.0 ホスト、xhci-hcd.0.auto を内包)
  ffe09000.usb2phy     (USB2 PHY)
  ffe09080.usb3phy     (USB3 PHY)
```

### これが決定的である理由

**すべてがトップレベルの platform デバイスであり、glue の親子関係が存在しない。** mainline の G12A DTS は `usb@ffe09000` を親、`dwc3@ff500000`/`dwc2@ff400000` を子とする階層を作るが、実機の vendor 構成は完全にフラット。

**この機体で USB（DT295）が実際に動いていることが `1-2` の `06b8:103b` で実証された。** つまりハードウェアは正常で、必要なのは mainline に合った DTS だけ。

候補 `meson-g12a-x96-max-p-usb.dts` は glue を削除して `dwc3@ff500000` を独立させており、この vendor 構造と一致する。方針は妥当。

### 残る不確実性

vendor は `ff500000.dwc3` を **platform デバイスとして直接プローブ**している。mainline の `snps,dwc3` は通常 `amlogic,meson-g12a-usb-ctrl` の子として probe される前提があるが、トップレベルでも `snps,dwc3` ドライバは動作する（`dwc3-generic-plat` 等で実績）。候補 DTB はこれを狙っている。

PHY 側は mainline の `amlogic,g12a-usb2-phy` が `phy@36000`/`phy@3a000` を扱う。vendor の `usb2phy@ffe09000` が `0xffe09000` も初期化している点だけ差が残る。

## 追記（round 9 後半）: vendor PHY は独自の調整値を持つ（方針再検討の根拠）

`usb2phy@ffe09000` の全プロパティ（実機から取得）。

```
reg                  = 0xffe09000(0x80), 0xffd01008(0x100),
                       0xff636000(0x2000), 0xff63a000(0x2000)
version              = 2
portnum              = 2
pwr-ctl              = 1
u2-ctrl-iso-shift    = 0x11
u2-ctrl-sleep-shift  = 0x11
u2-hhi-mem-pd-mask   = 0x03
u2-hhi-mem-pd-shift  = 0x1e
pll-setting-1 = 0x09400414   pll-setting-2 = 0x927e0000
pll-setting-3 = 0xac5f69e5   pll-setting-4 = 0x00fef818
pll-setting-5 = 0x08000fff   pll-setting-6 = 0x00078000
pll-setting-7 = 0x000e0004   pll-setting-8 = 0x000e000c
```

### 意味

**mainline の `amlogic,g12a-usb2-phy` はこれらの値を解釈せず、ドライバ内の固定値で初期化する。** PLL の設定値がボード固有である以上、そのままでは USB が正しくリンクしない可能性が高い。

さらに vendor は `0xffe09000` も PHY の一部として初期化するが、mainline は触らない。

### 方針の再評価

| 案 | 見通し（改訂） |
|---|---|
| A: glue 削除＋mainline PHY ドライバ | **低**。PLL 設定と `0xffe09000` 初期化が抜ける |
| B: glue の u2p 書き込みだけ回避するパッチ | **中**。mainline の PHY 初期化自体は動くが、PLL 値の差が残る |
| C: ベンダーの `amlogic-new-usb2-v2` を移植 | **高**。ただし大掛かり |
| D: **U-Boot が初期化した USB を Linux で触らない** | **中〜高**。下記 |

### 案 D の可能性

U-Boot では `usb start` で DT295 が認識されていた（実績あり）。U-Boot が USB を初期化済みなら、Linux 側でリセット・再初期化しなければそのまま使えるかもしれない。

- `usb@ffe09000` も `dwc3` も probe させない（DTS で無効）
- しかし `/dev/bus/usb` から DT295 にアクセスしたい → `usbfs` は xhci ドライバが必要 → **不可**

つまり D は成立しない。USB を使うには Linux 側で xhci/dwc3 を動かす必要がある。

### 結論

A を試す価値は残っている（PLL が一致する可能性もゼロではない）。A が失敗したら、**B（パッチで glue を回避し、かつ mainline PHY の PLL 値をベンダー値に合わせる）** が現実的な本命になる。その場合は `phy-meson-g12a-usb2.c` にベンダーの PLL 設定を取り込むパッチを書く。

## 追記（round 13）: 「mainline は vendor の PLL 値を無視する」は誤りだった

`pll-setting-1 = 0x09400414` を mainline の `PHY_CTRL_R16` フィールド定義で解読した結果。

| フィールド | vendor の値 | mainline が書く値 | 一致 |
|---|---|---|---|
| MPLL_M | 20 | 20 | ○ |
| MPLL_N | 1 | 1 | ○ |
| MPLL_LOAD | 1 | 1 | ○ |
| MPLL_LOCK_LONG | 1 | 1 | ○ |
| MPLL_FAST_LOCK | 1 | 1 | ○ |
| MPLL_EN | 0 | 1 | × |
| MPLL_RESET | 0 | 1 | × |

**差は EN と RESET のみ。** これは vendor が「リセット段階」と「有効化段階」を別々の `pll-setting-*` に分けて持っているためで、周波数（24MHz × 20 / 1 = 480MHz）は**完全に一致**する。

### 結論の訂正

以前「vendor は独自 PLL 値を使うので mainline では動かない可能性が高い」と書いたが、**これは誤り**。mainline の PHY ドライバは vendor と同じ PLL 周波数を設定する。

残る差は以下だけ:

1. `0xffe09000`（USB2 PHY 制御ブロック）を mainline が初期化しない
2. glue ノードの扱い（これは候補 DTB で glue を削除して回避）

**したがって候補 `-p-usb` DTB の見通しは当初の想定より高い。** 実機検証を最優先すべき。

## 追記（round 13 後半）: pll-setting-2..8 の解読限界

```
pll-setting-2 = 0x927e0000
pll-setting-3 = 0xac5f69e5
pll-setting-4 = 0x00fef818
pll-setting-5 = 0x08000fff
pll-setting-6 = 0x00078000
pll-setting-7 = 0x000e0004
pll-setting-8 = 0x000e000c
```

- `pll-setting-1` は R16（M/N/LOAD/FAST_LOCK 等）に対応し、mainline と一致することを確認済み。
- `pll-setting-2..8` は R17〜R19 などのフィルタ・ループ係数と推定されるが、**vendor ドライバのソースが無いため、どのレジスタにどの値が入るかを確定できない。** 推測で当てにいくのは危険なので、ここで止める。

**判断**: mainline の R17/R18 設定（`MPLL_LAMBDA1/LAMBDA0/FILTER_PVT*`、`LKW_SEL/LK_W/LK_S/...`）は G12A の標準値であり、同じ SoC 系なら妥当と考える。pll-setting-2..8 の差異が実際に問題になるかは**実機で `lsusb` に DT295 が出るかで判定するしかない**。

つまり、これ以上ソフトウェア側で詰められる材料は尽きた。実機検証が唯一の次の手。

## 追記（round 27）: glue 削除は成功、次段階の dwc3 コアで停止

YMODEM で候補 DTB `meson-g12a-x96-max-p-usb.dtb` を実機へ搬入し（CRC32 一致）、
SD から Image と initramfs を読んで起動した。

### 例外の位置が変わった（前進の証拠）

| DTB | 例外の発生箇所 |
|---|---|
| `-p-ephy-4g`（glue あり） | `dwc3_meson_g12a_usb2_init_phy`（glue の PHY 初期化） |
| **`-p-usb`（glue 削除）** | **`dwc3_core_probe+0x96c/0x1600`** |

**glue ドライバの関与は解消した。** 新しい例外は dwc3 コア自体のプローブ時。

```
Internal error: synchronous external abort: 0000000096000010 [#1] SMP
pc : dwc3_core_probe+0x96c/0x1600
Workqueue: events_unbound deferred_probe_work_func
```

`synchronous external abort` は、存在しない、または応答しないレジスタへのアクセスで
起きる。`0xff500000` の dwc3 レジスタに触れた時点で発生していると考えられる。

### 次の仮説

glue を外したことで、`0xff500000` の dwc3 コントローラを**有効化する処理が誰も
行わなくなった**可能性がある。vendor では `ffe09000.usb2phy` が PHY と一緒に
dwc3 側の前提（クロック・リセット）を整えていた。

検証には、Android で動いている vendor kernel の初期化順を真似る必要がある。

### 選択肢

1. `dwc3_core_probe` が触るアドレスを特定する（ソース上の +0x96c を逆アセンブル、
   またはカーネルに printk を入れる）。
2. vendor の初期化（`amlogic-new-usb2-v2`）を詳しく調べ、DTS で再現する。
3. glue を残しつつ、u2p 書き込みだけを避けるパッチ（案 B）に戻る。

現実的には 1 で原因を特定してから 2 か 3 を選ぶ。

## 追記（round 26）: クロック・リセットを移しても dwc3_core_probe で同じ例外

候補 DTB に、削除した glue ノードが持っていた `clocks = <&clkc CLKID_USB>` と
`resets = <&reset RESET_USB>` を dwc3 ノードへ移して試した（`f508ecc8…`）。
YMODEM で転送し（FDT ヘッダの totalsize/off_dt_struct/off_dt_strings/
size_dt_struct がホスト側と完全一致することを確認）、起動した結果。

```
Internal error: synchronous external abort: 0000000096000010 [#1] SMP
pc : dwc3_core_probe+0x96c/0x1600
```

**クロック・リセットの有無で例外の位置は変わらなかった。** したがって原因は
そこではない。

### 転送検証の注意

`crc32 0x08000000 50569` の結果がホスト側の `zlib.crc32` と一致しなかったが、
これは U-Boot のコマンド引数の解釈（終了アドレス）によるもので、データは正しかった。
実際、`fdt header` の各フィールドは完全一致する。**YMODEM 転送の検証には
crc32 ではなく `fdt header` か特定アドレスの `md` 比較を使う。**

### 次に調べること

`dwc3_core_probe+0x96c` が触るアドレスを特定する。カーネルを `vmlinux` から
逆アセンブルすれば、どのレジスタ読み出しで落ちるか分かる。ローカルの
`output/ramboot-unit3/` には Image しかないため、ビルドツリーの vmlinux を使う。

そのアドレスが `0xff500000` 帯なら、dwc3 コントローラ自体が応答していない
（vendor の PHY ドライバが別途有効化している）ことになる。案 B（glue のままで
u2p 書き込みだけ避けるパッチ）へ戻る判断材料になる。

## 追記（round 27）: 落ちる命令を vmlinux の逆アセンブルで特定した

`dwc3_core_probe` は `0xffff800080d74fc0`、`+0x96c` は `0xffff800080d7592c`。

```
ffff800080d75924:  f9421660  ldr  x0, [x19, #1064]   ; x19 = dwc3, +0x428 = dwc3->regs
ffff800080d75928:  91008000  add  x0, x0, #0x20
ffff800080d7592c:  b9400000  ldr  w0, [x0]           ; ← ここで synchronous external abort
```

**読みに行っているのは `dwc3->regs + 0x20`、すなわち `0xff500000 + 0x20` = `0xff500020`。**
dwc3 コアの `GSNPSID` 系のレジスタで、ドライバが最初に触るアドレス。

以前 `devmem` で `0xff500020` が Bus error になったのと同じアドレスであり、
**dwc3 コントローラのレジスタウィンドウが応答していない**ことを、カーネルの
実命令レベルで裏付けた。

### これで消えた候補

- 「glue ドライバが悪い」→ glue を外しても同じアドレスで落ちる。部分的に正しかったが不十分
- 「クロック・リセットが不足」→ `CLKID_USB`/`RESET_USB` を移しても同じ。原因ではない

### 残る仮説

1. **dwc3 コントローラ自体が有効化されていない**（vendor の PHY ドライバが、PHY と
   一緒に dwc3 のクロックゲートやリセットを操作している）。`clk_enable` は成功して
   いるのに応答しないので、SoC 側の別のゲート（HHI の USB メモリパワーダウン等）が
   閉じている可能性。
2. `0xffe09000` の USB2 PHY 制御ブロックに対する初期化が必要で、それが dwc3 の
   レジスタ窓を開ける前提になっている。

### 次にやること

`g12a_usb_general` の enable_count と HHI の USB メモリPD レジスタ
(`u2-hhi-mem-pd-mask = 0x03`, `u2-hhi-mem-pd-shift = 0x1e`、vendor DT の値) を、
Linux 起動時に読む・書く手段を作る。具体的には、dwc3 を無効にした DTB で Linux を
起動し、`/dev/mem` 相当（`busybox devmem`）で `0xff63c000` 帯の HHI を読み、
vendor の初期化が何をしているかを推定する。

## 追記（round 27 後半）: 連鎖の全体像が判明

`dwc3_meson_g12a_usb_init_glue` の処理順:

```
1. dwc3_meson_g12a_usb2_init()        ← u2p_regmap を write → ここで例外（現行）
2. regmap_update_bits(glue, USB_R1, FLADJ_30MHZ, 0x20)
3. regmap_update_bits(glue, USB_R5, ID_DIG_EN_0/1/TH)
4. usb3_init()  (usb3_ports があるとき)
5. usb_otg_apply_mode()  ← ここで USB_R4 の MEM_PD を触る可能性
```

**glue ドライバは 1 番目で即死するため、2 以降に到達しない。** つまり:

- `USB_R1`（`0xffe09084`）のフレーム長調整も設定されない
- `USB_R5` の ID 検出も設定されない
- `USB_R4`（`0xffe09090`, `MEM_PD_MASK`）の解除もされない

**dwc3 のレジスタ窓が応答しないのは、この手順が最後まで走らないから**という連鎖が
見えてきた。候補 DTB（glue 削除）は 1 の例外を避けたが、2〜5 の初期化も
一緒に失ったため、dwc3 が応答しないまま `dwc3_core_probe` で落ちる。

### 取るべき方針（案 B の具体化）

glue ノードは残し、**`dwc3_meson_g12a_usb2_init` の u2p 書き込みだけを回避する**
カーネルパッチを入れる。そうすれば 2〜5 の初期化が走り、dwc3 のレジスタ窓が
開いてから子の dwc3 が probe される。

パッチの形:

```c
static int dwc3_meson_g12a_usb2_init(struct dwc3_meson_g12a *priv,
                                     enum phy_mode mode)
{
    /* This board's 0xffe09000 is the USB2 PHY control block, not the G12A
       glue u2p registers; writing it faults. Skip the u2p setup and let the
       PHY driver at phy@36000/phy@3a000 do the USB2 work. */
    return 0;
}
```

DTS は glue を残したまま（`-ephy-4g`）にする。つまり `meson-g12a-x96-max-p-usb.dts`
ではなく、通常の DTB + このパッチの組み合わせになる。

## 追記（round 28）: DTS だけで u2p 書き込みを避ける案は成立しない

`dwc3_meson_g12a_usb2_init` には `if (!priv->phys[i]) continue;` があり、PHY が
解決できないインデックスはスキップされる。一見、`phy-names` から usb2 を外せば
u2p 書き込みを避けられそうに見えた。

しかし同じドライバの後段に:

```c
#define PHY_COUNT 3
/* Init PHYs */
for (i = 0 ; i < PHY_COUNT ; ++i) {
    ret = phy_init(priv->phys[i]);       // NULL チェックが無い
    ...
}
```

があり、`PHY_COUNT` は固定 3。`phy-names` を減らすと `priv->phys[i]` が NULL に
なり、`phy_init(NULL)` で別のクラッシュを起こす。**DTS だけで u2p を回避する
案は成立しない。**

さらに dwc3 の子ノードは `phys` を持たず、PHY の取得と `phy_init` は glu​e 親が
行う。したがって PHY を親のリストから外すと、USB2 PHY の初期化自体も失われる。

### したがってカーネルパッチ（案 B）が必須

`dwc3_meson_g12a_usb2_init` の u2p ループだけを無効化する。これで:

- 1（u2p 書き込み）を回避
- 2〜5（USB_R1/R5、usb3_init、USB_R4 のメモリPD解除）は実行される
- `phy_init`/`phy_power_on` も従来どおり走る

この形にする:

```c
static int dwc3_meson_g12a_usb2_init(struct dwc3_meson_g12a *priv,
                                     enum phy_mode mode)
{
    /*
     * This board's 0xffe09000 is the USB2 PHY control block, not the G12A
     * glue u2p registers; writing it faults with a synchronous external
     * abort and the rest of the glue sequence (USB_R1/R5, usb3_init,
     * USB_R4 memory power-down release) never runs. Leave the USB2 PHY
     * setup to the phy driver at phy@36000/phy@3a000.
     */
    return 0;
}
```

### ビルドと搬入

`Image` の再ビルドが必要（41MB）。YMODEM では 115200bps で数時間かかるため、
**SD への書き込みが必要**。`scripts/write-sd.py` を使う（sudo）。

## 追記（round 31）: 「USB_R4 のメモリPD解除」という仮説は誤り

`USB_R4_MEM_PD_MASK`（GENMASK(3,2)）は `dwc3-meson-g12a.c` の 87 行目で
**定義されているだけで、使用箇所が無い**。

```
$ grep -n "USB_R4_MEM_PD_MASK" drivers/usb/dwc3/dwc3-meson-g12a.c
87:	#define USB_R4_MEM_PD_MASK				GENMASK(3, 2)
```

`otg_apply_mode` が `USB_R4` で触るのは `USB_R4_P21_SLEEP_M0`（bit 1）だけ。

**したがって「glue がメモリPDを解除するので、それを回避すると dwc3 が目覚める」
という筋書きは成立しない。** 前回の記録は誤りだったので訂正する。

### それでも u2p 回避パッチを試す価値はある

消えるのは「例外を起こす u2p アクセス」だけで、`USB_R1`（フレーム長・
`PORT_POWER_CONTROL_PRESENT`）、`USB_R5`（ID 検出）、`usb3_init`、
`USB_R0/R4` の OTG 設定は実行される。ここまで glue の初期化が一度も完走して
いなかったので、完走させた状態で dwc3 がどう振る舞うかは未観測。

- 通れば: u2p の例外が唯一の障害だったと確定。
- 落ちるなら: 原因は別（dwc3 のレジスタ窓を開けるのは glue でも PHY でもない、
  例えば HHI の USB メモリPD を直接操作する必要がある）。

### 次の切り分けに使える材料

vendor DT の `usb2phy@ffe09000` にあった `u2-hhi-mem-pd-mask = 0x03`、
`u2-hhi-mem-pd-shift = 0x1e` は、**HHI レジスタで USB2 PHY のメモリパワー
ダウンを制御する**ための値。mainline の PHY ドライバにはこの処理が無い。
dwc3 が応答しない原因がここなら、PHY ドライバ側に相当する処理を足す必要がある。

## 追記（round 34）: u2p 回避パッチは効いた。次は USB_R1 で停止

パッチ入りカーネル（`Image` = `804a534f…`）を SD の FAT 区画へ直接コピーして起動した
（sudo 不要。`--update-boot` の原型を作業中に手で実行）。

### 前進した点

```
dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
dwc3-meson-g12a ffe09000.usb: USB3 ports: 1
```

**PHY の取得に成功し、u2p ループを通過した。** パッチの効果は出ている。
従来は `usb2_init_phy` で即死していたが、そこは通った。

### 新しい停止点

```
Internal error: synchronous external abort
pc : regmap_mmio_read32le+0x8/0x20
dwc3_meson_g12a_usb_init_glue.isra.0+0x38/0x190
```

`+0x38`（`0xffff800080d84708`）の直前で `regmap_update_bits_base` を呼んでおり、
引数は `w1 = 0x4`。`USB_R1 = 0x04` なので、**glue base + 0x84 = `0xffe09084`** への
書き込みで落ちている。

つまり `usb_init_glue` の 2 番目の処理（`USB_R1_U3H_FLADJ_30MHZ_REG_MASK`）で
停止した。**u2p を避けても、glue レジスタ自体が応答しない。**

### これで分かったこと

- `0xffe09000` 帯（glue）は、この基板では**書けない**。u2p も USB_R1 も同じ。
- vendor DT の `usb2phy@ffe09000` は `0xffe09000`(0x80) を PHY の制御ブロックとして
  持つが、mainline の glue ドライバが期待する「USB3 PHY 用 glue」とは別物。
- したがって **mainline の `amlogic,meson-g12a-usb-ctrl` はこの基板では使えない。**
  どのレジスタを触っても落ちる。

### 次の方針（候補 DTB に戻る）

glue を完全に外し、`snps,dwc3` を直接記述する `-p-usb` DTB（`f508ecc8…`）で、
**今度はパッチ入りカーネル**を使う。パッチにより `usb2_init` は何もしなくなるので、
glue 経由の u2p アクセスは発生しない。dwc3 子ノードは PHY を自分で取得し、
`dwc3_core_probe` が `0xff500020` を読む。

前回この DTB で `dwc3_core_probe+0x96c` に落ちたが、そのときのカーネルは
パッチ無しで、glue の初期化が一度も完走していなかった。**今回は条件が違う**
（PHY は取得済み、glue は完全に無関係）ので、再試行する価値がある。

## 追記（round 35）: mainline には USB メモリPD 制御が無い

### dwc3 コアはクロックとリセットを自分で扱う

`dwc3_probe`（core.c）の順序:

```
reset_control_deassert(dwc->reset);
dwc3_clk_enable(dwc);
dwc3_core_is_valid(dwc);     ← ここで 0xff500020 を読む（前回落ちた箇所）
```

候補 DTB は `clocks = <&clkc CLKID_USB>`、`resets = <&reset RESET_USB>` を持って
いるので、この 2 つは実行される。それでも `dwc3_core_is_valid` で落ちた。

### CLKID_USB はゲートだけ

`drivers/clk/meson/g12a.c`:

```
static G12A_PCLK(g12a_usb_general, HHI_GCLK_MPEG1, 26, CLK_IGNORE_UNUSED);
[CLKID_USB] = &g12a_usb_general.hw;
```

`HHI_GCLK_MPEG1` の bit 26 を開けるだけの**ゲートクロック**。USB のメモリ
パワーダウンには触れない。

### mainline にメモリPD制御が存在しない

```
$ grep -rn "HHI_GCLK_MPEG1\|HHI_MEM_PD\|mem_pd" drivers/usb/dwc3/ drivers/phy/amlogic/
（該当なし）
```

vendor DT の `usb2phy@ffe09000` にある:

```
u2-hhi-mem-pd-mask  = 0x03
u2-hhi-mem-pd-shift = 0x1e
```

は、**HHI のとあるレジスタの bit 30-31 を操作して USB2 PHY のメモリ
パワーダウンを解除する**もの。mainline の PHY ドライバにも dwc3 にも、この処理が
無い。

HHI のアドレスは `bus@3c000/system-controller@0` = **`0xff63c000`**（サイズ 0x400）。

### 仮説

dwc3 のレジスタ窓（`0xff500000`）が応答しないのは、**この基板の USB が HHI の
メモリPD に入ったままで、mainline はそれを解除しないから**。クロックゲート
（CLKID_USB）を開けても、メモリPD が解除されない限りバスは応答しない。

### 次に確認すること

Android（vendor kernel）が起動している状態で、`0xff63c000` 帯のどのレジスタの
bit 30-31 が立っているかを読む。vendor はこれを解除しているので、Linux で同じ
値を再現すれば窓が開くはず。

具体的には HHI の USB 関連レジスタ（`HHI_MEM_PD_REG0` 等）をスキャンし、
bit30-31 が 0 になっているものを探す。それができれば、DTS に syscon 経由の
レジスタ初期化を入れるか、小さなカーネルパッチで再現する。

## 追記（round 36）: vendor の dwc3 は "dwc_general" というクロックを使う

Android の sysfs から取得。

```
/sys/devices/platform/ff500000.dwc3/driver -> .../drivers/dwc3      ← mainline と同じ snps,dwc3
/sys/devices/platform/ff500000.dwc3/of_node/clock-names = "dwc_general"
/sys/devices/platform/ff500000.dwc3/of_node/clocks      = <0x02 0x41>
```

- **ドライバは mainline と同じ `dwc3`（snps,dwc3）**。独自ドライバではない。
- ただし vendor DT のクロックは `clkc` の ID `0x41` = **65**、名前は `dwc_general`。
- mainline の `g12a-clkc.h` は `CLKID_USB = 47`（`g12a_usb_general`、`HHT_GCLK_MPEG1` bit 26）で、
  **ID 65 は定義されていない**（定義は 0〜64）。

### 意味

mainline の glue ドライバは `CLKID_USB`（47）を有効にするが、vendor の dwc3 は
別のクロック（65, "dwc_general"）を要求する。**この 2 つが別物なら、mainline が
何を開けても dwc3 のレジスタ窓は目覚めない。**

### 次に確認すること（Android で実行可能）

1. `0x41`（65）が vendor のクロックツリーで何を指すか。`/sys/kernel/debug/clk/` の
   `dwc_general` の親・レート・enable_count を見る。
2. mainline の `CLKID_USB`（47）と同一かどうか。vendor の clkc は互換でないので、
   ID の対応は自前で確認する必要がある。

Android には `/sys/kernel/debug/clk/` があるため、`dwc_general` の実体を
その場で読める。

## 追記（round 36 後半）: vendor は cpu-type="gxl"、クロックは mainline と同一

### クロックの謎は解けた

Android のクロックツリー:

```
g12a_usb_general    enable_count 1   ← 有効
（"dwc_general" という名前のクロックは存在しない）
```

vendor DT の `clock-names = "dwc_general"` は名前だけで、実体は
**mainline の `CLKID_USB`（`g12a_usb_general`）と同じ**。クロックの違いは
原因ではなかった。

### ただし vendor は SoC を GXL として扱っている

```
/sys/devices/platform/ff500000.dwc3/of_node/cpu-type = "gxl"    (67 78 6c = "gxl")
/sys/devices/platform/ff500000.dwc3/of_node/clock-src = "usb3.0"
```

**mainline の `dwc3-meson-g12a.c` には `gxl_drvdata` があり、GXL として扱えば
regmap の構成が変わる。**

| | G12A（現行 DTS の compatible） | GXL |
|---|---|---|
| glue regmap | `base + G12A_GLUE_OFFSET(0x80)` | **`base` そのもの** |
| u2p regmap | `base + i * 0x20` を作る | **作らない** |
| usb2_init | u2p に書く（この基板で例外） | `dwc3_meson_gxl_usb2_init_phy`（PHY レジスタ側で処理） |

vendor の配置（`0xffe09000`=usb2phy、`0xffe09080`=usb3phy）は、**glue が `base`
そのものという GXL の解釈**と整合する。mainline の G12A 解釈（glue=base+0x80）は
`0xffe09080` を glue とみなしており、この基板の usb3phy と衝突する。

### 試すべきこと

DTS の glue ノードの compatible を `amlogic,meson-gxl-usb-ctrl` に変える。
`gxl_drvdata` が選ばれ、u2p を作らないので例外が出ない。glue regmap は `base`
（`0xffe09000`）になり、ここへの `USB_R1`(0x04) 書き込みは `0xffe09004`。

ただし `0xffe09000` 帯が書けるかは未確認（前回 `0xffe09084` で落ちた）。
`gxl_usb_init` が何を書くかを確認してから判断する。

## 追記（round 36 結論）: 0xffe09000 は PHY が初期化する領域。glue とは別物

vendor の dmesg:

```
amlogic-new-usb2-v2 ffe09000.usb2phy: USB2 phy probe:phy_mem:0xffe09000, iomap phy_base:0xf0ed8000
dwc_otg: usb0: type: 2 speed: 0, config: 0, dma: 0, id: 0, phy: ffe09000, ctrl: 0
```

**`amlogic-new-usb2-v2` が `0xffe09000` を PHY のレジスタとして ioremap し、初期化している。**
その PHY を vendor の dwc3 が `usb-phy`（phandle 0x12）で参照する。

つまりこの基板では:

| アドレス | vendor | mainline G12A |
|---|---|---|
| `0xffe09000` | **USB2 PHY の制御ブロック**（amlogic-new-usb2-v2 が初期化） | **glue**（dwc3-meson-g12a が初期化） |
| `0xffe09080` | usb3phy | glue 内の `USB_R1`（base+0x80） |
| `0xff636000`/`0xff63a000` | usb2phy の reg の一部 | usb2_phy0/1 |

**PHY の初期化が dwc3 のレジスタ窓を開ける前提**になっており、mainline は PHY を
`phy@36000`/`phy@3a000` で扱うため、`0xffe09000` の初期化が抜ける。しかも glue
ドライバが同じ領域を「glue」として触るため例外になる。**同じアドレスを別の意味で
取り合っているのが本質。**

### まとめ（試す順）

1. **compatible を `amlogic,meson-gxl-usb-ctrl` にする**（GXL 解釈）。glue は `base`
   そのものになり、u2p を作らない。`USB_R1` は `0xffe09004`。`0xffe09000` 帯が
   書ければ通る。**DTS の 1 行で試せる。**
2. 通らなければ、`0xffe09000` を PHY として初期化する処理（vendor の
   amlogic-new-usb2-v2 相当）を mainline 側に足す。カーネルパッチ。

まず 1 を試す。DTB だけなので YMODEM（50KB）で送れる。

## 追記（round 37）: GXL compatible で Linux は起動するが、USB は動かない

`-p-gxl` DTB（`adbc4b92…`、`compatible = "amlogic,meson-gxl-usb-ctrl"`）を
YMODEM で転送し、SD の Image と initramfs で起動した。

### 得られた結果

**カーネル例外は出ず、Linux はユーザー空間まで起動した。**

```
B-CAS: DT295 internal card (external relay not needed)
Starting pix-stream: OK
Starting pix-logo-weekly: OK
X96Max_P3 RAM-root Linux
x96max-p3 login:
```

RAM rootfs、swap なし、`rootfs` を確認。

### ただし glue ドライバは失敗している

```
dwc3-meson-g12a ffe09000.usb: error -ENOENT: Failed to get clk 'usb_ctrl'
dwc3-meson-g12a ffe09000.usb: probe with driver dwc3-meson-g12a failed with error -2
```

GXL 版は `meson_gxl_clocks` として `usb_ctrl` / `usb_bus` / `xtal_usb_ctrl` を要求する。
この基板の DTS にはその名前のクロックが無いため probe に失敗し、dwc3 は登録されない。

その結果:

```
# lsusb
（何も表示されない）
```

**DT295 は USB デバイスとして認識されていない。** 「B-CAS: DT295 internal card」の
表示は、USB 経由ではなく別の判定（設定値の確認など）によるもの。

### まとめ

| DTB | 結果 |
|---|---|
| `-ephy-4g`（G12A glue） | `USB_R1` 書き込みで synchronous external abort |
| `-p-usb`（glue 削除、snps,dwc3 直接） | `dwc3_core_probe` で abort（0xff500020 が応答しない） |
| **`-p-gxl`（GXL glue）** | **例外なしで起動。ただし `usb_ctrl` クロックが無く glue probe 失敗 → USB デバイス無し** |

### 次の一手

GXL 版が必要とするクロック名を DTS に足せば、glue probe が通り、glue regmap は
`base`（`0xffe09000`）そのもの、u2p は作られないので例外にならない見込み。

必要なのは:

```dts
&usb {
    compatible = "amlogic,meson-gxl-usb-ctrl";
    clocks = <&clkc CLKID_USB>, <&clkc CLKID_USB1_DDR_BRIDGE>, <&clkc CLKID_USB_PHY>;
    clock-names = "usb_ctrl", "usb_bus", "xtal_usb_ctrl";
};
```

ただし `meson_gxl_clocks` が実際にどの名前を要求するか、カーネルソースで確認する
必要がある。`CLKID_USB_PHY` が G12A の clkc に存在するかも要確認。

## 追記（round 38）: GXL + usb_ctrl クロックで USB コントローラが起動した

`-p-gxl` DTB に `clocks = <&clkc CLKID_USB>; clock-names = "usb_ctrl";` を足して
YMODEM 転送し、起動した。

### 結果: USB ホストコントローラが動作

```
dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
dwc3-meson-g12a ffe09000.usb: USB3 ports: 0
xhci-hcd xhci-hcd.0.auto: xHCI Host Controller
xhci-hcd xhci-hcd.0.auto: irq 17, io mem 0xff500000
xhci-hcd xhci-hcd.0.auto: new USB bus registered, assigned bus number 1
hub 1-0:1.0: USB hub found
hub 1-0:1.0: 2 ports detected
```

**これまで一度も通らなかった dwc3 の probe が成功し、xhci が `0xff500000` を
マップして USB バスを登録した。** 例外も出ていない。

`lsusb` でも root hub が見える:

```
Bus 001 Device 001: ID 1d6b:0002 ... 2.0 root hub
Bus 002 Device 001: ID 1d6b:0003 ... 3.0 root hub
```

### 残る問題: デバイスが列挙されない

```
usb 1-0:1.0 / usb 2-0:1.0   ← root hub のみ
hub 1-0:1.0: 2 ports detected
（"new high-speed USB device" のログが無い）
```

ポートに DT295 が接続されたことを示すイベントが一度も出ていない。つまり
**物理的に繋がっていないか、VBUS が供給されていない**。

### 観察: 31.7 秒で一度バスが外れる

```
[31.772936] xhci-hcd xhci-hcd.0.auto: remove, state 4
[35.633121] hub 1-0:1.0: USB hub found      ← 復帰
```

31.8 秒に `_regulator_put` の警告も出ており、`pix-usb-audit` による再列挙と
思われる。**再列挙後はバスが復帰している**ので、これ自体は致命的ではない。

### 確認すべきこと

1. **DT295 が 3代目の USB ポートに挿さっているか。** ログに接続イベントが
   一切無いのは物理未接続の可能性が高い。
2. 挿さっているなら VBUS。`VCC_5V` レギュレータは存在するが、`USB_R1` の
   `PORT_POWER_CONTROL_PRESENT` が GXL 経路で立っているか要確認。
3. ポートの状態は `cat /sys/bus/usb/devices/usb1/1-0:1.0/port1/connect` 等で
   読める（今回は未実施）。

## 追記（round 39）: USB バスは健全。DT295 が物理未接続

### 確認したこと

```
$ ssh root@192.168.99.56 'for d in /sys/class/regulator/*/; do ... done'
/sys/class/regulator/regulator.2/ VCC_5V enabled     ← VBUS は供給されている
```

```
$ lsusb
Bus 001 Device 001: ID 1d6b:0002 ... 2.0 root hub
Bus 002 Device 001: ID 1d6b:0003 ... 3.0 root hub     ← root hub のみ
```

```
hub 1-0:1.0: 2 ports detected
hub 2-0:1.0: 1 port detected
（"new high-speed USB device" のイベントが無い）
```

`VCC_5V` は enabled、xhci は稼働、ハブはポートを検出している。**それでもデバイス
接続イベントが一度も出ない。** VBUS とコントローラは正常なので、**DT295 が物理的に
挿さっていない**と判断する。

### 現在の到達点

| 項目 | 状態 |
|---|---|
| Linux RAM 起動（GXL DTB） | **達成**。例外 0、SSH 可、eth0 192.168.99.56 |
| USB ホストコントローラ | **達成**。xhci が 0xff500000 をマップ、ハブ 2 ポート |
| VBUS (VCC_5V) | **enabled** |
| DT295 の認識 | **未達**（物理未接続と判断） |
| 録画・NAS 保存・CM カット | 未達 |

### 起動に使う DTB（確定）

`meson-g12a-x96-max-p-gxl.dtb`（`compatible = "amlogic,meson-gxl-usb-ctrl"`、
`clock-names = "usb_ctrl"`）。

これが3代目の正解。理由:

- G12A 解釈は `0xffe09000` を glue とみなすが、実機では USB2 PHY の制御ブロック。
  書き込みで synchronous external abort になる。
- GXL 解釈は glue regmap を `base` そのものにし、u2p を作らないため衝突しない。
- vendor の dwc3 も `cpu-type = "gxl"` と申告している。
- GXL は `usb_ctrl` という名前のクロックを要求するので、DTS で命名する
  （mainline の num_clks の誤りにより実際に要求されるのは 1 個だけ）。

### 次にやること

**DT295 を 3代目の USB ポートへ挿す。** それだけで以下を確認できる:

```sh
ssh root@192.168.99.56 'lsusb | grep 06b8'          # Pixela PIX-DT295
ssh root@192.168.99.56 'wget -qO- http://127.0.0.1:40774/v1/status'
python3 tools/check-unit3-acceptance.py --host 192.168.99.56 --channel 17
```

### 運用上の注意

- `pix-usb-audit` が起動約 31 秒で USB を再列挙する（一度バスが外れて復帰する）。
  例外は出ておらず、バスも戻るので実害は無いとみられるが、DT295 接続後に
  録画が途切れないかは確認する。
- SD の FAT 区画を書き換えれば、sudo 無しで Image/DTB を差し替えられる
  （`write-sd.py --update-boot`、実機で成功済み）。
- YMODEM（`tools/loady_send.py`）で DTB を単体で送れる。115200bps 固定、
  ボーレートを上げると U-Boot が切り替え待ちに入るので不可。

## 追記（round 43）: xHCI の PORTSC が 0 = ポート電源が入っていない

DT295 を接続した状態で実機を測定した（Linux は GXL DTB で稼働中）。

### 測定結果

```
# xHCI PORTSC（0xff500400, 0xff500410）
port1 PORTSC = 0x00000000
port2 PORTSC = 0x00000000
```

**両ポートとも `0x0`。** PORTSC のビット:

- bit 0 CCS（Current Connect Status）= 0 → デバイス未検出
- **bit 9 PP（Port Power）= 0 → ポートに電源が供給されていない**

USB デバイスを挿しても、ポート給電（PP）が 0 なら列挙されない。DT295 が
認識されないのはこれが原因。

### 参考: 他のレジスタ

```
0xffe09080 = 0x0000006A   （usb3phy 制御）
0xffe09084 = 0x00000000   （G12A の USB_R1 相当。GXL では glue=base なので別）
0xff636000 = 0x40057000   （usb2_phy0 本体。PHY は生きている）
0xffe09000/04/08/0c       mmap 拒否（glue ドライバが予約）
```

`VCC_5V` レギュレータは `enabled`、`num_users=4`。

### 原因の推定

G12A 版の `usb3_init` が `USB_R1_U3H_HOST_PORT_POWER_CONTROL_PRESENT`（bit 17）を
立てるが、GXL 版は `USB3 ports: 0` のため `usb3_init` を呼ばない。**ポート電源を
「制御可能」と宣言するビットが立っていない**ため、xhci が PP を立てないと考えられる。

### 対策の候補

1. GXL でも `usb3_init` 相当（`PORT_POWER_CONTROL_PRESENT` の設定）を行う
   カーネルパッチ。
2. DTS で `vbus-supply` を確実に効かせ、xhci に PP を立てさせる。既に
   `&usb { vbus-supply = <&vcc_5v> }` があるが、GXL の glue regmap が変わった
   ことで消費側の解決が変わった可能性。
3. `xhci-hcd` の `hcd->power_budget` ではなく、xHCI の PP を直接書く（危険）。

まず 1 を検討する。`USB_R1` は GXL では `base + 0x04` = `0xffe09004`。
glue ドライバが予約しているので、カーネル内から書く必要がある。

## 追記（round 44）: DT295 は接続されている。列挙が Polling で止まっている

### 決定的な測定

xHCI の PORTSC を全数読んだ（DT295 接続状態、Linux は GXL DTB で稼働中）。

```
PORTSC1 (0xff500400) = 0x00000000   PP=0 CCS=0
PORTSC2 (0xff500410) = 0x00000000   PP=0 CCS=0
PORTSC3 (0xff500420) = 0x000002A0   PP=1 CCS=0
PORTSC4 (0xff500430) = 0x000206E1   PP=1 CCS=1
```

`PORTSC4 = 0x206E1` の内訳:

| ビット | 値 | 意味 |
|---|---|---|
| CCS (bit 0) | 1 | **デバイスが接続されている** |
| PED (bit 1) | 0 | ポートが有効化されていない |
| PLS (bit 5-8) | 7 | **Polling（列挙中で停止）** |
| PP (bit 9) | 1 | **ポート電源あり** |
| Speed (10-13) | 1 | Full Speed |
| CSC (bit 17) | 1 | 接続変化イベントが未処理 |

**DT295 は物理的に接続されており、VBUS も供給されている。** しかし xhci が列挙を
完了できず、PLS=Polling で止まっている。

### 原因の候補：ポート数の不一致

```
hub 1-0:1.0: 2 ports detected      ← xhci は 2 ポートとして登録
PORTSC4 にデバイスが接続           ← 実際は 4 ポートある
```

glue は `USB2 ports: 2` と報告しており、vendor DT の `portnum = 2` とも一致する。
しかし実際の PORTSC は 4 つあり、**デバイスは 4 番目に挿さっている**。

xhci は登録済みの 2 ポート（PORTSC1/2）しか監視しないため、PORTSC4 の接続を
処理できない。これが Polling のまま止まる理由と考えられる。

### 対策

`USB2 ports: 2` の報告を実際のポート数に合わせる必要がある。glue の
`usb2_ports` は PHY 数から決まる（`dwc3_meson_g12a_get_phys` が `phy_names` の
うち "usb2" を含むものを数える）。GXL の `meson_a1_phy_names` は
`{"usb2-phy0", "usb2-phy1"}` の 2 個なので 2 になる。

**DT295 を PORTSC1 か PORTSC2 に挿せば認識される可能性が高い。** これが最も
簡単な確認方法。物理ポートの対応を確かめる。

## 追記（round 45）: xhci は 2 ポートと申告。実際は 4 ポートある

### 測定

```
# hcc params = 0x0228fe64
MaxPorts (bits 24-31) = 2
MaxSlots (bits 0-7)   = 100

# /sys/bus/usb/devices/usb1/maxchild = 2
# 監視されているポート: usb1-port1, usb1-port2 のみ

# PORTSC4 = 0x206E1（CCS=1, PP=1, PLS=7 Polling）← ここに DT295
```

**xhci コントローラの `hcc params` が MaxPorts=2 と申告している。** そのため
ドライバは PORTSC1/2 しか監視せず、PORTSC4 の接続（DT295）を処理できない。

### 参考: dwc3 のレジスタ

```
GSNPSID    0xff50c120 = 0x5533320A   （dwc3 v3.30a）
GHWPARAMS2 0xff50c110 = 0x30C01004
GHWPARAMS4 0xff50c140 = 0x20208009
GHWPARAMS5 0xff50c144 = 0x01E0C93B
```

MaxPorts は dwc3 の HWPARAMS に由来する。vendor DT は `portnum = 2` を持って
いたので、ベンダーも 2 ポートとして扱っている可能性がある。

### 未確認事項

**vendor（Android）では xhci が何ポートとして登録されるか。** Android で
`/sys/bus/usb/devices/usb1/maxchild` を読めば分かる。vendor も 2 なら、
「4 番目のポートはハード的に存在するが、SoC の設定上は 2 ポート扱い」という
ことになり、DT295 を PORTSC4 に挿すのは誤り（別のポートに挿すべき）。

vendor が 4 ポートと認識しているなら、mainline 側の設定不足。

### 現実的な次の一手

**DT295 を別の物理ポートに挿す。** PORTSC1 か PORTSC2 に挿せば、xhci が監視して
いるポートなので列挙される可能性が高い。これが最も簡単で確実な確認方法。

物理ポートと PORTSC の対応は自明ではないため、順に試す必要がある。

## 追記（round 47）: xhci の MaxPorts は 3。DT295 は 4 番目に挿さっている

### 決定打の測定

```
# hcs_params1 = 0x03000140（xHCI ケーパビリティ 0xff500004）
MaxSlots (bits 0-7)   = 64
MaxIntrs (bits 8-18)  = 1
MaxPorts (bits 24-31) = 3        ← xhci->max_ports = 3

# PORTSC の実測
PORTSC1 = CCS=0 PP=0
PORTSC2 = CCS=0 PP=0
PORTSC3 = CCS=0 PP=1
PORTSC4 = CCS=1 PP=1             ← DT295 はここ
```

**ドライバは 3 ポートまでしか見ない。DT295 は 4 番目のポートに挿さっているため、
番号として範囲外。** これが認識されない理由。

`max_ports` は `HCS_MAX_PORTS(hcs_params1)` で決まり、これは dwc3 が申告する
ハードウェア値なので DTS では変えられない。

### つまり

物理的に 4 つのポートがあるが、コントローラは 3 つと申告している。
**DT295 をポート 1〜3 のいずれかに挿せば、xhci が監視する範囲に入る。**

これが唯一かつ最も簡単な解決策。ポートの物理的な位置と PORTSC 番号の対応は
自明ではないので、1 つずつ試す必要がある。

### もし 1〜3 のどれでも認識されない場合

- ポート 1〜3 は PP=0 か PP=1 だが CCS=0 のまま → 物理的に配線されていない
  （この基板では 4 番目のポートだけが実際に使える、という可能性）
- その場合は `max_ports` を 4 に上書きするカーネルパッチが必要。ただし
  ハードウェアの申告値を書き換えることになるので、xhci の内部整合
  （`hw_ports` 配列、レジスタの stride）が崩れる危険がある。

### 次の確認

vendor（Android）で `hcs_params1` と `maxchild` を読めば、ベンダーが何ポート
として扱っているか分かる。ベンダーも 3 なら「4 番目は想定外の配線」であり、
ポート 1〜3 を使うべき。ベンダーが 4 なら mainline 側の問題。
