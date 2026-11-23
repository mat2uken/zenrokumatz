# docs/ 索引（全体）

この索引は `docs/` 配下の主要文書を目的別に並べ、トピックごとの**現行文書**へ最短で辿れるようにする。古い結論は削除せず履歴として残すが、**旧（SUPERSEDED）** と明示して現行と混同しないようにする。

現行／旧の判断基準（[リポジトリ整理計画](repository-organization-plan-20261123.md) のルールに準拠）:

- **現行**: トピックごとに一つ。日付が新しく、実機検証や訂正を含み、他の文書から参照されている結論・手順。
- **旧（SUPERSEDED）**: 後続文書で訂正・廃止された結論を含む文書。本文は保持し、先頭に Status 行を付ける。
- **記録**: 検証ログや段階別の実施記録は「現行の結論」ではないが、事実の記録として残す。現状を知るには現行文書を先に読む。
- `docs/aegis/` は過程記録であり助言であって権威ではない（[Aegis README](aegis/README.md)、[BASELINE-GOVERNANCE](aegis/BASELINE-GOVERNANCE.md)）。結論は `docs/` の現行文書を正とする。

3代目の要約は [unit3-status.md](unit3-status.md) に分離した（2026-11-23 時点）。

---

## 1. 運用手続き

| 状態 | 文書 | 説明 |
| --- | --- | --- |
| **現行** | [ram-rootfs.md](ram-rootfs.md) | 3台のRAM起動・イメージ更新・媒体照合の手順。IP／DTB／identity を機体ごとに定義。 |
| **現行** | [build-on-nas.md](build-on-nas.md) | NAS（192.168.99.80）でDockerビルドする手順。Mac の OOM 回避。 |
| 旧（初期手順） | [second-unit-provisioning.md](second-unit-provisioning.md) | 2代目の初期設定。現行は ram-rootfs.md、完成状態は unit2-complete.md。 |

## 2. 設計

| 状態 | 文書 | 説明 |
| --- | --- | --- |
| **現行** | [repository-organization-plan-20261123.md](repository-organization-plan-20261123.md) | リポジトリ整理計画。現行／旧の判断基準と実施順を示す。 |
| **現行** | [dt295-external-descramble-design.md](dt295-external-descramble-design.md) | DT295 の外部複合設計。1800秒切り直しの廃止を目標に据え、段階ごとの方針を示す。 |
| **現行** | [epg-decommission-design.md](epg-decommission-design.md) | EPGホスト（mirakc／.40）廃止の設計。EIT 供給への移行根拠。 |
| **現行** | [eit-program-names.md](eit-program-names.md) | 放送波の EIT から番組名を取得する仕組み。 |
| **現行** | [dt295-station-logos.md](dt295-station-logos.md) | BS／CS／地上波の局ロゴ取得と CM カットへの利用。 |

## 3. 調査・研究

| 状態 | 文書 | 説明 |
| --- | --- | --- |
| **現行** | [dt295-internal-card-feasibility.md](dt295-internal-card-feasibility.md) | 内蔵カードを外部複合に使えるかの調査。純正ファームでは不可、CFW 差し替えで可能。 |
| **現行** | [cm-gpu-offload-study-20261121.md](cm-gpu-offload-study-20261121.md) | CM カットの CPU 負荷削減検討。GPU 導入より先に CPU 側の無駄を削る結論。 |
| **現行** | [cm-coverage-expansion-20261122.md](cm-coverage-expansion-20261122.md) | 非公開録画の分類とマーカーキャッシュ修復の実データ分析。 |
| **現行** | [cm-amatsukaze-research-20261123.md](cm-amatsukaze-research-20261123.md) | Amatsukaze／JLS の CM 検出方式調査と我々への移植案。一次資料は下記3件。 |
| 現行（一次資料） | [amatsukaze-research-sources/jls-source-report.md](amatsukaze-research-sources/jls-source-report.md) | JLS／Amatsukaze のソース読解報告（コミット付き）。 |
| 現行（一次資料） | [amatsukaze-research-sources/jls-web-report.md](amatsukaze-research-sources/jls-web-report.md) | ウェブ調査報告。本文未読の項目を明記。 |
| 現行（一次資料） | [amatsukaze-research-sources/our-side-notes.md](amatsukaze-research-sources/our-side-notes.md) | 我々側の実装メモ。正規ソースとの照合素材。 |
| 現行（未解決あり） | [dt295-logo-cfw-20261123.md](dt295-logo-cfw-20261123.md) | CFW 経路での局ロゴ取得の試行。選局は成功、ロゴ検出は失敗（未解決）。 |
| **旧（SUPERSEDED）** | [usb-host-investigation.md](usb-host-investigation.md) | 2026-11-10 の USB 調査引き継ぎ。PHY 解釈は aegis の 30／50 で更新済み。 |

## 4. 検証

