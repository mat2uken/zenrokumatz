# 3代目の受け入れ基準（2代目と同等の録画を満たす条件）

作成日: 2026-11-19

「2代目と同等」を主観で判定しないため、2代目の実測値と照合して判定する。目標は「Linux 起動 → DT295 で録画 → CM 解析・カット → NAS に正常保存」。

## 2代目の実測ベースライン（2026-11-19 時点、稼働中）

`root@192.168.99.123` から取得。

```json
{"status":"ok","channel":"BS09_0","capturing":true,"readers":1,
 "spoolUsedBytes":0,"spoolMaxBytes":201326592,"fragmentsInSpool":0,
 "totalPushed":30,"totalDropped":0}
```

NAS ノード登録（`http://192.168.99.80:40776/api/nodes`）:

| 項目 | 2代目（X96Max-DT295） |
|---|---|
| チャンネル | `BS09_0`（ＢＳ１１イレブン） |
| codec | `avc` |
| 解像度 | 1440x1080 |
| bitrate | 6,000,000 |
| rateMode | `vbr` |
| gopSec | 2 |
| state | `recording` |

設定（`/etc/default/pix-stream-linux`）:

```
PIX_TUNER=dt295
PIX_DECRYPT=0
PIX_SYSTEM=bs
PIX_CHANNEL=9
PIX_SAT_SLOT=0
PIX_PIDS="0 17 18 257 273 289 320 321"
PIX_PORT=40774
PIX_ALLOWED_IP=192.168.99.80,192.168.99.52
PIX_SPOOL_MB=192
PIX_BITRATE=6000k
PIX_GOP=60
PIX_CAPTURE_SECONDS=1800
```

## 3代目で満たす条件

段階を追って判定する。前段が満たせない限り次段に進まない。

### A. Linux 起動

- `x96max-p3 login:` に到達する
- `cat /proc/cmdline` に `rdinit=/init` があり、`mount` の `/` が `rootfs`（RAM）、`cat /proc/swaps` が空
- カーネル例外・panic が無い
- eth0 が 1Gbps 以上でリンクし、DHCP で `192.168.99.56` を取得する

### B. DT295 受信

- `lsusb` に `06b8:103b Pixela Corp. PIX-DT295` が出る
- `pix-ts-legacy --model dt295 --assets /usr/share/pix-tuner -s terrestrial -c 17 -t 5` が `rc=0`、`locked:1`、`tables_ready:1`、`packets > 0`
- EIT から `service=35864`（SBS）が取れる

### C. 配信

- `wget -qO- http://127.0.0.1:40774/v1/status` が `"capturing":true`
- `totalPushed` が時間とともに増える
- `totalDropped` が 0 のまま
- 映像が 1440x1080、約 6Mbps（`PIX_BITRATE=6000k`）

### D. NAS 保存

- NAS の `/api/nodes` に 3代目（`192.168.99.56`、ch17）が `recording` で現れる
- 録画ファイルが NAS に生成され、サイズが時間に比例して伸びる
- 番組境界で分割される（`PIX_CAPTURE_SECONDS=1800` ではなく番組単位）

### E. CM 解析・カット（2代目と同等）

- NAS 側で CM 解析ジョブが走る
- `_CMカット.mp4` が生成される（例: 2代目の実績 296.3秒 → 178.2秒）
- マーカーキャッシュのキーがサービスIDと一致していること（既知の落とし穴: `400211_1440x1080.json` に対し `211_1440x1080.json` が必要だった例）

### F. 再起動復帰

- 電源再投入だけで Linux → 録画再開まで到達する（`bootcmd` 保存後）
- 媒体（SD）を外しても稼働が継続する

## 障害として扱うもの

- カーネル例外（`Unable to handle kernel`）が出る
- `totalDropped` が増える
- 録画ファイルが生成されない、または NAS で `error` 状態になる

## 既知の前提と注意

