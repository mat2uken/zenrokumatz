# PIX-DT295 の STB 統合（作業記録）

作成日: 2026-11-03
対象: X96Max_V311（192.168.99.124、Android 8.1 / armv7a）
チューナー: PIX-DT295（USB `06b8:103b`、serial WD10043976）
参照実装: `rk2kk/pix-userland` ブランチ `dt295/transcode-hooks` / PR #40

---

## 1. 現状

| 項目 | 状態 |
|---|---|
| GR 受信 | **動作**（T17、10 秒で 104,927 パケット、TEI/CC/CRC 0） |
| 復号 TS 取得 | **動作**（スクランブルフラグ 0、MPEG-2 1440x1080 をデコード確認） |
| BS 受信 | **動作**（BS1 / TSID 16400、8 秒で 83,140 パケット、TEI/CC/CRC 0） |
| オンボード H.264 変換 | **動作**（1280x736 High@3.1、約 6.6 Mbps） |
| 変換済み H.264 のコピー経路 | **動作**（音声 23.35 秒／映像 23.49 秒、デコードエラー 0 行） |
| 既存 TS→MP4 経路 | **動作（AVC）**（582 フレーム/19.3 秒、エラーなし）。STB の HEVC 修正後の結果と処理速度は 7 章を参照 |
| 2 経路の切り替え | `live.conf` の `videoCodec=copy` でコピー経路、無指定なら STB 側エンコード |

今回の運用対象は **DT-295 1 台・1 番組・内蔵 AVC**。HEVC と 2 番組同時録画は対象外。
単体の録画・再開の確認結果は 10 章、NAS で共通処理する構成は 11 章に記載する。

## 2. ビルド（Android armv7a）

`pix-ts-legacy`（トランスコードフックを含む従来 CLI）を NDK でクロスビルドする。

```bash
NDK=~/Library/Android/sdk/ndk/27.0.12077973
CC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi24-clang

# 1) libusb 1.0.30 を armv7a 向けに静的ビルド
curl -sL -o libusb.tar.bz2 \
  https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
tar -xjf libusb.tar.bz2 && cd libusb-1.0.30
CC=$CC ./configure --host=arm-linux-androideabi --prefix=$PWD/../prefix \
  --disable-udev --disable-shared --enable-static AR=llvm-ar RANLIB=llvm-ranlib
make -j4 && make install

# 2) pix-ts-legacy をビルド（トランスコードフック有効）
make -j6 pix-ts-legacy CC="$CC" PKG_CONFIG=false \
  USB_CFLAGS="-I.../prefix/include/libusb-1.0" \
  USB_LIBS=".../prefix/lib/libusb-1.0.a -llog" \
  CFLAGS="-O2 -g -DDT295_CAPTURE_TESTING"
```

### Android 向けの必須パッチ

`src/dt295_capture.c` の録画ロックが `/tmp` 固定で、Android では `/` が読み取り専用のため
`DT295 recording lock is busy or unavailable` で必ず失敗する。`TMPDIR` →
`/data/local/tmp` → `/tmp` の順に書き込み可能な場所を選ぶよう変更する。

## 3. assets（bundle）の選択

DT295 の `dt295.assets` には**衛星レジスタ表**が入っていないものと入っているものがある。

| bundle | SHA-256 | BS |
|---|---|---|
| `rk2kk/pix-userland` の `assets/dt295.assets` | `5ea05c9d…` | **失敗**（exit 5, `satellite tune failed`） |
| `Khronos31/pix-userland` の `firmware/dt295.bin` | `3cabe15f…` | **成功** |

後者は公式 `StationTV X_UpdateTool_1.20.3000.2` のドライバ
`pxmb86m0x_usb.sys` から VA `0x845a0` の 144 バイト（33 バイト差分）を取り込んで生成されており、
`firmware/metadata/dt295-satellite-generation.json` に生成条件が記録されている。
公式アーカイブは `https://www.pixela.co.jp/products/tv_capture/pix_dt460/data/stationtvx_updater12030002.zip`
から取得できる（161 MB）。

