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
