# NAS でのビルド（Mac のメモリ不足を回避）

## なぜ NAS か

Mac（36GB）は Colima VM に 10GB を割り当てているが、Chrome・Defender・
Spotlight などで空きが 63MB まで逼迫し、`exit=137`（OOM）でビルドが
3 回以上失敗した。さらに Colima の VM 自体が起動できなくなった
（`signal: killed`）。

NAS（`ku@192.168.99.80`）は:

```
メモリ: 125 GB（空き 102 GB）
CPU:    28 コア
ディスク: 457 GB（空き 359 GB）
Docker: 29.1.3、sudo 不要
```

**桁違いの余裕がある。**

## 構築手順

### 1. イメージを NAS で作り直す（重要）

Mac の `x96max-builder:bookworm` は **linux/arm64**。NAS は **x86_64** なので
そのままでは `exec format error` になる。NAS でネイティブにビルドし直す:

```sh
scp scripts/Dockerfile ku@192.168.99.80:~/x96build/Dockerfile
ssh ku@192.168.99.80 'cd ~/x96build && docker build --platform linux/amd64 \
    -t x96max-builder:bookworm -f Dockerfile .'
```

`debian:bookworm` が無ければ先に `docker pull debian:bookworm`。

### 2. プロジェクトを転送

```sh
tar --exclude=output --exclude=work --exclude=.git -czf /tmp/proj.tgz .
scp /tmp/proj.tgz ku@192.168.99.80:/tmp/
ssh ku@192.168.99.80 'rm -rf ~/x96 && mkdir -p ~/x96 && cd ~/x96 && tar xzf /tmp/proj.tgz'
```

4.3MB で 0.3 秒。

### 3. identity キーを転送（initramfs に埋め込むため）

```sh
scp -r work/ramboot-identity/* ku@192.168.99.80:~/x96/work/ramboot-identity/
```

### 4. ビルドボリュームの権限

**必須**。`/build` が root 所有のままだと buildroot のダウンロードが
`Permission denied` で失敗する（`exit=23`）:

```sh
docker run --rm --user root -v x96max-nas-build:/build x96max-builder:bookworm \
    sh -c 'mkdir -p /build/cache; chown builder:builder /build /build/cache'
```

### 5. ビルド

```sh
ssh ku@192.168.99.80 'cd ~/x96 && docker run --rm \
    -v x96max-nas-build:/build \
    -v $HOME/x96:/project:ro \
    -v $HOME/x96/output/ramboot-unit3:/export \
    -v $HOME/x96/work/ramboot-identity:/identity \
    -e X96MAX_PROFILE=ramboot -e X96MAX_UNIT=3 -e JOBS=12 \
    x96max-builder:bookworm bash /project/scripts/build-in-container.sh'
```

## 成果物の取り出し

ビルド後、`~/x96/output/ramboot-unit3/` に成果物ができる。Mac へ:

```sh
scp ku@192.168.99.80:~/x96/output/ramboot-unit3/Image output/ramboot-unit3/
scp ku@192.168.99.80:~/x96/output/ramboot-unit3/rootfs.cpio.uboot output/ramboot-unit3/
scp ku@192.168.99.80:~/x96/output/ramboot-unit3/meson-g12a-x96-max-p-gxl.dtb output/ramboot-unit3/
```

## 注意

- 初回は buildroot のダウンロード + クロス GCC のビルドで時間がかかる
  （28 コア / JOBS=12 でも 30 分〜1 時間程度）
- 2 回目以降は `x96max-nas-build` ボリュームのキャッシュが効く
- **Mac 側の `output/` とは別物**。成果物は必ずコピーしてくる

## 実績（round 134）: NAS でフルビルド成功

### 結果

```sh
docker run --rm -v x96max-nas-build:/build -v $HOME/x96:/project:ro \
  -v $HOME/x96/output/ramboot-unit3:/export \
  -v $HOME/x96/work/ramboot-identity:/identity \
  -e X96MAX_PROFILE=ramboot -e X96MAX_UNIT=3 -e JOBS=12 \
  x96max-builder:bookworm bash /project/scripts/build-in-container.sh
```

`exit=0`、**`Image` 41,757,184 バイト生成**（`b8b68100`）。

### 途中で必要だったこと

1. **`/build` の chown**（無いと `Permission denied` で `exit=23`）
2. **`work/sources/siano-userland` の転送**（`work/` を除外して転送したため
   `ERROR: /project/work/sources/siano-userland does not exist` で失敗）
