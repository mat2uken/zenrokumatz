
## 追記（round 101 完了）: glue の例外を抜けた。次は dwc2 で止まる

### 進展

`skip-glue-init` を `usb_post_init` にも適用した Image（`6779f086`）で起動した
ところ、**dwc3 の glue 例外は解消した**。新しい例外は別のドライバ:

```
Call trace:
 dwc2_driver_probe+0x218/0x890 (P)
 platform_probe
 really_probe
 ...
 dwc3_meson_g12a_probe+0x370/0x590
```

**`dwc2`（`ff400000.usb`）** が USB2 PHY を触るときに synchronous external abort。

### 対処

この機体に dwc2 は不要（DT295 は dwc3 ホスト側）。DTS で無効化した:

```dts
&dwc2 {
	status = "disabled";
};
```

`meson-g12-common.dtsi` のラベルは `dwc2`（`usb_dwc2` ではない）。DTB に
`status = "disabled"` が入ったことを確認済み（`usb@ff400000` の末尾）。

### 段階の整理（どこまで進んだか）

| 段階 | 状態 |
|---|---|
| glue の u2p 書き込み | 解決（0002） |
| glue の USB_R1 書き込み | 解決（0004 の hunk 1） |
| glue の otg_apply_mode（post_init） | 解決（0004 の hunk 2） |
| **dwc2 の PHY アクセス** | **解決（DTS で dwc2 無効化）— 検証待ち** |
| xhci のポート電源（PPC） | 解決（0003） |
| DT295 の列挙・録画 | 未到達 |

**例外の発生源が dwc3 → dwc2 と移っており、着実に前進している。**

## 追記（round 102）: dwc2 を無効化しても例外が残る（原因は未特定）

### 実施

`&dwc2 { status = "disabled"; }` を入れた DTB（`971e4e7b`）を Android 経由で
SD へ書き込み、起動した。

### 結果

```
[1.318889] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.329999] Internal error: synchronous external abort: 0000000096000010
```

**例外はまだ出る。** ただしログが 2,892 バイトで途中から欠落しており
（シリアル出力の取りこぼし）、コールトレースを特定できなかった。

「Call trace」の行が含まれておらず、スタックの関数名も判別不能。

### 未確定

- `dwc2` の無効化が効いていない（DTS が読まれていない）のか
- 別のドライバが例外を出しているのか
- それとも `dwc3` の別の経路か

**次の起動では、ログを確実に取得してコールトレースを特定する必要がある。**
シリアルが欠落するため、`Console` のポーリング間隔を短くするか、
起動後に `dmesg` から読む（Linux に到達しないと不可なので、シリアル改善が先）。

### シリアル取得の改善案

現在の `tools/uboot_boot_linux.py` の `Console.pump(wait)` は指定時間だけ
読む。起動ログは数百 KB になるため、`booti` 後にもっと長く、かつ頻繁に
読む必要がある。`c.pump(85.0)` の 1 回では取りこぼす。

## 追記（round 105）: `-nousb` DTB で Linux は完全に正常動作（例外なし）

### 結果

`meson-g12a-x96-max-p-nousb.dtb`（`a5690cdf`、全 USB コントローラ無効）で起動:

```
$ ssh root@192.168.99.56 'uname -a'
Linux x96max-p3 6.18.62 #1 SMP PREEMPT aarch64 GNU/Linux

$ ip -4 addr show eth0
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> inet 192.168.99.56/24

$ uptime
12:03:36 up 1 min, load average: 0.00

$ dmesg | grep -ciE "error|abort|fault"
4          ← 通常メッセージのみ。例外なし。
```

**`Internal error` が無く、Linux がユーザー空間まで正常に起動した。**

### pix-stream も動作

```
$ ps | grep -c "[p]ix-simple"
1

$ tail -4 /tmp/pix-simple.log
12:03:03 simple: tuner: still waiting (60s)
```

**round 77 で実装した `wait_for_tuner` が設計どおり動作している。**
USB が無効なのでチューナーが見つからず待機を続けている（停止せず待つ）。

### 問題の範囲が確定

| 範囲 | 状態 |
|---|---|
| USB 以外（CPU、メモリ、eMMC、Ethernet、クロック、映像、サウンド） | **すべて正常** |
| USB（dwc3/dwc2/PHY/glue） | **例外の発生源** |

**例外は USB サブシステムに完全に限定された。** これは大きな前進で、
切り分けの範囲が狭まった。

### 次の一手

USB を段階的に有効化して、どのノードが例外を出すかを特定する。
現在 `-p-gxl`（dwc3 のみ有効、dwc2 無効）で `__bus_probe_device` 経由の
例外が出ている。考えられる候補:

1. `usb2_phy0` / `usb2_phy1`（`0xff636000` / `0xff63a000`）の PHY 初期化
2. `usb3_pcie_phy`（`0xffe09080`）
3. glue の残りの経路（`set_phy_mode`）

**`dmesg` が読めるようになったので、次は USB 有効版で起動して Linux に
到達できれば、シリアルに頼らず `dmesg` から完全なコールトレースを取れる。**
ただし現状は例外で到達前に停止するため、まず「例外を出さずに起動できる
最小の USB 構成」を探す。

## 追記（round 106）: PHY 単体でも例外 → 原因は USB2 PHY の regmap 読み取り

### 発見した経路（Linux から直接 SD を書けるようになった）

`-nousb` DTB で Linux が動作している状態で、**Linux から SD をマウントして
書き込める**ことが分かった:

```
# cat /proc/partitions
179  0  31166976  mmcblk0        ← SD カード
179  1    131072  mmcblk0p1      ← FAT 区画

# mount -t vfat /dev/mmcblk0p1 /mnt/sd
# df -h /mnt/sd
/dev/mmcblk0p1  126.0M  74.5M  51.5M  59%  /mnt/sd
```

`dd` も `mount` も busybox にある。**Android へ戻さずに DTB を差し替えられる。**

### 段階的切り分け: PHY 単体で例外

`meson-g12a-x96-max-p-phyonly.dts` を作成（`-nousb` から PHY だけ有効化）:

```dts
#include "meson-g12a-x96-max-p-nousb.dts"   // usb, dwc3, dwc2, usb2_phy0/1 は無効

&usb2_phy0 { status = "okay"; };
&usb2_phy1 { status = "okay"; };
```

起動結果: **ネットワーク不通、SSH 不通 = 例外**。

```
regmap_mmio_read32le+0x8
bus_probe_device_deferred
deferred_probe_work_func
```

**dwc3 も dwc2 も無効なのに、PHY 単体で落ちる。** つまり例外の発生源は
**`usb2_phy0` / `usb2_phy1` の regmap 読み取り**（`0xff636000` / `0xff63a000`）。

### 問題の階層が明確になった

| 構成 | 結果 |
|---|---|
| 全 USB 無効（`-nousb`） | **正常起動**（例外なし） |
| **PHY のみ有効** | **例外**（`regmap_mmio_read32le`） |
| PHY + glue（`-p-gxl`） | 例外（`dwc2_driver_probe` など） |

**USB2 PHY の regmap がこの基板で読めない。** mainline の
`phy-meson-g12a-usb2.c` は `reg` の最初の領域（`0xff636000`）を regmap として
使い、そこから読む。この読み取りが synchronous external abort になる。

### 次の一手

PHY ドライバがどのレジスタを読むかを確認し、そのアドレスが本当に読めるかを
`devmem` で調べる（`-nousb` で Linux が動くので確認できる）。
`0xff636000` は以前 `busybox devmem` で `0x40057000` が読めていたので、
**別の領域**（`reg` の 2 番目以降）が問題の可能性がある。

## 追記（round 107）: PHY を有効にすると glue が dwc3 を起こしてしまう

### 決定的な観察

PHY 単体テスト（`-phyonly`、dwc3/dwc2 は `disabled`）の起動ログ:

```
[1.318773] dwc3-meson-g12a ffe09000.usb: USB3 ports: 1
[1.324165] Internal error: synchronous external abort
```

**`dwc3-meson-g12a` が probe している。しかも `USB3 ports: 1`。**

以前の `-p-gxl`（dwc3 有効）では `USB3 ports: 0` だったので、`USB3 ports: 1` は
**PHY を有効化した結果として PHY が取れるようになった**ことを意味する。

つまり:

- `&usb2_phy0`/`&usb2_phy1` を `okay` にすると、`dwc3_meson_g12a_get_phys`
  が PHY を見つけられるようになる
- その結果 `usb3_ports` が 1 になり、glue の初期化経路（`usb3_init` など）が
  走って例外になる

### `&dwc3 { status = "disabled"; }` が効いていない理由

