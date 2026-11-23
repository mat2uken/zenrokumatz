# 全録3台の動作確認と修正（2026-11-21 23:42 〜 11-22 00:40）

3台（unit1=.109 テレビ静岡、unit2=.123 BS11、unit3=.56 SBS）の動作状況確認と、
見つかった問題の修正。元の依頼は「1台目がBS11のつもりがNHK BSになっている」。

## 1. unit2（.123）が NHK BS を録画していた → 衛星の再編が原因 → 修正済み

### 症状

`zenroku/recordings/` の BS 担当フォルダが `BS_400101_ＮＨＫ　ＢＳ` になり、
受信中の番組（/v1/program）も service=101（NHK BS）。
BS11（service=211, onid 4）の録画は 11/20 で途絶えていた。

### 原因

- unit2 は CFW 経路で `PIX_CFW_CHANNEL=BS15/0x40f1` にチューニングしており、
  ロック自体は正常（TSID 0x40f1 を受信）。
- ところが 2026-11 の衛星再編で**トランスポンダの中身が入れ替わった**:
  実機スキャン（`pix-cfw-grab scan --sat`）+ 各 IF の PAT 解析の結果:

| IF | TSID | PAT の programs | 中身 |
| --- | --- | --- | --- |
| BS1 | 0x4010 | 151,152,153 | BS朝日系 |
| BS3 | 0x4030 | 191,791,792 | WOWOW系 |
| BS5 | 0x4450 | 192 | WOWOW系 |
| **BS9** | **0x4090** | **211** | **BS11** |
| BS13 | 0x40d0 | 141,142,143 | BS日テレ系 |
| BS15 | 0x40f1 | 101,102,700… | **NHK BS** |
| BS19 | 0x4730 | 245,849 | BSテレ東系 |
| BS21 | 0x4750 | 252 | BS12系 |
| BS23 | 0x4770 | 256 | 放送大学系 |

  BS7 / BS11 / BS17 の IF はロックしなかった（再編で使われなくなった模様）。
  旧実測「BS15/0x40f1 = BS11」は当時は正しかったが、現在は NHK BS が乗っている。

### 修正

- `.123` の `/etc/default/pix-stream-linux` を `PIX_CFW_CHANNEL=BS9/0x4090` に変更し
  S97 を再起動 → `/v1/program` が service=211（BS11番組）に戻ったことを確認。
- リポジトリの `board/x96max/ramboot-overlay/etc/default/pix-stream-linux` も同値に
  更新（コメントに再編の経緯を記載）。

### 注意: 再起動で元に戻る → イメージ再ビルドして焼き直し済み（2026-11-22 01:35）

STB は RAM rootfs のため、ランタイムの変更は再起動で消える。対応:

- **リポジトリ側の設定が実機より古かった**: repo の unit2 設定は
  `PIX_TUNER=dt295`（純正経路）のままだった。実機は 11/20 の CFW 移行時に
  ランタイムで `dt295-cfw` に変えており、リポジトリが追従していなかった。
  これをそのまま焼くと CFW 経路が消える（= BS15 の再発どころか録画経路自体が
  変わる）。repo を実機の実態（dt295-cfw + BS9/0x4090）に合わせた。
  [tests/test_capture_continuity.py](../tests/test_capture_continuity.py) と
  [tests/test_ramboot_profiles.py](../tests/test_ramboot_profiles.py) の
  ガードテストも unit2/unit3 の CFW 移行後に更新していなかったので修正した。
- **イメージ再ビルド**（NAS の docker、X96MAX_UNIT=2、identity は
  work/ramboot-identity を /identity にマウント）→ テスト 274 件 OK。
  ビルドスクリプトの DTB 配備漏れ（SHA256SUMS に listed だが export 無し）
  も修正（[scripts/build-in-container.sh](../scripts/build-in-container.sh):
  manifest 由来で配備するように変更）。