3. **`work/ramboot-identity/dropbear_ed25519_host_key`**（initramfs に埋め込む）

### 所要時間

- 1 回目（クロス GCC + カーネル + 全パッケージ）: **約 50 分**
- 2 回目（キャッシュあり、siano-ts 追加後）: **数分**

### 注意

`board/x96max/ramboot-unit3.fragment` の `BR2_LINUX_KERNEL_INTREE_DTS_NAME`
に `gxl` が含まれていないため、`gxl.dtb` は NAS ビルドでは生成されない。
必要なら Mac 側のツリーで個別にビルドする（Mac の linux ツリーは残っている）。

### 判明したこと

**`ddr` クロックを有効化しても、`gxl` DTB では `USB_R1`（`0xffe09084`）が
書けない**（`regmap_mmio_read32le` で例外）。

`ephy-4g`（素の G12A）では `USB3 ports: 1` まで進むので、**`ephy-4g` と
新 Image の組み合わせ**を次に試す。

## 追記（round 135）: NAS ビルドに 0008 パッチが入っていることを確認

```
$ docker run --rm -v x96max-nas-build:/build alpine:3 \
    sh -c 'grep -n "num_clks = " .../dwc3-meson-g12a.c'

193:	.num_clks = ARRAY_SIZE(meson_gxl_clocks),   ← gxl（修正済み）
206:	.num_clks = ARRAY_SIZE(meson_gxl_clocks),   ← gxm（修正済み）
218:	.num_clks = ARRAY_SIZE(meson_gxl_clocks),   ← a1（元から正しい）
230:	.num_clks = ARRAY_SIZE(meson_g12a_clocks),  ← g12a（元から正しい）
241:	.num_clks = ARRAY_SIZE(meson_a1_clocks),    ← gsx（元から正しい）
```

**`num_clks` の修正は正しく適用されている。**

### `ddr` クロックの正体（参考）

`CLKID_USB1_DDR_BRIDGE` = `g12a_usb1_to_ddr`:

**USB1 → DDR ブリッジのクロック**。USB の DMA がメモリへ転送するためのもので、
**レジスタウィンドウ（`0xffe09000` 帯）の応答とは直接関係しない可能性が高い**。

ただし、有効化されていないクロックを有効にすること自体は無害なので、
検証する価値はある。

### 次の検証

1. **`ephy-4g` + 新 Image**（`ddr` 修正込み）→ `USB_R1` が通るか
2. もし通らなければ、`ddr` は無関係と判断し別の要因を探す

### 現在の成果物

| ファイル | SHA-256（先頭） | 内容 |
|---|---|---|
| `Image` | `b8b68100` | 0001+0002+0008（`ddr` 修正込み） |
| `rootfs.cpio.uboot` | `fde2833a` | チューナー待機＋CM 特徴量 |
| `gxl.dtb` | `d60f6c97` | `usb_ctrl` + `ddr` の 2 クロック |
| `ephy-4g.dtb` | `b031dc16` | 素の G12A（1代目・2代目と同じ） |

## 追記（round 138）: 起動後のネットワーク不通は要調査

### 観察

`usb start` を付けて起動したところ、Linux は例外なくユーザー空間まで到達した:

```
Starting dropbear sshd: OK
B-CAS: DT295 internal card (vendor firmware descrambles)
Starting pix-stream: OK
x96max-p3 login:
```

しかし **SSH・ping・シリアルすべてが応答しない**。

### 確認したこと

| 項目 | 結果 |
|---|---|
| `meson8b-dwmac ff3f0000` の初期化 | **されている**（`IRQ eth_wake_irq not found` の警告あり） |
| `/etc/network/interfaces` | DHCP 設定あり（`auto eth0`） |
| `S40network` | **rootfs に存在する**（Buildroot 標準） |
| 起動ログの `eth0` 設定 | **見えない**（シリアルログの欠落の可能性） |

### 以前との比較

`-nousb` DTB で起動したとき（round 105）は **SSH が通った**:

```
$ ssh root@192.168.99.56 'uname -a'
Linux x96max-p3 6.18.62 ...
$ ip -4 addr show eth0
inet 192.168.99.56/24
```

**同じ DTB 系列（`-nousb` は `ephy` から派生）で、当時は通っていた。**

### 考えられる原因

1. **シリアルコンソールが停止している**（Linux は動いているがコンソールと
   ネットワークの両方が死んでいる）