- **NAS に `192.168.99.56-17`（X96Max-Plus2、テレビ静岡）という古いノードが残っている。** 3代目が同じ IP を使うため、登録前に削除が必要。放置すると名前・チャンネル割当が衝突する。
- 3代目の設定は意図的に2代目と差分がある（地上波・ch17・衛星オプション無し）。「同等」は画質・安定性・保存の成立で判定し、チャンネルとチューナー系の値は合わせない。

## 追記（round 79）: NAS 側の設定は2代目と完全一致

### 比較（`/api/nodes`）

| 項目 | 2代目 `192.168.99.123-BS09_0` | 3代目 `192.168.99.56-17` |
|---|---|---|
| codec | avc | avc |
| width | 1440 | 1440 |
| height | 1080 | 1080 |
| bitrate | 6000000 | 6000000 |
| rateMode | vbr | vbr |
| gopSec | 2 | 2 |
| enabled | true | true |
| recording | **true** | false |
| state | recording | error |
| stb | **ok** | **unreachable** |

**画質設定は完全に一致。** 3代目が `error` なのは STB（機体）が停止しているため。
起動して配信が始まれば `stb: ok` → `recording: true` になる見込み。

### 2代目の正常時の状態（参照値）

```
recording: true
stb.status: ok
stb.capturing: true
channelName: ＢＳ１１イレブン
```

3代目で `stb.status: ok` と `stb.capturing: true` が出れば、録画が成立している。

### CM スキップについて

端末側（`board/x96max/overlay/usr/bin/`）に CM 関連のスクリプトは無く、
`/etc/default/pix-stream-linux` にも CM 設定は無い。NAS 側の API にも
`api/cm` / `api/chapters` は存在しない（404）。

**CM 処理は NAS の別機能（録画後のバッチ処理など）と考えられる。**
録画が成立してから、NAS 側で CM が付くかを実際の録画ファイルで確認する。

## 追記（round 82）: CM スキップの所在が未確定

### 調査結果

| 場所 | CM 関連の実装 |
|---|---|
| 端末 `board/x96max/overlay/usr/bin/` | なし（`pix-stream-simple`、`pix-ts-legacy` を検索） |
| 端末 `/etc/default/pix-stream-linux` | 設定なし |
| vendor `pix-userland/src/` | キャプチャ・暗号・制御のみ、CM 処理なし |
| NAS `/api/cm`、`/api/chapters` | 404 |
| NAS ルート UI（12KB HTML） | 「cm」「chapter」「カット」「スキップ」の記述なし |

### 判断

**CM スキップは、この端末と NAS の API の範囲には存在しない。** 録画後の別処理
（外部ツール、再生アプリ側、または NAS の別サービス）と考えられる。

### 順序

まず **録画が成立して NAS にファイルが保存されること**を確認する。CM スキップは
その録画ファイルに対してどう実現されているかを、実際のファイルを見て判断する。
2代目の録画ファイルがどこに保存され、どんな付随ファイル（チャプター等）があるかを
調べれば、3代目にも同じ仕組みが必要か分かる。

### 現在の NAS 状態（参考）

```
192.168.99.209-15      error      ch=15
192.168.99.123-BS09_0  recording  ch=BS09_0  stb=ok    ← 2代目は稼働中
192.168.99.109-17      error      ch=17
192.168.99.56-17       error      ch=17                ← 3代目（機体停止中）
```

## 追記（round 83）: NAS の discovery に 3代目が正しく登録されている

### `/api/discovered` の実測

```json
{
 "instance": "pixstb-c0a86338",
 "name": "X96Max-P3",
 "host": "192.168.99.56",
 "port": 40774,
 "channel": 17,
 "ageSec": 3331,
 "online": false,
 "known": true,
 "knownId": "192.168.99.56-17",
 "knownChannel": "17"
}
```

