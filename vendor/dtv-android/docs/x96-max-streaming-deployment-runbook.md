# X96Max ストリーミング録画 運用手順書

作成日: 2026-11-01
対象: X96Max_Plus2（192.168.99.57）と NAS sweedmaster02（192.168.99.80）

2026-11-04現在、192.168.99.209のSBS、192.168.99.57のテレビ静岡、192.168.99.124のBS11を録画している。CMカットは再開しており、ロゴ判定によるカットを公開しつつ、ロゴ無し番組の判定は検出のみのモードで実績を集めている（[CM判定の現状](cm-cut-learning.md)）。以下の初期配置手順や過去の検証値とは構成が異なるため、現在の保存処理と設定はリンク先を参照する。

---

## 1. 構成

```text
[X96Max_Plus2 192.168.99.57]          eMMC 書き込み: 0バイト
  BootReceiver (アプリ) → pix-stream-watchdog.sh をデタッチ起動
  /data/local/tmp/pix-stream/
    pix-stream-watchdog.sh  15秒ごとに欠けた部品だけを起動する常駐監視
    capture-agent.sh        NASの要求に合わせてチューナーを起動・停止する
    stb_streamer            自前C: fMP4生成 + RAMスプール + HTTPサーバー
    torne-c, torne-b25      PS3 torne の受信とスクランブル解除
    start_x96_recorder.sh   手動用の一括起動（冪等）
  起動直後は待機。NASが要求したチャンネルだけを同調・エンコードする。
  /dev/shm (RAM。/dev 自体が tmpfs なので追加マウント不要)
    torne/raw.fifo, clear.fifo   復号済みMPEG2-TS
    streamer.log, watchdog.log
  40774  HTTP chunked配信（接続元はNAS 192.168.99.80 のみ）
  40775  loopback TCP（JavaのHW変換 → CのfMP4生成）
       │
       │ HTTP/1.1 GET /v1/streams/live?after=<連番>
       │ ※TCP接続は常にNAS側が開く
       ▼
[NAS 192.168.99.80]
  systemd: pix-stream-manager.service（enabled）
    pix_stream_manager         管理APIと画面（40776）
      nodes.json               登録したSTB
      pix_stream_recorder      有効なSTBごとに子プロセスとして起動
  systemd: pix-stream-recorder.service（停止中。managerが置き換える）
    pix_stream_recorder        受信 → tmpfs → 通常MP4化 → JSON sidecar
  /dev/shm/pix_stream/         受信fMP4の一時領域（RAM、完了後すぐunlink）
  /mnt/xblk/video/pix-smb400/recordings/YYYY/MM/DD/...
       │ 既存pix-archiveが10秒間隔で走査しJSONを自動カタログ登録
       ▼
  miraview（既存Web UI）から再生
```

### 1.1 番組境界の扱い

STBはEIT p/f（PID 0x12、table 0x4E、section 0）から現在の番組を読み取る。
event_id が変わると、次のキーフレームで断片を確定し、`pixb` ボックスに番組情報を載せて送る。
NASはそのボックスでファイルを分割する。**接続は切らないので、境界で断片が欠けない。**

新しい読み手が接続したときは、STBが現在の番組情報を再送する。
NASは既に同じ番組を録画中なら再送を無視するので、二重分割にはならない。

### 1.2 映像と音声

| 項目 | 設定 |
|---|---|
| 映像（既定） | H.265/HEVC Main、1920x1080、30fps、5.0 Mbps VBR（実測 5.07 Mbps） |
| 映像（軽量） | H.265/HEVC Main、1280x720、2.5 Mbps VBR（実測 2.76 Mbps） |
| 映像（互換優先） | H.264 Baseline、5.5 Mbps VBR（実測 5.8 Mbps） |
| H.264の制約 | SoCがMain/Highを拒否するためBaselineのみ |
| 切り替え | ウォッチドッグの `PIX_CODEC=avc` / `PIX_BITRATE=<bps>` で指定する |
| スケーリング | GLES の GL_LINEAR（1440x1080 → 1280x720） |
| 音声 | 放送波のAAC-LC 48kHz ステレオをADTSから取り出してそのまま格納（再エンコードなし） |
| GOP | 5秒。断片は必ずキーフレームで始まる |

音声のPTSはトランスポート時刻、映像のPTSはエンコーダー時刻で別系統である。
最初の音声パケットを直近の映像フレーム時刻に合わせてから、以降は差分で進める。

### 1.3 HEVC と H.264 の使い分け

720p30では、HEVCはH.264のおよそ半分のビットレートで同等の画質になる。
実測では HEVC 2.76 Mbps、H.264 5.81 Mbps であった。

| 観点 | HEVC | H.264 |
|---|---|---|
| 保存容量 | 約半分 | 基準 |
| Chrome（macOS） | 再生・シークとも可 | 可 |
| Safari | 可 | 可 |
| WindowsのChrome | OSのHEVC対応に依存 | 可 |
| 古いTV・DLNA | 非対応の機種がある | ほぼ可 |

迷う場合はH.264にしておけば再生できない環境はほぼ無い。
解像度・コーデック・ビットレートは `/data/local/tmp/pix-stream/live.conf` で決める。
ウォッチドッグはこのファイルが存在すれば上書きしない。

### 1.4 1080p30 の実測

| 項目 | 実測値 |
|---|---|
| エンコード速度 | 29.7〜31.1fps（5秒ごとの測定で平均30.3fps） |
| フレーム落ち | 5400フレームすべてが 0.0333秒間隔。欠落0 |
| デコーダー遅延 | `fed` と `enc` の差が常に13前後で増えない |
| ビットレート | 5.0 Mbps 指定で実測 5.07 Mbps |
| メモリ | 976MB / 3794MB |
| デコード | ffmpeg でエラー・警告ゼロ |

**実時間に対して余裕はほぼ無い。** 測定中は30fpsを割らなかったが、負荷変動や温度上昇で
落ちる余地は残る。長時間の連続運転では、`fps` が30を下回り続けないかを監視すること。

1080p にすると垂直方向の情報が保たれる。元映像は 1440x1080i なので、
1280x720 へ落とすと垂直解像度が半分になる。


## 2. 日常操作

### 2.0 テレビ画面の状態表示

STBにHDMIでつないだテレビには、アプリが現在の状態を表示する。一番上の帯を見れば状況が分かる。

| 表示 | 意味 |
|---|---|
| ● 録画中 ch20 | 録画中。括弧内は接続中のクライアント数 |
| ○ 待機中 | NASからの要求待ち。チューナーは動いていない |
| ▲ チューナー未接続 | USBチューナーが見えていない。録画できない |
| × 配信サーバーが停止 | ネイティブの配信サーバーが落ちている |

帯にはIPアドレス、チューナーの接続状態、スプール使用量、欠落数、現在時刻も出る。
2秒ごとに更新される。異常時は画面を見るだけで原因の切り分けができる。

### 2.0 管理画面

ブラウザで `http://192.168.99.80:40776/` を開く。

