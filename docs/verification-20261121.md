# 確認の記録（2026-11-21 00:00–01:00）

前回の報告で「ご確認いただきたい」とした 3 点を、実機で確かめた。

## 1. NAS 側で実際に録画ファイルが作られているか → **作られている**

`ssh ku@192.168.99.80` で入れることが分かったので、直接確かめた。

### 録画は進行中

```
recorder  --stb-ip 192.168.99.123 ... --stream-id BS09_0   (00:08 起動)
staging_1138491_1795189503633.fmp4
  353,563,041 -> 367,237,701 bytes (+13.7 MB / 15 秒 ≒ 7.3 Mbps)
```

1代目（`--stb-ip 192.168.99.109`）も同じく書き込み中
（+3.3 MB / 5 秒）。**2台とも NAS の recorder が実データを受け取っている。**

### 番組境界も届いている

```
stb_linux_pipe: boundary service=101 event=17436 name=ヘチ　王座への道（１７）
stb_linux_pipe: boundary service=101 event=19083 name=大相撲（２０２６年）…十三日目
```

2代目は境界を正しく送出している。

### ただし公開（`.media` への移動）が 19:51 以降止まっている

`zenroku/recordings/.media` の最終公開は **2026-11-20 19:51**。以後、
完了したセグメントが staging に残ったままになっている。

```
/dev/shm/pix_stream/staging_1135242_1795186770305.fmp4  2,274,689,447  (1代目, 00:45 完了)
/dev/shm/pix_stream/staging_1138491_1795187300611.fmp4  1,797,922,273  (2代目, 00:45 完了)
```

**1代目も同じ状態**なので、これは私の CFW 変更が原因ではない。
recorder は 2 スレッドで、片方は STB からの受信で `recv` 待ち、
もう片方は futex 待ち。publish のワーカーが新しい仕事を拾っていない。

`/api/archive/recordings` は `[]`（公開されたものが無い）。
`/api/archive/programs` は 21,633 件を返すので、EPG 側は生きている。

**未解決**: recorder の publish ワーカーが動かない理由。`enqueue_job` は
`seg->have_moof` が真のときだけ積む。2代目の配信には `moof` が 4 個
入っていることは確認した（`ftyp` → `moov` → `moof` ×4）。したがって
`have_moof` は立つはずで、なぜ積まれないかは追えていない。

`/dev/shm` は 47 GB 空きなので、容量で止まっているわけではない。

## 2. ガードの扱い → **S97 の再起動では CFW は消えない**

ガード（`pix-stb-guard.py`）は `enabled` が真のノードが 180 秒応答しないと
`/etc/init.d/S97pix-stream start` を実行する。これを実機で再現した。

```
/etc/init.d/S97pix-stream stop  →  start
  ↓
IDENTIFY  status OK / signature 'FR01'      ← CFW は残っている
capturing: true / readers: 1                 ← 録画も継続
```

原因は `PIX_TUNER=dt295-cfw` のとき S97 が `pix-stream-simple` を選び、
その経路が `pix-cfw-ts`（CFW）を使うため。**純正の `pix-ts-legacy` は
呼ばれないので、ファームの hook は書き換わらない。**

したがってガードを止める必要は無い。`enabled=true` のままでよい。

（ただし `PIX_TUNER` を `dt295` に戻すと純正ファームが載って CFW が消える。
切り替えるときだけ注意する。）

## 3. 作業ツリーの他人の変更 → **unit 2 のビルドには影響しない**

未コミットで残っていたもの:

| 変更 | 誰のもの | unit 2 への影響 |
|---|---|---|
| `meson-g12a-x96-max-p-gxl.dts` の変更 | 3代目（GXL）の作業 | 無し（unit 2 は `-p` / `-p-ephy` / `-p-ephy-4g` のみ使う） |
| `patches/linux/0003〜0006` の削除 | 3代目の USB デバッグ | 無し（単位 3 向けの GXL パッチ） |
| `patches/linux/0008-...gxl-clock-count.patch` の追加 | 同上 | 無し |

確認として、この状態のツリーで unit 2 を**最初からビルドし直して exit 0**
（エラー 0）。成果物の中身も確認した:

```
usr/bin/bcasd                       33,864  （RESET 修正入り）
usr/bin/pix-cfw-ts                   4,646
usr/lib/pcsc/drivers/ifd-cfw-m2tv.bundle/Contents/Linux/libifd_m2tv.so  39,360
```

## ビルド環境について

Mac の Colima は 10 GiB しかなく、カーネルを作り直すと OOM（exit 137）に
なりやすい。NAS 側は 28 コア / 125 GiB / 358 GB 空きなので、重いビルドは
そちらへ移せる。手順は [build-on-nas.md](build-on-nas.md) に書いた
（Docker は x86_64 のままでよい。Buildroot はクロスコンパイルするので
QEMU は不要）。ただし **NAS でのビルド自体はまだ通していない**。