`dwc3` ラベルは **`meson-g12-common.dtsi` の `usb@ff500000`**（dwc3 コントローラ）
を指す。しかし glue ドライバ（`amlogic,meson-gxl-usb-ctrl`）は**親ノード**で、
その子として dwc3 を `of_platform_populate` する。**子が disabled でも、親の
probe は走る**。

親（glue）の probe 内で `usb_init` → `usb3_init` などが呼ばれ、PHY が揃って
いるので `USB3 ports: 1` になり、`USB_R1` を書いて例外。

### つまり

**`-phyonly` は「PHY だけのテスト」になっていなかった。** PHY を有効にすると
glue の probe が完全な経路に入ってしまう。

### 正しい切り分け

PHY 単体を見たいなら、**`&usb`（glue）自体を disabled** にして、PHY だけを
`okay` にする必要がある。ただし以前の記録（`-nousb` のコメント）にある通り、
glue を完全に切ると `clk: Disabling unused clocks` で USB クロックが止まり、
領域が Bus error になる。

**したがって「PHY 単体」の検証は構造的に難しい。**

### 本当に必要なこと

glue の probe を通しつつ、`USB3 ports: 1` の経路（`usb3_init`）で落ちないように
する。`skip-glue-init` は `usb_init_glue` を丸ごと return しているので、
そこで止まるはずだが、`USB3 ports: 1` の後に例外が出ている。

**`usb3_init` は `usb_init_glue` の中にある**ので、`skip-glue-init` で
スキップされるはず。別の場所（`set_phy_mode` など）で落ちている可能性。
完全なコールトレースが必要。

## 追記（round 110）: `-phyonly` の解釈は誤り。skip-glue-init が入っていなかった

### 継承関係の確認

```
phyonly.dts  →  nousb.dts  →  ephy.dts  →  p.dts
```

**`gxl.dts`（`skip-glue-init` と `dwc2` 無効化を含む）を継承していない。**

つまり round 106–107 で使った `-phyonly` DTB には:

- `amlogic,skip-glue-init` が**無い**
- `&dwc2 { status = "disabled"; }` が**無い**（`-nousb` が入れているので dwc2 は無効）

したがって round 107 の「PHY を有効にすると glue が dwc3 を起こす」は正しい観察
だが、「PHY 単体の例外」ではなかった。**glue がスキップされていない構成**での
結果だった。

### round 106 の結論を訂正

「PHY 単体で例外 → 原因は `usb2_phy` の regmap 読み取り」は**誤り**。
`skip-glue-init` が無いので、glue の `usb_init_glue` → `usb3_init` が走り、
そこで例外になった可能性が高い。

### 正しい切り分け

PHY の寄与を見たいなら、`gxl.dts` を継承した上で PHY を有効にした DTB が必要。
`-phyonly` を作り直すべき。

### 現在の診断パッチ（0005）の価値

`DBG usb_init_glue node=... skip=...` が入ったので、**次回の起動で
`skip` の値が 1 か 0 かが分かる**。これで:

- `skip=1` で例外 → glue はスキップされている。真の原因は別（PHY か post_init）
- `skip=0` → プロパティが届いていない（DTS の問題）

**推測を重ねるより、この 1 行を見るのが確実。**

## 追記（round 112）: 静的解析で「skip=0」が最有力候補と判明

### 各関数が glue を触るか（ソースで確認）

| 関数 | glue レジスタを触るか |
|---|---|
| `dwc3_meson_gxl_set_phy_mode` | **触らない**（`phy_set_mode` を呼ぶだけ） |
| `dwc3_meson_g12a_set_phy_mode` | 触る（`u2p_regmap`）※G12A 経路のみ |
| **`dwc3_meson_g12a_usb3_init`** | **触る**（`USB_R3`, `USB_R2`, `USB_R1`） |
| `dwc3_meson_g12a_usb_otg_apply_mode` | 触る（`USB_R0`, `USB_R4`, `USB_R1`） |

### 推論

GXL 経路では `set_phy_mode` は安全（PHY ドライバを呼ぶだけ）。
**危険なのは `usb3_init` と `otg_apply_mode`**で、どちらも
`usb_init_glue` / `usb_post_init` の中にある。

`skip-glue-init` のガードは:

1. `usb_init_glue` の先頭（`return 0`）→ `usb2_init`/`usb3_init`/`otg_apply_mode` をスキップ
2. `gxl_usb_post_init` の先頭（`return 0`）→ `otg_apply_mode` をスキップ

**両方にガードがあるので、`skip=1` なら glue は一切書かれないはず。**

それでも例外が出るということは:

- **`skip=0`**（`amlogic,skip-glue-init` が読めていない）、または
- ガードの外に別の glue 書き込み経路がある

### `skip=0` が起きる条件

`of_property_read_bool(priv->dev->of_node, "amlogic,skip-glue-init")` が偽に
なるのは:

- DTB にプロパティが無い（`-p-gxl` ではなく別の DTB を起動した）
- `priv->dev->of_node` が glue ノードを指していない

**round 106 で使った `-phyonly` は `gxl.dts` を継承していなかった**（`skip-glue-init`
が無い）。同じ取り違えが再発している可能性がある。

### 診断マーカーの価値

`DBG usb_init_glue node=<name> skip=<0|1>` がこの 2 点を一撃で判別する。
`Image` = `3d48ee0e`（4 マーカー確認済み）。**実機での確認が待たれる。**

## 追記（round 114）: 診断が決着。glue は正しくスキップされ、例外は dwc3_core_probe

### 実測（`Image` = `29ecb5ec`、7 パッチ）

```
[1.316034] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.316750] dwc3-meson-g12a ffe09000.usb: USB3 ports: 0
[1.322095] DBG usb_init_glue node=usb@ffe09000 skip=1      ← ガードは正しく動く
[1.329321] DBG gxl_usb_post_init
[1.330377] DBG gxl_usb_post_init skip=1                    ← ここも正しくスキップ
[1.336662] Internal error: synchronous external abort

pc : dwc3_core_probe+0x96c/0x1600
lr : dwc3_core_probe+0x95c/0x1600
```

### 判明したこと

1. **`skip=1`**。`amlogic,skip-glue-init` は正しく読めている。
   `of_node` は `usb@ffe09000`（glue ノード）を指している。
2. **`usb3_init` と `otg_apply_mode` は呼ばれていない**（DBG が出ない）。
   glue への書き込みは 1 つも発生していない。
3. **例外は `dwc3_core_probe`**。glue ドライバではなく、**子の dwc3 コントローラ**。

### 例外の正体

`dwc3_core_probe+0x96c` は以前（round 27 頃）に逆アセンブルで特定した
**`dwc3->regs + 0x20` の読み取り**、すなわち `0xff500020`。

```asm
ldr x0, [x19, #1064]   ; dwc3->regs
add x0, x0, #0x20
ldr w0, [x0]           ; ← ここで synchronous external abort
```

**`0xff500000` の dwc3 レジスタ窓が応答していない。**

### これまでの遠回りの総括

| 対処 | 結果 |
|---|---|
| glue の u2p 回避（0002） | 有効。例外が次へ進んだ |
| glue 全体のスキップ（0004） | 有効。glue は完全に無音になった |
| `dwc2` 無効化（DTS） | 有効。dwc2 の例外は消えた |
| PPC（0003） | 未検証（ここまで到達していない） |
| **残る唯一の障害** | **`dwc3_core_probe` が `0xff500020` を読めない** |

### つまり

**glue は無実だった**（少なくとも今の構成では完全にスキップできている）。
本当の障害は最初から一貫して **dwc3 コントローラのレジスタ窓が開いていない**こと。

これは round 37 で一度だけ「例外なく xhci まで到達した」現象と矛盾する。
そのときは `USB2 ports: 2 / USB3 ports: 0` で xhci が登録された。今回と同じ
`USB3 ports: 0` なのに、今回は `dwc3_core_probe` で落ちる。

**差分は PPC パッチ（0003）と dwc2 無効化の有無**。PPC パッチは
`xhci->hcc_params` を触るだけで `dwc3_core_probe` の前には効かない。

### 次の一手

dwc3 のレジスタ窓が開かない原因を追う。候補:

1. **クロック**。`CLKID_USB`（`g12a_usb_general`、HHI_GCLK_MPEG1 bit 26）は
   glue が取っている。dwc3 自身は `clocks` を持たない（GXL の drvdata は
   `usb_ctrl` を要求するが、glue のもの）。
2. **リセット**。dwc3 のリセットが解除されていない。
3. **PHY の電源**。`phy_power_on` が dwc3 probe より後なら、窓が開く前に読む。

