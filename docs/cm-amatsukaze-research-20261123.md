# Amatsukaze（join_logo_scp）の CM カット方式調査と我々への移植案

2026-11-23。[amatsukaze-cm-research-20261123](../prompts/amatsukaze-cm-research-20261123.md)
の依頼に対する成果物。A〜D の 4 部と、JLS 側でも未解決な問題の一覧で構成する。

## 0. 調査の方法と証拠の扱い

- ソース読解の対象:
  - nekopanda/Amatsukaze（ff26c44 と配布 7z 0.9.1.4）
  - yobibi/join_logo_scp（4107b3e、v5.1.1）
  - nekopanda/join_logo_scp fork（0ee68f2）
  - nekopanda/chapter_exe
  - rigaya/Amatsukaze（8c0d54a）
- ウェブ調査: rigaya ブログ、EncTools、Amatsukaze Wiki、GitHub Issue/PR、
  5ch・したらば掲示板、個人ブログ
- 一次報告の全文は [amatsukaze-research-sources](amatsukaze-research-sources/)
  に保存している
- 本文では、出典に書かれた内容を事実として扱い、本報告の推論には「推測」と
  明記する。分からない点は「不明」と書く
- ライセンス: Amatsukaze・JLS 系は GPL 混在のため、コードは一切持ち込まない。
  パラメータ名・規則・アルゴリズムの記述のみを扱う。JLS の README は転載・
  改変自由を明記するが、それでも規則の考え方までにとどめる

## A. アーキテクチャ比較表

| Amatsukaze / JLS の段 | 我々の対応 | 一致点・差分 |
| --- | --- | --- |
| 前段: TS を amtsN.dat にデコード。300 フレーム未満は解析しない | cm-scan が 1 デコードで特徴量とロゴ学習を同時実行 | 同等以上。1 回のデコードで全特徴量を出す設計は我々が上 |
| ロゴあり/なし: .lgd との相関 corr0/corr1、しきい値 0.2 | cm-overlay の 1 秒ビン勾配相関、Otsu で present/absent | JLS はフェード 20 段階評価と ±0.5 秒平滑を持つ。我々は録画ごとの Otsu で適応 |
| ロゴ合計 20 秒未満なら全体をロゴなし扱い（自動 LogoOff） | 無い。死んだマーカーが scenario の全ブロックに一律 +2.0 | JLS の下限規則が我々に無い（候補 1） |
| ロゴ生成: 周辺単色フレーム限定で画素回帰、2 回再学習、クリーンアップ | バンド勾配安定性とテンプレート構造 20px（直近修正） | 同型の誤拾い対策。JLS はフレーム資格と回帰消去が本体（候補 6） |
| 無音・SC: chapter_exe。無音はサンプル値 50 以下、SC は動き率 100/1000 超 | cm-scan の RMS、scene、黒率。無音と SC の一致で章点 | 無音×SC 同時条件は同一思想。JLS は固定線除外と 15 秒距離の ★ マークを持つ |
| 構成推測（ロゴあり）: ロゴ端から 15 秒刻みで最大 120 秒遡行。無音 SC を新規挿入 | cm_rules の absent ラン、snap、構造規則、anchored_edges | JLS は「境界に SC が無ければ作る」。我々は snap 失敗なら棄却 |
| 構成推測（ロゴなし）: AutoCM。相互距離加点と 3 構成連続条件、無音過多棄却 | cm_scenario の 15 秒連鎖、block_score、音圧ゲート | JLS の 3 つの防護が我々に無い（候補 2） |
| 境界補正: CutMrgIn/Out。ロゴ端と SC の距離ヒストグラムを自動検出 | snap（±2 秒）と anchored_edges（±3 秒）の固定窓 | JLS はオフセット分布を録画ごとに学習する（候補 3） |
| 頭末尾・番宣: 末尾は Trailer 保護。先頭はロゴ立ち上がりを最優先錨 | end_card と recording_start の棄却のみ。番宣クラスは無い | JLS にも先頭番宣の保護は無い。錨の発想は移植できる（候補 1） |
| PMT 更新: 頭尾 % 内の PMT 変化と 60 フレーム内 SC 一致で刈り取り | 無い。fMP4 録画に PMT は載らない | STB 側の改修が必要。3 局の PMT 挙動は不明（候補 4） |
| 局別運用: JL_NHK / JL_MBS / JL_ATX の切替、ChParamJL の局×番組表 | cm_rules の DEFAULTS 一式と NEWS 物販フロアのみ | 局別プロファイルの枠組みが我々に無い（候補 5） |
| 出力: trim.avs は残す区間、補集合が CM。手動 trim 取り込み | cuts、フロア、confidence、cm-cut、cm-verify で公開 | 我々の復号ゲートに JLS 側の同種は確認できない（未確認） |