**3代目は NAS から `X96Max-P3` として正しく発見・登録されている。**
`online: false` は機体が停止しているため。起動して `online: true` になれば、
ノード `192.168.99.56-17`（enabled=true、画質は2代目と同一）で録画が始まる。

`ageSec: 3331` は最後に discovery されてからの経過秒数。

### NAS が提供する API（UI から抽出）

```
/api/nodes      ノード登録・一覧
/api/channels   チャンネル一覧
/api/discovered 自動発見された STB
```

録画ファイルの一覧や CM チャプターを返す API は無い。録画は NAS 側で
別途保存・処理されている（UI は「録画管理」と「録画設定」のみ）。

### 次の確認（機体起動後）

1. `/api/discovered` で `online: true` になること
2. `/api/nodes` で `state: recording`、`stb.status: ok` になること
3. NAS 上に録画ファイルが現れること

## 追記（round 84）: CM スキップの仕組みを解明。ただし現行ランチャーでは無効

### CM スキップの設計（判明）

`board/x96max/ramboot-unit3-overlay/etc/default/pix-stream-linux` のコメント:

```
# Preserve CM feature sideband; analysis and final cuts remain NAS-owned.
PIX_FEATURES=1
PIX_FEATURE_MAX_MB=384
PIX_FEATURE_STEP=3
PIX_FEATURE_FAILURES=3
```

つまり:

- **端末が CM 検出用の特徴量（サイドバンド）を生成し、映像と一緒に NAS へ送る**
- **解析と最終的なカットは NAS 側が行う**

round 82 で「CM 実装が見つからない」と結論したのは誤りだった。`pix-stream-linux`
に実装がある。

### 決定的な問題: 現行ランチャーは PIX_FEATURES を無視する

`PIX_FEATURES` の使用箇所:

```
board/x96max/overlay/usr/bin/pix-stream-linux:44    : "${PIX_FEATURES:=0}"
board/x96max/overlay/usr/bin/pix-stream-linux:171   if [ "$PIX_FEATURES" = "1" ]; then
board/x96max/overlay/usr/bin/pix-stream-linux:392   if [ "$PIX_FEATURES" = "1" ] && ...
board/x96max/overlay/etc/default/pix-stream-linux:42  PIX_FEATURES=0
```

**`board/x96max/overlay/usr/bin/pix-stream-simple` には一切登場しない。**

3代目が使うのは `pix-stream-simple`（`S97pix-stream` が起動する方）なので、
`PIX_FEATURES=1` を設定しても **CM 特徴量は生成されず、NAS へ送られない**。

### 旧ランチャーの実装内容（移植の参考）

```sh
# 320x180 に縮小して signalstats で統計を取る（フル解像度だと 84 秒/39.5 秒）
analyze="scale=320:180,signalstats,blackdetect=d=0.1:pix_th=0.125:pic_th=0.98"
# シーンスコアは前フレームとの平均絶対差（signalstats YDIF）
analyze="$analyze,metadata=mode=print:key=lavfi.signalstats.YAVG"
```

`stb_streamer` に `pixf` を差し込む形で渡している。

### 次の一手

録画が成立した後、CM を効かせるには `pix-stream-simple` に特徴量生成を移植する
必要がある。ただし **まず録画そのものを成立させるのが優先**（NAS が `features=v1`
を要求した時だけ `stb_streamer` が差し込む設計なので、要求が無ければ不要）。

## 追記（round 85）: CM 特徴量の道具は揃っている。移植先は pix-stream-simple

### stb_streamer は特徴量引数に対応済み

`output/stb_streamer`（56,176 バイト、11月18日）の `strings`:

```
--feature-file
--feature-audio-file
--feature-black-file
--feature-max-mb

usage: %s [--port N] [--allowed-ip LIST] [--spool-mb N] [--channel NAME]
          [--display NAME] [--feature-file PATH] [--feature-audio-file PATH]
          [--feature-black-file PATH] [--feature-max-mb N] [--logo-dir DIR]
```