- **焼き直し**: 起動媒体は個体番号 AA010206130733341409（SanDisk USB, sda,
  62533286 sectors）で照合。旧 Image/rootfs は Mac の
  `work/backup-unit2-boot-20261122/` と NAS の `~/backup-unit2-boot-20261122/`
  に退避済み。新 Image + rootfs.cpio.uboot を書き込み、media 上の SHA256 が
  ローカルと一致することを確認（DTB は同一ハッシュのため触らず）。
- **再起動検証: 完了（2026-11-22 01:24）**。再起動後約 90 秒で SSH 復帰、
  新イメージの設定（dt295-cfw + BS9/0x4090）で立ち上がり、CFW プロセス群起動、
  `tuning BS9/0x4090 = IF 1202920 kHz / locked, TSID 0x4090`、
  `/v1/program` が service=211（BS11）を返し、NAS recorder も自動再接続した。
  SSH ホスト鍵も identity から引き継がれている（known_hosts 変更なし）。

## 2. pix-stb-guard が 15 秒周期でクラッシュし続けていた → 修正済み

- `pix-stb-guard.service` が `/home/ku/pix-stream-20261101/pix-stb-guard.py` を
  起動しようとして `No such file or directory` で落ちる（restart counter 3091 回）。
- 11/20 のソース整理でファイルが移動されたのに、unit ファイルのパスが更新されて
  いなかった。**監視・復旧が丸1日以上死んでいた**。
- 正規ソース `nas/src/pix-stb-guard.py` を同パスに配備（self-test 通過）して再起動。
  現在 active、ノード 3 台を監視している。

## 3. part 2 以降のセグメントが無音に破棄されていた → コード修正・配備済み

### 症状

recorder は `--max-segment-sec 7200`（2時間）ごと、および接続断のたびに再起動し、
番組の残りを「同じフォルダの part 2」として続ける設計（11/20 の修正）。
ところが 11/21 の実データでは:

| 番組 | 欠落区間 |
| --- | --- |
| お宝を掘り当てろ（NHK BS） | p02（22:43–23:00 の 17分）が無い |
| ジャンオニ（テレビ静岡） | p02（22:43–23:10 の 27分）が無い |
| 情報7days（SBS） | p02（22:41–22:54 の 13分）が無い |

manager には `continuing programme as part 2` のログがあり、CM キューの completed
エンベロープも存在するのに、ファイルが書かれない。

### 原因（コードレベルで特定）

- recorder が CM ジョブのエンベロープ ID に **recordingId** を使う
  （`pix_stream_recorder.c` の `job_id`）。part 継続時は recordingId を引き継ぐため、
  part 1 と part 2 が同じ ID になる。
- `cm_service.py` の `run_queue` は `queue_jobs` テーブルで同じ ID の完了実績を
  見つけると**処理をスキップして前回の結果を再利用**し、その後
  入力 fMP4 を `unlink` する。→ part 2 以降は毎回この経路で破棄される。

### 修正

- `nas/src/pix_stream_recorder.c`: ジョブ ID を **partId**（パートごとに新しく振る
  UUID）に変更。part 単位で一意になり、既存の「再配信時に結果を再利用」の挙動は
  同一パートに対してだけ効く。
- `nas/build.sh`: ローカルビルドの追加ソースに `$src/` プレフィックスが付いて
  いなかったので修正（リモートビルドは元から正しい）。
- NAS 上でリモートビルドして配備（バックアップ `*.before-20261121-2359`）、
  manager + guard を再起動。3ノードとも recording 状態で復帰。

### 追加修正: 日付をまたぐ番組の分割（2026-11-22 00:46 配備, backup *.before-20261122-0046）

part 2 の消失を直した直後の実データで、別の分割が見つかった。
土ドラ（23:40–00:35）の part 2（00:00–00:35）は
`2026/11/22/000003_土ドラ…/` に**別録画**になった。原因: フォルダの day と時刻を
**セグメントの取得開始時刻**から作っており、日付をまたぐ番組は part 2 が翌日の
フォルダに入り、find_continuation が part 1 を見つけられない。毎晩の深夜番組で
起きる。

