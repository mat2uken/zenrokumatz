# 1代目のSD起動RAM rootfs移行 - Intent

## TaskIntentDraft

- Requested outcome: 1代目も起動時のみSD/mmcを読み、稼働中はRAMだけに統一する
- Goal: 1代目も起動時のみSD/mmcを読み、稼働中はRAMだけに統一する
- Success evidence:
- 一時RAM起動、rootfsとswap/mmcマウント不使用、SSH同一鍵、映像音声・EIT・NAS録画復帰、保存済みbootcmdからの再起動、2代目回帰なし
- Stop condition: Stop when success evidence is satisfied or a blocker/risk requires pause.
- Non-goals:
- 4GB化、チャンネル変更、USBドライバ警告修正、commit/push
- Scope: 1代目192.168.99.109、2GB内部PHY DTB、PX-S1UD・テレビ静岡ch17・CM特徴量・既存B-CAS接続とSSH鍵を維持。共通ビルドを機体別入力に拡張
- Change kinds:
- migration
- Risk hints:
- high

## BaselineReadSetHint

- docs/aegis/plans/2026-11-18-ram-rootfs.md
- docs/ram-rootfs.md
- board/x96max/uboot-boot-default.md

## BaselineUsageDraft

- Required baseline refs:
- docs/aegis/plans/2026-11-18-ram-rootfs.md
- docs/ram-rootfs.md
- board/x96max/uboot-boot-default.md
- Delivered context refs:
- none
- Acknowledged before plan:
- docs/aegis/plans/2026-11-18-ram-rootfs.md
- docs/ram-rootfs.md
- board/x96max/uboot-boot-default.md
- Cited in plan:
- docs/aegis/plans/2026-11-18-ram-rootfs.md
- docs/ram-rootfs.md
- board/x96max/uboot-boot-default.md
- Missing refs:
- none
- Advisory decision: continue

## ImpactStatementDraft

- Compatibility boundary: X96MAX_PROFILE=rambootの既定は2代目のまま。1代目はX96MAX_UNIT=1を明示。通常diskビルドも残す
- Affected layers:
- none
- Owners:
- none
- Invariants:
- 2代目への配備・再起動なし。1代目は2GB DTB、既存host identityと機密B-CAS設定を隔離して維持。全媒体初期化・既存ext4削除なし
- Non-goals:
- 4GB化、チャンネル変更、USBドライバ警告修正、commit/push

## TaskStartSnapshot / approved extension

Branch main, HEAD c5f5fcf389fbd236a807a78c998456341309f093。2代目のRAM起動とロゴ/EIT等の未コミット変更がある。開始差分と主要入力のSHA256をwork/ramboot-unit1に保存した。commit/pushは行わない。

前段の2代目限定という境界は、今回のユーザー指示で1代目へ拡張された。方式は同じ外部gzip CPIO/uImageと標準/initで、1代目の2GiB DTBは変えない。ArchitectureReviewRequired: yes（共通ビルドの機体別設定と永続化入力の隔離）。TDD Mode: off、post-changeの回帰と実機検証を行う。

## Execution plan / owner boundaries

1. 現状: MemTotal1851460kB、MemAvailable1483368kB、SD/mmcblk0p2のext4 root。PX-S1UD、ch17、特徴量有効、B-CASは既存remote設定、ED25519ホスト鍵を維持する。SD CIDと区画を読取り採取済み。
2. 共通RAM機構を再利用し、X96MAX_UNIT=1|2で機体別overlay・成果物・identityを選ぶ。未指定のrambootは従来どおり2代目。ディスク用出力も維持する。鍵・B-CAS秘密情報・ネットワーク・公開鍵入力はGit除外のwork/ramboot-identity-unit1へ保存し、CPIO専用stagingへだけコピーする。
3. 起動サービスはDT295だけsimple、PX-S1UD/torneは既存pix-stream-linuxを選ぶ。1代目のCM特徴量を無効化せず、ch17を維持する。
4. ローダーはunit1でmmc0:1と2GiB DTB、unit2でUSBと4GiB ePHY DTBを使う。consoleの行長を500 bytes未満にし、3本の読込み失敗時にはbootしない。UARTのaml_dt照合後だけ切替作業を行う。
5. 元の起動ファイルとenvを退避し、一時RAM起動でroot・swap・マウント・同一SSH鍵・映像音声・EIT・NAS録画を確認する。起動時にSDを読むが、稼働中にext4を参照しないことを実機で検証する。
6. 成功後に限りbootcmdを保存して自動再起動を確認する。2代目には配備も再起動もしない。旧SD ext4と旧定義は復旧用に残す。全媒体初期化とraw sector書込みは禁止する。