配備先は `/data/local/tmp/pix-ts/assets/dt295.assets`（実行時に `--assets` で指定）。
bundle は機微データのためリポジトリへは入れない。

## 4. 動作確認コマンド（STB 上）

```bash
cd /data/local/tmp/pix-ts

# 機器一覧
./pix-ts-legacy --model dt295 --assets ./assets --list-json

# GR（T17）10 秒
./pix-ts-legacy --model dt295 --assets ./assets -c T17 -t 10 -o ./t17.ts

# BS（BS1 / TSID 16400）
./pix-ts-legacy --model dt295 --assets ./assets-sat -s bs -c BS1 --tsid 16400 -t 10 -o ./bs1.ts
```

`-c` は物理チャンネル。`T17` は静岡のテレビ静岡。BS は `BS1` のようなスロット表記か IF 周波数。

## 5. オンボード H.264 変換

> 変換部は **H.264 専用**（Fujitsu MB86M01 系）で、HEVC は出力できないことを
> 実測で確認した。PMT 宣言を HEVC に変えても実ビットストリームは H.264 のまま。
> 詳細と 2 ストリーム同時利用の可否は
> [dt295-encoder-capabilities.md](dt295-encoder-capabilities.md) を参照。

### フック

| 環境変数 | 適用位置 | 書式 |
|---|---|---|
| `DT295_XREG` | 受信プロファイル適用直後 | `addr:shift:width:value[,…]`（addr/value は 16 進、shift/width は 10 進） |
| `DT295_XREG2` | 番組メタデータ・PID ルート設定後、取得開始直前 | 同上 |
| `DT295_TOLERATE_PSI` | 変換に伴う PMT 書き換えを許容 | `1` |

### レジスタマップの入手先

MB86M01 のレジスタ定義は Elgato Game Capture HD の公開ドライバ
（`tolga9009/elgato-gchd`、MIT）に一式ある。主なもの:

| レジスタ | アドレス | ビット | 意味 |
|---|---|---|---|
| `v_ts_out_mode` | `0x100a` | shift 4, width 4 | 出力モード（gchd は 1 = Encode。DT295 既定は 2） |
| `v_h264_level` / `v_h264_profile` | `0x1526` | 0/8 | level / profile（DT295 既定 40 / Main） |
| `v_hsize_out` / `v_vsize_out` | `0x152c` / `0x152e` | 0/16 | 出力解像度（DT295 既定は 0） |
| `v_bitrate` / `max` / `ave` / `min` | `0x1532`–`0x1538` | 0/16 | ビットレート（DT295 既定は 0） |
| `v_gop_size` / `v_gop_struct` | `0x1518` | 8 / 0 | GOP（DT295 既定 15 / 0） |
| `v_rate_mode` / `v_vlc_mode` | `0x1510` | 0 / 3 | レート制御 / CABAC |
| `system_rate` | `0x1104` | 0/16 | TS 出力レート（DT295 は 0xFFFF 固定） |
| `v_format` / `v_in_source_type` | `0x1502` / `0x1504` | 0/8, 8/3 | 入力形式 |
| `v_v420` | `0x1710` | 12/2 | 4:2:0 |

### 確定したレジスタ設定（720p8m）

XREG で解像度・ビットレート・レベルを設定し、XREG2 で出力モードを切り替えると
**変換が成立する**。

```bash
DT295_TOLERATE_PSI=1 \
DT295_XREG="152c:0:16:500,152e:0:16:2d0,1532:0:16:1f40,1534:0:16:1f40,1536:0:16:1f40,1538:0:16:0,1526:0:8:1f" \
DT295_XREG2="100a:4:4:1" \
./pix-ts-legacy --model dt295 --assets ./assets-sat -c T17 -t 8 -o ./x17.ts
```

```bash
DT295_TOLERATE_PSI=1 \
DT295_XREG="152c:0:16:500,152e:0:16:2d0,1532:0:16:1f40,1534:0:16:1f40,1536:0:16:1f40,1538:0:16:0,1526:0:8:1f" \
DT295_XREG2="101c:8:8:1b,100a:4:4:1" \
./pix-ts-legacy --model dt295 --assets ./assets-sat -c T17 -t 30 -o out.ts
```