| 状態 | 文書 | 説明 |
| --- | --- | --- |
| **現行** | [verification-20261122.md](verification-20261122.md) | 3台の動作確認と修正（BS11 の衛星再編、EPG 廃止ほか）。 |
| 旧（時点記録） | [verification-20261121.md](verification-20261121.md) | 2026-11-21 の確認記録。現状は 2026-11-22 の検証記録を参照。 |
| **現行** | [cm-tuning-results-20261121.md](cm-tuning-results-20261121.md) | CM カットのチューニング実施記録。CPU 時間 −56.7〜75.2%。 |
| **現行** | [cm-scenario-guards-20261123.md](cm-scenario-guards-20261123.md) | ロゴ錨と安全弁の実装記録。実素材での確認は未実施。 |
| **現行** | [stb-stall-20261123.md](stb-stall-20261123.md) | 2代目 BS11 の13時間停止と、ガード／監査の誤判定の修正。 |
| **現行** | [verification-evidence-index.md](verification-evidence-index.md) | Git 管理外の実機証拠の要約と SHA-256。clone 後の参照先を示す。 |

## 5. 機体ごとの実施記録・引き継ぎ

### 3代目（192.168.99.56 / SM1）

| 状態 | 文書 | 説明 |
| --- | --- | --- |
| **現行** | [unit3-status.md](unit3-status.md) | 3代目の現状要約（round 214 時点）。動作・未解決・他機体との差分。詳細ログは aegis 過程記録を参照。 |

### 2代目（192.168.99.123 / G12A）

| 状態 | 文書 | 説明 |
| --- | --- | --- |
| **現行（完成状態）** | [unit2-complete.md](unit2-complete.md) | 2代目の全経路が動作した状態（2026-11-21 完成）。 |
| 記録（段階1） | [unit2-stage1-deployment.md](unit2-stage1-deployment.md) | 1800秒切り直し廃止の実機投入と経過観察。 |
| 記録（段階2・4） | [unit2-stage2-working.md](unit2-stage2-working.md) | 内蔵カードでの外部複合が成立。 |
| 記録（段階3） | [unit2-stage3-cfw.md](unit2-stage3-cfw.md) | CFW 導入と複合前の生 TS の確認。 |
| 記録（段階4） | [unit2-stage4-card.md](unit2-stage4-card.md) | 内蔵カードの PC/SC 化。 |

### 1代目・共通

| 状態 | 文書 | 説明 |
| --- | --- | --- |
| **旧（SUPERSEDED）** | [recording-split-and-stb-recovery.md](recording-split-and-stb-recovery.md) | 1800秒分割を維持すると書いた節は廃止済み。SSH 復旧の経緯は記録として残る。 |
| **旧（SUPERSEDED）** | [handoff-20261110.md](handoff-20261110.md) | 2026-11-10 の全体引き継ぎ。当時は受信・エンコード・NAS 保存が未確認。 |

## 6. Aegis 過程記録

全件のカタログは [aegis/INDEX.md](aegis/INDEX.md)（Date／Kind／Path／Title）。以下は主要な入口のみ。

| 状態 | 文書 | 説明 |
| --- | --- | --- |
| **現行（カタログ）** | [aegis/INDEX.md](aegis/INDEX.md) | `docs/aegis/` 配下の過程記録一覧。 |
| 現行 | [aegis/README.md](aegis/README.md) | Aegis ワークスペースの位置づけと権限の範囲。 |
| 現行 | [aegis/BASELINE-GOVERNANCE.md](aegis/BASELINE-GOVERNANCE.md) | 過程記録が権威ではない旨の明記。 |
| 現行（計画） | [aegis/plans/2026-11-17-dt295-logo-and-recovery.md](aegis/plans/2026-11-17-dt295-logo-and-recovery.md) | DT295 局ロゴとファームウェア復旧の既存計画。 |
| 現行（計画） | [aegis/plans/2026-11-18-ram-rootfs.md](aegis/plans/2026-11-18-ram-rootfs.md) | RAM rootfs の既存設計と実機記録。 |
| 記録 | [aegis/work/2026-11-18-unit1-ram-rootfs/10-intent.md](aegis/work/2026-11-18-unit1-ram-rootfs/10-intent.md) | 1代目の SD 起動 RAM rootfs 移行。 |
| 記録 | [aegis/work/2026-11-18-restart-reliability/10-intent.md](aegis/work/2026-11-18-restart-reliability/10-intent.md) | 2代目の再起動・録画再開信頼性。 |
| 記録（要約あり） | [aegis/work/2026-11-19-unit3-init/70-handoff.md](aegis/work/2026-11-19-unit3-init/70-handoff.md) | 3代目の引き継ぎ（round 211 時点）。要約は unit3-status.md。 |
| 記録 | [aegis/work/2026-11-19-unit3-init/30-usb-phy-findings.md](aegis/work/2026-11-19-unit3-init/30-usb-phy-findings.md) | 3代目の USB レジスタ配置の確定。旧解釈の更新元。 |
| 記録 | [aegis/work/2026-11-19-unit3-init/50-usb-phy-findings.md](aegis/work/2026-11-19-unit3-init/50-usb-phy-findings.md) | 3代目の USB PHY 調査（round 101–214 の追記、決着は round 137）。 |
| 記録 | [aegis/work/2026-11-22-cm-coverage/10-intent.md](aegis/work/2026-11-22-cm-coverage/10-intent.md) | CM カット対象拡大（マーカーキャッシュ修復）。 |
| 記録 | [aegis/work/2026-11-22-epg-decommission/20-checkpoint.md](aegis/work/2026-11-22-epg-decommission/20-checkpoint.md) | EPG ホスト廃止の完了候補チェックポイント。 |