**`dwc3_core_probe` は `phy_init`/`phy_power_on` より前に `core_init` を呼ぶ**
はずなので、PHY の準備なしにレジスタを読んでいる可能性がある。

## 追記（round 115）: 原因確定。dwc3 ノードに clocks と resets が無い

### 発見

`dwc3_core_probe+0x96c`（`0xff500020` の読み取り）で落ちる原因が分かった。

**DTS の `usb@ff500000` に `clocks` と `resets` が無い。**

```dts
usb@ff500000 {
	compatible = "snps,dwc3";
	reg = <0x00 0xff500000 0x00 0x100000>;
	interrupts = <0x00 0x1e 0x04>;
	dr_mode = "host";
	snps,quirk-frame-length-adjustment = <0x20>;
	snps,parkmode-disable-ss-quirk;
	/* clocks も resets も phys も無い */
};
```

### `dwc3_probe` の初期化順序（`core.c`）

```c
if (!data->ignore_clocks_and_resets) {
        dwc->reset = devm_reset_control_array_get_optional_shared(dev);
        dwc3_get_clocks(dwc);
}
reset_control_deassert(dwc->reset);   // ← DTS に resets が無いので no-op
dwc3_clk_enable(dwc);                 // ← clocks が無いので no-op
if (!dwc3_core_is_valid(dwc)) {       // ← 0xff500020 を読む → 例外
```

**`_optional_` 系の API は「無ければ NULL」を返すので、エラーにならずに
素通りする。** その結果、コントローラが gated されたままレジスタを読みに行き、
synchronous external abort になる。

glue ノードは `clocks = <&clkc CLKID_USB>`、`resets = <&reset RESET_USB>` を
持っているが、**子の dwc3 には継承されない**。

### 修正

`&dwc3` に同じクロックとリセットを追加した:

```dts
&dwc3 {
	clocks = <&clkc CLKID_USB>;
	resets = <&reset RESET_USB>;
};
```

DTB に反映されたことを確認済み（`usb@ff500000` に `clocks = <0x02 0x2f>` と
`resets = <0x05 0x22>`）。

### これまでの全対処との関係

| 対処 | 効果 |
|---|---|
| glue の u2p 回避（0002） | 例外を次へ進めた |
| glue 全体のスキップ（0004） | glue を完全に無音化 |
| `dwc2` 無効化 | dwc2 の例外を解消 |
| **dwc3 に clocks/resets（本ラウンド）** | **0xff500020 の例外を解消する見込み** |

**すべての対処が必要だった**。glue の例外を潰すたびに次の層が現れ、最後に
dwc3 自身のクロック欠落が残っていた。

### 未検証

実機での起動確認。`Image` = `29ecb5ec`（7 パッチ、4 マーカー入り）、
`gxl.dtb` に clocks/resets 追加。

## 追記（round 116）: clocks/resets を足しても dwc3_core_probe で落ちる

### 実測

`&dwc3 { clocks = <&clkc CLKID_USB>; resets = <&reset RESET_USB>; }` を入れた
DTB（`Image` = `29ecb5ec`）で起動:

```
[1.309468] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.310537] dwc3-meson-g12a ffe09000.usb: USB3 ports: 0
[1.315787] DBG usb_init_glue node=usb@ffe09000 skip=1
[1.323069] DBG gxl_usb_post_init
[1.324094] DBG gxl_usb_post_init skip=1
[1.330446] Internal error: synchronous external abort

pc : dwc3_core_probe+0x96c/0x1600      ← 変わらず
```

**clocks/resets の追加では解決しなかった。**

### 追加した DTB が実際に使われたか

`install-boot.py` の出力に `fatload ... meson-g12a-x96-max-p-gxl.dtb` が
含まれ、ハッシュ検証も通っている。DTB は新しかった。

しかし**その DTB に clocks/resets が入っているか**は、起動ログからは確認できない。
`dtc` で確認した限りでは入っていた（`clocks = <0x02 0x2f>`）。

### 考えられること

1. **クロックとリセットは既に有効**（glue が取っているので enable_count は 1）。
   dwc3 が追加で取っても二重取得で、実態は変わらない。
   → **`0xff500000` の窓が開かない原因は別**
2. `dwc3_core_is_valid` は `0xff500020` の **DRD コンフィグレーション**を読む。
   このレジスタは「コントローラが DRD モードか」を示す。**この基板の
   `0xff500000` は dwc3 のレジスタ窓として応答しない**（vendor DT でも
   `dwc3@ff500000` だが、vendor は専用ドライバで別の準備をしている可能性）。

### 重要な矛盾

round 37 では **一度だけ `xhci-hcd` が `0xff500000` をマップして動作した**
（`io mem 0xff500000`、`hcc params 0x0228fe64`）。あのときは
`dwc3_core_is_valid` を通っている。

そのときの構成: `-p-gxl`（`skip-glue-init` **なし**）、`Image` = `b3099f99`
（パッチ 0001-0002 のみ）、`extra-usb-port` なし。

**今回との違いは `skip-glue-init` の有無**。glue の初期化をスキップしたことで、
`0xff500000` の窓を開ける何かが実行されなくなった可能性がある。

### 仮説

**glue の初期化（`usb_init_glue`）の中に、dwc3 のレジスタ窓を開ける処理がある。**
具体的には `usb2_init` の `u2p` 書き込みか、`usb3_init` か、`otg_apply_mode`。
これらを全部スキップしたため、窓が開かない。

### 次の一手

`skip-glue-init` を外し、**どの書き込みが窓を開けるか**を 1 つずつ試す。
`usb_init_glue` の中の 4 段階（usb2_init / USB_R1 / USB_R5 / usb3_init /
otg_apply_mode）を個別に有効化して、どこまで進めば `dwc3_core_is_valid` が
通るかを調べる。

ただし各段階が例外を出す可能性があるため、**段階ごとに DTB とパッチを
切り替えて試す**必要がある。

## 追記（round 117）: probe の順序が判明。差は usb_post_init の有無だけ

### `dwc3_meson_g12a_probe` の後半（`dwc3-meson-g12a.c` 820-850 行）

```c
/* Init PHYs */
for (i = 0; i < PHY_COUNT; ++i) {
        ret = phy_init(priv->phys[i]);          // PHY を初期化
        ...
}
/* Set PHY Power */
for (i = 0; i < PHY_COUNT; ++i) {
        ret = phy_power_on(priv->phys[i]);      // PHY の電源を入れる
        ...
}

if (priv->drvdata->usb_post_init) {
        ret = priv->drvdata->usb_post_init(priv);   // ← skip-glue-init でスキップ中
        ...
}

ret = of_platform_populate(np, NULL, NULL, dev);    // ← 子の dwc3 を登録
```

**`usb_post_init` は `of_platform_populate` の直前**にある。つまり
`dwc3_core_probe` の**わずか前**に実行される。

### round 37 との差分

| 項目 | round 37（成功） | 現在 |
|---|---|---|
| `phy_init` / `phy_power_on` | 実行 | 実行（同じ） |
| **`usb_post_init`（`otg_apply_mode`）** | **実行** | **スキップ** |
| `of_platform_populate` → `dwc3_core_probe` | **成功** | 例外 |

**PHY の初期化は両方で実行されている。** 違うのは `usb_post_init` だけ。

### `otg_apply_mode` の中身（host モード側）

```c
} else {
        if (priv->otg_mode != USB_DR_MODE_OTG &&
            priv->drvdata->otg_phy_host_port_disable) {
                regmap_update_bits(USB_R1,
                        USB_R1_U3H_HOST_U2_PORT_DISABLE_MASK, 0);
                msleep(500);                     // ← 500ms の待機
        }
        regmap_update_bits(USB_R0, USB_R0_U2D_ACT, 0);
        regmap_update_bits(USB_R4, USB_R4_P21_SLEEP_M0, 0);
}
```

`USB_R0` と `USB_R4` を書き、`USB_R1` の U2 ポート無効を解除する。
**500ms 待つのは、ポートの有効化が落ち着くのを待つため**と思われる。

### 仮説

**`otg_apply_mode` の `USB_R4_P21_SLEEP_M0 = 0` が、USB のスリープを解除し、
`0xff500000` の窓を開ける。** これをスキップしたため、dwc3 のレジスタが
応答しない。

### 検証方法

`skip-glue-init` のガードを `usb_init_glue` のみに戻し、`usb_post_init` は
実行させる。`usb_post_init` が書くのは `USB_R1`（U2 ポート無効の解除）、
`USB_R0`、`USB_R4` の 3 つ。

ただし `USB_R1` への書き込みは以前（round 54/55）に例外になった実績がある。
`0xffe09004` への書き込みが通るかは微妙。

