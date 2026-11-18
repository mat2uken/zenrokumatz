# 1代目のSD起動RAM rootfs移行 - 完了記録

## 結果

1代目192.168.99.109を、SD/mmcから起動時だけ3本をロードするRAM rootfsへ移行した。一時起動で受け入れ条件を確認してから、U-Bootの `bootcmd=run linux_boot_ram` を保存した。UARTへ割込みを送らない再起動でも、同じSSHホスト鍵で接続でき、RAM root・配信・EIT・CM特徴量・NAS録画が復帰した。

[最終受入サマリー](<../../../../work/ramboot-unit1/acceptance-summary.json>) と [保存・自動再起動ログ](<../../../../work/ramboot-unit1/persist-autoboot-driver.log>) に実行証拠を残した。1代目の2GiB DTB、内部PHY、PX-S1UD、ch17、既存リモートB-CAS設定、hostname、局ロゴを維持した。2代目は再配備・再起動していない。

## 実機と回帰の証拠

- [保存後のruntime](<../../../../work/ramboot-unit1/final-autoboot-runtime.log>)：`rdinit=/init`、rootは `rootfs`、ブロックマウント・swapなし。MemTotal=1851456kB、MemAvailable=1415584kB。最終CPIO内と実機のEIT/launcher実行ファイルSHA256が一致した。
- [SD実切断下のAV](<../../../../work/ramboot-unit1/final-sd-absent-ffprobe.json>) と [メタデータ](<../../../../work/ramboot-unit1/final-sd-absent-metadata.log>)：SD controllerだけをunbindし、mmcblk0消滅下でもSSH・H.264/AAC・番組名・特徴量・NAS録画が継続した。配信カウンター68→70、drop=0。同CIDで再接続した。
- [自動起動後のNAS](<../../../../work/ramboot-unit1/final-autoboot-nas-nodes.json>)：1代目・2代目とも録画中。[AV](<../../../../work/ramboot-unit1/final-autoboot-ffprobe.json>) はH.264 1440x1080とAAC。[局ロゴAPI](<../../../../work/ramboot-unit1/final-autoboot-logos.json>) も取得できた。
- [1代目の全回帰](<../../../../work/ramboot-unit1/final-si-artifact-tests.log>)：実CPIOを含む106件、skipなし。[2代目のオフライン回帰](<../../../../work/ramboot-unit1/unit2-final-regression-tests.log>)：106件中105件成功、機体別ロゴ入力がない1件だけ想定skip。native SI8件とASan/UBSan8件、既存logo collector/CLIも成功した。運用手順のtextlintとリンク検査、差分空白検査も実施した。

## 実機で訂正した判断

初期87件と仕様・品質レビューのPASSは、CPIO構成・ローダー・daemon選択の範囲だった。PX-S1UDの実行producerまで成功したという判断には使えない。初回実機で、共有legacy launcherがDT295固定のproducerを呼ぶ問題、EIT欠落、特徴量のvideo map重複を発見した。元の1代目launcherをRAMへだけ戻して録画を復帰させ、canonical `ts_source` と共通EIT/encode/stream経路へ修正した。実際のshell関数を実行する10件を追加した。

初回coordinatorの待機失敗は、fragmentが元のhostnameを変更したためだった。RAM起動そのものは成功していた。hostnameを元に戻し、アーカイブ側でも保持を検査した。

さらに、現番組名を含む正常TSがあっても旧SI assemblerが欠落させていた。[採取データの診断](<../../../../work/ramboot-unit1/eit-analysis/diagnosis.md>) で、非0 pointerの前section末尾と、同一packetの後続sectionを落とすことを確定した。共有demuxにも前者があったため、そのownerを修正してPAT/EITも統合した。番組選択・時刻・文字デコード・キャッシュ方針は変えていない。修正版の固定時刻再生では番組名と全13,366,800バイトのstdout一致を確認した。実機でも一時起動時のevent1599と、保存後再起動時のevent1601の名前を取得できた。

独立レビューは修正後の配信sourceとSI sourceを再確認し、PASSを得た。初期PASSの不足は上記のとおり明示しており、最終実機確認で代用して隠していない。

## 境界と退役

既存のRAM設計を1代目にも適用した。機体別の設定・成果物・秘密入力だけを分離し、既定のRAMビルドは2代目のまま維持した。既存のRAM設計、運用手順、旧SD起動定義の3基準は参照を維持した。2代目限定という旧作業範囲だけを今回の明示依頼で拡張した。ADR Backfill Checkは既存方式の適用で新たな方式決定がないため追加なし。運用文書を2台対応へ更新した。

内部コードの退役はdelete-firstとし、死んだ `start_live`、DT295固定の重複producer、EIT専用の旧section assemblerを除いた。選局は `ts_source`、SI組立は共有demuxへ一本化した。launcherは432→401行、EITは600→542行。共有demuxにはpointer末尾の補完を追加した。テストは宣言された経路だけでなく、実行データと隣接モデルの挙動を確認する。未解決のowner増殖は残していない。

DT295の手動バッファ経路は、その機体の復号初期化を扱う役割に限定して残した。通常の2代目は引き続きsimpleを選ぶ。手動経路自体の実機受入は今回の範囲外であり、その支援を廃止する判断時に残存経路も見直す。ディスク用ビルド、旧SD ext4、FAT上の旧kernel/DTB、旧bootcmdは明示的な復旧用であり、稼働中のfallbackには使わない。raw sector書込みや永続データ削除は行っていない。

## 残るリスクと停止状態

目標の保存済み自動RAM起動と実機受入は完了した。信頼度はBとする。長時間連続運転やAC電源の物理抜差し試験は未実施。既知のUSB `_regulator_put` 警告、以前の2代目媒体先頭ゼロ化の原因、SIの新しいCC gap/duplicate耐性は解決したとは主張しない。今回の正常採取にはCC gap/duplicateはなかった。

再起動時にはSDが必要であり、RAM上の変更は再起動で消える。[運用手順](<../../../ram-rootfs.md>) に、機体別ビルド、秘密入力の保護、停止条件、ロールバックを記載した。通常録画中のメモリ使用量と番組切替を長時間記録すれば、継続運用の確認を補える。チューナー復旧の再試験も残る。

移行完了時点では、Gitはmain、HEAD=c5f5fcf389fbd236a807a78c998456341309f093のままで、commit/pushは対象外だった。タスク差分と既存の未コミット差分が残っており、Gitの作業ツリーはcleanではない。作業用branch/worktreeは作成していない。

この記録およびworkspace構造検査は、実機の受け入れ証拠やユーザーの判断を代替しない。