修正: フォルダの day / HHMMSS は**番組の startAt** から作る（番組情報が無い場合は
従来どおりセグメント時刻にフォールバック）。ファイル名は設計どおり各パートの
取得時刻のまま。あわせて `cm fmp4 queued` ログが cJSON 解放後のポインタを印刷して
いた使用後解放（ログの UUID が文字化けする既存バグ）を直した。

### 検証

- **partId 修正**: 00:46 の配備後、BS11 `BS_400211_ＢＳ１１イレブン/003001_生徒会にも穴はある！/…_p01.mp4`
  のようにフォルダが正しく作られ、新バイナリでの p01 公開を確認。
  継続（part 2 同一フォルダ書き込み）の実機確認は次回の max-segment 再起動
  （recorder 再起動から 2 時間後、02:46 頃）で行われる。確認方法:
  `ls .../2026/11/22/*/ | grep p02` と manager journal の
  `continuing programme as part 2` の後のファイル存在。
- **フォルダ名＝番組開始時刻**: 生徒会にも穴はある（00:30 開始）のフォルダが
  `003001_` になった（セグメント開始 00:47 ではない）→ 番組 startAt ベースの
  命名が効いている。日付跨ぎの実機確認は翌日の 23 時台以降の番組で行う。

## 4. pix-bs-deinterlace が 15 分ごとに失敗していた → 修正済み

- `source_root_missing: /mnt/xblk/video/pix-smb400/recordings` — 旧録画ルートを
  見に行っていた。`/home/ku/pix-bs-deinterlace/pix-bs-deinterlace.py` の
  `SOURCE_ROOT` を `/mnt/xblk/video/zenroku/recordings` に変更し、手動実行で
  `{"event":"done","processed":0}`（対象無し）を確認。

## 5. 11/20 までの録画が削除されていた（要確認・復旧可能）

- `zenroku/recordings/2026/11/` 配下に 21 日だけ残り、11/08–11/20 の録画フォルダ
  （BS11 の 温泉若おかみ 11/20、ロメリア戦記 11/19 等を含む）が存在しない。
- JuiceFS の trash に残っている:
  - `/mnt/xblk/.trash/2026-11-20-16/`, `2026-11-20-19/` — 11/08–11/16 分
  - `/mnt/xblk/.trash/2026-11-21-14/` — **11/17–11/20 分（約 359GB）**
- 削除元は 192.168.99.20（この Mac）からの SSH セッションが確認できる時間帯。
  意図的な整理（保存期間ポリシー）か、事故かは本人に確認する。
  trash の保持期間が過ぎると復旧できなくなるので、早めの判断が必要。

## 6. 残っている問題

- ~~**EPG が 4 日以上古い**（`epg_stale` 96h）~~ → **§8 で解決済み**
  （2026-11-22 09:00、mirakc 依存を廃止して STB の EIT から供給するよう置き換えた）。
- **find_continuation の公開レース**: 2つの staging がほぼ同時に publish されると、
  先方のフォルダがまだ無いため別フォルダ（別 recordingId）になる。
  11/21 19:00 の熱狂マニア（190000 と 204143 の 2 フォルダ）がこの例。
  内容は両方保存済みで損失はないが、同一番組が 2 フォルダに分かれる。
- 11/20 以前に分割済みだった旧フォルダの統合は未実施（trash からの復旧判断の後）。
- unit2 のイメージ再ビルド・焼き直し（§1 の注意）→ §1 のとおり完了済み。

## 8. EPG ホスト廃止（2026-11-22 02:00〜09:05、mirakc /.40 依存の完全除去）

設計は [epg-decommission-design.md](epg-decommission-design.md)。作業記録は
`docs/aegis/work/2026-11-22-epg-decommission/`。

### 経緯

- `.40`（mirakc）は 11/21 10:45 頃から不通で、EPG は 11/17 14:50 で止まり、
  監査が `epg_stale`（96h 超）を報告し続けていた（§6）。
