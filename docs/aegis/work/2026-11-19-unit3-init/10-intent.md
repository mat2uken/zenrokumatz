# 3代目 X96Max_P の立ち上げ - Intent

## TaskIntentDraft

- Requested outcome: 3代目のX96Max_PをSDからRAM-root Linuxで起動し、PIX-DT295でGR地上波SBS（161ch）を受信・録画できる状態にする
- Goal: `X96MAX_UNIT=3` のビルドプロファイルを追加し、SDへ書き込み、シリアルでLinux起動を確認して電源投入だけで起動するよう保存する
- Success evidence:
  - `X96MAX_UNIT=3` のビルドが成功し、`output/ramboot-unit3/` に起動3本とハッシュが揃う
  - SD書き込みの読み戻し照合が成功する
  - シリアルU-BootからRAM起動し、`rootfs`がRAM、swapなし、SSH到達
  - DT295でSBS（161ch）の受信とH.264配信が成立し、NASへ録画が継続する
  - 1代目（192.168.99.109）・2代目（192.168.99.123）に変更を加えていない
- Stop condition: 成功証拠がそろうか、DTB・カード・物理接続などでユーザーの判断が必要になった時点で止める
- Non-goals: 既存2機の再ビルド・再起動、カードの移設、commit/push
- Scope: 3代目1台、SD起動、DT295、GR地上波SBS。1代目PXS1UD/ch17と2代目DT295/BS11の設定は変えない

## BaselineReadSet

- docs/ram-rootfs.md
- docs/second-unit-provisioning.md
- scripts/build-linux.sh, scripts/build-in-container.sh
- board/x96max/ramboot.fragment, ramboot-unit{1,2}.fragment, ramboot-finalize.sh
- tests/test_ramboot_profiles.py
- docs/aegis/work/2026-11-18-restart-reliability/20-checkpoint.md（シリアル識別）

## ImpactStatementDraft

- Compatibility boundary: `X96MAX_PROFILE=ramboot`の既定（unit 2）は変えない。unit 3は明示指定のみ
- Affected layers: ビルド入口のunit検証、機体別fragment、機体別overlayのdefaults、DTB候補
- Owners: `scripts/build-linux.sh`と`scripts/build-in-container.sh`がunit選択の単一所有者。defaultsはfragmentのoverlayが所有者
- Invariants: 全媒体初期化はSDカード1枚のみ。既存2機のbuild output・identityは触らない
- Risk: high（実機の起動設定・媒体書換えを含む）

## TaskStartSnapshot

- Branch main, HEAD 02237d2。未コミット変更: package/stb-streamer/src/stb_linux_pipe.c, tests/test_program_boundary.py（既存の作業差分。触らない）
- シリアル: `/dev/cu.usbserial-01025FC4`（CP2104 serial 01025FCC）。NASの`ttyUSB1`＝2代目と同一のCP2104で、2台目とは別物
- SD: `/dev/disk10`（USB 31.9GB）。既存のX96Max_P起動SD（FAT X96BOOT）で、ユーザーが書換えを承認
- ArchitectureReviewRequired: yes（unit選択の所有者を共通ビルドへ拡張）
- TDD Mode: off。post-changeで回帰と実機検証を行う