仕様レビュー、品質レビュー、関連ホスト/CPIO回帰に加え、実機の起動・配信を完了条件とする。UARTの所在はユーザー回答で訂正済み。1代目はku@192.168.99.10の/dev/ttyUSB0へ接続されており、sudo -nと一時SSH/PTY中継で既存Linuxコンソールを確認した。物理的な差替えは不要。SD起動領域129MiBの圧縮バックアップとCRC、87件の実CPIO含む全テスト、仕様・品質レビューは合格。次はFATのファイル単位更新と一時起動で、自動起動の保存は実機確認後に限る。

## Repair and documentation notes

実機で初回CPIOのRAM rootと同一鍵SSHは成功した。ただし初回共有legacy launcherはrun_pipeline内でDT295を固定起動し、PIX_TUNERに応じる既存ts_sourceを迂回していた。EIT経路も欠落し、特徴量の映像mapが重複していた。元の1代目launcherをRAMへだけ戻す対照試験では1440x1080 H.264/AAC配信・NAS接続が復帰した。署名鍵・DTB・RAM容量は故障原因として除外できる。原因修復はts_sourceを唯一の選局元に戻し、EIT・変換・配信を共通所有させる。Decision: code-change、TDD Route: off。producer実行を含む10件のfixtureを追加し、実機の新source確認後に閉じる。

別の独立した不一致は、初回unit1 fragmentがhostnameをx96max-pへ変えていた点である。coordinatorが元のx96max-linuxを待って失敗したため、producer側のhostname設定を元に戻し、実CPIOテストでも保持を検査する。OSのRAM起動失敗とは扱わない。

追加のSI実機証拠では、13,366,800バイト・71,100パケットの採取からCRC正常な現番組名を6 present + 2 schedule回確認した。旧custom assemblerは非0 pointerの前セクション末尾を捨て、同じpacket内の後続sectionも捨てていた。共有ts_sectionにもpointer末尾欠落があったため、共有ownerを先に修正しPAT/EITもそこへ統合した。時刻・SID・キャッシュ・文字デコード・番組選択方針は変えない。synthetic native8件とASan/UBSan8件、既存ロゴcollector/CLI、採取TSの固定時刻再生とstdout全バイト一致が成功した。両リンクconsumerを再ビルドし、最終RAMイメージの冷起動で新しい番組名を取得するまで永続化しない。

SD controller ffe05000.mmcを全namespaceのMMC mount/swapなし確認後に実切断し、mmcblk0消滅下のSSH・H264/AAC・NAS録画継続とpushed193→199、zeroDropを確認した。同じCIDで再接続後203まで増加した。eMMCとUSBは切断していない。UART警告なし。これは最終メタデータ検証とは独立した媒体非依存の実機証拠である。

Retirement: internal code-retirement/delete-first。死んだstart_liveとDT295固定の重複producerを除き、DT295の手動バッファ能力はモデル限定で残す。公開APIと永続データは削除しない。既存SDのext4、FAT上の旧kernel/DTB、元のbootcmdは復旧用に保持する。

運用文書はstockのRunbook、読者は保守担当者、Step0で改稿する。主題は2台を取り違えずにRAMイメージを更新・検証・復旧すること。既存の節を維持し、機体別入力→ビルド→FAT更新→稼働確認→復旧の順で構成を確認した。根拠は現行profile/loader/finalizerと実機採取ログ。図は不要。textlintと内容の両方を検査する。

These records are Method Pack drafts / hints, not authoritative runtime decisions.
