# CM カット対象拡大（マーカーキャッシュ修復）- Intent

## TaskIntentDraft

- Requested outcome: 全録 3 局の CM カットで非公開（original_only）になっている録画を精査し、
  切れる対象を広げる。マーカーキャッシュ破損の修復、再発防止のライフサイクル堅牢化、
  非公開理由の分類と対象拡大施策の判断まで。
- 元プロンプト: セッション冒頭の指示（2026-11-22 15:08）／ prompts 配下の同内容
- Parent goal: goal-312d1b09-d8df-4f99-985e-49e345452457
- Success evidence（収録基準）:
  1. 修復後、テレビ静岡・SBS・BS11 のそれぞれで `cm-scan` 検知に進みルール判定まで完走
  2. 同一エラー `insufficient reference logo edges` による `detection_failed` が 0
  3. マーカー整合性の検証を実装し、破損から自動回復する（破損を作るテストを追加）
  4. 対象拡大の検討結果を実データ分類つきで文書化
  5. 公開ゲート（verified のみ公開・original 常時保存）に退行がない
- Stop condition: 上記の達成、または破壊的操作（サービス再起動・キャッシュ削除・既存公開物の
  変更）のユーザー確認待ちで止まる
- Non-goals: CM 判定アルゴリズム（`cm_rules.py` / `cm_scenario.py`）の再設計、STB 側の変更、
  `cm-scan` などバイナリの変更、過去の公開物の再処理
- Risk: medium（CM パイプラインの稼働系。ただし録画・保存経路には触れない）

## BaselineReadSet

- `/home/ku/pix-cm/config.json`
  - feature_mode=apply、logo_source は unit1 のみ
  - min_cut_confidence=0.5、timing_mode=detect、scenario_publish=false
  - retain_original=true、mode=cut、feature_marker_free_services=[35864]
- `dtv_android/experiments/cm-generic/cm_service.py`（load/save/fail/confirm_marker、measure）
- `dtv_android/experiments/cm-generic/cm_scan.cpp`（`OverlayMeasure`、参照の読み方）
- `dtv_android/experiments/cm-generic/cm_logos.py`（seed_marker）
- `/home/ku/pix-cm/markers/*.json` と `*.stats.json`（実測）
- `/home/ku/pix-cm/state/jobs/*/result.json`（11/22 のジョブ 89 本）
- `docs/cm-tuning-results-20261121.md`、`docs/verification-20261122.md` §7

## ImpactStatementDraft

- Compatibility boundary: マーカーキャッシュのファイル形式（`{roi, references, band, size}` と
  `.stats.json`）。参照ファイル名は固定名から内容アドレス名に変わるが、json が名前を持つため
  外部互換は保たれる。既存の整合したキャッシュはそのまま使える
- Affected layers: NAS の CM パイプライン（worker）。STB・録画・保存・公開ゲートは変更なし
- Invariants: 公開ゲート（verified のみ公開、original は常時保存）は緩めない。録画は止めない
- 変更の必要性: 破損は実行時状態の不整合であり、設定でも運用手順でも防げない。canonical の
  修正が必要（Decision: code-change）

## TaskStartSnapshot

- 作業先: /Users/kenichim/Desktop/work/X96MaxLinuxTSEnc（main、既存の未コミット差分あり）
- 正規ソース: /Users/kenichim/Desktop/work/dtv_android/experiments/cm-generic（main、clean）
- 現行リリース: `/home/ku/pix-cm/releases/20261121-cpu-tune-04`
- 着手時の状態: 11/22 のジョブ 89 本のうち 79 本が同一エラーで非公開
