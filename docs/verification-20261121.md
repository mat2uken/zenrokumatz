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

### 公開の仕組みが分かった（当初「止まっている」と誤認した）

`zenroku/recordings/.media` を見て「19:51 以降、公開が止まっている」と
書いたが、**これは誤りだった**。録画は `.media` ではなく
**CM 処理キュー（`/dev/shm/pix_cm_queue`）** に渡り、`cm_service.py` が
処理してから番組フォルダへ書かれる。

`journalctl _PID=<recorder>` で実際の流れが読めた:

```
00:45:03  boundary: service=101 event=19083 start=1795189500000 dur=1500000
00:45:06  cm fmp4 queued: producer=pix-stream-recorder-cm-fmp4-v2 pid=1138491
          input=/dev/shm/pix_stream/staging_1138491_1795187300611.fmp4
```

完了ジョブ（`/dev/shm/pix_cm_queue/completed/fd884d45-...json`）に
保存先が書かれている。

**実際に保存されたファイル:**

```
/mnt/xblk/video/zenroku/recordings/2026/11/21/
  BS_400101_ＮＨＫ　ＢＳ/
    000820_ヘチ　王座への道（１７）__fd884d45-…/
      20261121_000820_ヘチ　王座への道（１７）__p01.mp4   1.80 GB
       （同名の .json も。service / program / recordingId / partId 入り）
```

局名・番組名・日時が正しく付いている。**2代目の CFW 経路で録画が
完成している。**

### （当初の誤認の記録）`.media` だけ見て判断した

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

## 録画の継続確認（2026-11-21 02:06）

CFW 自動インストール（`8c0a20e`）を入れてから 1 時間以上経った時点での確認。

### 両機とも録画中

```
unit2 (123)  capturing=true  readers=1  totalPushed=1985  dropped=0
unit1 (109)  capturing=true  readers=1  totalPushed=3791  dropped=0
```

### CFW 経路が番組をまたいで 2 本録画した

| 公開時刻 | 番組 | サイズ | 経路 |
|---|---|---|---|
| 00:52 | ヘチ　王座への道（１７） | 1.80 GB | unit2 (CFW) |
| 01:00 | 大相撲（２０２６年）…九州場所　十三日目 | 694 MB | unit2 (CFW) |
| 00:58 | ＦＮＮ　Ｌｉｖｅ　Ｎｅｗｓ　α | 2.28 GB | unit1 |
| 01:59 | 救命病棟２４時＃６ | 2.97 GB | unit1 |
| 02:03 | ディノスＴＨＥストア | 741 MB | unit1 |

大相撲の録画は `ftyp` 先頭・`moov` が前方（faststart）で、メタデータの
番組名・開始・長さが EPG どおり。**境界検出 → セグメント確定 → CM キュー →
番組フォルダ公開**の1循環が CFW 経路で完結した。

### 現在も進行中

```
2代目 staging  2.91 GB -> 2.92 GB  (+10.6 MB / 12 秒 ≒ 7.1 Mbps)
1代目 staging    430 MB ->   435 MB  (+5.1 MB / 6 秒)
2代目の境界    boundary: service=101 event=22301 (運転席からの風景　小田急小田原線)
切り直し       0 回
```

### 現在の番組で端末末尾を確認

配信口から 6 秒取得（7.6 MB）し、映像 5 フレーム・音声 2 秒のデコードが
ともに終了コード 0。

### 補足

`…ＦＮＮ…__p01_CMカット.mp4` が 62 バイトに見えたのは**シンボリックリンク**
だから（実体は `.media/` の 2.28 GB MP4）。異常ではない。