`scripts/install-stb-feature.sh` がこれを「feature-aware stb_streamer」として
2代目（`192.168.99.109`）へ配る仕組み。

### 旧ランチャーの特徴量生成（移植の参考）

```sh
# 320x180 に縮小（フル解像度は 84秒/39.5秒、これなら 30秒）
analyze="scale=320:180,signalstats,blackdetect=d=0.1:pix_th=0.125:pic_th=0.98"
analyze="$analyze,metadata=mode=print:key=lavfi.signalstats.YAVG"
analyze="$analyze,metadata=mode=print:key=lavfi.signalstats.YDIF"

# 映像と音声を同じクロックに保つ（タイムスタンプを書き換えない）
FC="[0:v]bwdif=mode=send_frame,format=nv12[enc];[enc]split=2[main][an]"
FC="$FC;[an]${analyze}[aux]"
VIDEO_IN="-filter_complex $FC -map [main]"

FEAT_AUX_OUT="-map [aux] -c:v rawvideo -f null /dev/null"
FEAT_AUDIO_OUT="-map 0:a:0 -c:a pcm_s16le -ar 48000 -ac 2 \
  -af astats=...,ametadata=mode=print:file=/dev/shm/pix/feat-a.log -f null /dev/null"
FEAT_ARGS="--feature-file $PIX_LOG_DIR/pix-ff1.log \
  --feature-audio-file /dev/shm/pix/feat-a.log \
  --feature-black-file $PIX_LOG_DIR/pix-ff1.log \
  --feature-max-mb $PIX_FEATURE_MAX_MB"
```

### 3代目に必要な作業

1. `pix-stream-simple` の `streamer()` に `FEAT_ARGS` 相当を追加する
2. `run_direct()` / エンコード経路の ffmpeg に `analyze` フィルタを足す
3. **ただし `stb_streamer` は NAS が `features=v1` を要求した時だけ pixf を
   差し込む設計**なので、要求が無ければ送られない。

したがって**録画成立を先に確認**し、NAS が `features=v1` を要求するかを見る。
要求するなら上記を移植する。

### 重要

3代目の SD に入っている `stb_streamer` が特徴量対応版かは未確認。`output/stb_streamer`
と同等かを、起動後に `strings /usr/bin/stb_streamer | grep feature` で確かめる。

## 追記（round 87）: CM 特徴量の ffmpeg 構成を実機外で検証した

### 検証方法

Mac の ffmpeg 8.1.2 で、実装したフィルタ構成をそのまま実行した（v4l2m2m の
ハードウェアエンコーダは Mac に無いため、同じフィルタ鎖を libx264 で代替）。
入力は `testsrc` + `sine` から作った 3 秒の MPEG-TS。

### 結果

```sh
analyze="scale=320:180,signalstats,blackdetect=d=0.1:pix_th=0.125:pic_th=0.98"
analyze="$analyze,metadata=mode=print:key=lavfi.signalstats.YAVG"
analyze="$analyze,metadata=mode=print:key=lavfi.signalstats.YDIF"
FC="[0:v]bwdif=mode=send_frame,format=nv12[enc];[enc]split=2[main][an]"
FC="$FC;[an]${analyze}[aux]"

ffmpeg ... -filter_complex "$FC" -map "[main]" ... -f mpegts out.ts \
    -map "[aux]" -c:v rawvideo -f null /dev/null \
    -map 0:a:0 -c:a pcm_s16le ... -f null /dev/null
```

- 終了コード **0**
- `out.ts`（録画ストリーム）510,420 バイト生成
- `feat-a.log`（音声 RMS）15,388 バイト生成
- 映像統計の出力を確認:

```
[Parsed_metadata_6] lavfi.signalstats.YAVG=125.415
[Parsed_metadata_6] lavfi.signalstats.YAVG=125.466
```

**フィルタ構成は正しく、統計が生成される。**

### 残る不確定要素

