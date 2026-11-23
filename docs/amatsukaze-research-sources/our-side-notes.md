# 我々側の実装メモ（Amatsukaze調査報告書の素材）

正規ソース: /Users/kenichim/Desktop/work/dtv_android/experiments/cm-generic/（dtv_android repo, main）
測定記録: X96MaxLinuxTSEnc/docs/cm-coverage-expansion-20261122.md, docs/cm-tuning-results-20261121.md, docs/dt295-station-logos.md

## パイプライン（実関数）

1. **cm-scan** (`cm_scan.cpp`, 単一デコード)
   - 映像特徴: pts/luma/black_fraction/scene/logo_edge → `video.csv`、音声 RMS → `audio.csv`
   - ロゴ学習: `OverlayLearn`（上下バンド `top_`/`bottom_`）
     - `accumulate()` (L300): バンド内 per-pixel 勾配符号安定性（gx/gy 符号平均、magnitude）
     - `collect()` (L318): magnitude>=kMinMagnitude && stable>=kMinStable && 安定比>=kMinAgreement(0.55) のブロック窓 → Candidate、コンパクト性フィルタ「局マークは小さく密、幅広テロップ帯ではない」(L363)、w>=24,h>=12
     - `rank_windows()` (L425): 候補構造が最も強い窓を選び参照クロップに使う
     - `measurable()` (L232): §16 の修正。`build_overlay_template()` で mask_count>=20（kMinLogoMaskPixels）を要求 = 学習と照合でロゴらしさ判定を共通化（cm_overlay_template.h）
   - `OverlayMeasure`: キャッシュ済みマーカーをデコードループ内で測定（--overlay-roi/references/output）
2. **cm-overlay** (`cm_overlay.cpp` + `cm_overlay_template.h`): 参照PGMとの勾配相関を1秒ビンで → overlay-XX.csv
3. **cm_rules** (`cm_rules.py`)
   - `chapter_points()`: scene>=12 or black>=0.98 × silence(-42dB, 0.15s) 一致
   - `classify_bins()`: Otsu 2クラス + unknown帯（分離<0.25 でマーカー却下 min_cluster_gap）
   - `bin_runs`/`snap`/`structure_breaks`（too_short<12s, too_long>300s, unusual_length(5秒倍数外,<30s), short_break_not_a_spot_multiple(<120sで15秒倍数外±1.5s), end_card(末尾90s以内12s以下), edge_not_on_a_transition(±3s), boundary_at_recording_start(<2s), source_gap）
   - `anchored_edges()`: 実測「本物の境界は遷移2.7秒以内、誤検出は42/58秒外れ」
   - `spot_chain()`/`recover_spot_blocks()`/`corroborated_blocks()`: §12 実装。長さ棄却区間の部分採用は**timing経路も同一区間を提案した場合のみ**（2検出器一致）
   - `overlay_decision()`, `local_breaks()`（部分測定窓）, `analyze()`（候補順、overlays[0]をscenarioに渡す）
4. **cm_scenario** (`cm_scenario.py`, ロゴ無し経路)
   - UNIT=15.0, TOLERANCE=1.5, MIN_BLOCK=30, MAX_BLOCK=300, MIN_PROGRAMME=200, SCENE_CUT=25
   - `silences()`(scenario_silence_db=-55), `snap()`, `block_score()`(L115): 沈黙両端+2.0/units+inner/0.3units/**ロゴ項: present<=0.25→+2.0, >=0.6→-3.0**/先頭末尾5秒内-1.0
   - **問題**: logo_profile は overlay 行があれば常に計算。死んだマーカー（相関全域0）だと present 比 0 → 全ブロック一律 +2.0 → min_score=2.0 の下限として実質ノーゲート化。fnn の始端30秒ずれはまさに壊れたキャッシュマーカーでの計測（docs §15.1）
   - `choose()`（重み付き区間スケジューリング、200秒本編ギャップ要求）, `expand()`
   - 番宣+CM が6単位(90s)ブロックとして15秒倍数で整合してしまう（fnn: 1032.9(番宣開始)→1122.7、実CMは1063.03→1122.72）
5. **cm_service** (`cm_service.py`)
   - `measure()`(L788): sideband(rows)はキャッシュマーカー必須。フルスキャン: キャッシュをoverlay-00でinline測定→`cm_rules.analyze`→失敗なら学習候補(learned_candidates 先頭)+known_candidates(alt)で再測定。`bin_separation>=min_cluster_gap*1.5` でのみ save_marker
   - `known_candidates()`(L706), `learned_candidates()`(L741), marker trust: confirm/fail、2連続failでalt昇格
   - process_job: scenario カットは `scenario_preview` に記録のみ（`scenario_publish` false、L1313）。`min_cut_confidence`(L1431)、`cut_minimum()`(NEWS/物販30sフロア)、`filter_short_cuts()`(15sフロア−short_break_tolerance)
   - ロゴseed: `seed_marker_from_logo()` → `cm_logos.seed_marker()`（CDT/BS-15由来PNG、輪郭シルエットIoU一致、PLTE無しパレットPNG自前展開）
6. **ロゴ素材の実測**（docs/dt295-station-logos.md, cm-coverage §10.2）
   - テレビ静岡: CDTロゴが空（透明プレースホルダ）→ seed 不可
   - SBS: CDTロゴ 36x24 → seed できたが present_ratio 0.09 で使えず
   - BS11: BS-15(929)に実ロゴあり(48x27, "BS11"文字確認済み) だが logo_source.urls 構成上 未活用。シルエット方式は学習済みマーカー代用で roi[1312,27,94,49] 一致0.506 を再現済み

## 実測値（報告書で事実として扱う）

- scenario: 人手判定14件 → 完全一致2/境界ずれ6/CM無しに提案5(38%)。厳密精度15%、CM存在率62%（docs §14.3）
- mark: 提案2件、完全一致1、境界ずれ1、厳密精度50%（§14.3。母数小）
- 境界: 終端4個すべて0.03秒以内一致。始端2/4が外れ（+0.43秒、+30.1秒=番宣混入）（§14.2）
- 番宣は音圧(rms中央値-32.0/-30.0/-30.6dB)でもロゴ相関(死んだマーカーで全域0)でもCMと分離不能（§15.1）
- 棄却理由 4日302本: too_short 869件(中央3.5秒=ロゴ瞬断), short_break_not_a_spot_multiple 115, unusual_length 91, edge_not_on_a_transition 80, end_card 23（§9.2）
- マークは番組依存で変わる → present_ratio 中央値0.31〜0.58、参照群10本中8本が present_ratio 0（§8.2, §9.5）
- 経路相補性: テレビ静岡救命病棟で先頭45秒を両経路一致で追加採用（§12.2）
- 1本あたり追加コスト: CPU 636〜1112秒（キャッシュ/学習）、scenario 判定自体は軽い（Python 1.1秒）

## 測定道具（検証方法の引用元）

- `experiments/cm-generic` pytest 152件
- `measure_agreement.py`: 同一特徴量で mark vs scenario の一致/余分/見逃し（±1.5秒一致判定）
- `evaluate_recordings.py` + `apply_answers.py` + `review/mixed.json`: コンタクトシート5コマ、判定語彙 cm/mix/prog/?
- `score_decisions.py`: ラベル採点（本編削除秒数も出る）
- `boundary_sheet.py` + `candidate_sheet.py`: 0.25秒刻み境界確認、scene>=12・rms<-42 の数値照合
- 確定ラベル: fnn [563.03,623.22], tensei [222.09,251.32],[733.43,793.29]（境界0.03秒以内、循環しない基準点）
