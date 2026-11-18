# Proof Bundle - 2026-11-18-unit1-ram-rootfs

## Method Pack Boundary

This proof bundle is an advisory Aegis Method Pack record. It does not determine evidence sufficiency, produce authoritative `GateDecision`, or grant `completion authority`.

## Task Intent

- Requested outcome: 1代目も起動時のみSD/mmcを読み、稼働中はRAMだけに統一する
- Scope: 1代目192.168.99.109、2GB内部PHY DTB、PX-S1UD・テレビ静岡ch17・CM特徴量・既存B-CAS接続とSSH鍵を維持。共通ビルドを機体別入力に拡張

## Impact

- Compatibility boundary: X96MAX_PROFILE=rambootの既定は2代目のまま。1代目はX96MAX_UNIT=1を明示。通常diskビルドも残す
- Non-goals:
- 4GB化、チャンネル変更、USBドライバ警告修正、commit/push

## Terminal Evidence Refs

- docs/aegis/work/2026-11-18-unit1-ram-rootfs/evidence-bundle-draft-unit1-full-regression.json
- docs/aegis/work/2026-11-18-unit1-ram-rootfs/evidence-bundle-draft-unit1-saved-autoboot.json
- docs/aegis/work/2026-11-18-unit1-ram-rootfs/evidence-bundle-draft-unit1-sd-absent.json
- docs/aegis/work/2026-11-18-unit1-ram-rootfs/evidence-bundle-draft-unit2-offline-regression.json

## Formal Evidence

- docs/aegis/work/2026-11-18-unit1-ram-rootfs/evidence-bundle-draft-unit1-full-regression.json
- docs/aegis/work/2026-11-18-unit1-ram-rootfs/evidence-bundle-draft-unit1-saved-autoboot.json
- docs/aegis/work/2026-11-18-unit1-ram-rootfs/evidence-bundle-draft-unit1-sd-absent.json
- docs/aegis/work/2026-11-18-unit1-ram-rootfs/evidence-bundle-draft-unit2-offline-regression.json

## Terminal Non-Passed Evidence

- none

## Legacy Unclassified Evidence

- none

## Superseded Evidence Count

- 0

## Drift Check

- Scope status: 明示依頼のunit1 RAM移行を完了。実機で見つけたPX producerとSI組立は録画・番組名維持の必要修復として限定対応
- Compatibility status: 2GiB/内部PHY/ch17/CM/B-CAS/SSH鍵/hostname/局ロゴを保持。既定RAM profile2は維持、2代目実機未変更
- Retirement status: DT固定重複producer・死んだstart_live・EIT旧custom assembler退役。disk/旧ext4/旧kernel/旧bootcmdは明示復旧用に保持
- Advisory decision: continue
