# 1代目のSD起動RAM rootfs移行 - Evidence


## EvidenceBundleDraft: unit1-saved-autoboot

- Artifact key: unit1-saved-autoboot
- Slice ID: unit1-saved-autoboot
- Type: hardware
- Source: work/ramboot-unit1/acceptance-summary.json
- Summary: saveenv後の自動再起動から同一鍵SSH・rootfs/no swap/no block mounts・H264/AAC・実EIT名・CM・ロゴ・NAS両機録画を確認。実行binaryは最終CPIOと一致
- Verifier: parent-agent
- Evidence status: evidence-finalized

## EvidenceBundleDraft: unit1-sd-absent

- Artifact key: unit1-sd-absent
- Slice ID: unit1-sd-absent
- Type: hardware
- Source: work/ramboot-unit1/final-sd-absent-metadata.log
- Summary: 最終CPIOでSD controllerのみ実切断しmmcblk0消滅を確認。SSH/AV/EIT/CM/NAS継続、pushed68から70、drop0。同CIDで再接続
- Verifier: parent-agent
- Evidence status: evidence-finalized

## EvidenceBundleDraft: unit1-full-regression

- Artifact key: unit1-full-regression
- Slice ID: unit1-full-regression
- Type: test
- Source: work/ramboot-unit1/final-si-artifact-tests.log
- Summary: 実CPIO含む106件成功、skipなし。PX producerとSI callbackの実行fixtureを含む。仕様・品質の補完レビューとnative/ASan8件も成功
- Verifier: parent-agent
- Evidence status: evidence-finalized

## EvidenceBundleDraft: unit2-offline-regression

- Artifact key: unit2-offline-regression
- Slice ID: unit2-offline-regression
- Type: test
- Source: work/ramboot-unit1/unit2-final-regression-tests.log
- Summary: 2代目profileをオフライン再ビルドし106件中105件成功、機体別ロゴ入力なし1件skip。実機2代目は再配備・再起動せず録画継続
- Verifier: parent-agent
- Evidence status: evidence-finalized