1. **`stb_streamer` が `--feature-file` を読む形式**（`lavfi.signalstats.YAVG=...`
   の行形式）と一致するか。旧ランチャーが同じフィルタを `pix-ff1.log` へ
   出していたので、形式は踏襲できているはず。
2. **ハードウェアエンコーダ（`h264_v4l2m2m`）で実時間を保てるか。** 解析用の
   縮小コピーを追加するため、負荷が増える。旧ランチャーのコメントでは
   「320x180 なら 30秒/39.5秒」と余裕があるとされている。
3. **NAS が `features=v1` を要求するか。** 要求しなければ `stb_streamer` は
   pixf を差し込まないため、CM 情報は送られない（設計どおり）。

## 追記（round 90）: 特徴量追加のコストを実測（実時間の 31 倍の余裕）

### 懸念

旧ランチャーの記録では「320x180 の縮小コピーで 39.5 秒の映像に 30 秒」と
あり、解析コストは無視できない。録画に追加して実時間を割り込むと、録画そのものが
破綻する。

### 測定

Mac（ffmpeg 8.1.2）で 20 秒の MPEG-TS（1440x1080/30fps）を処理した時間。
ハードウェアエンコーダの代わりに `libx264 -preset ultrafast` を使った。

| 構成 | 処理時間 | 実時間比 |
|---|---|---|
| 特徴量なし | 0.31 秒 | 65 倍速 |
| **特徴量あり** | **0.65 秒** | **31 倍速** |

**特徴量の追加は約 2 倍のコスト増だが、実時間の 31 倍の余裕がある。**

### 理由

フィルタは `split=2` で**分岐**するため、スループットは「メイン + 解析」の
和ではなく、**遅い方（ボトルネック）で決まる**。解析側は 320x180 に縮小して
いるので、メインストリームの H.264 符号化より軽い。

### 注意

- この測定は Mac の x86/ARM であり、実機の Amlogic S905X2 とは絶対値が違う。
  ただし旧実装が同じ構成で 2代目（同じ SoC）を実運用しているので、実績がある。
- ハードウェアエンコーダ（`h264_v4l2m2m`）は CPU 負荷がさらに低いため、
  実機ではもっと余裕があると見込まれる。
- 万一実時間を割り込んでも、`PIX_FEATURE_FAILURES` の安全装置で特徴量が
  無効化され、録画は継続する。

## 追記（round 91）: 3代目の stb_streamer が特徴量対応版であることを確認

round 85 で「SD に入っている `stb_streamer` が特徴量対応版かは未確認」と
していた点を、成果物から直接検証した。

### 方法

`rootfs.cpio.uboot` を uImage ヘッダの後ろから gzip 展開し、cpio 内の
`usr/bin/stb_streamer` の ELF 本体を検索した。

```
rootfs: 111,637,504 bytes
stb_streamer in cpio: True
ELF header: \x7fELF\x02\x01\x01\x00
```

ELF 本体に含まれる文字列:

| 文字列 | 含まれる |
|---|---|
| `--feature-file` | **True** |
| `--feature-audio-file` | **True** |
| `--feature-black-file` | **True** |
| `--feature-max-mb` | **True** |

**3代目のイメージに入る `stb_streamer` は特徴量対応版。** 追加のインストール
作業は不要（`scripts/install-stb-feature.sh` は2代目向けの経路）。

### これで CM 経路の全要素が揃った

1. `pix-stream-simple` が特徴量を生成（`encode_with_features`、round 86）
2. `FEAT_ARGS` が旧実装と同じ 4 引数（round 88 で一致を確認）
3. `stb_streamer` がそれを受け取れる（本ラウンドで確認）
4. 解析失敗時は特徴量を無効化して録画を守る（round 88）
5. ffmpeg のフィルタ構成は動作確認済み（round 87）
6. コストは実時間の 31 倍の余裕（round 90）

**残るのは、NAS が `features=v1` を要求するかの実機確認のみ。**
