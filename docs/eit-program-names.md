# 放送波の EIT から番組名を取る

録画ファイル名を EPG サービスではなく、放送波に載っている EIT から決める仕組み。
2026-11-16 に、テレビ静岡の録画が `193125_テレビ静岡__<uuid>` のように局名だけの
名前になっていた問題への対応として入れた。

## なぜ必要だったか

NAS の録画側（`pix_stream_recorder`）は、番組名をアーカイブ EPG
（`pix-archive` → mirakc）から引いていた。mirakc が 11/10 に止まって以降は
テレビ静岡の EPG が 11/11 で切れており、名前を解決できない録画が局名へ
フォールバックしていた。外部サービスに依存しない経路を用意するのが目的。

EIT の番組名（`short_event_descriptor`、タグ 0x4D）は ARIB STD-B24 の 8 ビット
符号で、JIS X 0208 の漢字と 1 バイトの英数・かなを ESC で切り替える形式。
`eit_watch` の中で UTF-8 に変換している。

## 経路

```
siano-ts | torne-b25 | eit_watch --state /dev/shm/pix/program.txt | ffmpeg ... | stb_streamer
                          │                                                              │
                          │ service/event/start/duration/name                            │
                          └──────────────────────────────────────────────────────────────┘
                                                 │
                            pixb（番組境界）+ pixn（番組名）を断片の前に差し込む
                                                 │
                                                 ▼
                     pix_stream_recorder（NAS）が pixn の名前でファイルを作る
```

`/dev/shm/pix/program.txt` は次の形。`name=` が増えた。

```
service=35864
event=1550
start=1794839400000
duration=2700000
wall=1794837858576
name=ＦＮＮ　Ｌｉｖｅ　Ｎｅｗｓ　α【新コーナー社食で舌堤▼フィギュア中井亜美】
```

## 実装

| 場所 | 変更 |
| --- | --- |
| `package/stb-streamer/src/arib_text.c` | ARIB STD-B24 → UTF-8。漢字・英数・ひらがな・カタカナ、ESC の G0〜G3 指定と LS/SS、C0 の LS0/LS1。ARIB 独自の記号（85〜94 区）と DRCS は落とす |
| `package/stb-streamer/src/arib_table.h` | JIS X 0208 の対応表。`tools/gen-arib-table.py` が生成する |
| `package/stb-streamer/src/eit_watch.c` | `0x4D` から `event_name` を取り、`program.txt` とログに書く |
| `package/stb-streamer/src/stb_linux_pipe.c` | `pixb` の直後に `pixn`（size + "pixn" + UTF-8 の名前）を置く |
| `package/stb-streamer/src/stb_http.c` | `GET /v1/program`。録画側が接続途中から始めるときに現在の番組を引く |
| NAS `pix_stream_recorder.c` | `pixn` を読み、`pixb` の区切りで消す。名前は「EIT → アーカイブ EPG → 局名」の順。接続時に `/v1/program` で最初の断片を埋める。255 バイトを超える名前は文字境界で切る |

2 台目（PIX-DT295 / BS11）は `pix-reader` に `eit_watch` を挟んで同じ経路にした。

## 確認したこと

- 実放送 TS（`work/ts-samples/clear3.ts`、`px-s1ud-ch20-5s.ts`）で、EIT の名前が
  `【ドラマ１０】会社員（５）` のように復号できること。mirakc が付けた名前と一致する
- 1 台目（テレビ静岡）: `230545_ＦＮＮ　Ｌｉｖｅ　Ｎｅｗｓ　α【…】__<uuid>/…` が生成される
- 2 台目（BS11）: `230545_ロメリア戦記　＃７「椅子取りゲーム」__<uuid>/…` が生成される
- どちらの機体も、録画側の再接続直後に `seeded from STB:` で名前が入ること

## 過去の録画のリネーム

EIT の名前が残っていない録画（2026-11-10〜16）は、一度だけ Gガイドの過去番組表を
使って名前を付けた。`tools/rename-recordings.py` がその作業を行う。

```sh
python3 tools/rename-recordings.py --guide guide.json          # 計画
python3 tools/rename-recordings.py --guide guide.json --apply  # 実行
```

- ディレクトリ `<HHMMSS>_<番組名>__<uuid>` とファイル
  `<YYYYMMDD>_<HHMMSS>_<番組名>__p01.mp4` の両方を直す
- sidecar の `media.relativePath` と `program.name`、`.cm.json` の
  `original`/`output`、CM カットビューのディレクトリとシンボリックリンク、
  アーカイブ DB の `recordings.path` を同じ名前へ揃える
- UUID は残す。アーカイブ DB・CM sidecar・CM カットビューがこれを鍵にしている

355 件をリネームし、4 件は名前の元データが無いため残した（`SBS3`、サービス名が
`GR` の 1 件、11/16 02:48 と 04:49 のテレビ静岡＝Gガイドの欠測帯）。

## 注意

- `pixn` を知らない古い録画側は、この箱を mp4 に書いてしまう。先に NAS 側を
  更新してから STB 側を更新する
- `eit_watch` はバイト列を素通しするだけなので、映像の経路は変わらない
- 番組名は全角のまま入る。シェルで扱うときは引用する

## 録画側の CM 連携（2026-11-17 復旧）

`pix_stream_recorder` は NAS の `/home/ku/pix-stream-20261101/` でビルドしている。
このとき、置いてあった `pix_stream_recorder.c` が 11/08 版で、実際に動いていた
11/13 ビルド（CM キュー連携入り）より古いことに気づかず再ビルドしてしまい、
CM への fMP4 受け渡しが一時的に落ちた。11/13 のソースは残っていなかったため、
11/03 の CM 連携版（`/home/ku/work/dt295-nas-20261103/build/`）を参照して
次のとおり移植し直した。

- `cm_queue.c` / `cm_queue.h` を同じディレクトリへ置く
- `--cm-queue-dir`（既定 `/dev/shm/pix_cm_queue`）と tmpfs チェック
- `publish()` は fMP4 を `cm_queue_submit` でワーカーへ渡す。ワーカーが
  MP4 変換・CM 検出・カット・sidecar 作成を行う
- ステージングは最後の完全なフラグメント境界（`complete_bytes`）で切り詰める
- キューへ渡せなかったときだけ、従来どおり自前で MP4 化する

ビルド:

```sh
cd /home/ku/pix-stream-20261101
gcc -O2 -std=gnu11 -D_FILE_OFFSET_BITS=64 -I. -I/home/ku/work/dt295-nas-20261103/build \
    -o pix_stream_recorder pix_stream_recorder.c cm_queue.c fmp4_to_mp4.c \
    -l:libcjson.so.1 -lpthread
```

確認は `cm integration:` の起動ログ、`cm fmp4 queued:` の publish ログ、
`/dev/shm/pix_cm_queue/jobs.sqlite3` の `queue_jobs` で行う。