- mirakc の代替を STB 3 台の eit_watch に作った:
  - eit_watch が EIT schedule 表（0x50–0x5f）を event_id 単位で蓄積し、
    `/dev/shm/pix/epg.json`（tmpfs）に書き出す。3 分未満の項目は番組として
    扱わない。過去 24 時間より前を間引き、上限 2048 件。
  - stb_http に `GET /v1/epg` を追加（`/v1/program` と同じファイル返し）。
  - NAS の新ブリッジ `pix-epg-bridge`（systemd、127.0.0.1:40775）が 3 台の
    `/v1/epg` を 60 秒ごとに取得し、旧 mirakc と同じ API 形状
    （`/api/services`・`/api/programs`、合成 ID 規則は同一）で pix-archive に渡す。
    STB が死んでも前回データを提供し続ける。
  - pix-archive は `--mirakc http://192.168.99.40:40772` を
    `--epg http://127.0.0.1:40775` に変更（ソースは `nas/src/pix_archive.c` に
    canonical 化し、NAS 上で再ビルド）。collect / ingest / API は無改修。
- **ビルド上の落とし穴（要記録）**: NAS docker のビルドボリュームでは
  local パッケージ（stb-streamer）の再 rsync がスキップされる。
  そのため最初の 3 本の rootfs に旧 eit_watch が入った。
  unit1 はその rootfs で 1 回再起動した（epg 無効のまま起動、録画は継続）。
  `output/build/stb-streamer-local` を削除して再ビルドし、全機を焼き直した。
  落とし穴は docs/build-on-nas.md に追記した。
- **unit3 の DTB 漏れ（要記録）**: ramboot-post-image.sh の DTB 一覧が
  ハードコードだったため、unit3 が U-Boot でロードする
  `meson-g12a-x96-max-p-sm1.dtb` が SHA256SUMS と export から漏れていた。
  11/19 の構築時は Mac 側ローカルビルドで救助済みだったため気づかなかった。
  一覧を `BR2_LINUX_KERNEL_INTREE_DTS_NAME`（.config）から導出する方式に修正した。
  全機の manifest が自機の DTB を列挙するようになった。
- **unit3 の SSH 鍵（要記録）**: unit3 は 11/19 の構築時に unit2 と同じ identity
  （`work/ramboot-identity`）で焼かれていた。NAS known_hosts の旧エントリとも
  食い違い、guard から .56 への SSH が失敗する潜在問題になっていた。
  今回 `work/ramboot-identity-unit3`（固有鍵）で焼き直した。
  unit3 のホスト鍵が一度だけ変わり、Mac と NAS の known_hosts を更新した。

### 配備の実施

- NAS: bridge を配備して起動。pix-archive は再ビルドし unit を書き換えて再起動。
  guard を再配備（adb / PIX-SMB400 / .43 復旧経路の死んだコードを除去、self-test OK）。
  監査スクリプトは `nas/src/pix-recording-audit.py` に canonical 化した。
  pix-stb-health 結合（TIMERS・status.json）を除去して再配備した。
- STB 3 台: NAS docker で `X96MAX_UNIT=1/2/3` 再ビルド →
  `tools/flash-ramboot-over-ssh.sh`（新規、docs/ram-rootfs.md の手順を実装）で
  焼き直し → 再起動はユーザー承認のうえ 1 台ずつ実施。
  - unit1 (.109): 2026-11-22 08:37 + 08:46 の 2 回（§8 のビルド落とし穴による）。
    再起動後 rootfs / RAM、`/v1/status` capturing、manager 自動再接続、
    part 継続（pushed 増加）を確認。
  - unit2 (.123): 08:48。Image/DTB は同一ハッシュのため触らず rootfs のみ。
  - unit3 (.56): 08:52。ホスト鍵が identity-unit3 の固有鍵に変わった（上記）。
- ユーザー承認のもと削除したもの:
  - `pix-epg-window` timer/service と epg_window.py
  - pix-stb-health（`/home/ku/retired-epg-host-20261122/pix-stb-health` へ退避。
    監査側の結合除去済み）
  - pix-stream-20261101 の guard .bak 8 本・nodes.json.bak・stale-sources/pix-stb-guard.py
  - pix-archive-20261029 の .bak・candidate・staging・miraview 退避物・
    旧バイナリ退避・tar.gz
  - pix-archive-control-20261030-stage
  - pix-record-control-tests-20261029
  - pix-ts-commit-validation-20261029、pix-ts-tests-20261029
  - pix-stream-20261101 の死んだノードの last_seq_*
  現行サービスの稼働ファイルは退避物以外触っていない。