## 追記（round 118）: round 37 の成功時は dwc2 が動いていた（dwc2 無効化が裏目）

### round 37 のログ（成功、`-p-gxl`、`skip-glue-init` なし）

```
[1.322276] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.323384] dwc3-meson-g12a ffe09000.usb: USB3 ports: 0
[1.865640] dwc2 ff400000.usb: supply vusb_d not found, using dummy regulator
[1.874424] dwc2 ff400000.usb: Configuration mismatch. dr_mode forced to device
[1.881780] dwc2 ff400000.usb: EPs: 7, dedicated fifos, 712 entries in SPRAM
[1.892690] xhci-hcd xhci-hcd.0.auto: xHCI Host Controller
[1.901881] xhci-hcd: hcc params 0x0228fe64
[1.911082] xhci-hcd: irq 17, io mem 0xff500000
```

**`dwc2` が初期化を完了してから、`xhci` が `0xff500000` をマップして成功した。**

### 現在との差分（2 つ）

| 項目 | round 37（成功） | 現在（失敗） |
|---|---|---|
| `dwc2` | **有効（初期化される）** | **無効化した** |
| `skip-glue-init` | なし（glue の全経路を実行） | あり（全部スキップ） |

### 新たな仮説

**`dwc2` の初期化が `0xff500000` の窓を開ける。** `dwc2` は
`usb2_phy1` を使い、`phy_init`/`phy_power_on` を呼ぶ。dwc2 の probe は
dwc3 より先に走る（ログの時刻が 1.86 秒 vs 1.89 秒）。

round 106-107 で `dwc2` を無効化したのは、`dwc2_driver_probe` の例外を
避けるためだった。しかし**その例外は `skip-glue-init` が無い構成での話**。
現在は `skip-glue-init` があるので、dwc2 を有効に戻しても例外にならない
可能性がある。

### 次の一手

**`dwc2` を有効に戻す。** 現在の構成（`skip-glue-init` あり、7 パッチ）で
`&dwc2 { status = "okay"; }` にすれば:

- dwc2 が PHY を初期化し、`0xff500000` の窓が開く可能性
- dwc2 の probe 自体は `skip-glue-init` の影響を受けない（別ドライバ）
- 以前の dwc2 例外は glue 経由だったので、今は起きない可能性

**これが最短の検証。** 1 行の DTS 変更で試せる。

## 追記（round 120）: round 37 の成功は「2 パッチのみ」の構成だった

### ビルドログから確定

`work/build/build-unit3-20.log`（`Image` = `b3099f99`、round 37 で成功した版）:

```
Applying 0001-vdec-meson-sm1-codecs.patch
Applying 0002-dwc3-meson-skip-u2p.patch
```

**2 パッチのみ。** つまり:

| 項目 | round 37（成功） | 現在（失敗） |
|---|---|---|
| 0002 u2p 回避 | あり | あり |
| **0004 skip-glue-init** | **なし**（glue を全部実行） | **あり**（全部スキップ） |
| **dwc2** | **有効** | **無効**（round 115 で有効に戻した） |
| 0003 PPC | なし（旧版） | あり（修正版） |
| 0005-0007 診断 | なし | あり |

### 決定的な意味

**round 37 では `usb_init_glue` が完全に実行され、それでも成功した。**

つまり:

- `u2p` 書き込みは 0002 で回避していた（例外を避けるため）
- しかし `USB_R1`、`USB_R5`、`usb3_init`、`otg_apply_mode` は**実行された**
- そして `dwc3_core_probe` は通り、`xhci` が登録された

**現在 `skip-glue-init` で全部スキップしているのが原因**である可能性が高い。
round 37 では `USB_R1` への書き込みが**成功していた**（例外にならなかった）。

### 矛盾の解消

round 54/55 で「`USB_R1` 書き込みで例外」だったのは、**その時の構成が
round 37 と違った**ため。具体的には:

- round 54/55 は DTB に `extra-usb-port` を追加していた
- round 37 は追加していない

**DTB のプロパティ追加が、glue の初期化順序や PHY の状態を変えていた**
可能性がある。

### 次の検証

**round 37 の構成を正確に再現する**:

1. パッチを 0001 + 0002 のみにする（0003-0007 を外す）
2. DTS は `gxl.dts`（`usb_ctrl` クロック入り、`skip-glue-init` なし、
   `dwc2` 有効、`extra-usb-port` なし、`port-power-control` なし）
3. `Image` を再ビルド

これで round 37 と同じ条件になり、成功するか確認できる。成功すれば、
そこから 1 つずつ適用して「何が壊すか」を特定する。

**これが最も確実な方法。** 推測を重ねるより、既知の成功構成から始める。

## 追記（round 121）: dwc2 を有効にしても dwc2 は probe せず、同じ例外

### 実測（`Image` = `29ecb5ec`、`gxl.dtb` = `30d2605d`、`&dwc2 { status = "okay"; }`）

```
[1.326574] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.327658] dwc3-meson-g12a ffe09000.usb: USB3 ports: 0
[1.332966] DBG usb_init_glue node=usb@ffe09000 skip=1
[1.340264] DBG gxl_usb_post_init
[1.341264] DBG gxl_usb_post_init skip=1
[1.347593] Internal error: synchronous external abort

pc : dwc3_core_probe+0x96c     ← 同じ
```

**`dwc2 ff400000.usb` のログが一切出ていない。** つまり dwc2 は probe して
いない。dwc3 の方が先に走って落ちている。

### 順序の問題

round 37 のログでは dwc2 が **1.865 秒**、xhci が **1.892 秒**。dwc2 が先。
今回は dwc3 が 1.34 秒で落ちるので、dwc2 まで到達しない。

**`dwc3-meson-g12a` が glue の probe の中で子の dwc3 を登録し、それが即座に
走る**ため、dwc2 の出番がない。

### dwc2 仮説の否定

**dwc2 の PHY 初期化が `0xff500000` の窓を開ける、という仮説は否定された。**
dwc2 は probe すらしていない。

### 残る唯一の事実

- round 37: `usb_init_glue` が**完全に実行**され、`USB_R1`/`USB_R5`/`usb3_init`/
  `otg_apply_mode` が動いた → **xhci 成功**
- 現在: これらを**すべてスキップ** → `0xff500020` が読めない

**glue の初期化のどれかが `0xff500000` の窓を開けている**、という仮説に戻る。

### 次の一手（要ビルド）

round 37 の構成（2 パッチ + `gxl-noextra` DTB）を正確に再現する。
ビルドが OOM で 2 回失敗したため、`BR2_JLEVEL=1` に下げて再試行する。

```sh
BR2_JLEVEL=1 X96MAX_PROFILE=ramboot X96MAX_UNIT=3 ./scripts/build-linux.sh
```

## 追記（round 123）: round 37 の成功は再現しない（同一構成で失敗）

### 実施

round 37 と**完全に同一の構成**を用意して起動した:

| 項目 | 値 |
|---|---|
| パッチ | 0001 + 0002 のみ |
| `Image` | `c424cb56`（`skip-glue-init` = False、`DBG` = False を文字列検索で確認） |
| DTB | `meson-g12a-x96-max-p-gxl-noextra.dtb` = `fcd9e79f`（round 37 と同じファイル） |
| 書き込み | Android 経由、SD 上でハッシュ一致を確認 |

### 結果: 失敗

```
[1.308617] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.309642] dwc3-meson-g12a ffe09000.usb: USB3 ports: 0
[1.314967] Internal error: synchronous external abort

Call trace:
 regmap_mmio_read32le+0x8/0x20
 _regmap_update_bits
 regmap_update_bits_base+0x54/0x80
 dwc3_meson_g12a_usb_init_glue.isra.0+0x38/0x190
 dwc3_meson_gxl_usb_init+0x14/0x30
```

**`USB_R1`（`0xffe09004`）への書き込みで例外。** round 54/55 と同じ症状。

### 結論

**round 37 の「例外なしで xhci まで到達」は再現しない。** 同一の Image・DTB・
手順で、今回は glue の書き込みで落ちる。

### 考えられる説明

1. **round 37 の成功は偶発**。何らかのタイミング（電源投入からの経過時間、
   PHY の状態、U-Boot の初期化具合）に依存していた。
2. **機体のハード状態が変わった**。DT295 の挿抜、USB の再列挙、電源の
   入れ直し回数などで PHY の状態が変わる可能性。
3. **round 37 のログを取り違えている**。ただし `work/unit3-boot-gxl2.log` に
   `xhci-hcd: io mem 0xff500000` が記録されており、実際に動いていたのは確か。

