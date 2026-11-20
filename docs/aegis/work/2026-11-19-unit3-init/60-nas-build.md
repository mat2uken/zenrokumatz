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