### 検証結果（収録基準）

| 基準 | 結果 |
| --- | --- |
| 基準 | 結果 |
| --- | --- |
| 1. カタログの新鮮さ | **合格**。3 局で 11/22 朝〜11/29 23:45 の未来番組表 |
| 2. recorder フォールバック | **合格**（下記の補足参照） |
| 3. `epg_stale` 消滅 | **合格**。08:59 の監査から警告なし（最後の警告は 08:48） |
| 4. 参照ゼロ | **合格**（下記の補足参照） |
| 5. 録画 3 台停止なし | **合格**。3 ノード recording、dropped 0、part 継続 |
| 6. tests / docs / メモ | **合格**。tests 288 件 OK（274 + 追加 14） |

基準 1 の詳細: airings は 3 局（35864 / 211 / 35856）で 694 件を bridge 経由で収集した。
基準 2 の詳細: `/api/archive/services` と `/api/archive/programs` が
mirakc 無しで機能する。manager の channelName キャッシュが 3 局を正しく解決する。
再起動後のフォルダ命名も正しい（BS_400211_ＢＳ１１イレブン / GR_3219535864_テレビ静岡 /
GR_3219435856_ＳＢＳ１）。実機シミュレーションは行わず、tests/ の bridge・EIT テストと
自然観察で代替した（設計 §未解決点どおり）。
基準 4 の詳細: 対象範囲は systemd unit・cron・`/home/ku/pix-cm`・
`/home/ku/pix-stream-20261101` である。この範囲の grep（テキスト + バイナリ）で
`.40` / mirakc / adb の一致なし。NAS 全体ではサードパーティ
（`~/.vscode-server`、`~/.codex`、`windr_esender`）と CM ロゴのバイナリ .pgm
（画素偶然一致）のみ残る。録画系コード・unit・cron には無い。
pix-archive の deps 同梱 civetweb 文書（サードパーティ doc の語）は
deps を壊さないため保持した。
基準 6 の詳細: docs（設計・verification・build-on-nas・ram-rootfs・nas/README）と
OpenViking メモを更新した。

追加テスト: `tests/test_eit_sections.py` に schedule 蓄積の回帰 4 本
（短い項目のフィルタ、24 時間間引き、名後出し更新、他局/CRC 拒否）。
`tests/test_epg_bridge.py` に bridge の変換・HTTP・保持動作 5 本と
正規ソース参照ゼロの固定 5 本（guard の self-test 実行を含む）。

### 残っている問題

- recorder フォールバック命名の実機シミュレーションは未実施（§8 の注記どおり）。
- EPG の schedule の深さは放送局運用に依存する。本日時点で 8 日分を確認
  （11/29 まで）。深さが不足した場合の監視は `epg_stale`（未来分消化で再発）が担う。
- pix-archive の services テーブルには旧 mirakc 時代の他局の行が残る
  （history として保持。新規収集は 3 局のみ）。
- unit1 は §8 のビルド落とし穴で 1 回余分に再起動した。

## 9. 本日の録画品質チェック（2026-11-22 13:30〜13:45 実施、全録 3 局の当日分 60 フォルダ）

### 番組名

- フォルダ名・ファイル名は STB 実受信の EIT（present/following）が採った番組名で、
  全件もじばけ・欠落なし。sidecar の program.startAt / name とフォルダ名は一致。
- BS11 の買い情報系番組ではスケジュール EIT（060000 開始）と
  present EIT（060208 開始）が数分ずれる。フォルダ名は実際の放送開始
  （present）に従っており正しい。

### 番組分割（パート継続）

- 本日のマルチパート 14 フォルダを確認。パートはすべて同一フォルダ・同一
  recordingId に継続し、11/22 に修正済みの partId / startAt ベース命名が機能した。