**いずれにせよ、`0xffe09004` への書き込みが通るかどうかが不安定**で、
それが成否を分けている。

### 重要な観察

- 現在の構成（`skip-glue-init`）では **`USB_R1` を書かないので例外にならない**
- 代わりに `dwc3_core_probe` の `0xff500020` で落ちる
- つまり **「書けば glue で落ち、書かなければ dwc3 で落ちる」** の二択

**`0xffe09004` が書ける条件**を見つけるか、`USB_R1` を書かずに
`0xff500000` の窓を開ける方法が必要。

## 追記（round 124）: 1代目・2代目は「素の G12A glue」で USB が動いている

### 決定的な発見

`board/x96max/ramboot-unit1.fragment` と `ramboot-unit2.fragment` は、
どちらも同じ DTB を使っている:

```
BR2_LINUX_KERNEL_INTREE_DTS_NAME="... meson-g12a-x96-max-p-ephy-4g"
```

**`meson-g12a-x96-max-p-ephy-4g.dtb` = 標準の `amlogic,meson-g12a-usb-ctrl`**
（GXL でも G12A の特殊版でもない、素の G12A glue）。

```dts
usb@ffe09000 {
	compatible = "amlogic,meson-g12a-usb-ctrl";
	clocks = <&clkc CLKID_USB>;      /* clock-names なし */
	dr_mode = "host";
};
```

### 意味すること

**1代目と2代目は、この素の G12A glue で USB が動いている。** 2代目は
DT295 で実際に録画していた（NAS に `state: recording` の記録あり）。

つまり:

- `0xffe09000` を glue として扱う主線の解釈で**問題なく動く**
- `u2p_regmap` も `USB_R1` も**書けている**
- 私が round 54/55 で見た「`USB_R1` で例外」は、**別の要因**（当時 DTB に
  `extra-usb-port` を追加していた等）で起きていた可能性

### 遠回りの総括

私はこの 60 ラウンド以上、`gxl` 互換、`skip-glue-init`、`dwc2` 無効化、
`clocks`/`resets` 追加、`PPC` など**多数の回避策**を積み重ねてきた。
しかし **1代目・2代目は何もせず素の G12A で動いている**。

**答えは「素の `ephy-4g` に戻すこと」**の可能性が高い。

### 次の検証

1. パッチを 0001 + 0002 のみ（round 37 構成）
2. DTB を **`meson-g12a-x96-max-p-ephy-4g.dtb`**（`b031dc16`）
3. 起動して `xhci-hcd` が動くか確認

round 54/55 で `ephy-4g` が失敗した記録があるが、そのときは DTB に
`extra-usb-port` を足していた。**素の `ephy-4g` そのものは試していない**
（round 54 のログを確認する必要がある）。

### 注意

3代目の板は 4GiB ではなく 2GiB（`Machine model: X96Max_P (G12A, 2 GiB, ...)`）。
`ephy-4g` は 4GiB 用の名前だが、メモリ量の違いは DTB の USB 設定に影響しない
はず。ただし要確認。

## 追記（round 125）: `ephy-4g` で USB3 ports: 1 まで到達。それでも USB_R1 で落ちる

### 実測（`Image` = `c424cb56`、DTB = `ephy-4g` = `b031dc16`、2 パッチのみ）

```
[1.317909] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.318879] dwc3-meson-g12a ffe09000.usb: USB3 ports: 1      ← gxl では 0 だった
[1.324217] Internal error: synchronous external abort

Call trace:
 regmap_mmio_read32le+0x8
 _regmap_update_bits
 regmap_update_bits_base+0x54/0x80
 dwc3_meson_g12a_usb_init_glue.isra.0+0x38/0x190
```

**`USB3 ports: 1`** — `ephy-4g` では PHY が正しく数えられている（`gxl` では 0）。
これは 1代目・2代目と同じ状態。だが **`USB_R1` で落ちる**。

### 機体の正体（判明）

```
Machine model: X96Max_P (G12A, 4 GiB, external Ethernet PHY)
Memory: 3087124K/3538944K available
```

**この機体は 4 GiB。** 以前「2 GiB」と記録していたのは誤り。
`ephy-4g`（4GiB + external Ethernet PHY）がまさに正しい DTB。

**1代目・2代目と同じ条件になった。それでも `USB_R1` で落ちる。**

### レジスタの構造（G12A）

```c
#define G12A_GLUE_OFFSET  0x80
#define U2P_REG_SIZE      0x20

glue regmap   = base + 0x80   → 0xffe09080
u2p_regmap[i] = base + i*0x20 → 0xffe09000 (i=0), 0xffe09020 (i=1)
```

`USB_R1` = `0xffe09080 + 0x04` = **`0xffe09084`**。

### 残る謎

1代目・2代目は同じ DTB・同じコードで `USB_R1` を書けている。3代目は書けない。

**機体個体差か、PHY の状態か、電源投入のタイミングか。**
DT295 の挿抜、USB の再列挙、電源の入れ直し回数などが影響している可能性。

### 次の一手

1代目（`192.168.99.109`）が起動していれば、その `dmesg` で
`USB_R1` の書き込みがどうなっているか比較できる。ただし現在は停止中。

**または 3代目で `0xffe09084` が読めるか**を Linux から確認する
（`-nousb` DTB なら Linux が起動するので、`devmem` で調べられる）。

## 追記（round 126）: `-nousb` では USB レジスタを読めない（クロックが止まる）

### 実測

`-nousb` DTB（全 USB 無効）で Linux を起動し、`busybox devmem` で読んだ:

```
0xffe09000   Bus error
0xffe09020   Bus error
0xffe09080   Bus error
0xffe09084   Bus error
0xffe09090   Bus error
0xff500000   Bus error
0xff500020   Bus error

0xff634000   0x00000000      ← HHI は読める
0xfffe0000   Bus error       ← 元から無い領域
```

### 原因

```
[2.102487] clk: Disabling unused clocks
```

**USB のクロックが止められたため、USB 領域が Bus error になる。**
`-nousb` のコメントに書かれていた既知の現象。

**機体固有の問題ではなく、この調査方法が使えないだけ。**

### 結論

`-nousb` では USB レジスタを調べられない。**USB を有効にした DTB で
Linux が起動できれば**調べられるが、その構成では `USB_R1` で落ちる。

### 別の方法

1. **U-Boot から読む**。U-Boot の `md` コマンドで `0xffe09084` を読めるか。
   U-Boot は USB のクロックを止めていないので、値が見えるはず。
2. **2代目（`192.168.99.123`）が起動したとき**に同じレジスタを読んで比較。

**U-Boot の `md` が最も手軽**。次の起動で確認する。

## 追記（round 128）: glue を `okay` にすると子の status に関係なく USB_R1 を書く

### `-clkon`（glue okay / dwc3・dwc2 disabled）でも同じ例外

```
[1.308375] USB2 ports: 2
[1.309329] USB3 ports: 1
[1.314747] Internal error

Call trace:
 regmap_mmio_read32le
 _regmap_update_bits
 regmap_update_bits_base
 dwc3_meson_g12a_usb_init_glue.isra.0+0x38    ← USB_R1
```

**子を両方 `disabled` にしても `usb_init_glue` が走る。**

### 理由（`dwc3-meson-g12a.c` 761-771 行）

```c
/* Get dr_mode */
priv->otg_mode = usb_get_dr_mode(dev);
...
ret = priv->drvdata->usb_init(priv);      // ← 無条件
```

**glue の probe は子の `status` を見ずに `usb_init` を呼ぶ。**
だから glue を `okay` にする限り、必ず `USB_R1` を書いて例外になる。

### トレードオフ（問題の核心）

| 構成 | クロック | USB_R1 書き込み | Linux 起動 |
|---|---|---|---|
| glue `disabled`（`-nousb`） | **止まる** | 走らない | **する** |
| glue `okay`（`-clkon`） | 保たれる | **走って落ちる** | しない |

**レジスタを読むにはクロックが要るが、クロックを保つには glue を
`okay` にする必要があり、そうすると `USB_R1` で落ちる。**

### 唯一の観測手段

**U-Boot から `md` で読む。** U-Boot は glue のクロックを止めないので、
Linux を起動せずにレジスタを確認できる。ただし autoboot が先に走るので、
確実にプロンプトで止める必要がある。

### 重要な疑問

1代目・2代目は glue `okay` で `USB_R1` を書けている。3代目だけ書けない。

**同じ DTB（`ephy-4g`）、同じコード（2 パッチ）で、なぜ違うのか。**

可能性:
- 機体のリビジョン差（`X96Max_P` にも複数ロットがある）
- U-Boot が残した glue の初期状態
- DT295 の挿抜で PHY の状態が変わった

