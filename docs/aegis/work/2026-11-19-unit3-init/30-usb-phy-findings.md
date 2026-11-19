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