出典は次のとおり。JLS 側:

- JL/JL_標準.txt、JL_NHK.txt、JL_MBS.txt、JL_ATX.txt
- JLコマンド説明_全般.txt、Autocutメモ
- JlsAutoReform.cpp、JlsReformData.cpp、CMAnalyze.hpp
- LogoScan.hpp、chapter_exe.cpp

我々側:

- cm_scan.cpp、cm_overlay_template.h、cm_rules.py
- cm_scenario.py、cm_service.py
- [cm-coverage-expansion-20261122](cm-coverage-expansion-20261122.md) の実測

### 我々にあって JLS に無いもの

1. **公開前の復号ゲート**（cm-verify のパケット照合と境界デコード）。
   JLS 側で同種の仕組みは確認できなかった（未確認としておく）
2. **2 検出器の一致でだけ切り抜く規則**（corroborated_blocks）。
   JLS はロゴあり/なしのどちらか一方で構成推測し、同一録画で両方走らせて
   統合する機能を持たない
3. **学習と照合で同じロゴらしさ判定を使う構造**（cm_overlay_template.h）。
   JLS も学習と照合で同じ評価式だが、候補採用の段で学習側だけ緩い運用が
   起こりうる（推測）

### JLS にあって我々に無いもの（移植の選択肢）

1. ロゴ合計の下限規則（WLogoSumMin = 20 秒）
2. 無音なし経路の 3 つの防護（相互距離加点、3 構成連続条件、無音過多棄却）
3. CutMrgIn/Out の自動検出（ロゴ端と SC の距離分布の学習）
4. 先頭のロゴ立ち上がりを始端の錨にする規則（setFirstArea）
5. PMT 更新による頭末尾の刈り取り（--pmt-cut）
6. 局別・番組別の規則プロファイル（JL スクリプト切替と ChParamJL）

## B. 採用候補リスト（優先順）

症状は依頼文書の 4 分類（始端ずれ、番宣誤認、ロゴ不安定、精度不足）に合わせる。

### 候補 1（優先 1）: ロゴ証拠の生死判定と、ロゴ消失遷移への始端錨付け

- **何を採るか**: 規則とパラメータ。
  (a) ロゴの present 合計が下限未満のときは、そのマーカーを scenario に
  使わない。Otsu の分離が現行しきい値に満たないときも同じ扱いにする
  （JLS の WLogoSumMin = 20 秒相当）。
  (b) 採用できるマーカーでは、ブロック始端を「present から absent への遷移」
  （ロゴ消失 = CM 開始）へ後方にだけ動かす。JLS の setFirstArea が最初の
  ロゴ立ち上がりを最優先にするのと同じ錨の考え方
- **効く症状**: 始端ずれ、番宣誤認。fnn の実例では始端が 30.1 秒早く、
  1034〜1060 秒は本編の番宣だった（§14.2）。番宣は CM 中に局ロゴを出すので、
  ロゴの有無だけが残された区別手段になる
