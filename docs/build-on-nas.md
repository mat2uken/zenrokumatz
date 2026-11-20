# NAS（192.168.99.80）でビルドする

Mac の Colima は 10 GiB しかなく、カーネルを丸ごと作り直すと OOM で落ちる
（実測: exit 137。`docker run --name x96max-build` の衝突で 125 になることもある）。
NAS 側は **28 コア / 125 GiB / 358 GB 空き**なので、そちらで作る方が確実。

## 前提の確認（実測）

| 項目 | NAS の状態 |
|---|---|
| SSH | `ku@192.168.99.80` に鍵で入れる |
| CPU / RAM / 空き | 28 コア / 125 GiB / 358 GB |
| Docker | 29.1.3（`sudo -n docker` で動く） |
| gcc / make / cpio / rsync / python3 / git | 揃っている |
| bison / flex | **無い**（Buildroot が要る） |
| aarch64 クロス gcc | 無い（Buildroot が自分で作る） |
| QEMU の arm64 エミュレーション | **未登録**（`--platform linux/arm64` は exec format error） |

## 方法

`scripts/Dockerfile` は `FROM debian:bookworm` だけで、アーキテクチャを
指定していない。Buildroot は**クロスコンパイル**するので、コンテナ自体は
ホストの x86_64 のままでよい。QEMU は要らない。

```sh
# 1. プロジェクトを送る（.git と output は除く）
rsync -az --delete \
      --exclude .git --exclude output --exclude work --exclude nas/out \
      ./ ku@192.168.99.80:~/x96max/

# 2. ビルドイメージを作る（初回だけ）
ssh ku@192.168.99.80 'cd ~/x96max && sudo -n docker build -t x96max-builder:bookworm -f scripts/Dockerfile scripts'

# 3. unit 2 をビルドする
ssh ku@192.168.99.80 'cd ~/x96max && sudo -n docker run --rm --user root \
  -v x96max-build-nas:/build -v "$PWD:/project:ro" -v "$PWD/output/ramboot:/export" \
  -e X96MAX_PROFILE=ramboot -e X96MAX_UNIT=2 -e JOBS=20 \
  x96max-builder:bookworm bash /project/scripts/build-in-container.sh'

# 4. 成果物を Mac へ戻す
rsync -az ku@192.168.99.80:~/x96max/output/ramboot/ output/ramboot/
```

## 注意

- **`bison` と `flex` が無い。** コンテナの中で入れるので、ホスト側に
  入れる必要は無い（Dockerfile が `apt-get install` している）。
- `FR30_TOOLCHAIN` を使う場合（CFW のファームを再ビルドするとき）は、
  `/build/fr30-elf` を NAS のボリュームにも作る必要がある。
  手順は [unit2-stage3-cfw.md](unit2-stage3-cfw.md) と同じ。
- 2つの環境で作った `rootfs.cpio.uboot` は、タイムスタンプや一時ファイルの
  都合で**バイト単位では一致しない**ことがある。同じ機能かを確かめるには
  中身を展開して `usr/bin/bcasd` などの大きさと配置を見る。

## 実績

- Mac（Colima 10 GiB / JOBS=3）: exit 0 で完走。ただしカーネルを消して
  やり直すと OOM になりやすい。
- NAS: 本手順は**まだ通していない**（Docker と容量は確認済み、ビルドは未実施）。
