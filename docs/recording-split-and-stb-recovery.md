> **Status: 旧（SUPERSEDED）** — 本文 §1「この切り直しは … STB 側の周期は維持する」の1800秒分割を維持する記述は、後続の [docs/unit2-stage1-deployment.md](unit2-stage1-deployment.md) と [docs/dt295-external-descramble-design.md](dt295-external-descramble-design.md) で廃止済みとされている（2代目は `PIX_CAPTURE_SECONDS=-`、`-t -` で切り直し0回を実証）。SSH 経由の STB 復旧やソース管理の経緯は記録として残るが、設定値は現行手順を参照。以下は当時の本文を保持する。

# 録画の細切れ分割とテレビ静岡の停止（2026-11-19/11-20）

同一番組が複数のフォルダに分かれる問題と、11/20 にテレビ静岡（STB 192.168.99.109）の
録画が丸一日欠けた問題の調査結果と修正。

## 1. 番組が細切れに分割されていた

### 症状

11/20 の BS11「温泉若おかみの殺人推理　１０」（event=36733、10:00–12:00、7200 秒）が
4 フォルダに分かれていた。11/19 の番組も同様に 2〜4 分割されていた。

| 開始 | 終了 | 長さ |
| --- | --- | --- |
| 10:00:01 | 10:26:14 | 1573.3s |
| 10:26:29 | 10:56:29 | 1799.7s |
| 10:56:50 | 11:26:45 | 1795.3s |
| 11:27:01 | 11:56:59 | 1798.3s |
| 合計 | | 6966.6s（番組全体 7200 秒） |

### 原因

STB は `PIX_CAPTURE_SECONDS`（既定 1800 秒）ごとにキャプチャを意図的に切り直す。

```
03:45:35 simple: pipeline stopped after 1803s; restarting
04:15:49 simple: pipeline stopped after 1804s; restarting
04:46:03 simple: pipeline stopped after 1804s; restarting
```

NAS 側のレコーダーは、読む側の接続が切れて繋ぎ直されるたびに新しい Segment を開き、
**毎回新しい録画フォルダを作っていた**。STB は番組境界（`pixb`）を正しく 1 回しか
送っていないのに（`boundary: service=211 event=36733` は 10:00:03 の 1 回だけ）、
再接続のたびにフォルダが増えていた。

この切り直しは DT295 のカード交換が止まる問題を避けるための技術的措置なので、
STB 側の周期は維持する。

### 修正

`pix_stream_recorder.c` の `publish()` で、保存先を決める前に**同じ日の同じ番組が
すでにあるか**を調べ、あればそのフォルダを引き継いでパート番号だけ進める。

- 一致の判定は `program.eventId`（と `program.startAt`）。JSON は整形出力なので
  文字列一致ではなく cJSON で読む。
- 2 つ目のパート以降は `recordingId` も先頭パートから引き継ぎ、同じ録画として扱う。
  `partId` はパートごとに新しく振る。
- フォルダ名は先頭パートの時刻と名前を保つ。ファイル名は各パートの時刻を使う。

```
continuing programme as part 2: .../144500_テレショップ__85895fdc-.../
```

実データでの確認（同一番組を 2 回録画）:

```
p01 rid=8313dcf7 part=7caf35fa ev=1671 dur=164.0
p02 rid=8313dcf7 part=85fe9269 ev=1671 dur=175.9
```

### 実装で踏んだ落とし穴

`recordingId` を先頭パートから引き継ぐ処理を足したとき、探索関数の入力と出力に
同じ変数を渡してしまい、未定義動作で `p02` が保存されなかった（ログには
`continuing programme as part 2` が出るのに、ファイルも CM ジョブも作られない）。
引数を分け、`build_json` に `recordingId` を明示的に渡す形にして解消。

修正後の実データ確認（同一番組を 2 回録画）:

```
p01 rid=747d1606 part=3e61f3ed ev=1672
p02 rid=747d1606 part=ae9ff9d9 ev=1672
（同じフォルダに .mp4 と .json がそろって存在）
```

### 注意

NAS 上の `pix-stream-20261101/pix_stream_recorder.c` は 11/8 の古いコピーで、実行中の
バイナリ（11/17）と一致しない（`build_json`、`seeded from STB`、`programme window
ended` が無い）。**正規ソースは `work/build/pix_stream_recorder.c`**。ここを直して
ビルドすること。

ビルド（NAS 上）:

```
gcc -O2 -Wall -I/usr/include/cjson -o pix_stream_recorder \
    pix_stream_recorder.c fmp4_to_mp4.c cm_queue.c -lcjson -lpthread
```

`libcjson-dev` が未導入だったため導入した（`/usr/include/cjson/cJSON.h`）。

## 2. 11/20 にテレビ静岡の録画が無い