- **期待効果と根拠**: 番宣は音圧でも内容でも CM と区別できない（§15.1 実測）。
  JLS はこの状況に対して「ロゴあり = 本編、ロゴなし = CM」の二分だけを使う
  （JL_標準.txt、jls-web 調査の掲示板報告と一致）。あわせて、死んだマーカーが
  全ブロックに与えていた一律 +2.0 が止まる。効果の数値は未検証のため
  予測しない（不明）
- **実装コスト**: Python のみ。cm_rules.analyze から cm_scenario への引き渡しと、
  cm_scenario の logo_profile / block_score と新関数 1 つ。再ビルド不要
- **リスク**: 低。ロゴの分離が確認できた録画だけで動き、始端は後方にだけ
  動く（削除量が減る方向）。本編を切る方向には働かない
- **検証方法**: pytest を追加し 152 件を維持する。evaluate_recordings.py の
  6 本で再評価する。fnn の scenario 予覧の始端が 1063.03 秒 ±0.25 秒になることを
  boundary_sheet.py で確認する。score_decisions.py で本編削除秒数の減少を確認する

### 候補 2: AutoCM 相当の構成推測強化

- **何を採るか**: アルゴリズム。JLS の AutoCM から 3 つ。
  (a) 各候補縁を、±30 秒を 5 秒刻み、±3 分を 15 秒刻みで見て無音 SC が揃う
  度合いで相互加点する（15 秒単位は 21 点、近さ 100ms 以内は +10）。
  (b) cutlevel 6 相当: 3 構成以上の連続で CM、2 構成は長さが厳密に 15/30 秒の
  ときだけ認める。(c) checkMuteMany 相当: 区間内にどの境界にも属さない無音が
  多い構成は CM 判定を取り消す
- **効く症状**: 精度不足。scenario の誤提案 5/14（38%）と境界ずれ 6/14
- **期待効果と根拠**: JLS のロゴなし経路は README 自身が「精度は多少落ちます」
  と明記する。その落ち込みをこの 3 つの防護で抑えている（JlsAutoReform.cpp、
  Autocutメモ）。コミュニティもロゴなし運用の誤判定を事後確認前提とする
  （したらば、5ch の報告）。効果の数値は未検証（不明）
- **実装コスト**: cm_scenario.py を中心とした Python 改修。中規模
- **リスク**: 中。厳格化で見逃しが増えうる。既存の expand と
  音圧ゲートとの重ね合わせ調整が要る
- **検証方法**: §8.5 の計画どおり人手ラベル 20〜30 本を作り、厳密精度、
  CM 存在率、見逃しを測る。過去 6 日間の scenario 提案 94 本を再判定して、
  棄却数と残存数の集計も行う

### 候補 3: CutMrgIn/Out の自動検出

- **何を採るか**: アルゴリズム。ロゴ遷移と確定 SC の距離分布を録画ごとに
  ヒストグラム化し、境界面の系統オフセットを推定する。それで snap の窓を補正する。
  JLS は未指定時にこの検出を行い、フェード有無で ±2/±1 フレームの平滑も
  かける（JlsReformData.cpp detectCutmrgSub）
- **効く症状**: 始端ずれ。mark 経路の境界品質全般
- **期待効果と根拠**: JLS の readme は、ロゴ別の固定値での指定を推奨し、
  未指定の場合は自動検出の処理が入るとしている。我々の固定 ±2 秒窓より
  録画ごとの実態に沿う（推測）
- **実装コスト**: cm_rules.py。小〜中。Python のみ
- **リスク**: 低〜中。分布が薄い録画では現行の固定窓へフォールバックする
- **検証方法**: 確定ラベル 3 区間（fnn 1、tensei 2）の 8 境界と、公開済み
  カットの cm-verify 境界デコード一致率を前後比較する

### 候補 4: PMT 更新による頭末尾の確定

- **何を採るか**: 規則。番組頭尾の % 帯内で PMT 構成が変化する位置を候補にする。
  そこから 60 フレーム以内にシーンチェンジが一致するときだけ番組境界として
  刈り取る（Amatsukaze の --pmt-cut）。起源は nekopanda/Amatsukaze Issue #6。
  実運用値は「頭 5%、末尾 30%」や「5:5」の報告がある