- 再起動をまたいだ継続の例その 1: unit2 再起動（08:48）直後のショップチャンネル。
  p02→p03 が同一フォルダで継続した（08:47:49 "continuing programme as part 2"）。
  NAS 再起動（11:49）後の BS11 ショッピング情報は、archive EPG の clock
  フォールバックで番組 1130 開始のフォルダに 114938 パートとして入った。
  startAt ベース命名が mirakc 無しで機能した実例である。
- 発生した欠損は次の 2 種類のみ:
  1. STB 再起動 1 回あたり 60〜90 秒の切れ目（01:22 / 08:37 / 08:46 / 08:48 / 08:52）。
  2. NAS 再起動で RAM 上の staging が消えた in-flight 分
     （BS11 11:30-11:49 の 19 分、SBS 11:35-11:49 の 14 分）。
- **唯一の実質欠損: エバーライフＴＶショッピング（テレビ静岡 08:30-09:00）の
  08:37:48-08:55:18（約 17.5 分）**。§8 の移行作業中に起きた。
  (a) 1 回目の再起動後のセグメント 398.3 秒が "no programme on air" で廃棄された。
      当時は archive EPG に 35864 の番組が未収集だったためである。
  (b) 2 回目の再起動で残りが消えた。
  現在は archive に 8 日分の番組表があるため、同じ状況でも
  clock フォールバックで解決され廃棄されない。

### 映像・音声

- audio_missing チェック: 直近 2 日 174 本、欠落 0。
- A/V ずれ: 本日の 18 本をサンプリング測定、最大 |ずれ| 0.019 秒（実質ゼロ）。
- コーデック/サイズ: テレビ静岡・SBS は h264 1440x1080、BS11 は **1920x1080**。
  BS11 の放送が衛星再編で 1440x1080i → 1920x1080i に変わった
  （STB エンコーダはスケーラを持たないためソース解像度がそのまま出る）。
  監査プロファイルを 1920 を受け付けるよう更新済み（nas/src/pix-recording-audit.py）。
- 本日の decode 系問題: なし（11/21 の FNN Live News α 1 本に decode 3 点の警告が
  残るが、那是 11/20 深夜の録画で本日分ではない）。

### CM カット

- 本日 82 ジョブのうち **5 本が packet 照合 verified で公開**された。
  公開分（いずれも BS11）: マジカル★エクスプローラー、生徒会にも穴はある！ p02、
  きみが死ぬまで p01、ごりやくさん、世界の鉄道 p01。
  公開分はすべて `verified=true`。フレーム数/パケット数一致、ハッシュ不一致 0、
  PTS 誤差 ≤10ms、再エンコードなし。
- 非公開 77 本はすべて original を保存したうえでの安全側フォールバックである。
  内訳は detection_failed 72（ロゴ参照不足。買い情報・ニュース系）、
  timing_breaks 2、no_breaks 3。誤カット防止ポリシーどおり。
- CM カットの実体は `.media/<recordingId>.mp4` へのシンボリックリンク（62 バイトは
  リンク自体で正常）。

## 7. CM カットの状況（11/21 分）

- CM キュー: completed 42 / failed 0 / pending 0。**16 本に `_CMカット.mp4` を公開**
  （元 MP4 は常時保存、公開ゲートはパケット照合 verified のみ、方針どおり）。
- ロゴ不在番組（SBSお天気ガイド等）は `detection_failed`（cm-scan ロゴ辺不足）で
  original_only — 誤カット防止の保存側フォールバックとして正しい挙動。
- STB 側 pixf 特徴量は unit1/unit3（PIX_FEATURES=1）で供給、unit2 は NAS 側
  cm-scan にフォールバック（BS11 は学習済みマーカーで処理）。

## 10. CM マーカーキャッシュの破損と修復（2026-11-22 15:00〜16:05）

### 10.1 何が起きていたか