| 操作 | 方法 |
|---|---|
| STBの自動検出 | 電源が入っていれば「ネットワーク上のSTB」に自動で出る |
| STBの登録 | 自動検出欄の「追加」を押す。手入力も可能 |
| 録画設定 | そのSTBの「設定」を押し、チャンネル・受信・形式をまとめて変えて「適用」 |
| 録画の開始／停止 | そのSTBの「開始」／「停止」（1つのボタンで切り替わる） |
| STBの削除 | そのSTBの「削除」 |
| 状態の確認 | 「状態」が実測（録画中／ch不一致／データ停止／STB待機／停止／応答なし）を出す |

登録内容は `/home/ku/pix-stream-20261101/nodes.json` に保存される。

チャンネルは番号だけでなく**放送局名**も表示する（例: `17 テレビ静岡`、
`BS15_0 ＮＨＫ ＢＳ`）。局名はアーカイブのサービス一覧
(`/api/archive/services`) から取り込み、10分ごとに更新する。地上波 (GR) と
BS/CS に対応し、BS は `BS15_0` のようなトランスポンダ表記をそのまま使う。

チャンネルは**受信できるものだけをプルダウン表示**する。どれを出すかは各STBの
「受信」設定で決まる（`gr` = 地上波のみ、`gr,bs,cs` = 地上波＋BS/CS）。
torne は地上波のみ、DT-295 は地上波＋BS/CS。

**同じSTBは1ノードだけ**。ノードは「1台のSTBで1本録る」単位で、チャンネルは
その録画の設定であってSTBの識別子ではない。同じホスト:ポートを別チャンネルで
追加しようとすると `409 already registered as ...` で拒否される。二重登録すると
2本のレコーダーが同じチューナーのchを奪い合い、両方録れなくなるため。

「状態」は設定ではなく実測を出す。STBの実chが設定と違えば「ch不一致」、
レコーダーが生きていてもSTBのフラグメントが30秒進まなければ「データ停止」。
「録画」列はNAS側のレコーダープロセスの生死で、「状態」とは別の情報。

### 2.0.0 自動検出の仕組み

STBは自分から30秒ごとに「ここにいる」と知らせる。NASはそれを受けて一覧に出す。
ライブラリは使わず、DNSメッセージを直接組み立てている。

| 項目 | 内容 |
|---|---|
| サービス名 | `_pixstb._tcp.local` |
| 送り先 | マルチキャスト 224.0.0.251:5353 と ブロードキャスト 255.255.255.255:5353 |
| 通知内容 | 名前、IPアドレス、ポート、チャンネル（設定値であり実chではない） |
| 応答 | NASからの問い合わせにも即座に応答する |
| 停止の判定 | 90秒以上連絡がなければ「応答なし」 |
| 登録済みの判定 | ホスト:ポートで照合する（チャンネルは使わない） |

**ブロードキャストを併用する理由**: スイッチのIGMPスヌーピング設定によっては、
マルチキャストがNASのポートまで届かないことがある。実機でもマルチキャストだけでは
届かず、ブロードキャストを併用して届くようになった。
「開始」したSTBは有効として記録され、STBやNASが再起動しても自動で再開する。

APIを直接使う場合:

```bash
# 一覧
curl -s http://192.168.99.80:40776/api/nodes
# 追加
curl -s -X POST http://192.168.99.80:40776/api/nodes \
  -H 'Content-Type: application/json' \
  -d '{"name":"X96Max","host":"192.168.99.57","port":40774,"channel":"20","tuners":"gr","enabled":true}'
# 開始 / 停止 / 削除
curl -s -X POST http://192.168.99.80:40776/api/nodes/192.168.99.57-20/start
curl -s -X POST http://192.168.99.80:40776/api/nodes/192.168.99.57-20/stop
curl -s -X DELETE http://192.168.99.80:40776/api/nodes/192.168.99.57-20
# チャンネルと形式の変更（IDは host-channel なので、chを変えるとIDも変わる）
curl -s -X POST http://192.168.99.80:40776/api/nodes/192.168.99.57-20/settings \
  -H 'Content-Type: application/json' \
  -d '{"channel":"17","tuners":"gr","codec":"hevc","width":1440,"height":1080,"bitrate":4000000,"rateMode":"vbr","gopSec":2}'
# 放送局名の一覧（地上波。ch -> 局名）
curl -s http://192.168.99.80:40776/api/channels
```

### 2.0.1 時刻合わせ

X96Max_Plus2はRTCバッテリを持たないため、電源を抜くと時刻が2015年へ戻る。
そのままでは番組名の特定と録画時刻がずれるので、二重に備えている。

| 仕組み | 内容 |
|---|---|
| STBの自前同期 | `timesync`（SNTP）を起動直後と5分ごとに実行する |
| Androidの自動時刻 | `auto_time` を起動時に有効化する |
| サーバー | ntp.nict.jp、pool.ntp.org、time.cloudflare.com の順に試す |

ずれが1秒未満なら時刻は変更しない。サーバーは環境変数 `PIX_TIMESYNC_INTERVAL` で
間隔を変えられる。動作は `/dev/shm/timesync.log` で確認できる。

```bash
adb -s 192.168.99.57:5555 shell cat /dev/shm/timesync.log
adb -s 192.168.99.57:5555 shell date
```

### 2.0.2 STBの待機と起動

STBは起動直後こそ待機である。チューナーは動かず、エンコーダーも停止している。
NASが接続すると、そのチャンネルを同調して録画を始める。

読み手が離れると30秒で停止し、待機へ戻る。
停止時にアプリを強制終了してはならない。チューナーを止めるとFIFOがEOFになり、
トランスコーダが自分でコーデックを解放して待機する。
強制終了すると単一インスタンスのデコーダーが残り、次の起動が失敗する。

### 2.1 STB側

通常は何もしない。再起動すると `BootReceiver` がウォッチドッグを起動し、15秒ごとに
欠けた部品を立ち上げ直す。

手動で一括起動したいとき:

```bash
adb -s 192.168.99.57:5555 shell sh /data/local/tmp/pix-stream/start_x96_recorder.sh
```

ウォッチドッグだけを起動するとき:

```bash
adb -s 192.168.99.57:5555 shell "nohup sh /data/local/tmp/pix-stream/pix-stream-watchdog.sh >/dev/null 2>&1 &"
```

ウォッチドッグはpidfileで単一実行を保証する。二重起動すると部品の取り合いになる。

状態確認:

```bash
adb -s 192.168.99.57:5555 shell "ps -ef | grep -E 'stb_streamer|torne' | grep -v grep"
adb -s 192.168.99.57:5555 shell "tail -3 /dev/shm/streamer.log"
adb -s 192.168.99.57:5555 shell df -h /data      # 使用量が変わらないこと
```

停止:

```bash
adb -s 192.168.99.57:5555 shell "pkill -f pix-stream-watchdog; pkill -f stb_streamer; pkill -f torne-c; pkill -f torne-b25; am force-stop com.example.dtvandroid"
```

### 2.2 NAS側