### 症状

11/19 12:18 の「ぽかぽか」を最後に、11/20 13:39 まで約 25 時間録画が止まっていた。

### 原因

1. STB 109 の配信監督プロセス（S97）が止まり、`siano-ts` も `stb_streamer` も
   動いていない状態だった。`/var/run/pix-stream.pid` だけが古い値を残していた。
2. `pix-stb-guard` は 25 時間この状態を検知していたが、**復旧手段が adb だけ**で、
   Linux STB には adb が無いため毎回 `adb not usable` で失敗していた。
3. チューナー自体は健全だった（`siano-ts --list` は `status=ready`、実受信で
   `ISDB-T lock acquired`）。再開できなかったのは監督プロセスを起こす手段が
   無かったため。

### 修正

- ただちに `/etc/init.d/S97pix-stream start` で復旧。テレビ静岡の録画が再開した。
- `pix-stb-guard.py` に **SSH 経由の復旧**（`restart_stream`）を追加した。
  - NAS → STB は `~/.ssh/id_ed25519_guard` で鍵認証する。
  - `S97pix-stream start` を実行し、`/v1/status` が返るまで待つ。
  - カーネルと USB チューナーの状態、カード中継を壊さないので、adb での再起動より先に試す。
  - 対象は `SSH_HOSTS`（Linux STB）。
- 実測: 停止した 109 を **15.3 秒**で復旧。電源 OFF の STB 56 では 3.1 秒で安全に
  失敗し、誤って再起動しないことを確認した。

## 3. 残っている作業

- STB は RAM rootfs なので、`authorized_keys` はイメージに焼き込む必要がある。
  `board/x96max/overlay/root/.ssh/authorized_keys` に guard の公開鍵を追加済み。
  焼き直すまでは各 STB で手動追加した状態が再起動で消える。
- 11/19・11/20 に分割済みの既存フォルダの統合はまだ。
- STB 192.168.99.56（3代目）と 192.168.99.209 は電源/死活の問題で別件。

## 4. ソース管理の整理（2026-11-20）

### 見つかった問題

NAS の `/home/ku/pix-stream-20261101/*.c` は、実行中のバイナリのソースでは
なかった。どのファイルも数版前のスナップショットで、`pix_stream_manager.c` は
11/3 版（バイナリは 11/14）、`pix_stream_recorder.c` は 11/8 版（バイナリは 11/17）。

真のソースはリポジトリ側に散っていた。

- `X96MaxLinuxTSEnc/work/build/` — 実行中の recorder に対応（ただし `work/` は
  gitignore されていて管理外）
- `dtv_android/nas_src/` — 別系統。`cm_queue.c` の `features` 引数などがあり、
  現在の recorder と組み合わせられない

これが「古いソースを直してビルドすると機能が消える」罠の原因だった。

### 整理後

`X96MaxLinuxTSEnc/nas/` を唯一の正とした。

```
nas/
  README.md     正規ソースと手順
  build.sh      ビルド（--apply で配備と再起動）
  src/          pix_stream_recorder.c, pix_stream_manager.c, pix-stb-guard.py,
                fmp4_to_mp4.c/.h, cm_queue.c/.h, feature_sidecar.c/.h
```

- ビルドしたバイナリが実行中のものと機能一致することを文字列で照合済み
  （双方とも「実行中にあって新ビルドに無い」項目ゼロ）。
- NAS の古いソースは `stale-sources/` へ移し、参照専用である旨の README を置いた。
- `dtv_android/nas_src/README.md` に、正規ソースは `nas/src/` であることと、
  未統合の差分（`features` 引数、`feature_sidecar`）を記録した。ファイルは残す。

`--apply` は manager を再起動するため書きかけの区間が切れるが、レコーダーは同じ
番組を次のパートとして同じフォルダに続けるので、番組は分かれない。

## 5. STB イメージへの鍵の焼き込み

### 見つかった問題

`board/x96max/overlay/root/.ssh/authorized_keys` に guard の公開鍵を足しても
イメージに入らなかった。`ramboot-finalize.sh` が
`/identity/root/.ssh/authorized_keys`（ユニット別の identity ディレクトリ）で
overlay を上書きするため、鍵はそちらに足す必要があった。

### 対応

`work/ramboot-identity-unit1/` と `work/ramboot-identity-unit3/` の
`root/.ssh/authorized_keys` に guard の公開鍵を追加し、イメージを再生成した。
両ユニットの `rootfs.cpio.gz` に 2 本（運用者の鍵と guard の鍵）が入ることを確認。

### 注意

`work/ramboot-identity*/` は gitignore されている（unit ごとの秘密情報を持つため）。
鍵を足す先を忘れると、焼き直したイメージから SSH 復旧が効かなくなる。