2. **`pix-stream` の起動が USB をリセットし、その影響で何かが止まった**
3. `panic_on_oops` が入る前の Image だったので、別の oops が起きて
   kworker が死に、ネットワークスタックも止まった

### 3 が有力

`usb start` で glue が初期化された後、Linux の `pix-usb-audit` や
`pix-stream` が USB を触って再度 oops を起こした可能性。ログには
`pix-stream: OK` と出ているが、その後の動作で落ちたかもしれない。

### 次の確認

電源投入後、`usb start` 付きで起動し:

1. 起動直後に ping / SSH を試す（時間をおいて再試行）
2. シリアルで `login:` が出ているか確認
3. 通れば DT295 の認識と録画を確認

**`panic_on_oops=1 panic=8` が入った Image なら、oops 時に自動再起動するので
「固まる」状態を避けられる。**

## 追記（round 143）: 起動時ネットワーク診断を追加

### 背景

`usb start` を付けて起動すると Linux は例外なく立ち上がるが、**SSH・ping・
シリアルすべてが無応答**になる（round 137）。シリアルも落ちるため、原因を
調べる手段が無かった。

### 実装

**`/usr/bin/netdiag`** — ネットワーク状態を `/dev/shm/netdiag.txt` に記録:

```sh
ip -4 addr show
ip link show eth0
cat /sys/class/net/eth0/carrier
cat /sys/class/net/eth0/operstate
dmesg | grep -i "dwmac\|eth0\|PHY" | tail -8
```

**`/etc/init.d/S51netdiag`** — `S40network` の後に 20 秒待って実行。
`S51` は Buildroot の `S40network` より後なので、DHCP の結果が反映される。

### 使い方（起動後）

シリアルでログインできれば:

```sh
cat /dev/shm/netdiag.txt
```

ネットワークが死んでいても、コンソールから原因を読める。

### 成果物

`rootfs.cpio.uboot` = `8f190905`（`netdiag`、`S51netdiag`、
`wait_for_tuner` を含むことを cpio 展開で確認済み）。

### テスト

`NetdiagScript` に 3 件追加。**272 テスト通過。**

## 追記（round 144）: netdiag は動いたが、シリアルもネットワークも死んでいる

### 起動結果（`usb start` + `ephy-4g` + netdiag 入り rootfs）

```
[   24.844876] In-situ OAM (IOAM) with IPv6
OK
Starting netdiag: OK          ← 診断スクリプトは実行された
B-CAS: DT295 internal card (vendor firmware descrambles)
Starting pix-stream: OK
Starting pix-logo-weekly: OK

X96Max_P3 RAM-root Linux
x96max-p3 login:
```

**例外なしでユーザー空間まで起動。** `netdiag` も実行された。

### しかし

| 確認 | 結果 |
|---|---|
| シリアル入力（`\r`、`root`） | **無応答**（0 バイト） |
| ping | **不通** |
| SSH (22) | **不通** |
| pix-stream (40774) | **不通** |

**起動ログは取れているのに、起動後にシリアル入力もネットワークも死んでいる。**

### 重要な矛盾

シリアル出力（起動ログ）は流れたのに、**入力が届かない**。これは:

1. Linux 起動後にコンソールが停止した（`pix-stream` が何かを壊した）
2. カーネルは動いているが userspace の一部（getty、dhcpcd）が動いていない
3. 電源/ハードの問題（DT295 の消費電力で給電が不安定など）

### 次の切り分け

**`pix-stream` を無効化**して起動する。それでシリアルとネットワークが
復活すれば、`pix-stream`（USB を触る処理）が原因と確定する。

方法: `S97pix-stream` を `S97pix-stream.disabled` にリネームした rootfs を作る。
または `PIX_TUNER` を無効値にして起動。

**`/etc/default/pix-stream-linux` の `PIX_TUNER` を空にすれば、
pix-stream は起動するがチューナーを探さず待機するだけになる**（実装済みの
`wait_for_tuner`）。これなら SD の書き換えだけで試せる。

### 別の可能性

`panic_on_oops=1` を入れたので、oops が起きれば panic → 8 秒で再起動する。
**再起動を繰り返しているなら、シリアルログに panic の痕跡が残る**。
今回のログには `panic` の文字が無いので、oops ではない。

## 追記（round 145）: 切り分け用の「USB を触らない rootfs」

### 目的