```bash
sudo systemctl status  pix-stream-recorder.service
sudo systemctl restart pix-stream-recorder.service
sudo systemctl stop     pix-stream-recorder.service    # 既存pix-archiveには影響しない
tail -f /home/ku/pix-stream-20261101/recorder.log
```

環境変数で上書きできる（systemdの`Environment=`を追加して使う）:

| 変数 | 既定値 | 意味 |
|---|---|---|
| `PIX_STB_IP` | 192.168.99.57 | 対象STB |
| `PIX_STB_PORT` | 40774 | STBのHTTPポート |
| `PIX_CHANNEL` | 20 | 地デジチャンネル番号 |
| `PIX_ARCHIVE_URL` | http://192.168.99.80:40773 | 既存pix-archiveのAPI |
| `PIX_RECORDINGS_ROOT` | /mnt/xblk/video/pix-smb400 | 録画ルート |
| `PIX_TMPFS_DIR` | /dev/shm/pix_stream | 受信一時領域 |
| `PIX_STREAM_MAX_SEGMENT_SEC` | 1800 | 1区分の最大長 |

## 3. 障害時の一次対応

| 事象 | 確認 | 対処 |
|---|---|---|
| 録画が流れない | STBで `pidof stb_streamer` と `tail /dev/shm/watchdog.log` | ウォッチドッグが動いていれば15秒で復旧する。動いていなければ手動起動 |
| デコーダーが `0xfffffff4` で失敗し続ける | `logcat -s LiveTs:I` に `Failed to initialize` | 単一インスタンスのデコーダーが解放されていない。STBを再起動する |
| アプリが `LIVE-TS` を返して終了を繰り返す | `logcat -s MainActivity:I` | トランスコーダは意図的に再接続を繰り返す。3秒間隔で再試行するのが正常 |
| 番組名が付かない | NASの `recorder.log` に `boundary: service=...` があるか | STBがEITを読めていない。受信できていればNASがチャンネルと時刻からEPGを引く |
| `refusing to finalize` が出る | 同ログ | その区間は公開されない。STB側の初期化セグメント異常なのでSTBを再起動する |
| NASのtmpfsが満杯 | `df -h /dev/shm` | 残っている `staging_*.fmp4` を削除する |
| 既存miraviewに録画が出ない | カタログDBを確認 | pix-archiveの走査は10秒間隔。`media.relativePath` が実体と一致しているか確認 |

## 4. ロールバック

| 段階 | 手順 | 影響 |
|---|---|---|
| STB側 | `pkill` で停止 | なし（STB 57は新規機） |
| NAS側 | `sudo systemctl stop pix-stream-recorder.service` | なし。既存pix-archive・miraview・既存録画は継続 |
| 完全撤去 | `systemctl disable --now` とユニット・配置先の削除 | なし |

**本構成は不可逆な変更を伴わない。** 既存DB・既存録画・既存pix-archiveバイナリは読み取り専用のまま扱う。

## 5. 既知の制約

- **H.264のプロファイル**: このSoCのH.264エンコーダーはBaselineのみ。Mainを要求すると
  `0xffffffc3` で拒否される。HEVCはMainが使える。
- **HEVCの再生互換性**: Chrome（macOS）とSafariでは再生できることを実機確認済み。
  WindowsのChromeや古いDLNA機器では再生できない場合がある。
- **音声は再エンコードしない**: 放送波のAACをそのまま格納する。2音声や5.1chには未対応。
- **番組境界の粒度**: 境界はキーフレーム単位（最大5秒）で丸める。
- **欠落の扱い**: 断片の連番に欠番があれば `partial` として記録する。黙って詰めない。
- **空moovの防止**: トラック情報を作れない区間は公開せずに破棄する（`refusing to finalize`）。
- **長時間運転**: 連続稼働の実績は数時間。24時間以上の試験は未実施。
- **NAS再起動**: systemdでenabled。再起動後は自動で接続し直す。
- **デコーダ指定はコーデックと一致させる**: `encode.conf`の`PIX_DECODER_MIME`に
  ストリームと違う値を書くと、復号が0フレームのまま止まり、チューナーが
  再起動を繰り返す。2026-11-04に`video/avc`を指定したまま`codec=hevc`で
  運用していた2台目がこの状態になり、録画が数分おきに切れていた。指定を外すと
  同じ機体が1440x1080で30fps・ドロップ0まで回復した。性能差と誤認しやすいので、
  まずこの指定を疑う。プログラム側でも止める（下記）。
- **二重の防御**: `capture-agent.sh`は書き出した`live.conf`を読み直し、
  `codec`と矛盾する`decoderMime`があれば削除してログに残す。受信側の
  `pix_stream_recorder`は受信した`moov`の`avcC`/`hvcC`を見て、要求した
  コーデックと違えば`codec mismatch:`を記録する。どちらも
  `tests/test_capture_agent_decoder.sh`と受信側の単体確認で検証済み。
  `video/mpeg2`（既定）はアプリ側の自動選択を意味するため、明示するのは
  コーデックと一致する場合だけにする。
- **解像度の上限は機種差ではない**: 2台目が1440x1080のHEVCで14fpsしか出なかった
  のは、デコーダ指定の不一致が原因だった。指定を外すと**1440x1080で30.1fps・
  ドロップ0**まで戻る（実測）。解像度を下げる必要はない。
- **ノード設定の変更手順**: `nodes.json`はマネージャー起動時に上書きされる。
  変更はマネージャーを**停止してから**編集し、起動する。

---

## 6. 2 台目 (192.168.99.209) の追加

2026-11-02 に X96Max_Plus_2101W (franklin / Android 9 / build 20230626) を
同じパイプラインで稼働させた。1 台目との違いは **起動のさせ方** だけ。

### 6.1 配備

```sh
cd ~/Desktop/work/dtv_android
scripts/build_stb_arm.sh                     # bin/stb_streamer, bin/timesync, bin/su_shim

export STB_SERIAL=192.168.99.209:5555
adb connect $STB_SERIAL && adb -s $STB_SERIAL root

./tools/deploy-pipeline.sh deploy --name X96Max-2 --mdns-channel 20
./tools/deploy-pipeline.sh init-install      # /vendor/etc/init/pix-stream.rc
adb -s $STB_SERIAL reboot
```

`torne-c` / `torne-b25` はリポジトリに含まれない。稼働機から
`artifacts/stb-bin/` へ控えておき、`deploy` がそこから配る。

### 6.2 起動は init が行う (アプリからは起動できない)

1 台目は `BootReceiver` が `su -c` でウォッチドッグを起動している。これは
SuperSU の常駐 `daemonsu` が代行するから成立する。2 台目の `su` は素の
setuid バイナリで、**アプリのプロセスからは実 root を取れない**:

```text
enter uid=10059 euid=0 gid=10059 egid=10059 caps=0 argc=3   # シムの入口
mkdir: '/dev/shm': Permission denied                        # ウォッチドッグ
Uid:	10059	10059	10059	10059                            # 実行中の sh
```

