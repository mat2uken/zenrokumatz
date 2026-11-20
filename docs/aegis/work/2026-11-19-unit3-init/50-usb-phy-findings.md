
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