- **効く症状**: 番宣誤認（頭末尾の番宣と番組前後の除去）。ロゴ無し局の
  前後確定
- **期待効果と根拠**: 設計の要点は誤爆防止で、PMT 変化単独ではなく
  シーンチェンジ一致を必須にしている（Issue #6 のやり取り）。NHK BS では
  稀に誤カットの報告があり、% 帯の制限が効いている（5ch の報告）
- **実装コスト**: 大。我々の録画は fMP4 で PMT を持たないため、STB 側で
  PMT 更新イベントを sideband の特徴量に載せる改修が要る。本調査の対象外の
  STB 改修を含む
- **リスク**: 中。SC 一致条件を外すと NHK BS と同じ誤カットに向かう
- **検証方法**: まず測定だけ行う。3 局の TS サンプルで PMT version の増分時刻
  と既知の CM 境界の一致率を数え、有意なら sideband 仕様の検討に進む。
  **3 局が CM 境界で PMT 更新を出すかは不明**であり、この測定で判断する

### 候補 5: 局別プロファイル

- **何を採るか**: 運用の枠組み。service_id ごとに cm_rules と cm_scenario の
  パラメータ上書き（ロゴ重視度、15 秒単位の許容、無音しきい値、フロア）を
  持たせる。JL_NHK / JL_MBS / JL_ATX の切替と ChParamJL の局×番組表が実例
- **効く症状**: 精度不足。BS11 は本編と CM のどちらも暗転で区切るため、
  SC が取れない。この報告が繰り返しある（5ch CM カットスレ）。
  30 秒 CM が 29 秒になる境界の丸め問題も報告がある（局の特定は本文では不明）
- **期待効果と根拠**: JLS 系の実運用は「規則を局別に切り替えて破綻を吸収
  する」のが定石（EncTools、vafee-lab の切替表、掲示板）。BS11 には時間的
  制約（30 分枠なら 24 分最近傍）の併用が合理的（推測）
- **実装コスト**: cm_service の config から cm_rules.settings への経路。小
- **リスク**: 低。既定は現行値のまま
- **検証方法**: §9 と同じ集計で局別の公開率と棄却理由の分布を前後比較する

### 候補 6: ロゴ学習の強化

- **何を採るか**: アルゴリズム。
  (a) 参照窓の資格を「候補の周辺縁が単色」で判定する（nekopanda のしきい値
  12/255 相当）。(b) テンプレートから安定背景の重みを画素単位で消す工程
  （GetLogo のクリーンアップ相当。現行は行と列の単位）。(c) 複数のロゴ候補を
  保持しスコア最大を選ぶ。新旧ロゴの混在は期間指定で分ける
- **効く症状**: ロゴ不安定の残部。学習できるロゴがバンド内に無い録画は
  0 候補のままである（§16.6）。番組依存マークの入れ替わりも対象である
- **期待効果と根拠**: nekopanda の学習は単一色背景フレームだけを回帰に使い、
  rigaya 版は細線除去、平坦画像棄却、縁の独占率 65% を課す。コミュニティも
  「きれいに消えないロゴは除外」とする（したらば）
- **実装コスト**: 大。cm_scan.cpp と cm_overlay_template.h の C++ 改修と
  再ビルド、配備
- **リスク**: 中。学習候補が減って検出機会を損なう可能性がある
- **検証方法**: §16.4 と同じ前後比較（候補数、採用率、
  insufficient reference logo edges の 0 件維持）。テレビ静岡 11/22 17:00 の
  0 候補録画で候補が復活するかを見る

### 見送り候補

- **番宣の内容特徴判別**: 不可であることが我々の実測で確定している（§15.1）。
  JLS と掲示板の知見には、内容ベースの判別が存在しない
