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
# 1. プロジェクトを送る（.git と output は除く。work/ は機体 identity が
#    入っているので、上書き・削除しないため exclude する）
rsync -az --delete \
      --exclude .git --exclude output --exclude work --exclude nas/out \
      ./ ku@192.168.99.80:~/x96max/

# 2. ビルドイメージを作る（初回だけ）
ssh ku@192.168.99.80 'cd ~/x96max && sudo -n docker build -t x96max-builder:bookworm -f scripts/Dockerfile scripts'

# 3. unit をビルドする（identity は機体ごとに変える: unit2=ramboot-identity、
#    unit1=ramboot-identity-unit1、unit3=ramboot-identity-unit3）
ssh ku@192.168.99.80 'cd ~/x96max && sudo -n docker run --rm --user root \
  -v x96max-nas-build:/build -v "$PWD:/project:ro" -v "$PWD/output/ramboot-unit1:/export" \
  -v "$PWD/work/ramboot-identity-unit1:/identity" \
  -e X96MAX_PROFILE=ramboot -e X96MAX_UNIT=1 -e JOBS=20 -e FORCE_UNSAFE_CONFIGURE=1 \
  x96max-builder:bookworm bash /project/scripts/build-in-container.sh'

# 4. 成果物を Mac へ戻す
rsync -az ku@192.168.99.80:~/x96max/output/ramboot-unit1/ output/ramboot-unit1/
```

## 注意

- **`bison` と `flex` が無い。** コンテナの中で入れるので、ホスト側に
  入れる必要は無い（Dockerfile が `apt-get install` している）。
- **`FORCE_UNSAFE_CONFIGURE=1` が要る。** `--user root` で動かすと host-tar の
  configure が落ちる。根拠は「root では実行できない」のエラー（2026-11-22 実測）。
- **`site = local` のパッケージの再 rsync はスキップされる。**
  例は stb-streamer である。ビルドボリュームの
  `output/build/stb-streamer-local/` は
  初回 rsync のままで止まり、リポジトリのソースを直しても再コンパイルされない。
  ソースを変えたら、まず次のコマンドでパッケージの作業ディレクトリを消す。
  そのあとでビルドする。

  ```sh
  sudo -n docker run --rm -v x96max-nas-build:/v alpine \
    sh -c 'rm -rf /v/output/build/stb-streamer-local'
  ```

  2026-11-22 にこれを怠って、EPG 追加の入った rootfs に旧 eit_watch が入った
  （docs/verification-20261122.md §8）。焼く前に export の rootfs から
  `usr/bin/eit_watch` を取り出して strings で確認すると確実である。
- **identity は機体ごとに分ける。** unit3 は 11/19 の構築時に unit2 の identity
  が流用された。そのため SSH ホスト鍵が混線した
  （docs/verification-20261122.md §8）。
  `scripts/build-linux.sh` の対応表（unit3 → `ramboot-identity-unit3`）に従う。
- 2つの環境で作った `rootfs.cpio.uboot` は、タイムスタンプや一時ファイルの
  都合で**バイト単位では一致しない**ことがある。同じ機能かを確かめるには
  中身を展開して `usr/bin/bcasd` などの大きさと配置を見る。
- `FR30_TOOLCHAIN` を使う場合（CFW のファームを再ビルドするとき）は、
  `/build/fr30-elf` を NAS のボリュームにも作る必要がある。
  手順は [unit2-stage3-cfw.md](unit2-stage3-cfw.md) と同じ。

## 実績

- NAS: 2026-11-22 に unit1/unit2/unit3 の rootfs 更新ビルドを通して完了。
  カーネルは volume のキャッシュで流用し、rootfs と post-image だけが更新される
  （1 unit あたり数分）。unit2 は docs/verification-20261122.md §1 の再起動検証で
  実機に焼いて確認済み。
- Mac（Colima 10 GiB / JOBS=3）: exit 0 で完走。ただしカーネルを消して
  やり直すと OOM になりやすい。