| レジスタ | 値 | 意味 |
|---|---|---|
| `0x152c` / `0x152e` | 1280 / 720 | 出力解像度 |
| `0x1532`–`0x1538` | 8000 / 8000 / 8000 / 0 | ビットレート（kbps） |
| `0x1526` | level 31 | H.264 Level 3.1（profile は Main のまま） |
| `0x100a` shift 4 | 1 | 出力モード = Encode（既定は 2 = パススルー） |
| `0x101c` shift 8 | 0x1B | 出力 PMT の映像を H.264 として宣言 |

結果:

| 指標 | パススルー | 変換モード |
|---|---|---|
| レート | 15.8 Mbps | **6.6 Mbps** |
| 映像 | MPEG-2 1440x1080 | **H.264 High@3.1 1280x736** |
| 30 秒連続 | – | 204,451 パケット、TEI/CC/CRC 0、エラー 0 |
| 変換後の復号 | – | 750 フレーム/24.9 秒をエラーなしでデコード |

### タイムラインの扱い（コピー経路）

変換ストリームは 1 PES に 2 アクセスユニット（フィールド）、PES 間隔 33.37 ms
（3003 ticks @90 kHz）で、B ピクチャを含むため PES に PTS と DTS の両方が入る。
コピー経路は次のように組み立てる。

- アクセスユニットのタイムスタンプは **PES 単位**で割り当てる。PES が届いたら
  次の AU がその PTS/DTS を取り、同じ PES の 2 個目は +16.683 ms（フィールド間隔）。
  バイト位置で対応付けると、AU の終端は次の PES が来るまで分からないため
  2 個目が次の PES のタイムスタンプを拾い、トラック長が 2 倍になっていた。
- サンプル長は **DTS の差分**から決める（PTS は表示順なので差分に使えない）。
- 音声のタイムラインは PES のタイムスタンプを使わず、映像の現在位置に
  アンカーして 1024 サンプルずつ進める。AAC フレームは PES 境界をまたぐので、
  端数は次の PES へ持ち越す（持ち越さないと 3 分の 2 のフレームが落ちる）。

実測: 23.49 秒の映像に対して 23.35 秒の音声、デコードエラー 0 行。

### 変換ストリームの注意点

- **PMT は 0x02（MPEG-2）のまま**。デバイスは変換時に PSI を書き換えないため、
  受け側は映像 PID を PMT から取り、コーデックは H.264 と決め打ちする必要がある
  （実装は `LiveTsCopyStreamer`）。
- **B フレームを含む**（`has_b_frames=4`、PES に PTS と DTS の両方が入る）。
  fMP4 の trun には composition offset が必要なので、シンクの記録ヘッダ
  未使用 2 バイトに cto(ms) を載せ、DTS を基準に正規化するよう変更した。
- 変換後の解像度は 1280x736（SPS の cropping 無し、マクロブロック境界）。
- 実験後、フックなしの通常受信は正常に戻る（機器は壊れない）。

## 6. STB 側の統合

### 2 経路の切り替え

| 経路 | 使うもの | 出力 |
|---|---|---|
| TS→MP4(HEVC) | `LiveTsStreamer`（MPEG-2 デコード → VPU で HEVC） | HEVC fMP4 |
| チューナー変換 H.264 | `LiveTsCopyStreamer`（デマックス + AVCC 変換のみ） | H.264 fMP4 |

`/data/local/tmp/live.conf` の `videoCodec=copy` で切り替える。
`capture-agent.sh` は `PIX_TUNER=dt295` かつ `DT295_TRANSCODE=1` のとき、
この行を自動で足す。

### capture-agent.sh の DT295 対応

```sh
PIX_TUNER=dt295          # torne（既定）から切り替え
DT295_CONF=/data/local/tmp/pix-dt295.conf
```