euid だけ 0 になり CapEff は 0 のまま (user namespace 内の root)。
`setuid(0)` は CAP_SETUID が無いため real uid を書き換えられず、
非 setuid の `sh` を exec した時点で euid が real uid に戻る。

そこで `vendor/etc/init/pix-stream.rc` の service として init から起動する:

```text
service pix-stream /system/bin/sh /data/local/tmp/pix-stream/pix-stream-watchdog.sh
    class late_start
    user root
    group root system
    seclabel u:r:su:s0
```

確認:

```sh
adb -s 192.168.99.209:5555 shell getprop init.svc.pix-stream   # -> running
./tools/deploy-pipeline.sh status
```

アプリの `BootReceiver` は残っているが、ウォッチドッグが先頭で
`/dev/shm` に書けるかを確認し、書けなければ
`/data/local/tmp/pix-stream/refused.log` に残して終了するので二重起動しない。
`id -u` は userns 内でも 0 を返すため判定には使わない。

### 6.3 端末の識別子

`/data/local/tmp/pix-stream/stb.conf` に置く (無ければ 1 台目と同じ既定):

```text
PIX_STB_NAME=X96Max-2
PIX_MDNS_CHANNEL=20
```

ウォッチドッグがこれを読み、`stb_streamer --name ... --mdns-channel ...` に渡す。
管理画面の「ネットワーク上のSTB」に表示される名前とチャンネルがこれで決まる。

### 6.4 mDNS インスタンス名の衝突 (修正済み)

以前は IP の下位 3 オクテットだけを名前に入れていたため、
192.168.99.57 と 192.168.99.209 が同じ `pixstb-c0a863` を名乗り、
**先に名乗った方が一覧から消える**状態だった。4 オクテットに修正した
(`pixstb-c0a86339` / `pixstb-c0a863d1`)。修正後のバイナリを 2 台目にだけ
配っているので、1 台目は旧名のままだが衝突しない。

### 6.5 同時運用の制約

NAS の管理プロセスはレコーダーを `--stream-id <channel>` で起動する。
つまり **stream-id はチャンネル番号**なので、2 台を同じチャンネルで
同時に録画すると `last_seq_<ch>` と出力が衝突する。
2 台目を本番運用するときは 1 台目と別のチャンネルを割り当てること。

### 6.6 確認済みの範囲

| 項目 | 結果 |
| --- | --- |
| init による起動 | 再起動 3 回とも `running`、部品は各 1 プロセス |
| アプリからの二重起動 | `refused.log` に記録して即終了 |
| 待機状態 | チューナープロセス 0、`/data` 使用量は増減なし |
| HTTP 40774 | NAS (192.168.99.80) から 200、他のホストから 403 |
| NAS の自動検出 | `X96Max-2 / 192.168.99.209 / ch35` として表示 |
| NAS へのノード登録 | `192.168.99.209-35` として登録、レコーダーが接続 |
| 実録画 | **不可(下記 6.7 の物理要因)** |

### 6.7 物理層の切り分け結果 (2026-11-02)

#### チューナー個体の不良だった

最初に挿していた CECH-ZD1 は **505MHz 以上が受信できない**個体だった
(13/15/17/18ch のみ受信、19ch 以上は `lock status=-110`)。
アンテナ線自体は別チューナーで正常動作を確認済み。1 台目で稼働していた
個体に交換したところ 6 局すべて受信できたため、**初期個体の故障**と確定。

#### このフィードの実際のチャンネル割当

標準の親局割当とは違うため注意。**35ch は存在しない**。

| ch | 周波数 | 局 (service_name から同定) |
| --- | --- | --- |
| 13 | 473MHz | NHK Eテレ |
| 15 | 485MHz | SBS 静岡放送 |
| 17 | 497MHz | **テレビ静岡** |
| 18 | 503MHz | 静岡朝日テレビ |
| 19 | 509MHz | 静岡第一テレビ (`Daiichi-TV1`) |
| 20 | 515MHz | NHK総合 (1 台目が録画していた局) |

局名の同定は `tools/sdt_names.py`。17ch は service_name のバイト列が
EUC-JP の「テレビ」(0xA5C6 0xA5EC 0xA5D3) の下位バイト `c6 ec d3` で
始まることから判別した。

ノードは `192.168.99.209-17` に登録し直し、端末側の `stb.conf` も
`PIX_MDNS_CHANNEL=17` に更新済み。

#### B-CAS は .209 でだけ通らない (未解決)

1 台目で動いていたチューナー(カード込み)を丸ごと .209 に挿しても、
初期化は失敗する:

```text
B25_STATS card_commands=2 failed=1
```

起動順を変えても同じ (0.3 秒間隔 / 連続起動 / ロック前後)。
カードは 2 コマンドまで応答するので検出はされている。
**同じ個体が 1 台目では `card_commands=4` で成功する**ため、カードでも
チューナーでもなく **.209 側の USB ホスト環境**が原因と判断した。

試す価値のある順:

1. 別の USB ポートに挿す (USB3 側のポートも試す)
2. セルフパワーの USB ハブ経由にする (給電不足の切り分け)
3. それでも駄目なら 2 台目での録画は保留し、1 台目にチューナーを戻す

#### 現在の状態

- 2 台目: パイプライン起動済み・待機 (`init.svc.pix-stream=running`、
  チューナープロセス 0、`/data` 変化なし)。ノードは `enabled=false`。
- 1 台目: チューナーが無いため `192.168.99.57-20` は停止中。
  チューナーを戻したら `POST /api/nodes/192.168.99.57-20/start` で再開。

### 6.8 6.7 の結論の訂正 (2026-11-02 夜)

6.7 に「カードが応答しない」「.209 の USB ホスト環境が原因」と書いたが、
**いずれも誤りだった**。切り分けのやり直しで分かったこと:

#### B-CAS は正常だった

`torne-c` のカード用ソケットに直接コマンドを送る `tools/cardprobe.c` を追加した。
同じカードで次の応答が返る:

```text
reset: ok
init: proxy_status=0 len=61
  card_status=0x2100  bcas_id=000ba70d2ba9
  => CARD OK (expected 0x2100)
```

`0x2100` は正常応答。**カードもリーダーも USB 経路も問題ない。**

これまで `B25_STATS ... failed=1` を「カード失敗」と読んでいたが、これは
**テストで b25 の標準入力に TS を流していなかった**ための失敗で、
カード初期化とは無関係だった。実パイプラインと同じ配管
（`torne-c --raw | torne-b25`）で流すと:

```text
B25_STATS card_commands=13 failed=0
TS_STATS stage=clear packets=229759 scrambled=0
```

復号も通る（`scrambled=0`、ffprobe で映像/音声を認識）。

#### 直したパイプラインのバグ: カードソケットの取り残し

`capture-agent.sh` の `stop_capture` は「停止中」判定で早期 return するため、
待機状態から再開したときに前回の `card.sock` が残ったまま `torne-c` を起動し、
`ERROR stage=card_socket status=98`（EADDRINUSE）で落ちていた。
`start_capture` の直前に必ず `rm -f` するよう修正した。