## 追記（round 129）: probe の順序を確認。順序は原因ではない

### `dwc3_meson_g12a_probe` の順序（755-790 行）

```c
if (priv->vbus) regulator_enable(priv->vbus);      // VBUS を入れる
priv->otg_mode = usb_get_dr_mode(dev);
ret = priv->drvdata->usb_init(priv);               // ← USB_R1 を書く（ここで落ちる）
/* Init PHYs */
for (...) phy_init(priv->phys[i]);                 // PHY 初期化は後
/* Set PHY Power */
for (...) phy_power_on(priv->phys[i]);             // PHY 電源も後
```

**`usb_init` が `phy_init`/`phy_power_on` より前**にある。これは mainline の
設計であり、1代目・2代目も同じ順序で動いている。

**したがって順序は 3代目固有の問題の原因ではない。**

### 残る唯一の謎

1代目・2代目と 3代目で、以下がすべて同じなのに結果が違う:

| 項目 | 1代目 | 2代目 | 3代目 |
|---|---|---|---|
| DTB | `ephy-4g` | `ephy-4g` | `ephy-4g` |
| パッチ | 0001+0002 | 0001+0002 | 0001+0002 |
| `USB_R1`（`0xffe09084`） | 書ける | 書ける | **書けない** |

### 決定的な確認方法

**2代目が起動したときに `0xffe09084` を読む。** 読めれば「レジスタは存在する」
3代目で Bus error なら「領域が無い」。

ただし 2代目は別の機体で、シリアルは 3代目に繋がっている。2代目には
SSH で入る必要がある（現在は停止中）。

### 代替

3代目の U-Boot で `md 0xffe09084 1` を実行する。autoboot を確実に止める
必要がある（`catch_prompt` が Enter を送り続けるので、タイミング次第で
Android に進んでしまう）。

## 追記（round 132）: レジスタは存在する。vendor は正しく書けている

### Android の `aml_reg` で実測（決定的）

```
0xffe09084 = 0xFF020000     ← USB_R1（G12A glue の USB_R1）
  U3H_FLADJ_30MHZ (bits24-19) = 0x20    ← mainline が書く値と一致
  PORT_POWER_CONTROL_PRESENT (bit17) = 1
  P30_PCS_TX_SWING_FULL (bits31-25) = 127

0xffe09000 = 0x0B           ← U2P_R0
0xffe09080 = 0x6A           ← usb3phy 制御
0xffe09090 = 0x0            ← USB_R4
```

**レジスタは存在し、vendor（Android）は正しく書けている。**
値も mainline の期待値と完全に一致（FLADJ=0x20、TX_SWING=127）。

**「領域が無い」という仮説は否定された。** 問題は「mainline だけが書けない」こと。

### probe の順序（`dwc3-meson-g12a.c` 720-745 行）

```c
ret = devm_clk_bulk_get(dev, num_clks, clks);
ret = clk_bulk_prepare_enable(num_clks, clks);      // ← クロック有効化
platform_set_drvdata(pdev, priv);
priv->reset = devm_reset_control_get_shared(dev, NULL);
ret = reset_control_reset(priv->reset);             // ← リセット実行
```

**GXL の `num_clks` は `ARRAY_SIZE(meson_g12a_clocks)` = 1**（mainline のバグ、
round 88 で発見）。`meson_gxl_clocks` は 2 つ（`usb_ctrl`、`ddr`）だが、
**1 つしか有効化されない**。

`meson_g12a_clocks` は `{ .id = NULL }`（名前なし）。GXL 版は
`usb_ctrl` を要求するので、DTS で `clock-names = "usb_ctrl"` を付けて通した。

**しかし `ddr`（2 つ目）が有効化されていない。** これが USB のレジスタ窓に
必要なクロックかもしれない。

### 新しい仮説

**`ddr` クロック（`meson_gxl_clocks[1]`）が有効化されていないため、
USB のレジスタ窓が応答しない。**

mainline のバグ（`num_clks` の取り違え）が原因。3代目だけではなく、
1代目・2代目も同じバグの影響下にあるはずだが、そちらは動いている。
差は DTS のクロック構成かもしれない。

### 検証方法

`num_clks` を正しい値（2）に修正するパッチを入れ、`ddr` クロックも
有効化する。DTS に `clock-names = "usb_ctrl", "ddr"` と 2 つのクロックを
追加する必要がある。

`ddr` に対応するクロック ID は `CLKID_USB1_DDR_BRIDGE`（55）。

## 追記（round 136）: round 73 の成功時は U-Boot で `usb start` を実行していた

### ログの照合

round 73 で DT295 が認識されたときのログ（`work/unit3-boot-gxl2.log`）には
`usb start` は含まれていない。**しかし round 73 の手順を思い返すと、U-Boot で
`usb start` を実行して DT295 の認識を確認してから `booti` した**（round 73 の
会話で `usb start` → `usb info` を実行し、PIXELA PIX-DT295 を確認している）。

そのログは別ファイル（`work/unit3-uboot5.log` 等）に残っている可能性がある。

### 仮説

**U-Boot で `usb start` を実行すると、USB コントローラと PHY が初期化され、
その状態が Linux に引き継がれる。** これにより mainline の `USB_R1` 書き込みが
通るようになる。

根拠:

- U-Boot の `usb start` は XHCI を初期化し、PHY を立ち上げる
- round 73 では `usb start` の後、DT295 が `2: Vendor specific, PIXELA PIX-DT295`
  として見えていた（＝実際に動作していた）
- 現在は `usb start` をせずに `booti` している

### 検証方法

```sh
# U-Boot で
usb start
# その後 booti
booti 0x01080000 0x10000000 0x08000000
```

これで `USB_R1` が通るか確認する。**すぐ試せる**（ビルド不要）。

### なぜこれが効くか

U-Boot の `usb start` は:
1. `0xffe09000` の glue を初期化（vendor の実装）
2. `0xff500000` の XHCI を初期化
3. PHY を立ち上げる

その結果、glue のレジスタが「書ける状態」になり、`0xff500000` の窓も開く。
Linux はその状態を引き継ぐので、mainline の初期化が通る可能性がある。

## 決着（round 137）: U-Boot の `usb start` が鍵だった。Linux が例外なく起動

### 実測（`Image` = `b8b68100`、DTB = `ephy-4g` = `b031dc16`、`--usb-start` あり）

```
[1.303430] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.304359] dwc3-meson-g12a ffe09000.usb: USB3 ports: 1
（例外なし）

Starting crond: OK
Starting dropbear sshd: OK
B-CAS: DT295 internal card (vendor firmware descrambles)
Starting pix-stream: OK
Starting pix-logo-weekly: OK

X96Max_P3 RAM-root Linux
x96max-p3 login:
```

**`Internal error` が無く、ユーザー空間まで完全に起動した。**

### 決定的だったこと

**U-Boot で `usb start` を実行してから `booti` する。**

```sh
usb start
booti 0x01080000 0x10000000 0x08000000
```

round 73 で DT295 が認識されたときも、この手順を踏んでいた。

### なぜ効くのか

U-Boot の `usb start`（vendor の実装）が:

1. `0xffe09000` の glue を vendor の解釈で初期化
2. `0xff500000` の XHCI を初期化
3. USB2/USB3 PHY を立ち上げる

その状態を Linux が引き継ぐため、mainline の `usb_init_glue` が書く
`USB_R1`（`0xffe09084`）が**通る状態になっている**。

`usb start` をしないと glue が未初期化のままで、`USB_R1` へのアクセスが
synchronous external abort になる。

### これまでの遠回りの総括

60 ラウンド以上かけて glue のスキップ、dwc2 無効化、clocks/resets 追加、
PPC、`num_clks` 修正などを試したが、**答えは「U-Boot で `usb start` を
1 回実行してから起動する」**だった。

`ephy-4g`（1代目・2代目と同じ素の G12A）が正しい DTB で、パッチも
0001+0002 だけで足りる可能性がある（0008 の効果は未検証）。

### ツール

`tools/install-boot.py --usb-start` で自動化:

```sh
python3 tools/install-boot.py --dtb meson-g12a-x96-max-p-ephy-4g.dtb \
    --files Image --usb-start
```

### 残る問題

起動後、**SSH とシリアルの両方が応答しない**。ログには `dhcpcd` や `eth0` の
設定が見えない。ネットワークが上がっていない可能性があり、要調査。

### 追加した改善

`panic_on_oops=1 panic=8` を bootargs に追加。oops で箱が固まる代わりに
自動再起動する（毎回の電源再投入を避けるため）。

## 追記（round 139）: `usb start` 成功の再現条件を整理