`usb start` 付きで Linux は起動するが、シリアル入力もネットワークも
死んでいる（round 144）。`pix-stream` が USB を触る処理で何かを壊した
可能性を切り分ける。

### 方法の比較

| 案 | 変更 | リスク |
|---|---|---|
| `S97pix-stream` を削除 | overlay を一時変更 → リビルド | `pix-logo` などが失敗する可能性 |
| **`PIX_TUNER` を無効値に** | **設定 1 行** | **`wait_for_tuner` が待機するだけ。USB を触らない** |
| `PIX_DECRYPT=1`（存在しない値） | 設定 1 行 | `wait_for_tuner` はスキップされるが `stb_streamer` が動く |

**`PIX_TUNER` 案が最小で安全。** `wait_for_tuner` を round 77 で実装済みで、
チューナーが見つからなければ**無期限に待機**する（3 回失敗して
`dt295_recover` → `pix_usb_reset` に至る経路を避ける設計）。

```sh
PIX_TUNER=none
```

これで:
- `pix-stream` は起動する
- `wait_for_tuner` が `06b8:103b` を探し続ける（見つからない）
- **USB を一切触らない**
- `dt295_recover` も走らない

### 判定

| 結果 | 意味 |
|---|---|
| シリアルとネットワークが**復活** | `pix-stream` の USB 操作が原因 |
| それでも**死んでいる** | 別の原因（Ethernet ドライバ、電源など） |

### 手順

1. `board/x96max/ramboot-unit3-overlay/etc/default/pix-stream-linux` の
   `PIX_TUNER=dt295` を `PIX_TUNER=none` に変更
2. NAS でビルド（数分）
3. SD へ書き込み（Android か Linux 経由）
4. 起動して確認
5. **確認後、`PIX_TUNER=dt295` に戻す**

## 追記（round 146）: `PIX_TUNER=none` の切り分け用 rootfs を SD へ書き込み済み

### 実施

1. `board/x96max/ramboot-unit3-overlay/etc/default/pix-stream-linux` の
   `PIX_TUNER=dt295` → `PIX_TUNER=none` に変更
2. NAS でビルド（`rootfs.cpio.uboot` = 35,197,351 バイト）
3. cpio 展開で `PIX_TUNER=none` が入っていることを確認
4. SD へ `--update-boot --only rootfs.cpio.uboot` で書き込み、検証OK
5. **設定を `PIX_TUNER=dt295` に戻した**（作業ツリーはクリーン）

### この rootfs の意味

`wait_for_tuner` が `06b8:103b` を探し続ける（見つからない）。
**USB を一切触らず、`dt295_recover` → `pix_usb_reset` も走らない。**

### 期待される結果（起動後）

| 観察 | 意味 |
|---|---|
| シリアル入力が効く / ping が通る | **`pix-stream` の USB 操作が原因** |
| それでも死んでいる | 別の原因（Ethernet、電源、カーネル） |

### 注意

**この SD は切り分け用**。原因が分かったら、`PIX_TUNER=dt295` の
rootfs（`8f190905`）に戻す必要がある。

元の rootfs は Mac の `output/ramboot-unit3/` に残っていないので、
戻すときは NAS で再ビルドする（設定は既に元通りなので、そのままビルドでよい）。

## 追記（round 147）: netdiag が SD へも書き出すようにした

### 問題

`usb start` 付きで起動すると、**シリアル入力もネットワークも死ぬ**。
`/dev/shm/netdiag.txt` に記録しても、**シリアルが効かないと読めない**。
RAM 上なので電源を切ると消える。

### 解決

`netdiag` が **SD の FAT 区画にもコピー**するようにした:

```sh
for dev in /dev/mmcblk1p1 /dev/mmcblk0p1; do
    mount -t vfat "$dev" /mnt/netdiag && \
        cp /dev/shm/netdiag.txt /mnt/netdiag/netdiag.txt && sync && \
        umount /mnt/netdiag && break
done
```

**SD を Mac に挿せば `netdiag.txt` が読める。** シリアルもネットワークも
死んでいても原因が分かる。

### 記録内容

```
=== netdiag <date> ===
ip -4 addr show
ip link show eth0
carrier: <0|1>
operstate: <up|down|unknown>
--- dmesg ---
dwmac / eth0 / PHY / usb 関連の末尾 12 行
--- ps ---
プロセス一覧の先頭 20 行
```

`ps` を入れたのが重要。**getty や dhcpcd が動いているか**が分かる。

### ビルド済み