- 11/22 は CM ジョブ 89 本のうち 79 本が original_only になっていた。
- 理由はすべて `insufficient reference logo edges`（15:16 時点の集計）。
- 配備までの間に 1 本増え、当日の件数は 90 本になった。
- うち同一エラーは 80 本だが、すべて旧リリースで配備前のジョブになる。
- 配備後の同一エラーは 0 件。
- §7 では「ロゴ不在番組」と解釈していたが、これは誤りであった。局ロゴは取れており、
  マーカーキャッシュが壊れていた。
- 壊れていたのは `markers/<service>_<WxH>.json` の roi と参照 PGM の大きさの不一致。
  3 局とも同時に壊れていた。
- 原因は `save_marker` が代替マーカーを固定名 `alt-0..2.pgm` に保存していたこと。昇格後も
  マーカーがその固定名を参照し続けるため、次の代替保存が稼働中の参照画像を上書きする。
- `cm-scan` は roi と大きさの合わない参照を全画面参照として縮尺し、平坦部で勾配が立たず
  デコード前に exit 1 する。失敗がルール判定より手前なので `fail_marker` が呼ばれず、
  自動昇格も働かない。壊れたキャッシュが以後の全ジョブで再利用されていた。

### 10.2 直したこと

- 参照クロップを内容アドレス名 `ref-<sha1>-<i>.pgm` で保存し、保存済みの組を不変にした。
- `load_marker` で roi と参照サイズを検証し、不整合なら `stats.alt` を検証して自動昇格する。
- `save_marker` でも壊れたペアを書かない。
- `cm-scan` / `cm-overlay` の起動失敗を `fail_marker` に計上し、その録画は学習し直して
  解析を続ける（従来はジョブ全損）。
- 3 worker の更新を `flock` で直列化し、未参照クロップを 7 日後に削除する。
- 正規ソースは `dtv_android/experiments/cm-generic`（commit `2b0e00b`、pytest 133 件 OK）。

### 10.3 配備と実績

- リリース `20261122-marker-integrity-02`（差分は Python 2 ファイルのみ、バイナリは同一）。
- config 切替と worker 4 つの再起動を実施。`deploy-release.sh` は `ku` 権限では再起動に
  失敗するため、`sudo -n systemctl restart` を別途実行した。
- 配備後の実ジョブで 3 局とも自動修復した。

| 局 | 修復時刻 | 修復後の roi | 実行者 |
| --- | --- | --- | --- |
| SBS | 15:49:57 | `[1208,32,192,80]` | 本番ジョブ |
| BS11 | 16:00:07 | `[1752,16,80,64]` | 本番ジョブ |
| テレビ静岡 | 15:49:50 | `[1328,24,72,48]` | 検証実行（ライブ） |

- 配備後のジョブは 16:09 時点で 4 本ある。3 局すべてがルール判定まで完走した。
- 内訳は次のとおり。
  - `cut_and_verified` 1 本（テレビ静岡・overlay 1 箇所）
  - `no_breaks` 2 本（テレビ静岡・BS11）
  - `timing_breaks` 1 本（SBS・候補 4 箇所）
- `insufficient reference logo edges` は 0 件で、例外もない。
- 公開ゲートの挙動に変化はない。公開された番組は `validation.verified=true`、
  `stored_output` あり、元 MP4 も保存されている。

### 10.4 実番組での検証（配備前、マーカーの複製を使用）

| 局 | 番組 | 判定 | カット |
| --- | --- | --- | ---: |
| テレビ静岡 | 救命病棟２４時（60 分） | overlay / breaks_found | 4 |
| SBS | みなスポ（30 分） | overlay / breaks_found | 1 |
| BS11 | Healing Music（5 分） | overlay / breaks_found | 1 |

テレビ静岡と SBS のカット数は 11/21 の公開時と一致した。旧リリースを同じ破損コピーに
実行すると同一エラーを再現するため、新リリースだけが完走することも確認している。

### 10.5 分析と残課題

分類と拡大余地の検討は [cm-coverage-expansion-20261122.md](cm-coverage-expansion-20261122.md)
にまとめた。要点は次のとおり。