- **ロゴフェードの追従**（logoframe の fadein/fadeout 相当）: mark 経路の
  精度は既に高く、瞬断の実測原因もフェードではなかった（§11.3）。優先度は
  候補 6 のあと

## C. 最小の実装案（優先 1 の詳細）

> **実装済み（2026-11-23）**: 実際に入れた形は名称と適用範囲が下記と異なる。
> 関数名は `logo_evidence` / `scenario_logo` / `mute_density_ok` / `anchor_start` で、
> 錨は「始端がロゴあり区間にあるときだけ後方へ動かす」本編保護に限定した。
> 詳細と実測は [cm-scenario-guards-20261123](cm-scenario-guards-20261123.md) にある。

対象は `dtv_android/experiments/cm-generic/`。変更は Python のみで、
バイナリの再ビルドと配備は不要。

1. **cm_rules.py**
   - `analyze()`（L561）: scenario へ `overlays[0]` をそのまま渡す箇所が 2 つ
     ある。L594（corroborated 用）と L633（最終フォールバック用）を変える
   - 新しい補助関数 `live_logo_state(overlay, options)` を追加する。
     既存の `classify_bins()`（L138）を呼び、次を満たすときだけ
     （states 辞書, clusters）を返す
     - `clusters['gap'] >= min_cluster_gap`（0.25、既定のまま）
     - present 状態のビン合計 >= 新定数 `scenario_logo_min_present_seconds`
       （既定 20.0 秒）。値は JLS の WLogoSumMin = 20 秒に由来する
   - 満たさない overlay は None を返す。死んだマーカー（相関が全域で 0 付近）は
     Otsu の分離が立たないため自動的に除外される
2. **cm_scenario.py**
   - `logo_profile()`（L104）: 相関の生値ではなく states 辞書を受け取る形に
     変える。live_logo_state が None のときは None を返し、ロゴ項と錨を無効にする
   - `block_score()`（L115）: ロゴ項の判定を states で行う。しきい値
     （present 比 0.6 で -3.0、0.25 以下で +2.0）は現行のまま
   - 新関数 `logo_anchored_start(block, states, edges, options)` を追加する
     - ブロック先頭から `LOGO_ANCHOR_WINDOW`（既定 30.0 秒）の範囲で
       present から absent への遷移を探す。遷移があれば、遷移後の最初の
       absent 秒を目標にする
     - `edges`（snap 済みの沈黙縁）から `LOGO_ANCHOR_TOLERANCE`
       （既定 1.2 秒）以内の縁へ始端を移す
     - 始端は後方にだけ動かす。移動後に `MIN_BLOCK`（30 秒）以上かつ
       15 秒倍数が `TOLERANCE`（1.5 秒）内に収まるときだけ採用し、
       それ以外は元の始端のままにする
   - `decide()`（L217）: `choose()` のあと、live logo があるとき各ブロックに
     `logo_anchored_start` を適用する
3. **cm_service.py**
   - 変更不要。`measure()` 経由で cm_rules.analyze が呼ばれるため、
     上記の変更が自動的に反映される。sideband の marker-free 経路は
     overlays が空のまま（L945）なので影響しない
4. **テスト（test_cm_scenario.py）**
   - 死んだマーカーではロゴ項と錨が無効になること
   - present から absent への遷移で始端が後方にだけ動くこと
   - 錨の適用後に 15 秒倍数から外れる場合は不採用になること
5. **配備と運用**
   - `assemble_release.py` で新リリースを作る（差分は cm_rules.py と
     cm_scenario.py のみ）。`scenario_publish` は false のまま変えない
   - 始端を後方へ動かすと番宣が出力に残る。fnn の人手判定では番宣混入を
     境界誤りとしているため、判定基準に合わせる方向である。ただし運用の
     既定方針は番宣を CM 枠の一部として切ることで、7 秒程度の短尺番宣は
     残す。短尺番宣が混じったブロックは 15 秒倍数から外れるため錨が
     不採用になり、現行挙動を維持する。番宣区画の扱いは
     `review/mixed.json` の 5 件の境界詰めと同時に判断する

