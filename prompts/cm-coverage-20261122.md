# 次のセッションへ渡すプロンプト（CM カット対象の拡大 — マーカーキャッシュ修復と対象拡大の検討）

作業先は `/Users/kenichim/Desktop/work/X96MaxLinuxTSEnc` です。以下をそのまま新しいセッションに貼り付けて使います。

---

全録システムの CM カットで「非公開（original_only）」になっている録画を精査し、
カットできる対象を広げるための実装・検証を進めてください。まず前提を固めてから
修復・検証・対象拡大の検討まで進めます。2026-11-22 時点の調査で**失敗の大半は
単一の技術的原因（マーカーキャッシュの破損）**まで絞れており、これを直すことで
対象が大きく広がる見込みです。

## 背景と現状（2026-11-22 15:00 時点）

全録（録画→NAS保存→CMカット）は Linux STB 3 台で運用中:

| 台 | IP | 役割 | service |
| --- | --- | --- | --- |
| unit1 | 192.168.99.109 | テレビ静岡 ch17 | 35864（GR） |
| unit2 | 192.168.99.123 | BS11（BS09/0x4090） | 400211（BS、2026-11-22 から放送が 1920x1080i に変更） |
| unit3 | 192.168.99.56 | SBS ch15 | 35856（GR） |

- CM パイプラインの正: `/home/ku/pix-cm/releases/<id>/` 配下の
  `cm_service.py` と `cm-scan` / `cm-overlay` / `cm-cut` / `cm-verify` バンドル。
  現行リリースは `20261121-cpu-tune-04`。`systemd pix-cm-worker@1..3` が
  `run-worker.sh` 経由で `--config /home/ku/pix-cm/config.json` で動く。
  ソースの canonical は Mac の
  `/Users/kenichim/Desktop/work/dtv_android/experiments/cm-generic/`。
- 2026-11-22 の CM ジョブ 86 本のうち公開は 5 本（いずれも BS11・マーカー学習経路）。
  非公開 81 本のうち **76 本が同一エラー**:
  `cm-scan exit=1: ERROR: insufficient reference logo edges`。
  残り 5 本は no_breaks / timing_breaks（ルールが正しく判断した正当な非公開）。

### 確定済みの原因（本日実験で再現・検証済み）

1. マーカーキャッシュ（`/home/ku/pix-cm/markers/<serviceId>_<WxH>.json`）の
   `roi` と `references` が**別物ペアになっている**（破損）。参照 PGM のサイズが
   roi と一致せず、cm-scan の `OverlayMeasure` が
   `need(mask_count_ >= 20 ...)`（cm_scan.cpp:560）で落ちる。実測:
   - テレビ静岡 `3219535864_1440x1080.json`: roi `[56,974,192,80]`（左下、データ放送系）+
     `alt-*.pgm`（右上ロゴの 72x48 クロップ）→ 不一致
   - SBS `3219435856_1440x1080.json`: roi `[56,24,56,64]`（左上）+ `alt-*.pgm`
     （192x80 クロップ）→ 不一致
   - BS11 `400211_1920x1080.json`: roi `[1752,24,64,56]` + `alt-*.pgm`（80x64 クロップ）→ 不一致
2. しかし各 `.stats.json` の `alt` フィールドには**整合する有効ペアが残っている**:
   - テレビ静岡: roi `[1328,24,72,48]` + `alt-*.pgm` → **本日実験で再現成功**
     （`reference_mask_pixels=970`、5 分番組のフルスキャン完了）
   - SBS: roi `[1208,32,192,80]` + `alt-*.pgm`（192x80 ✓ サイズ整合）
   - BS11: roi `[1752,16,80,64]` + `alt-*.pgm`（80x64 ✓ サイズ整合）
3. 破損したキャッシュが自己回復しない理由: `fail_marker` は**ルール判定後**にしか
   呼ばれず、cm-scan 自体の起動失敗（constructor エラー）は失敗回数に数えられない。
   そのため `fails >= 2` による alt 昇格が一度も発動せず、同じ破損キャッシュが
   全ジョブで永久に再利用される（デッドロック）。
4. マーカー構造の前提:
   - `markers/<serviceId>_<WxH>.json` = `{roi: [x,y,w,h], references: [pgm 名], band}`
   - `markers/<serviceId>_<WxH>/` = 参照クロップ実体（旧命名 `ref-<idx>-<k>.pgm` と
     退避命名 `alt-<n>.pgm` が混在）
   - `.stats.json` = 信頼管理（confirms / fails / current_roi / alt）
   - `cm_logos.seed_marker`（config `logo_source`）は STB の `/v1/logos` の局ロゴから
     正しい形のマーカーを生成できる。`config.json` の `logo_source.urls` は
     unit1（テレビ静岡）のみ。unit3（SBS）も `/v1/logos` あり。unit2（BS11）は
     CDT が BS に無いためロゴ無し → 学習経路のみ。
   - `feature_mode: apply` は unit1/unit3 の STB 側特徴量 sideband を使う設定。
     ただし sideband 経路も marker キャッシュが無いと `FeatureUnavailable` になる。
     その場合は NAS 全デコードへフォールバックする。
     マーカー修復は sideband 解放でもある。

### 動作実績（前提として信頼してよいもの）

- 公開 5 本は全て `verified=true`（packet_hashes+boundary_decode）、フレーム/パケット
  数一致、ハッシュ不一致 0、PTS 誤差 ≤10ms。公開ゲート（verified のみ公開、
  original は常に保存）は正しく機能している。
- 監査（pix-recording-audit）の audio_missing / problems はクリーン。
  BS11 の 1920x1080 は衛星再編による実放送サイズであり、プロファイルは更新済み。