#### .209 のアプリ側: HEVC エンコーダが ge2d を開けない

`.209` だけ次のエラーが出ていた（`.57` には出ない）:

```text
E ge2d     : GE2D Error: open /dev/ge2d failed! error no 13
E AMLVHEVCENC: encode open ge2d failed, ret=0xffffffff
E ACodec   : [OMX.amlogic.video.encoder.hevc] ERROR(0x80001001)
```

`/dev/ge2d` は `system:system 0660`、エンコーダは `media.codec`(uid 1046) で
開けない。DAC で落ちるので SELinux とは無関係。ueventd が毎回作り直すため、
ウォッチドッグで `chmod 666 /dev/ge2d` するようにした。
これで `setupVideoEncoder succeeded` まで進むようになった。

#### .209 の残課題 (未解決)

現在 `torne-c` / `torne-b25` / カード / 復号 / エンコーダ初期化までは通るが、
**fMP4 断片が 1 つも生成されない**（ストリーマのスプールが 0 のまま）。
アプリはシンクに接続し `service id discovered` まで出ているので、
デコーダ出力とエンコーダ入力の受け渡しで止まっている可能性が高い。
同じ APK の `.57` は正常に録画できている（`pushed=53`）ので、
端末差（ファームウェア 2023 ビルドのコーデック HAL）を疑って継続する。

### 6.9 2 台目が録画できない原因と修正 (2026-11-02)

6.8 の残課題（fMP4 断片が 0）を詰めて解決した。原因は 3 つで、
**いずれもアプリ/エージェント側のバグまたは権限**だった。

#### (1) FIFO ホルダーのリーク — アプリがセッション終了を検知できない

`capture-agent.sh` は `clear.fifo` の書き込み端を保持するために
`( sleep 86400 ) > "$TUNE/clear.fifo" &` を起動するが、停止時に殺していなかった。
実測では **1 台あたり 6 個**の `sleep 86400` が残留していた。

```text
pid=3483 comm=sleep fd=/dev/shm/torne/clear.fifo
pid=4083 comm=sleep ...
...
```

ホルダーが生きている限り FIFO は EOF にならず、アプリの
`LiveTsStreamer` は `in.read()` で永久にブロックする。設計は
「チューナー停止 → FIFO が EOF → コーデック解放 → 再初期化」なので、
**この 1 個が残るだけで再初期化が起きなくなる**。

修正: `release_holder()` を追加し、`stop_capture` の先頭と
`start_capture` のホルダー生成前に必ず殺す（`holder.pid` で追跡）。

確認: スレッドダンプでブロック位置を特定できた。

```text
at java.io.FileInputStream.read(FileInputStream.java:307)
at com.example.dtvandroid.LiveTsStreamer.run(LiveTsStreamer.java:206)
```

#### (2) /dev/ge2d の権限 — エンコーダが起動できない

`media.codec`(uid 1046) は `/dev/ge2d`(system:system 0660) を開けず、
HEVC エンコーダが `0x80001001` で落ちていた。ueventd が毎回作り直すので、
ウォッチドッグで `chmod 666 /dev/ge2d` する（6.8 で追加済み）。

#### (3) カードソケットの取り残し（6.8 で修正済み）

#### 結果

2 台同時録画が成立した。

| | 状態 |
| --- | --- |
| .57 (ch20 NHK総合) | 断片 100 / 38,555 フレーム、`fps 29〜30`、`dropped=0` |
| .209 (ch17 テレビ静岡) | 断片 136 / 29,227 フレーム、`fps 27〜29`、`dropped=0` |
| STB の eMMC | 両機とも変化なし（.209 は 307MB のまま） |

`frags` はストリーマが RAM スプールへ積んだ断片数。`dropped=0` は
NAS へ送る途中で捨てた断片がないことを意味する。

セッションの再初期化も確認できた: FIFO が EOF になるとアプリは
`LiveTsStreamer` を抜けて再起動し、`tsPackets` のカウンタが新しい
セッションとして 0 から数え直される（.209 で観測）。

`.209` の fps が 26〜28 とやや低い（.57 は 29〜30）。同じ SoC なので
エンコーダのスケーリング経路の差と思われるが、`totalDropped=0` で
録画は成立している。長時間運転で欠落が出るようなら追う。

#### 再発防止のメモ

- 一時ファイル（fifo ホルダー・pid ファイル・ソケット）は
  **停止経路で必ず解放する**。待機状態からの再開でも掃除する。
- デバイスノードの権限は ueventd が毎回作り直すため、
  ブート時のどこかで再適用する必要がある。

### 6.10 2 台目の fps 改善 (2026-11-02)

#### 症状と原因

`.209` は 25〜26fps、`.57` は 29〜30fps で、同じ APK・同じ設定なのに差があった。
同一の TS ファイルを PES トランスコード経路で流す比較では:

| | 1440x1080 → 1920x1080 HEVC |
| --- | --- |
| `.57` | 22.5fps (4933ms) |
| `.209` | 19.0fps (5843ms) |

**エンコーダ自体が約 18% 遅い**（ファームウェア差による個体差）。
これは変えられないので、**無駄な仕事を減らす**方向で解決した。

#### 原因の一つ: アップスケールして符号化していた

放送は 1440x1080 なのに、アプリは 1920x1080 へ拡大してから符号化していた。
水平方向に 33% 拡大するだけで情報は増えず、画素は 44% 多い
(1440x1080 = 155 万画素 / 1920x1080 = 207 万画素)。

`dstW=1440 dstH=1080` にすると、この無駄が消える。

| | 設定 | 実測 fps |
| --- | --- | --- |
| 変更前 | 1920x1080（アップスケール） | 25〜26 |
| 変更後 | 1440x1080（ネイティブ） | **28〜29** |

画質は落ちない（元の解像度をそのまま符号化するため）。NAS 側でも
`ffprobe` で `hevc 1440x1080` を確認済み。

#### 端末ごとの符号化設定: encode.conf

`$APP/encode.conf` があれば、エージェントがその値で上書きする。

```sh
# .209 のみネイティブ解像度で符号化する
adb -s 192.168.99.209:5555 shell \
  'printf "PIX_DST_W=1440\nPIX_DST_H=1080\n" > /data/local/tmp/pix-stream/encode.conf'
```

`.57` は 1920x1080 のままで 30fps を維持できているので触っていない。
容量を減らしたい場合は `.57` にも同じ設定を入れるとよい
（同じ画質をより少ないビットレートで保存できる）。

#### 運用上の注意（今回踏んだもの）

- **`strategy.txt` はアプリから削除できない**。`/data/local/tmp` が
  `0771 shell:shell` なので、アプリの uid では消せない。残っていると
  起動時に PES テスト経路に入ってしまい、**ライブ録画が始まらない**。
  手動テストの後は必ずホスト側から消すこと。
- **`am force-stop` でアプリを止めると MPEG2 デコーダが解放されない**。
  次回起動で `dec=0`（デコーダが出力しない）になる。必ず再起動で解放する。