- 修復で戻るのは「本来切れていた録画」である。11/17〜11/21 の公開率は 21〜43%。
- 番組全体が広告の録画（当日 25 本）と、ブレークを含まない分割パートは構造的に切れない。
- 最大の拡大余地は scenario 経路（6 日で 94 本）。精度未検証のため公開は保留する。
- NEWS_OR_SHOPPING の 30 秒フロアは維持する。15 秒フロアの境界だけ課題が残る。

手動の検証実行で SBS の現在マーカーが `[56,24,56,64]` に置き換わった（`fails>=2` の
正規経路）。どちらも SBS の局ロゴ由来で、当該番組のカット数は当時と一致している。

### 10.6 15 秒フロアの公差（18:35 配備）

検出器は短い区間を「15 秒スポットの整数倍 ±1.5 秒」で判定しているが、公開フロアは
15.0 秒未満を無条件に落としていた。このため、構造ルールがスポットと認めた区間を
フロアだけが落とす事例が 6 日間で 4 件あった。幅は 14.98 / 14.91 / 14.45 / 14.01 秒で、
境界のスナップで 15 秒をわずかに下回っただけである。

- 修正はフロアを「下限 − `short_break_tolerance`（1.5 秒）」にしたこと。新しい緩和では
  なく、検出器がすでに使っているスポットモデルをフロアにも適用する。
- 公開される区間は「構造ルールがスポットと認めた区間」の部分集合のままなので、精度は
  下がらない。ニュース・物販の 30 秒フロアは 28.5 秒になり、15 秒 1 本のスポットは
  従来どおり落ちる（実測 4 件で確認）。
- 過去のジョブに対する再適用では 4 カットが公開に変わる。3 局の範囲では、BS11 の 1 本が
  録画ごと公開に復帰し、テレビ静岡の 2 本に 1 箇所ずつ加わる。残る 1 本は NHK BS で、
  現在の録画対象ではない。

対象拡大の判断は計測で行った。canonical に `measure_agreement.py` を追加している。同じ
録画から本番経路（マーカー）と拡大候補（時刻・無音）の判定を作り、境界の一致を数えた。
結果として両者は別の位置を切ったため、scenario 経路の公開は見送った。詳細は
[cm-coverage-expansion-20261122.md](cm-coverage-expansion-20261122.md) §8。

配備は `20261122-cm-coverage-01`（差分は `cm_service.py` のみ、バイナリは同一）。
再起動が走行中のジョブに当たると `cm-scan exit=-15`（SIGTERM）で 1 本が
`detection_failed` になる。これは配備時のノイズであり、検出器の失敗ではない。

### 10.7 対象拡大の追加変更（22:40 配備）

精度を保ったまま広げる施策を 3 つ検討し、2 つを配備、1 つを実測で撤回した。

| 施策 | 結果 |
| --- | --- |
| 代替マークを学習候補と同じパスで測る | 配備（追加デコードなし。効果は未実証） |
| CM の無い番組をマーカーの失敗に数えない | 配備（4 日で 62 件の誤カウントを解消） |
| 瞬断結合（短い present を挟む欠落の結合） | **実測で効果なし。撤回** |

瞬断結合は 2 本で試して判定が変わらず、欠落ランも 16 本から 15 本に減るだけだった。
実際の棄却理由は「15 秒の倍数から 3 秒外れ」（87 秒・72 秒の欠落）であり、細切れでは
なかった。詳細は [cm-coverage-expansion-20261122.md](cm-coverage-expansion-20261122.md) §11。

配備は `20261122-cm-coverage-02`（差分は `cm_service.py` のみ、バイナリと `cm_rules.py`
は同一）。稼働中ジョブが無い時間帯を選んで再起動したため、`exit=-15` のロスは無い。

配備後の実ジョブは 2 本。SBS で `timing_breaks`（scenario・候補 1 箇所）と
`cut_and_verified`（overlay・2 箇所、`verified=true`）を確認した。SBS のマーカー統計は
`fails` が 1 から 0 になり、切れ目の無い番組で失敗が増えないことを確認している。
