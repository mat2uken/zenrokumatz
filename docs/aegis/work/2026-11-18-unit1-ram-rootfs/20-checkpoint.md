# 1代目のSD起動RAM rootfs移行 - Checkpoint

## Current Checkpoint

- Current todo: 完了：保存済みSD-to-RAM自動起動からSSH・AV・EIT・CM・NAS復帰を実機確認
- Active slice: completed
- Completed todos:
- 機体別profile/identity・3本FAT照合・一時起動・最終SD実切断・saveenv・割込みなしreset・実行binary一致・106件回帰・運用手順更新・UART解放
- Evidence refs:
- work/ramboot-unit1/acceptance-summary.json
- work/ramboot-unit1/persist-autoboot-driver.log
- work/ramboot-unit1/final-autoboot-runtime.log
- work/ramboot-unit1/final-autoboot-nas-nodes.json
- work/ramboot-unit1/uart-released.log
- Blocked on: none
- Next step: 通常録画を継続。今後の更新は機体別RAMビルドとFAT3本照合を行い、長期観測は別途実施する

## Recent Checkpoint History

## Checkpoint Update

- Current todo: 最終SI修正入りCPIOの冷起動、EIT名・AV・CM・NAS後にsaveenvと再起動受入
- Active slice: final-si-cpio-build
- Completed todos:
- RAM起動・既存鍵SSH・PX修正配信・SD controller実切断中H264/AAC/NAS継続・同CID再接続成功
- Evidence refs:
- work/ramboot-unit1/sd-detached.log
- work/ramboot-unit1/sd-absent-ffprobe.json
- work/ramboot-unit1/sd-absent-nas-nodes.json
- work/ramboot-unit1/sd-reattached.log
- work/ramboot-unit1/eit-analysis/diagnosis.md
- Blocked on: none
- Next step: job bash-8b44ab80190f4385の両consumer再ビルド・最終CPIO/106テストを回収。新archiveをFATへ更新しcoordinatorでone-shot、EIT冷起動と同一鍵と名前・AV/CM/NASを確認。db reviewerのSI修正レビュー受領後のみpersist
## Checkpoint Update

- Current todo: RAM起動・同一鍵SSHは実機成功。共通legacy配信のDT295固定producerを修正し再検証
- Active slice: unit1-stream-runtime-repair
- Completed todos:
- 機体別プロファイル・秘密入力・7ロゴ保存・87実CPIOテスト・仕様品質レビュー・FATバックアップと3本SHA256更新
- Evidence refs:
- work/ramboot-unit1/one-shot-runtime.log
- work/ramboot-unit1/one-shot-stream-diagnostic.log
- work/ramboot-unit1/baseline-restored-status.log
- Blocked on: 共有legacyのrun_pipelineがPIX_TUNERを無視してDT295を起動。原本1代目スクリプトでRAM上の配信を一時復帰済み。永続切替は未保存
- Next step: 既存ts_sourceと共通EIT/映像配信へ修復し実機再配備、ホスト名保持版CPIOを再ビルド

## DriftCheckDraft

- Scope status: 明示依頼のunit1 RAM移行を完了。実機で見つけたPX producerとSI組立は録画・番組名維持の必要修復として限定対応
- Compatibility status: 2GiB/内部PHY/ch17/CM/B-CAS/SSH鍵/hostname/局ロゴを保持。既定RAM profile2は維持、2代目実機未変更
- Retirement status: DT固定重複producer・死んだstart_live・EIT旧custom assembler退役。disk/旧ext4/旧kernel/旧bootcmdは明示復旧用に保持
- New risk signals:
- 長期連続運転・物理AC電源抜差し・既知USB警告原因は今回未検証。SIのCC耐性再設計は対象外
- Advisory decision: continue

## DriftCheckDraft

- Scope status: 明示依頼のunit1 RAM移行を完了。実機で見つけたPX producerとSI組立は録画・番組名維持の必要修復として限定対応
- Compatibility status: 2GiB/内部PHY/ch17/CM/B-CAS/SSH鍵/hostname/局ロゴを保持。既定RAM profile2は維持、2代目実機未変更
- Retirement status: DT固定重複producer・死んだstart_live・EIT旧custom assembler退役。disk/旧ext4/旧kernel/旧bootcmdは明示復旧用に保持
- New risk signals:
- 長期連続運転・物理AC電源抜差し・既知USB警告原因は今回未検証。SIのCC耐性再設計は対象外
- Advisory decision: continue
