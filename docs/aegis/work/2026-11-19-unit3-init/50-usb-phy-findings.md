
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