### 成功した 2 回

| 回 | DTB | Image | パッチ | `usb start` | 結果 |
|---|---|---|---|---|---|
| round 73 | `gxl`（`usb_ctrl` のみ） | `b3099f99` | 0001+0002+旧0003 | **あり** | xhci 到達 |
| round 137 | `ephy-4g` | `b8b68100` | 0001+0002+0008 | **あり** | **ユーザー空間まで起動** |

**共通点: `usb start` を実行している。**

### 失敗した回（`usb start` なし）

| 回 | DTB | Image | 結果 |
|---|---|---|---|
| round 116 | `gxl` | `29ecb5ec` | `dwc3_core_probe+0x96c` |
| round 123 | `gxl-noextra` | `c424cb56` | `usb_init_glue+0x38` |
| round 125 | `ephy-4g` | `c424cb56` | `usb_init_glue+0x38` |
| round 128 | `clkon` | `29ecb5ec` | `usb_init_glue+0x38` |

**すべて `usb start` なし。**

### つまり

**`usb start` の有無が成否を分ける。** DTB やパッチの違いより決定的。

### 次に確認すべきこと

**`usb start` だけで十分か、Image のパッチも必要か。**

- `ephy-4g` + `b8b68100`（0008 入り）+ `usb start` → **成功**
- `ephy-4g` + `c424cb56`（0008 なし）+ `usb start` → **未検証**

もし後者でも成功するなら、**0008（`ddr` クロック）は不要**。最小構成が絞れる。

### 現在の課題

起動後のネットワーク不通。`panic_on_oops=1 panic=8` で自動再起動するので、
次回の起動で「oops が起きているか」がログから分かる。

## 追記（round 140）: 2代目の実績ログに `usb start` → USB メモリ起動 → DT295 録画の完全な手順があった

### 発見した記録（`work/restart-recipe-persist-result.log`）

```
linux_boot_ram='run ram_boot_setup; for ram_try in 1 2 3; do if usb start; then sleep 3; run ram_boot_load; fi; ...'

1 Storage Device(s) found

[1.262464] dwc3-meson-g12a ffe09000.usb: USB2 ports: 2
[1.302238] xhci-hcd: xHCI Host Controller
[1.310272] xhci-hcd: hcc params 0x0228fe6c hci version 0x110    ← PPC=1
[1.319476] xhci-hcd: irq 17, io mem 0xff500000

{"model":"DT295","phase":3,"packets":5550080,"elapsed_ms":437443,"usb_bytes":1043513344,...}
```

**`usb start` → USB メモリから起動 → xhci が `0xff500000` をマップ →
DT295 で 5,550,080 パケット（約 1GB）を録画。**

### 機体

`aml_dt = g12a_u211_4g`、プロンプト `g12a_u211_v1#` = **2代目**。

### つまり

**2代目では「USB メモリ + `usb start`」で完全に動作していた。**
`hcc params 0x0228fe6c`（PPC=1）は vendor の U-Boot が `usb start` で
設定したもの。

### 3代目への適用

3代目（`g12a_u212_v1`）でも同じ手順が使えるはず:

1. USB メモリ（FAT32）に `Image`、`rootfs.cpio.uboot`、DTB を置く
2. U-Boot で `usb start`
3. `fatload usb 0:1 ...` でロード
4. `booti`

**round 137 では SD からロードしつつ `usb start` だけ実行して成功した。**
USB メモリからロードする必要はなく、`usb start` の副作用（vendor の
USB 初期化）が本質。

### 起動後のネットワーク不通について

2代目のログでは録画が動いていたので、ネットワークも当然動いていた。
3代目で不通なのは別の問題（`pix-stream` が USB を触って oops した可能性）。

## 追記（round 141）: ネットワーク不通の原因候補を絞る

### 確認したこと

| 項目 | 結果 |
|---|---|
| `S40network` | **rootfs に存在**（`S41settime` → `S42pix-usb-audit` → `S50crond` の順で実行） |
| 起動ログの `eth0` 設定 | 出ていない（ただしシリアルログの欠落が激しい） |
| `pix_usb_reset` の実行条件 | `dt295_recover` からのみ。DT295 が見つかれば走らない |
| `nousb` 起動時 | **SSH が通った**（round 105） |

### 考えられる原因

1. **`usb start` が Ethernet に干渉**。U-Boot で USB を初期化した状態が
   Linux の `meson8b-dwmac` に影響している可能性。ただし 2代目は
   `usb start` + USBメモリ起動で録画まで動いていたので、干渉するとは考えにくい。
2. **シリアルログの欠落**。実際は `eth0` が設定されているが、ログが落ちている。
   round 137 では `Starting crond: OK` `dropbear sshd: OK` は見えたので、
   その前後の行が落ちている可能性。
3. **DHCP の失敗**。`pre-up /etc/network/nfs_check` が失敗して `ifup` が
   途中で止まった可能性。`nousb` のときは成功していた。

### 3 が有力

`nfs_check` は NAS の NFS が応答するか確認するスクリプト。`usb start` で
起動時間が変わったことでタイミングが変わり、失敗した可能性。

### 次の確認

電源投入後、起動して:

1. **シリアルで `login:` が出ているか**（出ていればコンソールは生きている）
2. `root` でログインして `ip -4 addr show eth0` を実行
3. `ifup eth0` を手動で実行して結果を見る
4. `dmesg | tail -20` でエラーを確認

**シリアルが生きていれば、ネットワークが死んでいても調査できる。**
round 137 ではシリアルも無応答だったので、まずそこを確認する。

## 追記（round 142）: `nfs_check` は Buildroot 標準スクリプトだった（原因ではない）

### 確認

`/etc/network/nfs_check` は `board/x96max/overlay/` には無いが、
**Buildroot が生成する標準スクリプト**:

```sh
#!/bin/sh
# This allows NFS booting to work while also being able to configure
# the network interface via DHCP when not NFS booting. ...
# This script is run by ifup and will attempt to detect if a NFS root
# mount uses the interface to be configured (IFACE), and if so does
# not configure it.
```

**NFS ルート起動時にインターフェースを設定しないようにするためのもの。**
RAM 起動では NFS を使わないので、`ifup` を止めない。

**私の推測（`nfs_check` が無くて `ifup` が失敗）は誤り。**

### ネットワーク不通の原因は未特定

`nousb` DTB では SSH が通った（round 105）が、`usb start` + `ephy-4g` では
通らない（round 137）。差分は:

1. `usb start` の有無
2. DTB（`nousb` は USB 全無効、`ephy-4g` は USB 有効）
3. Image（round 105 は `b3099f99`、round 137 は `b8b68100`）

**`usb start` が Ethernet に干渉している可能性**が残る。

### 次の確認（シリアルが生きていれば）

`login:` でログインして:

```sh
ip -4 addr show eth0
dmesg | grep -i dwmac | tail -5
cat /sys/class/net/eth0/carrier
ethtool eth0 2>/dev/null | head -5
```

これで「リンクは上がっているが DHCP が失敗」か「リンク自体が落ちている」か
分かる。

# 🎉 解決（round 153）: `pix-stream` の USB 操作がネットワークを壊していた

### 決定的な実験

`PIX_TUNER=none`（`wait_for_tuner` が待機するだけで USB を触らない）にした
rootfs で起動したところ、**SSH が通った**:

```
ssh is up

$ uname -r
6.18.62

$ ip -4 addr show eth0
inet 192.168.99.56/24 brd 192.168.99.255 scope global eth0

$ lsusb
Bus 001 Device 002: ID 06b8:103b Pixela Corp. PIX-DT295     ← DT295 認識

$ cat /dev/shm/netdiag.txt
[17.543] meson8b-dwmac eth0: configuring for phy/rgmii link mode
[20.956] meson8b-dwmac eth0: Link is Up - 1Gbps/Full - flow control rx/tx
carrier: 1
operstate: up
```

**ネットワークも USB も完全に正常。**

### 原因

**`pix-stream`（`PIX_TUNER=dt295`）が USB を操作すると、Ethernet と
シリアルが死ぬ。**

`pix-stream` の起動時の流れ:
```
S97pix-stream → pix-stream-simple → wait_for_tuner（DT295 を発見）
             → run_worker probe → pix-ts-legacy が DT295 を open
             → 何らかの USB 操作でイーサネット/コンソールが停止
```

`PIX_TUNER=none` では `wait_for_tuner` が待機するだけで、`pix-ts-legacy` が
DT295 を open しない。**そのため USB を触らず、ネットワークも無事。**

### したがって

**Linux 起動・ネットワーク・DT295 認識のすべてが成功している。**
残る問題は「`pix-stream` が DT295 を open したときに何が壊れるか」。