## JLS / Amatsukaze でも解決していない問題

過度な期待を避けるため、JLS 側でも解決していないことを明記する。

1. **ロゴなし経路の本編中 CM 判定**: JLS の README 自身が「精度は多少落ちます」
   と書き、コミュニティも誤判定の事後確認を前提とする。JLS の実運用は
   ロゴなし経路を頭末尾カットと時間制約の役に限定している（JL_ATX）
2. **先頭番宣の個別識別**: JLS には末尾の Trailer 保護（tr1stprd = 45 秒）は
   あるが、先頭番宣を識別・保護するコマンドは存在しない。「ロゴあり = 本編、
   ロゴなし = CM」の二分に依存するため、ロゴが取れない局の先頭番宣は
   JLS でも CM として切られる
3. **番宣の内容特徴判別**: 我々の実測（分離不能）と同じ結論である。JLS と
   掲示板が使う手段は位置と長さ、ロゴの有無、時間制約、番組名マッチに限る
4. **BS11 の暗転区切り**: 本編と CM の頭と尻が暗転になる。このため SC が
   検出できず、コミュニティの定石は手動 trim である
5. **ロゴ学習の完全自動化**: rigaya 氏自身が「100% うまくいくわけではない」と
   明記する。フェード、白背景、ロゴ差し替えは再取得、期間指定、フェード指定
   などの運用対処にとどまる
6. **境界の偶発誤判定**: 2026 年に JLS 本体へ境界処理の修正 PR が出ている
   （ロゴ端付近の 15 秒 CM の取り残し、ロゴ立ち上がり直前の短区間の誤取り込み）。
   いずれもマージは確認できなかった。境界問題は JLS 側でも現在進行形である

## D. 出典

### D-1 我々側の実測と実装

- [cm-coverage-expansion-20261122](cm-coverage-expansion-20261122.md):
  - §8.2 measure_agreement、§12 一致ブロック
  - §13.2〜14.3 人手判定と厳密精度 15%、CM 存在率 62%、境界 8 個の照合
  - §15.1 番宣の分離不能、§16 学習器のロゴらしさ共通化
- [cm-tuning-results-20261121](cm-tuning-results-20261121.md):
  処理コスト、cm-verify の境界デコード
- [dt295-station-logos](dt295-station-logos.md): 局ロゴの受信と seed の実測
- `dtv_android/experiments/cm-generic/`:
  - cm_scan.cpp（OverlayLearn、collect、measurable）
  - cm_overlay_template.h（build_overlay_template、kMinLogoMaskPixels）
  - cm_rules.py（DEFAULTS、classify_bins、analyze、corroborated_blocks）
  - cm_scenario.py（block_score、logo_profile、decide）
  - cm_service.py（measure、known_candidates、process_job の公開ゲート）

### D-2 ソース読解の出典（repo とコミット）

- yobibi/join_logo_scp（4107b3e、v5.1.1）
  - readme.txt
  - JL/JL_標準.txt、JL_NHK.txt、JL_MBS.txt、JL_ATX.txt
  - JL/doc/JLコマンド説明_全般.txt、Autocutメモ
  - src/JlsAutoReform.cpp、JlsAutoScript.cpp、JlsDataset.cpp
  - JlsReformData.cpp、JlsScrFuncReg.cpp、JlsIF.cpp
- nekopanda/Amatsukaze（ff26c44 と配布 7z 0.9.1.4）
  - Amatsukaze/CMAnalyze.hpp、LogoScan.hpp、AMTLogo.hpp
  - TranscodeManager.hpp、EncodeServerData.cs、LogoAnalyzeViewModel.cs
  - ProfileSettingPanel.xaml
- nekopanda/join_logo_scp（0ee68f2）: 修正内容.txt
- nekopanda/chapter_exe
  - src/chapter_exe.cpp、mvec.cpp、改造版_読んでね.txt