`rootfs.cpio.uboot` = `7549f99b`（`PIX_TUNER=none` + SD 書き出し版 netdiag）。

### 未実施

SD への書き込み。**SD が Mac に接続されていない**（機体の中）。
接続されれば `--update-boot --only rootfs.cpio.uboot` で書き込む。

## 追記（round 148）: netdiag にプロセスとログを追加

### 追加した記録

```
--- ps ---
（プロセス一覧 20 行）

--- pix-stream log ---
tail -15 /tmp/pix-simple.log

--- tuner log ---
tail -5 /tmp/pix-ts.log

--- usb devices ---
lsusb / /sys/bus/usb/devices
```

**`ps` で getty や dhcpcd が動いているか**、**pix-stream のログで
「どこまで進んだか」**、**lsusb で DT295 が見えているか**が SD から分かる。

### ビルド済み

`rootfs.cpio.uboot` = `35567de0`（`PIX_TUNER=none` + SD 書き出し +
プロセス/ログ記録）。

### シリアル入力が効かない件について（別仮説）

以前も同じ症状があり、**USB ケーブルの抜き差しで復帰した**（round 5 頃）。
機体側ではなく **CP2104 アダプタの送信が死んでいる**可能性がある。

症状:
- 起動ログ（出力）は流れる
- 入力（`\r`、`root`）が 0 バイトも届かない
- ネットワークも死んでいる（これは別問題）

**もしアダプタの問題なら、Mac 側の USB を挿し直せば復帰する。**
`netdiag` の SD 書き出しは、この切り分けにも使える。

## 追記（round 149）: SD マウントの競合リスクを検討し、boottrace は作らないことにした

### 検討したこと

起動の各段階で SD に breadcrumb を書く `S43boottrace` を実装しようとしたが、
**SD を起動中にマウントすると `pix-stream` と競合するリスク**があるため取り消した。

### `netdiag` の SD マウントは安全

`S51netdiag` は `S40network` の 20 秒後に**1 回だけ**実行し、即アンマウントする。
`S97pix-stream` より前なので競合しない。

```
S40network  →  S51netdiag（20秒後に1回、マウント→書込→即アンマウント）
            →  S97pix-stream
```

**タイミングが離れているので安全。**

### 現在の診断手段

| 手段 | 得られる情報 |
|---|---|
| シリアル出力 | 起動ログ（入力は効かない） |
| `/dev/shm/netdiag.txt` | ネットワーク状態（シリアルが効けば読める） |
| **SD の `netdiag.txt`** | **上記＋ps/pix-streamログ/lsusb** |
| `panic_on_oops=1 panic=8` | oops なら自動再起動 |

**SD 経由が唯一の確実な手段。**

## 追記（round 150）: SD の内容が古いまま（要更新）

### 現状の不一致

| ファイル | Mac の最新 | SD にあるもの |
|---|---|---|
| `Image` | `b8b68100` | **不明**（round 137 以降未更新の可能性） |
| `rootfs.cpio.uboot` | **`35567de0`**（SD書き出しnetdiag） | **`426c8ac2`**（古い） |
| `ephy-4g.dtb` | `b031dc16` | 不明 |

**SD の rootfs は `426c8ac2` で、最新の `35567de0` ではない。**
SD 書き出し機能が入っていない版なので、**今の SD では netdiag の結果を
SD から読めない**。

### 更新が必要な理由

`35567de0` は:
- `PIX_TUNER=none`（USB を触らない切り分け用）
- SD の FAT へ netdiag を書き出す
- ps / pix-stream ログ / lsusb を記録

**これが無いと、シリアルもネットワークも死んだ状態では何も分からない。**

### 手順（SD が Mac に接続されたら）

```sh
cd ~/Desktop/work/X96MaxLinuxTSEnc
cp output/ramboot-unit3/ramboot.img output/ramboot-unit3/sdcard.img
H=$(shasum -a 256 output/ramboot-unit3/ramboot.img | cut -d' ' -f1)
grep -vE "  (sdcard\.img|rootfs\.cpio\.gz)$" output/ramboot-unit3/SHA256SUMS > /tmp/s
echo "$H  sdcard.img" >> /tmp/s
cp /tmp/s output/ramboot-unit3/SHA256SUMS
/usr/bin/python3 scripts/write-sd.py --image output/ramboot-unit3/sdcard.img \
    --device /dev/diskN --update-boot
```

`--only` を付けない（Image・rootfs・DTB をすべて最新にする）。