設定ファイル（端末内に置く。リポジトリには入れない）:

```sh
DT295_ASSETS=/data/local/tmp/pix-stream/assets
DT295_TRANSCODE=1
DT295_XREG=152c:0:16:500,...    # 上記 720p8m
DT295_XREG2=101c:8:8:1b,100a:4:4:1
```

DT295 はチューナー内で B-CAS を解除するので `torne-b25` とカードソケットは使わない。

チャンネルは NAS からの要求（`capture.req` の `start <ch>`）で決まる。`17` は
`-c T17`、`BS15_0` は `-s bs -c BS15_0` として pix-ts-legacy に渡す。
`DT295_SYSTEM` / `DT295_CHANNEL` は使わない。

## 7. この機体での HEVC エンコーダー

初期調査では VPU の共通バッファー確保に失敗したが、原因は
32 bit mmap オフセットと 2 GB 超の物理アドレスだった。
HEVC ファームウェアがないという当時の判断は誤りで、DTB の配置修正で初期化できた。

その後、Surface 入力処理にも形式の不一致が見つかった。
ベンダーライブラリに RGBA→NV21 変換を組み込み、新 APK で RGBA を自動指定して、
HEVC 1280x720 の正常な映像を確認した。ただし実時間の処理速度は約 9 fps で、
30 fps の実時間処理には届いていない。今回の修正で DT-295 の連続受信録画は試していない。
適用方法と検証結果は [HEVC 調査報告](dt295-hevc-investigation.md) の 11 章を参照。

AVC の既存確認結果は Main 1280x720、582 フレーム/19.3 秒である。

### 自動フォールバック

`MainActivity` は HEVC のセッションが `NG:` で終わると同じプロセス内で
AVC に切り替える（`encoder fallback: hevc -> avc`）。以降のセッションは AVC で
走るので、`live.conf` に `codec=hevc` のままで 1 台目・2 台目と同じ設定を配れる。

### AVC の CSD 欠け（修正済み）

Amlogic の AVC エンコーダーは CODEC_CONFIG バッファに SPS しか完全に入れず、
PPS が 1 バイトに欠けることがある（`csd parsed as Annex-B sps=9 pps=1`）。
そのまま init を送ると `non-existing PPS 0 referenced` で再生できない。
`LiveTsStreamer` は不完全な CSD を捨て、出力フォーマットの `csd-0`/`csd-1`
（開始コード付き）から取り直し、開始コードを除いて使うようにした。

## 8. 確認済みの組み合わせ

| 経路 | live.conf | 結果 |
|---|---|---|
| チューナー変換 H.264 コピー | `videoCodec=copy` | H.264 High 1280x736 + AAC、750 フレーム/24.9 秒 |
| TS→MP4（STB 側エンコード、修正前） | `codec=hevc`（AVC へ自動フォールバック） | H.264 Main 1280x720 + AAC、582 フレーム/19.3 秒 |
| 保存 TS→MP4（HEVC 修正後） | `codec=hevc`、修正版ライブラリと APK | HEVC 1280x720 + AAC、実時間 9.2 fps。取得した 111 フレームを正常デコード |
| パススルー受信 | （チューナーのみ） | MPEG-2 1440x1080、270,858 パケット/25 秒、異常 0 |

## 9. 残り

- 以前の変換試験ではセッションの切り替え時に AAC のデコードエラーが数件出た。
  今回の保存 MP4 のデコード結果は 11 章に記載した。実際の音声再生は未確認。

## 10. 1 番組の内蔵 AVC 録画（2026-11-03 追記）

### 設定と制御

本章は NAS 統合前の単体試験の記録。現在の接続先と録画方法は 11 章を参照。

