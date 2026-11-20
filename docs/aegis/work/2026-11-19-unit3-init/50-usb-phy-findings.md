
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