- rigaya/Amatsukaze（8c0d54a）
  - Amatsukaze/CMAnalyze.cpp、LogoScan.cpp
  - LogoScanEdgeAreaPolicy.h、LogoScanWeakSideRescuePolicy.h
- pull/1 と pull/2（yobibi/join_logo_scp、2026-06、境界修正）
  - マージは確認できなかった

### D-3 ウェブ出典

- JLS 本体と JL スクリプト本文:
  https://github.com/yobibi/join_logo_scp
  https://raw.githubusercontent.com/yobibi/join_logo_scp/master/JL/JL_標準.txt
  https://raw.githubusercontent.com/yobibi/join_logo_scp/master/JL/JL_NHK.txt
  https://raw.githubusercontent.com/yobibi/join_logo_scp/master/JL/JL_MBS.txt
  https://raw.githubusercontent.com/yobibi/join_logo_scp/master/JL/JL_ATX.txt
- PMT 更新の起源と設計: https://github.com/nekopanda/Amatsukaze/issues/6
- rigaya 改造版の記事群と自動ロゴ解析: https://rigaya34589.blog.fc2.com/?cat=33
- 設定と局別スクリプトの解説: https://enctools.com/amatsukaze-cm-cut/ 、
  https://enctools.com/my-amatsukaze-settings/
- 局別切替の実例: https://vafee-lab.com/ubuntu/2026061808.html 、
  https://vafee.blog.jp/archives/1080098377.html
- 無音・境界診断と BS11 の暗転報告（CM カットスレ）:
  https://mevius.5ch.io/test/read.cgi/avi/1701608228/
- 番宣・ロゴなし運用の報告（Amatsukaze スレ）:
  https://jbbs.shitaraba.net/bbs/read.cgi/computer/44736/1562757840/
  https://mevius.5ch.net/test/read.cgi/avi/1603846355/
  https://mevius.5ch.net/test/read.cgi/avi/1752302318/
- 自動 LGD 生成の世代管理: https://github.com/tobitti0/logoframe （GPL-3.0）

### D-4 推測の区分

本報告で「推測」と明記した主な箇所。

1. 「我々の学習器は単色背景フレーム限定の事前フィルタを持たないため、
   安定した非ロゴ領域がそのまま学習に入る」（jls-source 調査の推測）
2. 「始端の 30 秒ずれは、先頭のロゴなし番宣が CM 候補列へ連結した結果に
   相当する」（同上）
3. 「境界ずれ 6/14 は相互加点と連続数条件が無い限り構造的に抑えにくい」
   （同上）
4. 「CutMrg の自動検出は固定 ±2 秒窓より録画ごとの実態に沿う」（本報告の推測）
5. 「BS11 には時間的制約の併用が合理的」（本報告の推測）
6. 「JLS 側に公開前の復号ゲートは確認できない」（未確認として扱う）

### D-5 不明点

1. 3 局が CM 境界で PMT 更新を出すか。TS サンプルでの測定が必要
2. 我々の fMP4 録画に PMT は載らないため、PMT 利用は STB 側の改修を要する
3. logoframe.exe 単体のソースは未確認（Amatsukaze 内蔵実装で代替して読解）
4. ChParamJL1/2.csv の実配布内容は未取得（リポジトリは空テンプレート）
5. BS11 の CM 構成を公式に記した資料（PDF）は未読
6. 掲示板の「SBS」が静岡放送を指すかは本文から特定できない
7. Amatsukaze 同梱の JLS 実行ファイルの正確な版数は「ver3.06n 系」と推測
8. Amatsukaze の LICENSE 本文は未確認。LogoScan.hpp の冒頭に MIT 表記を
   観察したとの調査員報告があるが、リポジトリ全体は GPL 混在のため、
   いずれの場合もコードは持ち込まない方針を維持する
9. 未読の掲示板スレ（Amatsukaze その4〜7・その9、CM カットスレ ★1〜6）
   が残る。URL の一覧は
   [jls-web-report](amatsukaze-research-sources/jls-web-report.md) にある