- キュー/パイプライン自体は健全（11/22 の NAS 強制再起動後も自動復旧済み）。

## 目的

1. マーカーキャッシュの破損を修復し、GR 2 局 + BS11 で CM 検知が再び機能するようにする。
2. 同種の破損が再び起こらないよう、マーカーキャッシュのライフサイクルを堅牢化する。
3. そのうえで「非公開になる理由」を分類し、カットできる対象を広げる施策を検討・実装する。

## 修復・実装の方針（提案）

- **即時修復**: `.stats.json` の `alt`（整合ペア）を marker json に昇格して書く。
  ただし alt が「局ロゴ」である保証はない（telop/データ放送の可能性）ので、
  修復後に実際の番組で scan → rules を通し、相関とカット結果を確認してから採用する。
  駄目なら `cm_logos.seed_marker` で局ロゴから作り直す（テレビ静岡・SBS は可能）。
- **堅牢化（コード修正）**:
  - load 時に `roi` と参照 PGM のサイズ整合を検証し、不一致なら alt へ自動降格
  - cm-scan の起動失敗（reference エラー等）も `fail_marker` に数える
  - save 時に roi/refs の整合を検証してから書く
  - canonical は `dtv_android/experiments/cm-generic/cm_service.py`、リリースは
    `pix-cm/releases/<新しい日付-id>/` として作る（現行の直接書換はしない。
    過去のリリース運用にならう）
- **対象拡大の検討候補**（実データで判断して採否を決める）:
  - `NEWS_OR_SHOPPING` の 30 秒フロアの妥当性検証（誤カット実績と突き合わせる）
  - `no_breaks` の 3 本はロゴが CM 中に消える/別位置の可能性 → 別検知
    （音声特徴、シーンカット、パケット NULL 領域など）の要否
  - `timing_breaks` の原因分解（タイムスタンプ前提の見直し）
  - スポーツ中継（駅伝など長時間枠）の扱い（CM 挟み方の違い）
  - sideband（STB 特徴量）の本格運用と `feature_marker_free_services` の見直し

## 制約と方針

- 正規ソース: `dtv_android/experiments/cm-generic/`（cm_service.py / cm_scan.cpp 等）。
  NAS 上の releases は配備物。直すときは dtv_android 側を直して新リリースとして配備。
- 破壊的な操作（マーカーキャッシュの削除・既存公開物の変更・サービス再起動）は
  ユーザー確認を取る。
- STB 側は触らない（今回は NAS 側のみで完結する想定）。
- 公開ゲート（verified のみ `_CMカット.mp4` を公開、original は常時保存）は
  絶対に緩めない。
- テスト/検証の記録は `docs/aegis/work/<日付>-cm-coverage/` と
  `docs/verification-YYYYMMDD.md` に残す。

## 収録基準（完了の定義）

1. 修復後、テレビ静岡・SBS・BS11 のそれぞれで少なくとも 1 本の番組が
   cm-scan 検知に進み、ルール判定まで完走する（公開まで行かなくてよい）。
2. 同一エラー（`insufficient reference logo edges`）による detection_failed が 0 になる。
3. マーカーキャッシュの整合性検証が実装され、破損状態が自動回復する
   （意図的に破損させるテストを tests に入れる。`dtv_android` 側のテスト基盤に従う）。
4. 「対象拡大」の検討結果が、実データに基づく分類とともに文書化される
   （どのジャンルが増え、どれが構造的に無理か）。
5. 既存の公開物・ゲート挙動に退行がない（verified ゲート、original 保存）。

## 最初に読むもの・調べるもの

1. `/home/ku/pix-cm/config.json`（feature_mode・logo_source・min_cut_confidence 等）
2. `dtv_android/experiments/cm-generic/cm_service.py` の
   `load_marker` / `save_marker` / `fail_marker` / `measure` / `feature_rows`。
   roi と refs を混在させた書き込み経路の特定が最初の仕事である。
3. `cm_scan.cpp` の `OverlayMeasure`（mask_count の定義）
4. `/home/ku/pix-cm/markers/*.json` と `*.stats.json`（本ドキュメントの実測値と突き合わせ）
5. `/home/ku/pix-cm/state/jobs/*/result.json`。本日 86 本の分類データである。
   staging の epoch から日時が取れ、`detection_error` が失敗理由。
6. `docs/cm-tuning-results-20261121.md`（チューニング記録・GpuGate 状態）
7. `docs/verification-20261122.md` §9（本日の録画・CM 品質チェックの記録）

## 実験コマンド（検証済みの手順）

- 再現（参照サイズ不一致の確認）:
  `cm-scan <source.mp4> <outdir> 4 --duration <dur> --windows 16 --references 3
   --overlay-roi <roi> --overlay-references <pgm1>;<pgm2>;<pgm3>
   --overlay-output <outdir>/overlay-00.csv`
  （outdir は先に作っておくこと。参照は `;` 区切り）
- 有効ペアでの成功例（テレビ静岡・ドレみ〜る 5 分番組）:
  `--overlay-roi 1328,24,72,48` + `markers/3219535864_1440x1080/alt-{0,1,2}.pgm`
  → `reference_mask_pixels=970`、video.csv / logo.json / overlay-00.csv が生成
- ffprobe は `/home/ku/work/TVerRec/bin/ffprobe`（config.json と同じ）

## 接続情報

- NAS: `ssh ku@192.168.99.80`（root 権限は `sudo -n`）
- STB: `ssh root@192.168.99.109` / `.123` / `.56`（鍵認証。scp 不可 → `ssh cat >`）
- マーカー修復の対象ファイルは `/home/ku/pix-cm/markers/`（修正前に一式バックアップ）
