# 2台目のRAM rootfs化

## Goal / approved scope
2026-11-18、ユーザーは「RAM起動方式で進める」を承認。
起動時だけUSBからカーネル・DTB・rootfsを読み、運転中はUSB媒体に依存しない。
まず一時起動、SSHと配信検証後に自動起動へ反映する。1台目は移行しない。

## Architecture / Tech Stack
Buildrootの外部gzip initramfsをLinux 6.18.62へ渡し、BusyBox initで既存サービスを起動する。
initramfsにはdevtmpfsをマウントする/initを用意する。USB rootfsへのswitch_rootはしない。
設定と公開鍵はイメージに含める。録画はNAS、一時データはRAM。
既存ext4出力は1台目と復旧用に残す。別のOS管理機構や永続overlayは追加しない。

## Baseline / authority refs
- README.md: ビルドとUART、旧2GiB構成。
- configs/x96max_defconfig、board/x96max/linux.fragment: Buildroot / kernel設定。
- board/x96max/dts/amlogic/meson-g12a-x96-max-p-ephy.dts: 2台目の外部PHY、現状2GiB指定。
- board/x96max/uboot-boot-default.md: 既存U-Bootの起動設定管理。
- board/x96max/overlay/usr/bin/pix-stream-simple、pix-dt295-reset: USB復旧の所有者。

## TaskStartSnapshot / compatibility
開始HEAD c5f5fcf、main。ロゴ取得・DT295ソフトリセット・EIT等の未コミット変更あり。
既存変更は上書き・コミットせず保持する。USB媒体とeMMC OS領域の初期化は禁止。
U-Bootはg12a_u211_4gを示すが、実容量・安全なメモリ範囲は未検証。
MBRゼロの原因は未確定。USB列挙差と読み書き対象の取り違えを除外するまで追加セクタ書込みはしない。
ArchitectureReviewRequired: yes（起動媒体へのランタイム依存と設定永続化境界が変わる）。

## Change Necessity / owner / risks
Decision: code-change。説明や起動引数だけでは/initとCPIO成果物、USB復旧の安全性は供給できない。
最小変更は既存Buildroot生成・起動スクリプトと共通USBリセット所有者。
USB復旧の直接sysfs書込みは共通安全チェックへ集約し、利用中ストレージがあれば拒否する。
自動再試行の無限リセットを避ける。RAM起動でもUSB上のデータ利用を巻き込まない。
RAM上の変更は揮発性。永続化は成果物再作成により行う。
4GiB設定は低位MMIO/予約領域とU-BootのDT書換えを確認してから反映する。

## TDD Route / verification
Mode: off; decision: skipped; authority: session default. RED/GREENは要求しない。
ホストテスト、シェル構文、CPIO内容と権限、DTB範囲、起動ログを検査する。
実機でrootがRAM、USBマウント・swapなし、SSH、メモリ量、チューナーと配信を確認する。
USB起動媒体をOSから安全に切り離してもSSH・配信が継続することを検証する。
自動起動変更は一時起動成功後だけ。再起動後も同じ構成になることを確認する。

## Implementation checkpoint (2026-11-18 10:05 JST)
- Buildroot標準のfs/cpio/initを再利用。独自/initは追加しない。
- 最初のCPIO成果物: 約34MiB gzip、展開後約111MiB。権限・設定・コンソール検査は成功。
- U-BootのAndroid DTで使用可能RAM [0x00100000, 0xf5800000)を読取り確認し、専用4GiB DTBへ反映。
- 一時TFTP転送は成功したが、生gzipはWrong Ramdisk Image Formatで拒否された。古いU-Boot向けに標準BR2_TARGET_ROOTFS_CPIO_UIMAGEを選択し再生成中。
- ホスト鍵はwork/ramboot-identity（Git除外）に一度だけ生成し、CPIO専用fakeroot stagingへ埋め込む。共有targetや1台目のイメージに鍵をコピーしない。
- USBリセット安全策39件のfixtureテスト成功。実機検証は未実施。自動起動のsaveenvはまだ実行しない。
- レビューで旧pix-stream-run/pix-stream-linuxにも周期的ポート切断が残っていたため、両方の直接切断を撤去。明示復旧は共通安全チェック経由だけにする。

## Tasks
1. 既存成果物とU-Bootを読取り確認。実機は現在U-Bootプロンプトに停止中。
2. BuildrootのCPIO出力、/init、2台目4GiB DTB、再現可能な生成手順を追加。
3. 共通USBリセットに利用中ブロックデバイス拒否とエラー打切りを実装。手動と自動の呼出元を接続してテスト。
4. USBへ追加書込みせずに一時RAM起動（必要ならローカルネットワーク経由でロード）。SSH・メモリ・配信を検証。
5. 起動媒体の内容を読取り検証・必要なファイルのみ配備。成功した手順で自動起動を設定して再起動検証。
6. 運用・更新・ロールバックと実測結果を記録。未達は未達として報告。コミット/pushは今回行わない。