接続中の実機は、本体ラベルと USB ID `06b8:103b` が PIX-DT295 と一致した。
公式ドライバーの INF では DT-295W は `06b8:103c` と定義されている。
DT-295 はシングルチューナーであり、別周波数の 2 番組同時録画には使えない。
仕様の根拠は [能力調査の 1 章](dt295-encoder-capabilities.md#1-チューナーの仕様と実機の識別)を参照。

[設定例](../tools/pix-dt295.conf.example)を端末の `/data/local/tmp/pix-dt295.conf` に置く。
`PIX_TUNER=dt295` と `DT295_TRANSCODE=1` を指定すると、`capture-agent.sh` は
内蔵 AVC を有効にし、`codec=avc` と `videoCodec=copy` を `live.conf` に書く。
STB は映像を再エンコードせず、H.264 と AAC を fMP4 に格納する。
解像度・ビットレートは DT295_XREG で決まり、HTTP のエンコード設定では変更しない。

`stb_streamer` と `capture-agent.sh` の起動後は次の HTTP 要求で
物理チャンネル T17 の受信と録画を開始できる。

```sh
ffmpeg -i 'http://192.168.99.124:40774/v1/streams/17?after=0' \
  -t 20 -map 0:v:0 -map 0:a:0 -c copy -movflags +faststart t17.mp4
```

`-t` は保存する秒数。配信は fMP4 で、ffmpeg が再エンコードせず通常 MP4 に格納する。
curl を指定時刻に中断する方法では、末尾の断片が途中まで書かれ、保存ファイルの
デコードに失敗するケースを実測したため、録画には上記の手順を使う。
ファイルの確認には `ffprobe` と映像・音声の全件デコードを使う。
HTTP クライアントが切断すると、30 秒後にチューナーを停止する。

アプリが通常画面で起動済みの場合、初回は `live.conf` を用意してから
アプリを起動し直す必要がある。`MainActivity` は起動時に設定ファイルの有無を確認する。
録画中の切り替えではアプリを強制停止せず、チューナーの停止による EOF で
コピー処理を終了させ、次の設定を読み込ませる。

### 修正した箇所

- DT-295 の生存確認を `pix-ts-legacy` に変更した。以前は torne のプロセスを
  探していたため、正常受信中の DT-295 を異常と判断して再起動していた。
- 停止時は起動時に記録した `pix-ts-legacy` の親 PID にだけ SIGTERM を送る。
  同名の書き込み用子プロセスは親に終了させる。以前は停止処理から DT-295 が
  抜けており、再開時の排他ロックが残っていた。
- 衛星受信の引数を分割せずに渡し、任意指定の `DT295_TSID` は未指定でも動く形にした。
- fMP4 の受信待ちを制御用 mutex の外へ移した。接続前・データ待ちの間も
  HTTP のチャンネル要求を処理できる。

### 確認結果と未解決事項

ログ・映像は `work/dt295-avc-20261103/` に保存した。

| 確認 | 結果 |
|---|---|
| USB 再接続前の内蔵 AVC 録画、T17 | 45 秒の取得で 38,425,331 バイト。映像 H.264 1280x736、44.561 秒。音声 AAC 48 kHz stereo、44.534 秒 |
| 上記ファイルの映像・音声全件デコード | ffmpeg 終了コード 0、エラーログ 0 行 |
| 正常受信中の不要な再起動 | 約 68 秒間、受信の親 PID が変わらないことを確認 |
| HTTP 開始遅延の再現テスト | 修正前は 947 ms 待って失敗。修正後は接続前・接続済みの両方で 0 ms、終了コード 0 |
| 修正版 native の実機 HTTP 要求 | 約 1 秒で `capture.req` を生成。以前の約 30 秒待ちは解消 |
| 物理的な再接続後の T17 受信 | 78.3 秒、513,655 パケット。TEI/CC/CRC 0、USB 切断なし |
| 同セッションの HTTP 保存ファイル | 映像約 39.65 秒。映像・音声の全件デコードで終了コード 0、エラーログ 0 行 |
| 自然停止と再開 | HTTP 切断から 30 秒後に停止し、`exit=0 state=16`。次の受信は別の親 PID、新しい init と断片で開始 |
| 再開後の自然停止 | `exit=0 state=16`、TEI/CC/CRC 0 |
| ffmpeg による通常 MP4 保存 | 終了コード 0、14,968,103 バイト。映像 H.264 1280x736、20.037 秒。音声 AAC 48 kHz、20.010 秒 |
| 上記通常 MP4 の映像・音声全件デコード | 終了コード 0、エラーログ 0 行 |

USB 再接続前のファイル取得では、HTTP 開始遅延を避けるための要求ファイルの手動作成と、
初回のアプリ起動し直しを行った。物理的な再接続後は修正版全体で録画・停止・再開を確認した。

停止処理を親 PID だけに修正した後も USB 切断が発生した。
対象 USB の `authorized` を 0→1 にして 1 回だけ論理的に再接続したが、
8 秒の直接受信試験は約 6.4 秒で USB 切断、終了コード 7 となった。
切断原因は未確定だが、その後ユーザーが物理的に抜き差しし、上表の受信と再開に成功した。
地上波 T17 と BS15_0（ＮＨＫ ＢＳ）を実機で確認済み。CS と電源再投入後の自動起動は確認していない。

確認用の通常 MP4 は `work/dt295-avc-20261103/replug-ffmpeg-20s.mp4`。
再接続後のログは同ディレクトリの `replug-*` に保存した。
最終確認時は `stb_streamer`・`capture-agent.sh`・アプリを起動したまま、
チューナーは停止したオンデマンド待機状態。HTTP 接続元は検証に使った Mac
（`192.168.99.24`）に限定している。3 回の受信はすべて HTTP 切断から 30 秒後に
正常停止し、物理的な再接続後の追加 USB 切断はなかった。実際の音声再生は未確認。

## 11. NAS の共通受信・保存経路への統合

### 構成

DT-295 の内蔵 AVC 出力も他の STB と同じ `stb_streamer` から fMP4 で配信する。
NAS の `pix-stream-manager.service` が受信プロセスを起動し、番組ごとの fMP4 を
同じ `/dev/shm/pix_cm_queue/` に投入する。
`pix-cm-worker.service` が再エンコードせず通常 MP4 と JSON を
`/mnt/xblk/video/pix-smb400/recordings/` に保存する。
CM カットは引き続き無効で、保存後の入力削除・失敗時の入力保持も共通処理に従う。

| 項目 | DT-295 の設定 |
|---|---|
| 管理画面 | `http://192.168.99.80:40776/` |
| ノード ID | `192.168.99.124-17` |
| 配信先 | NAS `192.168.99.80` のみ。単体試験時の Mac 限定設定から変更 |
| 受信チャンネル | 地上波 T17（テレビ静岡） |
| 映像・音声 | 内蔵 AVC、H.264 1280x736 + AAC |
| NAS 登録値 | AVC / 1280x736 / 8 Mbps / VBR / GOP 1 秒 |

開始・停止は他の STB と同じ管理画面または API を使う。

```sh
curl -s -X POST 'http://192.168.99.80:40776/api/nodes/192.168.99.124-17/start'
curl -s -X POST 'http://192.168.99.80:40776/api/nodes/192.168.99.124-17/stop'
```

エンコーダーの解像度・ビットレートは 10 章の `DT295_XREG` で設定する。
NAS の登録値はこれに合わせた値であり、内蔵エンコーダーのレジスタを変更するものではない。

### 共通処理に合わせた変更

- `LiveTsCopyStreamer` も EIT の現在番組から `sendBoundary` を送るようにした。
  対象サービスの section 0 だけを使い、時刻の変換は既存の `LiveTsStreamer` と共用する。
  `stb_streamer` は他の経路と同じ `pixb` を出力し、NAS が番組を分割する。
- NAS の再開位置を `last_seq_<host>_<port>_<channel>` に保存するようにした。
  同じ T17 を受信する別の STB と、断片の連番を混ぜずに再開できる。
- 共通の `cm-cut` で、カット指定が空の検査・MP4 格納は B フレームを受け付けるようにした。
  DTS と PTS の差を維持する。実際にカットを指定した場合の B フレーム拒否は維持する。
  保存ワーカーは共通のものを使う。

本番 NAS recorder の変更前ソースは、`dtv_android-cm-integration` の
`8501a7dfcb255a55e931e469a644dd3bca640885` に作業中の CM キュー対応を加えたもの。
同じコンパイル条件で稼働版を再現したうえで、再開位置のファイル名だけを変更して配備した。
`cm-cut` も稼働版を再現し、B フレームを拒否する条件 1 か所だけを変更した。
ビルド用ソースとログは `work/dt295-nas-20261103/` および
NAS の `/home/ku/work/dt295-nas-20261103/` に保存した。

### 確認結果

- NAS の共通 manager から開始・停止し、fMP4 を受信した。サービス 35864、event 1087 の
  番組マーカーと init を受け取り、断片の欠落は 0。
- 最初の保存では共通 `cm-cut` が B フレームを拒否した。NAS の保持入力を修正後のツールで
  再処理し、映像 1,800 パケット、音声 1,409 パケットの内容と PTS−DTS 差がすべて一致した。
  通常 MP4 の映像・音声全件デコードは終了コード 0、エラーログ 0 行。
- 既存 HEVC の短い入力は、修正前後の `cm-cut` が同じ MP4 を出力した。
  映像 111 パケット、音声 79 パケットで、全件デコードも終了コード 0。
- 11:47 の実放送で、同じ受信セッションのまま event 1087→1088 を検出した。
  NAS が前番組を別ジョブへ分割し、受信を継続した。

保存ワーカーは `20261103-original-only-avc-01` に切り替えた。
`cm_service.py` は変更せず、`cm-cut` と manifest を更新した。
保持されていた失敗ジョブ 2 件も同じキューで再処理し、通常 MP4 と JSON の保存を確認した。
最初の保存ジョブは `2f8e4c40-602f-4261-b02d-94de5cb18450`、
実番組切替で分割したジョブは `f1031af1-1e5e-4b6f-8117-987b1bd40119`。
最初の NAS 保存 MP4 のローカル写しは `work/dt295-nas-20261103/nas-final-20s.mp4`。

この 2 件は番組途中から開始した検証録画で、共通ワーカーが
`source_duration_shortfall` により `partial` として保存した。
パケットの内容と時刻の一致は確認したが、番組全体を欠けずに録画できた結果ではない。
最終確認時は NAS の manager・worker が稼働し、DT-295 のノードは有効で録画継続中。
STB の接続数は 1、スプールの欠落数は 0。電源再投入後の動作と実際の音声再生は未確認。

### 確認コマンドの誤起動

NAS recorder は `--help` に未対応であり、確認時に既定値で別の受信プロセスが起動した。
これにより .57 の映像設定が一時的に1280x720・GOP 5秒になった。
該当プロセスを停止し、1440x1080・4 Mbps・GOP 2秒に戻して11:34:17に受信再開を確認した。
既存 manager の録画プロセスは停止していないが、この時間帯の録画には影響し得る。
短い重複録画とジョブ `d3289486-cf5a-4d30-a621-2b029c1748e3` は保持した。
経緯は `work/dt295-nas-20261103/inspection-command-incident.txt` に記録した。

## 12. 起動しないときの切り分け（2026-11-04 追記）

DT295 が起動しない原因は、ファームウェアの状態語 `0x82008` でほぼ決まる。
`pix-ts-legacy` が起動できるのは `0x00`（コールド）と `0x10`（常駐・未認証）だけで、
`0x11`（常駐・認証済み）は **USB の再挿抜でしか戻せない**。従来はこの区別が無く、
どの原因でも 15 秒ごとに再起動を繰り返していた。

- 状態を読む `dt295-state`（[ブート状態](dt295-boot-states.md)）。2026-11-05 に実機配置済み
- 再挿抜・再試行・占有の判断と、自動復帰の動き（同 7 章）
- エビデンスの取得 `capture-agent.sh evidence`（同 8 章）

変換のレジスタ設定は手で書かず、
[トランスコードプロファイル](dt295-transcode-profiles.md) の
`python3 tools/dt295_profiles.py export <name>` で生成する。