### `pix-ts-legacy` が疑わしい

`pix-ts-legacy` は vendor のファームウェアを使って DT295 を操作する。
これが USB のアイソクロナス転送を開始したとき、`meson8b-dwmac` の
DMA と競合する可能性がある（同じ AHB バス上）。

### 次の検証

1. `PIX_TUNER=dt295` に戻す（本来の設定）
2. `pix-stream` を起動して、**ネットワークがいつ死ぬか**を観察
3. 死ぬタイミングで `dmesg` を確認（`netdiag` は 20 秒後なので、
   もっと早い段階の記録が必要かもしれない）

### 到達点（更新）

| 項目 | 状態 |
|---|---|
| Linux RAM 起動 | **達成** |
| ネットワーク（1Gbps） | **達成** |
| DT295 認識 | **達成** |
| SSH | **達成** |
| **録画パイプライン** | **`pix-stream` 起動時にネットワークが停止** |
| NAS 保存・CM カット | 未達 |

# 🔬 再現成功（round 154）: `pix-stream` 起動 → システム全体ハング

### 実験

稼働中の Linux（`PIX_TUNER=none`、SSH・1Gbps・DT295 認識すべて正常）で:

```sh
sed -i "s/^PIX_TUNER=none$/PIX_TUNER=dt295/" /etc/default/pix-stream-linux
/etc/init.d/S97pix-stream start
```

### 結果: **即座に全停止**

```
SSH DEAD          ← 3 秒後には SSH 不通
ping: 100% packet loss
シリアル: 0 バイト（無応答）
```

**ネットワーク・シリアル・システム全体がハング。**

### 確定した因果関係

| 状態 | 結果 |
|---|---|
| `PIX_TUNER=none`（DT295 を触らない） | **SSH・1Gbps・DT295 認識すべて正常** |
| `PIX_TUNER=dt295` + `pix-stream` 起動 | **3 秒以内にシステム全体ハング** |

**`pix-stream` が DT295 を open した瞬間にカーネルごと停止する。**

### 犯人

`pix-stream-simple` → `run_worker probe` → `pix-ts-legacy` が DT295 の
USB アイソクロナス転送を開始する。その瞬間に:

- Ethernet（`meson8b-dwmac`）が停止
- シリアル（`ttyAML0`）が停止
- おそらくカーネル全体がロック

**アイソクロナス転送の DMA が、他の DMA（Ethernet、UART）と衝突している**
可能性が高い。または `dwc3` の xHCI リングが壊れてバスがロック。

### vendor（Android）では動く理由

Android は vendor の `dwc3` ドライバと vendor の USB 初期化を使っており、
アイソクロナス転送の設定（TRB、bandwidth、interval）が正しい。
mainline の `dwc3`/`xhci-hcd` では、この DT295 のアイソクロナス要求を
正しく扱えていない可能性がある。

### 次の一手

1. **`pix-ts-legacy` を単体で実行**して、どこまで進むか確認
   （`pix-stream` を介さず、直接 DT295 を open する）
2. `dmesg` を**ハング直前まで**取得する（シリアルに流れる前に止まるので困難）
3. `panic_on_oops=1` が効いているなら、ハングではなく panic になるはず。
   効いていないなら、oops ではなく**完全なデッドロック**。

### 重要な意味

**Linux 起動・USB 認識・ネットワークはすべて達成済み。**
残る唯一の障害は「DT295 からの実際のストリーム読み出し」。
これは `pix-ts-legacy`（vendor バイナリ）と mainline の xHCI の相性問題。

# 🔬 重大な訂正（round 155）: `pix-ts-legacy` 単体は正常に動作する

### 実験

稼働中の Linux（`PIX_TUNER=none`、SSH 可）で `pix-ts-legacy` を単体実行:

```sh
pix-ts-legacy --model dt295 -s terrestrial -c 17 \
    -a /usr/share/pix-tuner -t 30
```

出力を SD の FAT に 1 行ずつ書き出し（`sync` 付き）。

### 結果: **ハングしない。データが出ている**

```
$ wc -l < /mnt/sd/tuner-probe.log
1605        ← 1605 行の TS データ

$ ls -l /mnt/sd/tuner-probe.log
310292      ← 310 KB

$ ps | grep -c "[p]ix-ts-legacy"
5           ← まだ稼働中

$ nc -z 192.168.99.56 22  → SSH alive
$ ping 192.168.99.56       → ping OK
```

**`pix-ts-legacy` は DT295 から 310KB のデータを読み出し、SSH も
ネットワークも生きている。**

### 訂正

round 154 で「`pix-ts-legacy` が DT295 を open した瞬間にハング」と書いたが、
**それは誤り。** `pix-ts-legacy` 単体は問題ない。

ハングの原因は **`pix-stream` の別の部分**:

```
pix-stream-simple
  ├─ wait_for_tuner
  ├─ run_worker probe      ← pix-ts-legacy を起動（これは正常）
  ├─ encode()              ← ffmpeg (m2v → h264_v4l2m2m)
  ├─ fragment()            ← ffmpeg (TS → fMP4)
  └─ streamer()            ← stb_streamer
```

**`encode()` の `h264_v4l2m2m`（ハードウェアエンコーダ）が疑わしい。**
VPU の DMA が Ethernet と衝突する可能性がある。

### 次の一手

`encode`/`fragment`/`streamer` を 1 つずつ試す:

1. `encode` だけ（`pix-ts-legacy | ffmpeg -c:v h264_v4l2m2m -f mpegts /dev/null`）
2. `fragment` だけ
3. `stb_streamer` だけ

**VPU（`h264_v4l2m2m`）が最も疑わしい。** 2代目では動いていたが、3代目は
同じ SoC でも個体差があるかもしれない。

# 🎯 根本原因を特定（round 156）: VPU（h264_v4l2m2m）がシステム全体をハングさせる

### 決定的な実験

稼働中の Linux（`PIX_TUNER=none`、SSH・1Gbps・DT295 認識すべて正常）で:

**1. `pix-ts-legacy` 単体**

```sh
pix-ts-legacy --model dt295 -s terrestrial -c 17 -a /usr/share/pix-tuner -t 15
```

```
"locked":1, "cn_raw":1797
"tables_ready":1, "program_count":3
"packets":31809, "confirmed_bytes":5959668
"have_pat":1
```

**6MB のデータを読み出し、TS テーブルも取得。SSH/ping は生きている。**
→ **DT295 からの受信は完全に正常。**

**2. VPU エンコード（`h264_v4l2m2m`）**

```sh
ffmpeg -f mpegts -i /tmp/t.ts -vf "bwdif=mode=send_frame,format=nv12" \
    -c:v h264_v4l2m2m -b:v 2000k -f mpegts /tmp/o2.ts
```

→ **即座にハング**

```
SSH DEAD
ping NG
```

### 確定した原因

**`h264_v4l2m2m`（Amlogic VPU ハードウェアエンコーダ）を開始した瞬間に、
システム全体がハングする。**

典型的な症状: エンコード開始 → 数秒以内に Ethernet・UART・シェルが停止。

### 考えられる理由

1. **VPU の DMA がメモリバスを占有**し、Ethernet の DMA が飢餓状態になる
2. **VPU ドライバ（`meson-vdec` / `hantro` / `v4l2-mem2mem`）の初期化不良**。
   このカーネル（6.18.62）で Amlogic G12A/SM1 の VPU サポートが不完全
3. **`amlogic-vpu` のクロック/リセット**が他の周辺と共有されており、
   有効化時に衝突

### 2代目で動いていた理由

2代目（`g12a_u211_4g`）は同じ SoC だが、**録画が実際に動いていた記録がある**
（`restart-recipe-persist-result.log` に DT295 で 555 万パケット）。
つまり VPU エンコードも動いていた。

**3代目だけが落ちる**のは:
- 個体差（VPU のリビジョン）
- カーネルのバージョン差（2代目は別のカーネルで動いていた可能性）
- 電源容量（VPU 起動時の消費電流増加で給電が不安定）

### 回避策

**ハードウェアエンコードを使わない。** ソフトウェアエンコードなら
VPU を触らないので、ハングしないはず:

```sh
-c:v libx264   # または -c:v mpeg2video でパススルー
```

ただし CPU 負荷が高い。3代目の CPU（4 コア A55）で実時間を保てるかは要検証。

### 次の一手

1. **`-c:v libx264` で試す**（ソフトウェアエンコード）
2. 実時間を保てれば、これで録画パイプラインが完成する
3. 保てないなら、VPU をデバッグ（`dmesg` を SD に逃がしてハング直前を見る）