### 6.11 録画設定（チャンネルとフォーマット）の選択 (2026-11-02)

管理画面 (http://192.168.99.80:40776/) から、STB ごとにチャンネルと符号化形式を選べる。

| 項目 | 選択肢 |
| --- | --- |
| チャンネル | 受信できるチャンネルのプルダウン（例: 17 テレビ静岡、BS15_0 ＮＨＫ ＢＳ） |
| 受信 | 地上波のみ / 地上波＋BS/CS（STBのチューナーに合わせる） |
| コーデック | HEVC (H.265) / H.264 |
| 解像度 | 1920x1080 / 1440x1080 (放送ネイティブ) / 1280x720 |
| ビットレート | bps を直接入力 (200k〜20M) |
| レート制御 | VBR (画質優先) / CBR (ビットレート一定) |
| GOP | キーフレーム間隔 1〜30 秒 (断片の切れ目になる) |

エンコーダに渡せるのはこの 5 項目。H.264 のプロファイルは SoC が
Baseline しか受け付けず (Main/High は `0xffffffc3` で拒否)、HEVC も
Main 固定なので、選択肢には出していない。フレームレートは放送波に
合わせて 30fps 固定 (下げると取りこぼしになる)。

各STBの「設定」を押すと録画設定が開き、チャンネルと形式をまとめて変えられる。
「適用」でその場で切り替わり、STB の再起動は不要。チャンネルを変えた場合は
録画中の番組がそこで確定し、新しいチャンネルで録画が始まる。

チャンネルは文字列で管理する。地上波は `17`、BS は `BS15_0` のような
トランスポンダ表記で、ノードIDも `host-ch`（例: `192.168.99.124-BS15_0`）に
なる。BS は同じトランスポンダに複数のサービスが乗るため、録画されるのは
そのトランスポンダの先頭サービス（プルダウンに出る局名）になる。

#### 指定が STB まで届く経路

```text
管理画面 (manager)
  └ POST /api/nodes/<id>/settings
      └ recorder を再起動 (--channel --codec --width --height --bitrate --rate-mode --gop)
          └ STB へ GET /v1/streams/<ch>?after=N&codec=..&w=..&h=..&bitrate=..&mode=..&gop=..
              └ stb_streamer が /dev/shm/pix/capture.req に書く
                 例: "start 17 hevc 1440 1080 5000000 vbr 5"
                  └ capture-agent が live.conf を書き換えてチューナーを再起動
                      └ アプリが次のセッションで live.conf を読む
                         (dstW/dstH/codec/bitrate/bitrateMode/gopSec)
```

`capture.req` の形式は省略可能なので、古いレコーダー (書式なし) でも
従来どおり STB 側の既定値 (`encode.conf`) で動く。

#### 実測

| 設定 | 結果 |
| --- | --- |
| `.209` HEVC 1440x1080 5Mbps | 27.5〜29.2fps |
| `.209` H.264 1280x720 4Mbps | 29.6〜30.2fps (ffprobe で h264 を確認) |
| `.57` HEVC 1920x1080 5Mbps | 29.9〜30.1fps |

`.209` はエンコーダが遅い個体なので、1920x1080 ではなく
**1440x1080 (放送ネイティブ) を既定**にしている。H.264 を選ぶと
1080p では 30fps を割るため、720p と組み合わせるのが安全。

#### ソースの置き場所

NAS 側の C ソースはリポジトリの `nas_src/` に置いた
(`pix_stream_manager.c`, `pix_stream_recorder.c`, `cm_queue.c`, `cm_queue.h`, `cJSON.h`)。
NAS には cJSON の開発用ヘッダが無いため、ヘッダだけ同梱している。
ビルドは NAS 上で:

```sh
cd /dev/shm/pix-stream-build
gcc -O2 -Wall -I. -o pix_stream_manager pix_stream_manager.c -l:libcjson.so.1 -lpthread
gcc -O2 -Wall -I. -o pix_stream_recorder pix_stream_recorder.c cm_queue.c \
    feature_sidecar.c -l:libcjson.so.1 -lpthread
```

CM対応版では受信fMP4をtmpfsのキューへ渡し、MP4化の前にCMを検出する。
CM処理用サービスの配置・起動と受信子プロセスの交換は
[CM_SERVICE.md](../experiments/cm-compare/CM_SERVICE.md)を参照。
以前の`fmp4_to_mp4.c`をリンクするビルド手順は使わない。

#### 切替時にデコーダが固まる問題を修正

フォーマットを変えるとセッションが切れて再初期化されるが、そのとき
「`fed` は増えるのに `dec=0`」になって再起動するまで復旧しない症状があった。
原因はアプリの解放処理で、`MediaCodec.stop()` は呼んでいたが
**`release()` を呼んでいなかった**こと。この SoC は MPEG2 デコーダが
単一インスタンスなので、stop だけではハードウェアが GC のファイナライズまで
掴まれたままになる。

修正: `finally` で `stop()` のあとに `release()` を呼ぶ。さらに起動失敗時は
1 秒待ってから再試行するようにした。修正後は VBR↔CBR・GOP 2↔5 の変更が
**再起動なしで**反映されることを実機で確認した。

なおコーデック自体の切替 (HEVC↔H.264) は、エンコーダの作り直しが入るため
反映までに 1〜2 分かかることがある (数回再試行して落ち着く)。

#### フォーマット変更直後に「応答なし」になる件 (2026-11-02)

管理画面から形式を変えると、STB 側でセッションが張り直される。
このとき **デコーダが前のセッションを掴んだまま無出力**になることがあり、
その状態では

- ストリーマは応答するが断片が出ない
- レコーダーは数十バイトで切れて再接続を繰り返す
- 管理画面の状態取得が一時的にタイムアウトして「応答なし」に見える

という見え方になる。原因は単一インスタンスの MPEG2 デコーダが
解放待ちで詰まることで、従来は再起動するまで復旧しなかった。

対策としてアプリに**無出力の監視**を入れた。10 フレーム以上デコーダへ
投入しているのに 5 秒間 1 フレームも出てこなければ、そのセッションを
諦めて切断し、次のセッションをやり直す。

```text
LIVETS attempt 7 ran=5302ms
NG:decoder produced no frame in 5s
LIVETS attempt 8 ran=9262ms   <- ここで復帰
```

復帰まで実測で 10〜20 秒。再起動は不要になった。
修正後、両機とも `NG:` は 10 分間で 0 件、`fps` は 28〜30 で安定している。

> 形式を変えた直後に管理画面が一瞬「応答なし」になっても、数秒待てば
> 自動で復帰する。連打すると復帰が遅れるので、1 回変えたら状態が
> 「録画中」に戻るのを待つこと。

### 6.12 2 台同時稼働で 2 台目だけが無出力になる件 (2026-11-02)

2 台を同時に録画させると、`.57` は 30fps で安定するのに `.209` だけが
`fps=0.0` のまま止まることがあった。プロセスは全部生きており、FIFO には
2.4MB/s 流れているのに、アプリのログには `diag` すら出ない状態だった。

#### 原因 (1): FIFO はバイト列で、アプリ側に再同期がなかった

`read` ループは 188 バイトの窓を埋めてから `pkt[0] != 0x47` なら
**窓ごと捨てる**だけだった。FIFO はパケットを保存しないので、セッションの
開始位置がパケットの途中だと、以後すべての窓が捨てられ続ける。

- `tsPackets=0` のまま毎秒 12,000 パケットを読み続ける (CPU 19%)
- `readyPes` が永遠に完成しないので、内側ループから出られず `diag` も出ない
- たまたま窓の先頭が `0x47` になると、ゴミを PAT として解釈し
  `service id discovered: 46656` のような値が出る

```text
/data/anr/trace_03 (kill -3 のダンプ)
"Thread-2" ... Native
  at libcore.io.IoBridge.read(IoBridge.java:501)
  at java.io.FileInputStream.read(FileInputStream.java:307)
```

対策として、窓の先頭が同期バイトでなければ **1 バイトずつずらして**
`0x47` を探すようにした。ずらした回数は `diag` の `resync=` に出る。

あわせて、読み取りを 64KB のバッファにまとめた。従来は 188 バイトごとに
`read()` を呼んでおり、毎秒 12,000 回のシステムコールになっていた。

#### 原因 (2): 無出力監視の時計がセッション開始時刻だった

6.11 で入れた「無出力 5 秒でセッションを諦める」監視の起点が
セッション開始時刻だった。チューナが起動する前にセッションが開くと、
最初の PES が届いた瞬間に「17 秒無音」と判定して落としてしまう。

落とすと FIFO の読み手が消える → `torne-b25` が EPIPE で終了 →
`torne-c` が `consumer_closed` で停止 → エージェントがチューナを再起動、
という連鎖で **約 20 秒周期の再起動ループ**になる。管理画面からは
「録画中なのに断片が増えない」ように見える。

対策は 2 つ。

- 監視の起点を「最初にデコーダへ投入した時刻」に変更
- 「PES が 30 秒完成しない」を別条件で打ち切る (こちらは PES 完成でリセット)

修正後は、チューナ起動が遅れてもセッションは落ちない。

#### 原因 (3): 2 台目は読み取りが追いつかずチューナの受信キューが溢れていた

上の 2 つを直したあとも、`.209` だけ約 5 分おきに
`ERROR stage=receive_queue_full status=-105` でチューナが落ちていた。

```text
STATS received=694947840 written=672276804 overflows=1
      queue_peak=8355840 queue_capacity=8388608 max_write_s=0.367825
```

書き込み (672MB) が受信 (694MB) に 3% 足りず、8MiB のキューが
318 秒かけて満杯になり、最後は 0.37 秒の書き込み待ちで溢れていた。
アプリは 188 バイトごとに `read()` を呼んでおり、これが毎秒 12,000 回の
システムコールになっていた。64KB のバッファにまとめたところ
`fps` が 28〜29 から 30 に戻り、キューも溢れなくなった。

> 188 バイト単位の `read()` は `strace` で数えた値ではなく、
> パケット数 (毎秒 12,000) と 1 パケット 188 バイトという仕様からの計算値。
> 効果は `fps` とチューナの継続時間で確認している。

#### 確認 (2 台同時・連続運転)

16 分間の連続運転での実測値。

| 項目 | `.57` (ch20) | `.209` (ch17) |
| --- | --- | --- |
| `fps` | 29.7〜30.3 | 29.5〜30.7 |
| `resync` | 0 | 0 |
| `tuner disappeared` | 0 件 | 0 件 |
| スレッド数 | 32 で一定 | 30 で一定 |
| RSS | 約 135MB で横ばい | 約 143MB で横ばい |
| NAS 録画 | 番組名つきで保存 | 番組名つきで保存 |

> 2 台目が止まったときは、まず `diag` が出ているかを見る。
> 出ていなければ入力の同期ずれ (この節)、出ていて `dec=0` なら
> デコーダの詰まり (6.11)、`fps` だけ低いなら符号化設定 (6.10) を疑う。

### 6.13 受信キューの溢れを再起動ではなくフレームドロップで吸収する (2026-11-02)

6.12 の修正で `fps` は 30 に戻ったが、それでも長時間運用ではまれに

```text
ERROR stage=receive_queue_full status=-105
```

でチューナが落ちることがあった。エンコーダが放送レートにわずかに届かない
時間が続くと、その差が FIFO → `torne-b25` → `torne-c` の受信キュー (8MiB)
に積み上がり、最後は 0.4 秒程度の書き込み待ちで溢れる。溢れるとチューナが
終了し、5〜10 秒の欠落になる。

そこで、**参照されない B ピクチャだけを落とす**ようにした。

- MPEG2 のピクチャヘッダ (`00 00 01 00`) から `picture_coding_type` を読む。
  放送波は `I B B P B B ...` で、実測でもサンプル 70 フレームのうち
  P=17 / B=53 と 2/3 が B ピクチャだった
- 1 フレームの処理時間 (デコーダ投入から `eglSwapBuffers` まで) の移動平均が
  33ms を超えたら「借金」を積み、1 フレーム分になったら次の B を捨てる
- 捨てたフレームの分も表示時刻だけは進める。止めると映像の時刻が実時間から
  ずれて音声とずれるため

```text
diag ... fps=22.4 ... drop=43 work=18     <- 1.5 秒フリーズ直後 (借金を返している)
diag ... fps=30.1 ... drop=79 work=18     <- 数秒で復帰。チューナは落ちていない
```

#### 確認

- 1.5 秒のフリーズを 1 回: `drop` が 10 増え、`fps` は数秒で 30 に戻り、
  `tuner disappeared` は増えない
- 0.7 秒のフリーズを 18 回 (45 秒のうち 23% 停止): 8MiB のままでは
  これを超えて 2 回再起動した

そこで `torne-c` の受信キューを 8MiB から 32MiB に広げた
(`capture-agent.sh` の `PIX_TUNER_BUFFER_MIB`)。32MiB は約 15 秒分で、
同じ 18 回のフリーズを入れてもチューナは落ちず、アプリは B を 63 枚
落として数秒で 30fps に戻った。

```text
READY queue_bytes=33554432 transfers=8 transfer_bytes=61440
```

キューを広げても、追いついている間は溜まらないので遅延は増えない。
機種ごとに変えたい場合は `encode.conf` か環境変数で
`PIX_TUNER_BUFFER_MIB` を上書きする (`torne-c` の上限は 32)。

> 落とすのは B ピクチャだけなので、P/I を落としたときのように次の I まで
> 映像が崩れることはない。`drop` が増え続ける場合はまず符号化設定
> (6.10 / 6.11) を見直すこと。

## CM特徴量のsideband (`feature_mode`)

STBは録画中に映像と音声の特徴量を測り、`pixf`としてストリームへ差し込む。
NASの受信側はそれを`/dev/shm/pix_stream/feat_<pid>_<ms>.bin`へ書き出し、
jobの`features`として渡す。STBはcutを決めない。

### モード

| 値 | 動き |
|---|---|
| `off` | sidebandを読まない。従来どおり全編`cm-scan` |
| `shadow` | 従来どおり`cm-scan`で判定し、sidebandとの差を`feature_shadow`へ記録する |
| `apply` | 局ロゴのcacheとsidebandが揃う場合だけ`cm-scan`を省く。揃わなければ`shadow`と同じ経路へ戻る |

切替は `/home/ku/pix-cm/switch-mode.sh off|shadow|apply` で行う。
worker 4本を再起動する。

### 判定の段階

`apply`は次の順に試し、決まった段階で止める。

1. 局ロゴのcacheがある場合、そのマーカーで判定する
2. マーカーで判定できない場合、sidebandの行だけで`scenario`（無音と長さ）で判定する
3. どちらも不可なら`cm-scan`を実行して従来どおり判定する

2は`feature_allow_scenario`（既定true）で切り替える。マーカー無しで決めた場合は
`detection.feature_note='no usable marker'`、`feature_source=capture`が記録され、
マーカーは保存しない。

cutが記録の範囲`[0, duration]`を外れた判定は採用しない。時刻の対応が崩れて
いる可能性があるため、`cm-scan`へ戻す。

### 電源投入時の起動

Linux機（X96Max_P、192.168.99.109）は、2026-11-14から**電源を入れるだけでLinuxが起動する**。U-Bootの`bootcmd`を`run linux_boot`へ変更して保存したため。

| 変数 | 値 |
|---|---|
| `bootcmd` | `run linux_boot`（電源投入時に実行） |
| `linux_boot` | SDカードからImageとDTBを読み、`booti`する一連のコマンド |
| `bootcmd_android` | 元のAndroid起動（`ddr_auto_fast_boot_check 6 0 0 50;run storeboot`） |

Androidへ戻すときは、シリアルでU-Bootのプロンプトに入り次を送る。

```text
setenv bootcmd 'run bootcmd_android'
saveenv
reset
```

`tools/uboot_set_boot.py --target android --reboot-from-linux`でも同じことができる。Linuxへ戻すときは`--target linux`。詳細は`board/x96max/uboot-boot-default.md`。

**SDカードを抜くとLinuxは起動しない。** その場合も同じ手順でAndroidへ戻せる。

### アドレスと時計

アドレスは**DHCPサーバ側のMAC予約**で固定している。デバイス側は`iface eth0 inet dhcp`。

| 項目 | 値 |
|---|---|
| MACアドレス | `90:0e:b3:0e:17:1b` |
| 予約アドレス | `192.168.99.109` |

MACはefuse由来で、U-Bootが`mac`変数としてデバイスツリーの`local-mac-address`へ渡す。OS側で付け替える設定は無く、`addr_assign_type`は0（恒久）である。電源再投入後も同じ値になることを確認済み。

基板にRTCが無くNTPクライアントも無いため、時計は起動時に`/etc/init.d/S41settime`がNASの`/api/time`から取得する。`S40network`より後ろに置いているのは、通信が立ち上がってからでないと取得できないため。

### 録画が止まったときの復旧

映像のデコードが`Invalid frame dimensions 0x0`で失敗し、パイプラインが数秒で落ち続けることがある。TSのパケットは届いており、スクランブルビットも0だが、映像ESにMPEG-2のシーケンスヘッダが無い状態になる。

2026-11-15未明にこの状態が約10時間続いた。チューナー単体では受信でき、復号チェーンも単体では動くため、切り分けに時間がかかった。復旧した手順は次のとおり。

1. パイプラインを止め、`pix-stream-linux`、`siano-ts`、`torne-b25`、`eit_watch`、`ffmpeg`、`stb_streamer`をすべて`kill -9`する
2. USBのチューナーを再初期化する

```sh
echo 1-1 > /sys/bus/usb/drivers/usb/unbind
sleep 4
echo 1-1 > /sys/bus/usb/drivers/usb/bind
sleep 5
siano-ts --list          # bus=1 address=2 のように出れば認識済み
```

3. `/dev/shm/pix/launcher.lock`を消してから`/etc/init.d/S97pix-stream start`

復旧後は`wget -qO- http://127.0.0.1:40774/v1/status`が`"capturing":true`を返し、10秒あたり約300フレーム、6Mbps前後で伸びる。

ソフトウェアの再起動では直らず、USBの再初期化で直った。同じ症状では、まずこれを試す。

### 映像と音声の時計

映像と音声は**同じ放送時計**に載せる。`pix-stream-linux`の`FC`/`VF`に`setpts=N/FR/TB`を入れず、1段目に`-copyts -start_at_zero`を付ける。

以前は映像だけを`setpts=N/FR/TB`で0起点に作り直していたため、`-c:a copy`で通す音声が放送時刻（数万秒）のまま取り残された。長時間動かすほど差が開き、次の症状が出た。

- 音声の開始位置が映像から数時間ずれる（19:00の録画で17,311秒）
- AACのデコードが失敗する（同録画で25万行の誤り）
- CM解析が音声の無音を使えず、`source_av_duration_mismatch`で見送られる

2026-11-14の修正後、12分間の連続動作で開始・終了とも0.02秒以内に収まっている。

### 局ごとの傾向

マーカーが使えるかは局によって違う。実測では次のとおり。

| 局 | cacheのROI | 中身 | `overlay`到達 |
|---|---|---|---|
| テレビ静岡 | 32,40 | 時計・天気（以前は1184,48の局ロゴ） | 届く場合あり |
| SBS | 40,24 | 番組テロップ | 届かない |
| NHK | 40,24 | 未確認 | 届く場合あり |

時計や番組テロップは常時表示でCM中も残るため、CM区間の手掛かりにならない。局ロゴが取れる局でのみ`overlay`が成立する。

`feature_marker_free_services`には、マーカー無しでも通常経路と一致することを実測で確かめた局だけを入れる。テレビ静岡（35864）を登録済み。他局は追加前に同じ照合を行う。

### 採用の判断

`feature_shadow.decision_agreement.same`を見る。同じoverlayを使い、
sidebandの行とNAS測定の行からそれぞれ`cm_rules.analyze`を回してcut区間が
一致したかを示す。輝度や行数の一致は補助指標で、判定には使わない。

`apply`で実際に`cm-scan`を省いたかは`detection.feature_source`で分かる。
`capture`なら省略、`nas`なら実行している。

### 戻し方

`/home/ku/pix-cm/switch-mode.sh shadow` を実行する。`apply`で本編の誤カット、
A/Vずれ、`totalDropped`の増加、`cm-verify`の失敗が出た場合は、記録を残して
すぐ`shadow`へ戻す。

### 受信側の前提

- `cm-queue`は`features`を無視できる。旧jobは従来どおり処理される。
- `feat_*.bin`は区間ごとに作られ、処理後に消える。
- sidecarの区間長が媒体と合わない場合は採用しない。`feature_reason`に理由が残る。