## Repair / retirement
Repair: USBコントローラ切断と実行中rootfsの依存衝突をRAM起動で除去する。
Retirement: 2台目のUSB root指定は検証後に外す。ext4方式は1台目用に維持する。
根拠のない周期的な固定USBポートの切断はやめ、異常復旧時だけ安全チェック付きリセットにする。
USB/SSDのMBR消失は独立の未解明事項として扱い、RAM起動成功だけで原因解決とはしない。

## Final verification / completion boundary

Goal status: done（2代目のRAM起動移行）。信頼度B: 実機と関連回帰の根拠あり。長時間運転と残るカーネル警告は別の確認対象とする。

- Buildrootの完全ビルドは終了0。標準CPIO/initとuImage wrapperを使用し、独自のOS切替機構は追加していない。
- 一時TFTP起動、USBからの一時起動、保存したbootcmdによる自動再起動を順に検証した。自動起動は3本の読込み成功後だけbootiする。古いconsoleの行長制限に合わせ、定義を2行に分離した。
- 起動後はrootfs /、USBマウントなし、swapなし。MemTotal=3391520kB、最終検証のMemAvailable=2928980kB。U-BootはDTのRAM上限をさらにd8000000へ制限しているため、4GiB全量がLinuxへ見えるとは主張しない。
- SanDisk媒体のUSB認可を外し、/sys/block/sda消失を確認した。SSHが継続し、H.264 1920x1080とAACをffprobeで検出した。配信カウンターは61から70へ増加した。
- 起動用FATをマウントした状態では手動リセットがunbind前に拒否された。アンマウント後の手動リセットは終了0、DT295再列挙と配信再開を確認した。現在の使用回数は1/bootで、制限は最大3回・300秒間隔である。
- 最終NAS APIでは2代目BS11、1代目テレビ静岡ともrecording:true。番組名のEIT取得と配信の進行を確認した。1代目への再起動・設定配備は行っていない。
- 新しいSSHホスト鍵のfingerprintを照合して認証した。TFTP起動後、USB起動後、自動再起動後も同一鍵で接続できた。秘密鍵はGit除外の機体別入力に保持し、成果物も非公開で扱う。

### Media safety / rollback

RAM起動後のLinux読取りでも先頭1MiBは全ゼロだった。serialと容量を照合し、FAT先頭とext4 superblockの位置が既存イメージの区画に一致することを確認した。元の先頭641MiBをNASのRAMへ退避し、さらにホストへgzip保存してCRCと展開サイズを検証した後、既知のMBRの512バイトだけをLinuxから復元した。媒体全体の初期化はしていない。USB再認可と再起動後の区画読込みは成功したが、ゼロ化の原因や以前のU-Boot書込みの見え方は未解明のままである。

旧kernelはFAT内のImage.before-ramboot、旧bootcmdはbootcmd_before_rambootへ退避した。旧linux_boot_usbとext4は自動フォールバックに使わず、手動復旧用として残す。1代目のディスクビルドも維持する。

### Evidence / residual risk

- [全62テストの結果](<../../../work/ramboot/final-tests.log>): 終了0、skipなし。RAM起動14件、USB安全策39件、既存媒体書込み9件。
- [USBなしの実測](<../../../work/ramboot/usb-detached-status.log>)、[音声・映像](<../../../work/ramboot/usb-detached-ffprobe.json>)、[保存後の自動起動ログ](<../../../work/ramboot/serial-persist-autoboot.log>)。
- [マウント中拒否と配置ハッシュ](<../../../work/ramboot/usb-deploy.log>)、[実機リセット](<../../../work/ramboot/manual-reset-hardware.log>)、[最終runtime](<../../../work/ramboot/final-runtime.log>)、[NASの最終状態](<../../../work/ramboot/final-nas-nodes.json>)。
- [運用手順](<../../ram-rootfs.md>) は構成と操作条件を点検し、textlint終了0。シェル構文、git diff --checkも成功した。独立した仕様レビューと品質レビューはPASS。レビュー後のconsole行長修正は回帰テストと実機保存・自動起動で再検証した。
- コントローラーのunbindで既存の_regulator_put警告が1回出る。RAM rootとSSHは生存し、再列挙・配信復帰に成功したが、警告自体を修正したとはしない。次の確認価値が高いのはregulatorのenable/disable対応と長時間運転である。
- 複雑性は範囲内。新しい安全策の所有者を1本にし、旧3経路の直接リセットと周期切断を除去した。USB側root指定は2代目のbootcmdから退役し、1代目と明示的な復旧だけに保持する。
- 臨時TFTPサーバーは停止し、NAS上の一時RAMファイルを削除した。ホスト上のバックアップと機体別SSH identityは保持した。リポジトリは既存の並行作業を含め未コミット。HEADは開始時のc5f5fcfのまま、commit/pushなし。
