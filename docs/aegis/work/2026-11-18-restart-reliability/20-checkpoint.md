# 進行状態（2026-11-18 19:40 JST頃）

## 追記（2026-11-19 09:20頃）停止中断で所有権が残る欠陥の修正
- **症状**：native-first化後、停止中の割り込みで `native exit acknowledgement missing or invalid; retaining ownership` となり、exit ACKが無いまま所有権が永久に残る（実機で1回発生）。
- **根因（2層）**：(1) `worker` PIDはrun_workerの`wait "$worker"`の後に代入され、caught TERMでそのwaitが中断すると変数が未設定のまま`finish`へ入り、`cleanup_children`が即returnしてworkerを追えない。(2) worker死亡後もメタデータの`exit`が無いことを「未drain」と同一視して所有権を保持していた（nativeが起動する前の中断でも同じ）。
- **修正（[pix-stream-simple](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/board/x96max/overlay/usr/bin/pix-stream-simple>) のみ）**：workerが自分のPIDを`worker.$$/worker`へ公開し、`cleanup_children`は変数が空ならこの記録から復元して停止・回収する。ACK判定は3分岐に整理：(a) ACKあり=従来どおり診断ログ。(b) 強制終了なしでnative PID公開済み・ACK無し=**従来どおり所有権保持**（安全側の契約は不変）。(c) native起動前の中断または強制終了=メタデータは残骸として退役し、所有権を解放。残骸削除失敗は`result`を変えずログのみ（所有権を残す理由にしない）。
- **検証**：lifecycle 33件・audit 10件・native seam 5件をhost／target BusyBox両方でPASS。新規回帰2件：trapped wait内でのTERM後も公開済みworkerを回収してexit143で終了し所有権が消える／native PID公開前にworkerをKILLすると残骸を退役しS97 stopが成功する。実機でも新supervisorでlive stop 0→再起動（owner残らず、NAS capturing true）。
- **残る限界**：ACK欠落時の自動回復は行わない（tunerがbusyかもしれず、安全側）。ただしworkerが強制終了された場合は残骸を退役するため、以前より「停止不能」は起きにくい。

## 追記（2026-11-19 08:00〜08:52 JST）USB劣化監査の追加
- **目的**：USB先頭1MiBが消える事象の原因は未特定（U-Boot/Linux不明）。再発を時刻ごと記録する観測を追加した。
- 追加：[pix-usb-audit](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/board/x96max/overlay/usr/bin/pix-usb-audit>)＋[S42pix-usb-audit](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/board/x96max/overlay/etc/init.d/S42pix-usb-audit>)＋[tests/test_usb_audit.py](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/tests/test_usb_audit.py>)。USBはVID/PID/serialで特定し、先頭1MiBのsha256とeMMC環境コピー64KiBのsha256を記録、変化時`USB_FIRST_MIB_CHANGED`/`EMMC_ENV_CHANGED`。記録はRAM上`/tmp/pix-usb-audit/`、2000行で回転。**読み取り専用**、失敗は起動/終了を止めない。
- 実装上の落とし穴と修正：BusyBox ashで`dd|sha256sum`のpipe読み端が継承されて停止する事象と、`dd`を`count=61079552`（カード全体）にしたミスで1時間以上ブロックした事象を実機で踏み、**pipe排除＋64KiBのみ読む**形へ修正。watcher起動も`$0`再実行へ変更（旧`$WATCHER`は実ファイルを再実行せずwatchが動かなかった）。
- 実機確認：`check`0.22s、watcherは300s周期で稼働（PID1777）、`S42 stop`でPID同一性確認後に停止。target BusyBoxでもtests10件PASS、host10件PASS。
- 事故と復旧：調査中にS97を連続再起動し、native-first化で顕在化した**停止途中の強制終了でworker metadataが残り『native exit acknowledgement missing』→所有権保持**に至った。worker.1294はexit ACKを書く前に強制終了されていた（native PID公開のみ）。orphan化した自前`dd`/`sha256sum`を除去し、exit欠落を確認したうえでownerディレクトリを退役→S97start。録画・NAS配信は復帰（totalPushed増）。**この停止中断経路は未修正の残課題**。

## 最優先の現在状態（2026-11-19 07:56 JST、全項目PASS・残課題はfield観測事実2点）
- **コールドブートPASS**：[off](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-cold-poweroff-result.log>)で`POWERDOWN_VERIFIED`（native end exit0/state16確認後にpoweroff）、ユーザーがUSBを抜き差し後、UART入力ゼロで起動ログを取得（[helper出力](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-cold-autoboot-console.log>)は待機helperの欠陥で空、[観測記録](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-cold-autoboot-passive.log>)に実測内容を保存）：`X96Max_P2 RAM-root Linux`→sda 62533286 sectors→`Run /init as init process`。watch helperの`pump()`はfd非blockingで空を返すと待機loopを抜けるため待ちに失敗し（`No confirmed unit2 Linux login during passive capture`）、親のecho probeで実際には起動済と判明。**watch scriptの欠陥は未修正**、次回はreadinessをtime.monotonicで持つよう直す必要あり。
- コールドブート後の実測：root=`rootfs`（RAM）、sda mount 0、4script hash一致、`pix-stream.stopped`無し。capture owner=195、weekly owner=225、`/tmp/pix-ts.log`生成、USB CPIO=`7d142f43…d55a`/Image=`de2e0459…69a4`/MBR=`b433d6dd…d55a`不変、eMMC env tailは保存時snapshotと**バイト一致・CRC有効**。実時刻はJST 07:56で正しい（S41）。
- **コールドブート後のNAS実録画成長**：API `capturing:true/readers:1/totalDropped:0`、recorderのstaging inode同一のまま **12157194→49896069 bytes /40.3秒**（[1](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-nas-recording-after-cold-1.json>)/[2](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-nas-recording-after-cold-2.json>)）。これで「電源断・再起動を契機に録画が止まる」事象は、通常終了→電源断→USB抜き差し→自動起動→**NAS実ファイル増加**まで一貫して再現不能。
- 残課題（field観測だが原因未特定、隠さず報告）：(1) 手動ロゴ取得のcancel時にfetchの`resume`がS97起動成功後も呼び出し側に非0を返すことがある（録画自体は復帰、BB12通りでは再現せず、文言を『verify recording』へ是正）。(2) その後の手動cancelでS98startが`already running`+終了コード1を返す観測があり原因未解明（今回のcold testではweekly正常起動、stale owner無し）。

## 直前の状態（21:50 JST頃、履歴）
- **最終CPIOをUSBへ配備済み**：`rootfs.cpio.uboot` SHA=`7d142f4353e665462144a0d75551c817c15f4e90cb79174f2725c2c42e1b869d`（35189531B）。[置換log](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-usb-cpio-update-final.log>)、独立readbackで同hash・MBR=`b433d6dd…d55a`不変を再確認。Image/2DTBは従来hashのまま。[payload hash一覧](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-release2-SHA256SUMS>)は4点とも最終値（Image/2DTB不変、CPIO=7d142..）で確定済み。
- **release2 build/検証PASS**：target BusyBox 142 run=141PASS+1skip（[log](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-release2-target-tests.log>)、22.379s）、Archive26=25PASS+1skip（[log](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-release2-archive-tests.log>)）。weekly B修正＋fetch警告文言改善を含む最終source。
- **weekly実機2経路PASS**：[手動cancel](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-weekly-live-manual.log>)＝実fetch capture owner確認→S98stop4s→native END0/state16→fetchがresume実行→S97自動起動→**main capture -t1800復帰**。 [shutdown](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-weekly-live-shutdown.log>)＝S99 fence後S98stop2s→『stop requested; leaving the pipeline stopped』→S97stop→`SHUTDOWN_FENCE_PREVENTED_LATE_RESUME`→手動S97startで復帰。
- **field観測の新事実**：手動cancel時fetchの`resume`が**S97起動成功後に呼び出し側だけ非0観測**（`Starting pix-stream: OK`の直後に`restart failed`）。録画は復帰済み。target BBでEXIT trap失敗・signal中断foreground wait・二重TERMを12通り再現试验したが**いずれも0のままで一般BBバグは再現せず**、原因未特定。よってfetchの文言を『resume returned nonzero; verify recording before retrying S97 start』へ変更し、test_logo_fetch.test_restore_failure_is_nonzeroで文言assert追加（host19PASS）。**この非0の実体は未解明**として残す。
- **環境保存＋無人warm boot PASS**：[persist log](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-recipe-persist-console.log>)で`mmc env offset 0x4d400000`/`HBG4e`/3定義readback一致→`ENV_SAVE_OK`→**reset後キー入力ゼロ**で`X96Max_P2 RAM-root Linux`→`x96max-p2 login:`到達。独立readback [after.bin](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-emmc-env-after.bin>)はCRC有効・`bootcmd=run linux_boot_ram`・`bootdelay=5`・3定義長154/311/231で一致。S95不在、4script SHA一致（simple6bb4/S984166/weekly d45d/fetch 1bd5）、block mount無し、MBR不変。
- **保存後rebootでNAS実録画成長確認**：API `capturing:true/readers:1/totalDropped:0`、NAS recorder PID3988744のstaging inode1237866が **63409309→109305001 bytes /52.9秒**（[before](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-nas-recording-after-reboot.json>)/[after](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-nas-recording-after-reboot-2.json>)）。readersカウンタではなく実ファイル書込で再開を証明。
- **残りはコールドブートのみ**：[power-cycle script](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-power-cycle.py>)は`after.bin`CRC＋3定義一致を前提gateにし、S99→S98→S97→owner消失→native END0/state16→sync→poweroff→UARTで`reboot: Power down`確認まで。watchモードは**UART入力ゼロ**でlogin捕捉。ユーザーの抜き差し準備を確認してから実行する（不在のまま録画停止を続けない）。

## 以前の状態（21:15 JST頃、履歴）
- **unit2 RAMはnative-first版へ切替済み**。`pix-stream-simple` SHA=`6bb4d3db41a4404f919696a9515040c89903344c683a8984dc17cb0dfa8ed3fa`、S98=`4166d91fe5e29cb0ca06b4b97dc524b09e0a93056be0cd79dd51e484ba126a6f`。旧INT rootのままnative親のみを正常終了→owner消失→新source atomicinstall→S97startの順で切替：[移行log](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-native-first-ram-migration.log>)。**以後は旧root用parent-stop helperを使わない**（新nativeはwrapper childなのでPPID条件が異なる）。通常S97stopでnative-first停止する。
- **実機native正常終了2種類PASS**：[通常録画中](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-native-first-live-steady.log>)＝stop3s、phase3/END exit0/state16、writer_error0/pending0/rejected0/uncertain0、USB active transfers0/error0/timeouts0。 [起動直後probe](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-native-first-live-probe.log>)＝-t6のfcntl ownerを実確認してstop、2sで同じEND0/state16/phase3/writer_error0、0 packets時の初期化中cancelも正常。どちらもS97startで復帰、vendor lock inode9保持。
- NAS再接続を実確認：最後はcapturing=true/readers1/totalPushed55/totalDropped0。weeklyはこのfield試験中ずっと停止済みのまま。現rootは新metadata付き、正常終了のworkerdirはouterがGCする。
- **残課題はtarget BusyBoxのweekly/fetch二重cancel競合**。final candidate buildは成功、archive9PASS(skip1)だが、[target full117](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-native-first-final-busybox-tests.log>)は116PASS/1FAIL。唯一 `test_manual_logo_stop_cancels_fetch_then_restores_recording` がS98『fetch did not drain (ownership retained)』。native-first全件・起動pubguardはtargetでもPASS。job bash-0c835a842a664414は収集済exit1（BBでchain停止）。USB/eMMCへ最終版を配備していない。
- child `9e01c432-4ca0-4ab5-bb9c-3f0b049930eb` に **readonly診断だけ** を再依頼中。Docker context=colima-x96max-linux、image=x96max-builder:bookworm、volume=x96max-build:/build:ro、project:ro、--init --network none、/bin/shをwork/busybox-test-sh adapterへ。既存source/testsはまだ修正禁止（temp instrument可）。14回baseline/3回instrumentで再現。保存診断 [dir](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/weekly-owner-diag-20261118/>) はfetch release143→capture drain→LOCK解除→weeklyから2度目TERM→fetch restore_done1/release1→weekly child_status1 proxyでOWNERだけ残留、traceはcapture-closed→resume、FETCH_LOCKなし。『再wait not-child』ではなくfetch実rc1の枝だった。**BB一般foregroundwaitバグ仮説は最小BB/Bash各6回で反証**、INIT自身のentry/exitを記録して原因を絞っている。次にsource修正する前に証拠と最小owner-level修正を合意する。
- private raw eMMC before backup CRC64KiB有効。**saveenv未実行、USB最終CPIO未更新、reboot/cold未実施**。USBは9c92..の中間CPIO（old groupTERM）、現在host exportは6bb4..+S98最終を含むcandidate（weekly修正前）でarchive9は一致済。残バグ修正後にweeklyだけsnapshot更新→build/archive→target full全PASS→RAMへ必要ファイル更新→その後にCPIO FAT置換/保存/自動boot/coldへ。
- 追加5行outer publication guard込み child最終109PASS、independent review33PASS（5行追加前）＋guard方針承認。actual parentreadでrootのtrapがcancelled記録→worker=$!→exit143trap復帰の順を確認。simple380行、INT候補313から+67、匿名pipeline保持、FIFO/globalPIDscan/newwatchdogなし。
- ユーザーにはnative終了が実機2経路で正常になったことと残weekly競合を説明済み。電源を抜く合図はまだ。長く待たせているので、実際のpoweroff前に現在すぐ抜き差し可能かを確認してから停止し、不在のまま録画を止め続けない。

## 以下は過去の経緯（20:34以降、上の状態が優先）
- **unit2は起動・NAS録画中**。最後のNAS確認は `capturing=true/readers=1/totalPushed=171/totalDropped=0`。unit1未変更。
- USBの4payload更新・sync/unmount済み：[更新log](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-usb-update-result.log>)。Image=`de2e...`、DTB=`903faa.../b031dc...`、CPIO=`9c92b663f7eb5d52bc00712f2888e74cde8df08bd69083227788651203f6c603`。**このCPIOは下記native停止順修正前なので最終版ではない**。現在のsnapshot/build exportも同じ中間CPIO。
- [RAM-onlyレシピ試験](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-recipe-trial-console.log>)成功。実U-Boot2015.01で3定義をreadback完全一致後 `run linux_boot_ram` だけで新USB Image/DTB/CPIO起動。追加usb resetやfdt RAM編集なし。kernel #14、boot-onあり、sda1/MBR全512B正常、block writes0、RAM-rootでUSBマウントなし、8 scriptsのlive SHAとsource一致、NAS自動復帰58pushedまで確認：[first boot](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-live-first-boot.log>)。eMMC環境はまだ旧版・saveenv未実行。
- Native S98 manualstopはstream PID保持/STOPFILEなし、S97 stop/startはowner消失・vendor inode9保持でexit0：[process-only検証](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-live-lifecycle.log>)。**これはnative正常終了の証明ではなかった**。stop2秒でphase3/endがない点を追跡した。
- **native seamの確定不具合**：simple/fetchの初手group TERMがpix-ts-legacy内部のTS/diag writer2子までabort。vendorのchild_loopはTERM=DFL/INT=IGN、親はINT/TERM両handler。hostの単一Python capture mockでは覆えていなかった。groupTERMでdiagが消え正常終了stateの証拠を失う。USBstop自体が省略されたとまでは断定しない（native doneはrc非0でもUSB stop/shutdownを呼ぶ）。実native ts_outputリンクfixtureで旧sourceの2件FAILを再現済み。
- **INT一斉signal案もsimpleでは不採用**：RAMへ候補2scriptsをatomic hotpatchし旧capture親だけTERMでnative正常終了を確認後に試験。旧→候補移行はphase3/writer_error0/END exit0 state16：[parent-only対照](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-native-parent-migration.log>)。候補groupINT stopはphase4/writer_error32/EPIPE/rejected+uncertain16356/END **exit70 state16**：[INT現物反証](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-native-int-steady.log>)。下流FFmpegまで先に止まるためnative clean endではない。検証後S97をstartしNAS録画復帰済み。現在実機RAMだけINT候補、USBは前記groupTERM版。どちらも最終版と呼ばない。
- **実装child再開中** `9e01c432-4ca0-4ab5-bb9c-3f0b049930eb`：native親PIDだけ停止→実exit ACK→残余media group停止、の順へ最小変更を検討・実装。大規模PID/proc探索/FIFOはまず避け、既存匿名pipelineのfirst-stage wrapperがnativeを&起動して$!とwait exitを既存OWNER配下のworker専用recordsへ出す案を依頼。worker TERM handlerがparentのみTERM→ACK後に残余PGをTERM/reap、outer35+5/KILL/failclosed維持。**native非0だけでowner保持すると正常なNAS再起動/既存data-error自動retryを壊すため、ACK到着/プロセス返却とnativeclean0は分離**するよう追加指示済み。新owner/desired-state/watchdogは作らない。80行程度の設計receiptを受けて実装承認済み：workerはrole関数自体を&/builtinwaitにしてTERMtrapを即時に処理し、native PID/exit recordsはOWNER/worker.$$の下に限定。native ACK!=0でも全group返却ならstop APIはprocessdrain成功でcodeを診断へ、ACK無し/不正/強制/残groupだけfailclosed。通常probe offair/normal native-error retryを保つ。review子 `75723a02-7068-4e9a-b6ce-9fa3492346fd` もplanを並行readonly確認中。parentはsource/testsへ並行編集しない。
- 新native tests：[C fixture](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/tests/dt295_stop_signal_fixture.c>) と [Python](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/tests/test_dt295_stop_signal.py>)。現4件はwriter signal/END-ACKまで、USB/下流reader EOF停止順を覆わない。child final50 PASS（native4+logo19+lifecycle17+pipeline10）もINT候補でありfield反証によりrelease証拠ではない。
- 先のfinal host153件OK(skip9)、archive+life26OK(skip1)、target BusyBox101+probe5 OKはnative4行変更より前。BusyBox検証は **Docker --init必須**（PID1によるorphan reapが実機と同等）。--initなしではprobeのgroup zombieが残りfalse teardown failureを1件観測。Linux /tmp fixture path再帰置換も修正済み：lifecycleのmappingsはsingle-pass regex、common libraryは実archiveで実行bit不要。これらは生産sourceの故障と混同しない。
- **権限**：U-Boot3試行はRAM試験済みで環境保存許可も取得済み。物理cold testもユーザーが『停止完了の合図後に抜き差しできる』と回答。**まだ抜かないよう伝えてあり、poweroff未実施**。最終修正・warm検証後、こちらでpoweroffを確認してから10秒抜き差しを依頼する。
- 保存前のeMMC環境をoffset0x4d400000から128KiB読み取り保全：[opaque backup](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-emmc-env-before.bin>)。先頭64KiBのCRC32有効、old bootcmd=`run linux_boot_ram`/旧retryなしrecipeを確認（秘密値は出力せず）。ファイルは600。read-only、まだsaveenvなし。
- [保存用script](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-recipe-persist.py>)作成済みだが **未実行**。new native clean stopをgate→reboot/intercept→board+offset+mmc1/HBG4e確認→3定義readback→single saveenv(制御文で成功marker)→U-Boot reset、以降キー送信なしで90秒autoboot capture。実行前にnative修正のfield clean0/state16と最終CPIO再更新が必要。保存後Linuxから同eMMC範囲をreadしCRC+3定義一致を独立検証。
- 次：child native親順序修正→review+native seam/target BusyBox→RAM安全切替（[native親のみstop](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-native-parent-stop.sh>)は動作実証済み、current root PIDから自動検証する）→S97 steady/probeでEND0/state16→snapshot更新/rebuild/archive→USB **CPIOだけ** 9c92..から最終値へ逐次NEW/hash/rename/sync/unmount（4payload更新scriptは旧payloadhashを要求するためそのまま再実行不可）→eMMC saveenv/test→cold poweroff/on→NAS実ファイル増加。
- 20:49頃進捗：childはsimple約+65lineのworker phaseを実装済み。native pipe END/ACK＋実simple/S97のsteady/probe host seamはPASS、negative/PIDpub/normalretry/offairを追加中。reviewで **native_rc!=0をrole_statusへ上書きするとzero-byte firmware probeが300秒休止へ誤分類されrecoveryへ到達しないP1** を発見し、子へ当該上書き撤去と既存test_usb_reset_safetyのzero-output recovery case再実行を依頼済み。その他 source-only reviewはphase/outer budgetに新P1なし。最終source handoffとfresh全関連testsはまだ待ち。
- 親が将来用に [poweroff/passive cold capture](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-power-cycle.py>) と [CPIOだけのFAT置換](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-usb-cpio-update.sh>) を作成、**未実行**。poweroff scriptはsaved eMMC after snapshot CRC+3定義一致を要求し、正常nativeEND0/state16 gateを通らないとpoweroffしない。watchはUART入力ゼロ。長く待たせた場合はpoweroff前にユーザーの現在の抜き差し準備を確認する（停止してから不在で何時間も録画不能にしない）。
- NAS実ファイル検証helper [read-only snapshot](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-nas-recording-snapshot.py>) 作成/実行済み。ku@.80 sudo -n python3 - へstdinで渡し、unit2のpix_stream_recorder args .123に限定したopen staging fmp4 FDをfstatする。現在native recorder PID3956441、同一inode1237319のstagingファイルが **772926396→946837418 bytes /204.8秒** 増加（before-final json2個）。APIreaderだけでなく実録画書込を証明した。最終warm/coldでも同helper2時点を比較する。
- 20:55頃最新：child fresh108（USB47/native5/lifecycle27/logo19/pipeline10）と独立review33件PASS、S98 ESRCH修正込み。review P1のnative_rc上書き削除済み。parentのtarget BusyBox core70件PASS、candidate image build bash-46c22c574ca943c9もexit0で両job収集済み。ただしこのbuildにはS98最終修正が無く、次のouter guardも無いためrelease用でない。
- **childはさらに最終限定5行＋1test対応中**：outer `setsid &`→`worker=$!` 間TERMでworker未記録のままfinishがorphanを残す既存窓を、cancel-defer→PID格納→元exit143trap復帰→cancelならfinish、で閉じる許可済み。子がinactiveに入る直前の送信が未処理だったため、inactive後に再送し『受信・再開済み』報告あり。fresh109とsource再freezeを待つ。他scope探索はしない指示。独立reviewerも方針妥当と確認（33PASSはこの5行追加前）。
- **次のRAM切替順に注意**：metadataありworkerを古いINT supervisorが起動すると後者はWORK子を削除できないため、**新sourceを先に/usr/binへ入れない**。最終source2filesを/tmp/pix-native-first-stageへstage/hash確認→旧sourceのまま [native親だけstop](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-native-parent-stop.sh>) でEND0/state16/owner消失→[停止済みをgateしたinstall](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-native-first-install.sh>) でsimple+S98をatomic置換→S97start。この順なら旧supervisorが新workerを勝手に起動しない。
- 現在running bash jobなし。source編集はchildだけ。未commit/no push、foreign Cとtest_program_boundary/private dropbearは引続き除外。

## 以前の追記（上の状態が優先）
- 19:59 JST頃、unit2は旧CPIO＋RAM boot-onで引き続き録画中。USB内の4 payloadは旧版SHA一致・readonly mount後unmount済み。[old payload確認](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-usb-old-payloads.log>)、block writesは依然0。
- `/tmp/pix-restart-stage` を実機RAMに新規作成し、新Image `de2e...` と2種DTB `903faa...` / `b031dc...` を転送・SHA確認済み。rootfs.cpio.ubootは未転送。`/mnt/pix-restart-boot` は空の既存directoryとして残る。USB書込は限定MBR修復1回のみ。
- **追加許可取得済み**：U-Boot最大3試行レシピをRAM-only試験し、正常起動を確認した後、eMMC環境変数領域へ保存してよい。OS/bootloader本体の書換不可。まだsaveenvは未実行。
- 最初のP1/S98副作用とmanual CLI shutdown drainは修正済み（S99 shutdownフック＋S97 inhibit、S98はlogoだけstop）。しかしreviewでsuccessor fetchがpre-pause LOGO_STOP/TERM中止するhandoff変種を再現。元実装childが **started_pipeline bool自体の撤去**、正常lock解放後はS97のdesired-stopだけを根拠に常にresume/skipへ修正し、新取消回帰を追加中。mkdir取得〜pid公開もcancelledflagでTERMを遅延しstale lockを防ぐ。さらにreleaseのSIG_IGNがresume経由で新サービスへ継承する問題を実fixtureで再現し、caught noop trapへ変更中。子 `9e01c432-4ca0-4ab5-bb9c-3f0b049930eb` の最終44件結果を待ち、review子 `75723a02-7068-4e9a-b6ce-9fa3492346fd` へ再確認すること。**まだ配備禁止**。
- 親helperの追加発見：actual target BusyBox ashでは、callback中TERMをcancelled=1だけ記録して戻ると2回目guardのhere-doc readでhang。ホストdashには出ない。[trace](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-busybox-cancel-trace.log>)。修正は **unbind/bindのsysfswrite〜所有記録の短いchanging windowだけsignal defer、それ以外はexit143→cleanup**。cleanupは追加TERMを無視してbind完遂。bind_attempted設定前にもchanging=1を置き、write前取消でbindを省略しない。3回帰追加、[host51件](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-host-safety-tests.log>) と [target BusyBox ash51件](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-busybox-tests.log>) それぞれPASS(exit0)。testsの送信はshell PPIDでなくPython os.getppidへ置換。
- BusyBox検証adapter：[wrapper](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/busybox-test-sh>)。targetは **glibc** でありmuslではない。ephemeral Docker root内だけ `/bin/sh` の元 `/usr/bin/dash` を確認してwrapperへsymlink置換。wrapperはtarget ld-linux＋target libでtarget busybox shを起動（host rootは一切変更しない）。source/volumeはro・network none。例外的51件だけでlifecycle全件検証済みとは言わない。
- source snapshotへS97/S98/S99/fetch/helperを1回refreshしbuild exit0済みだが、**その後のabort-handoffとtrap修正前の中間物**。再コピー・再build必須。Archive testはS99とmanual-resetの同一性・regular executableも検査するよう親が追加済み。
- full host suiteは無関係の既存synthetic EIT testcaseで160秒以上stall（test内部timeout10sでも復帰せず）しjobを停止・collect済み。子も消失確認済み。[途中log](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-full-tests.log>)。同じ8 EIT testsはLinux containerで1.072s/全部PASS：[Linux EIT](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-linux-eit-tests.log>)。これをMac全suite成功とは表現しない。次は安定sourceで関連modulesを明示してhost＋BusyBox（またはLinux full suite）実行し、無関係Macのstallに再度長時間費やさない。
- relevant running bash jobなし。直近jobs `bash-3c233159735147ed`=safety51×2 exit0、`bash-95528b6810e64231`=中間build exit0、`bash-983536c96f7c49fb`=host全体stallでkilled、`bash-690d7e64f08941a6`=Linux EIT8 exit0、全collect済み。

## 実機・現在の安全な状態
- unit2 `192.168.99.123` はRAM起動・録画中。1台目とNASの配信設定は変更なし。
- 現在のLinuxは旧Image/旧CPIO。U-Bootで旧DTBに **RAM上だけ** `regulator-boot-on` を追加して起動した。保存環境とUSB内の起動ファイルはまだ旧版。新スクリプトは実機へ未反映。
- SanDisk `0781:5580` / serial `AA010206130733341409` / `62533286` sectors / `SSD U100 32GB 1` をLinuxとU-Bootで同定。現在はUSB3 `2-1` として列挙し、`sda1` も認識。
- RAM変更後、LinuxのUSB先頭512バイトは正常イメージの全512バイトと一致。NAS `readers=1 / capturing=true / totalPushed` 増加、BS11番組情報も更新。

## 起動媒体障害の新しい証拠
1. 旧状態からチューナーの実fcntl所有者と旧supervisorへTERMし、45秒以内の自然終了・ロック消失を確認して再起動。旧S97の3/5秒KILLは使っていない。
2. 旧DTBでの自動warm bootはLinux到達したが、USB先頭 **1MiBが全ゼロ**。FATは1MiBから正常、Linuxのディスクwrite/sectors-writtenカウンターは0。O_DIRECTの再読でも先頭512バイトゼロ。bootcodeだけのゼロとの混同ではない。
3. 次のU-Bootでも全512バイトゼロ、fatload失敗を確認。したがってLinuxのキャッシュだけの見え方ではない。ただしどの段階の何が消したかは未確定。
4. ユーザーに条件付き復旧範囲を質問し、**同じSanDiskの先頭512バイトだけ正常イメージから書き戻す許可を取得済み**（起動失敗確認時のみ、FATや他領域変更なし）。FAT VBRの全512バイト一致とSanDiskの機種・容量を確認し、RAMへ正常MBRを構成→全512バイト検証→USB LBA0 count1だけ書込→全512バイト読戻し一致を確認した。
5. その後旧3ファイルをロードし、fdtコマンドでRAMのVCC_5Vへboot-onを追加して起動。USB3列挙・sda1・正常MBR保持・録画再開を確認。MBR修復/usb reset/待機という交絡があるため、**これだけでboot-onが事故の唯一の原因/完全解決とは断言しない**。修正版の永続反映と反復warm boot、可能ならpoweroff後の起動検証が必要。

### 根拠ログ
- 旧設定warm boot：[console](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-baseline-console.log>)、[Linux disk read](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-baseline-disk-read.log>)、[ゼロprefix原本](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-baseline-usb-prefix.bin>)。
- U-BootゼロMBR/限定修復：[first comparison](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-boot-on-console.log>)。writeの返答は `1 blocks write: OK` であり、実験コードが `written` を期待したため一旦安全停止（書込自体は実行済み）。
- 修復読戻しとRAM変更起動：[resume](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-boot-on-resume.log>)、[Linux read](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-boot-on-disk-read.log>)、[正常MBR](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-boot-on-sector0.bin>)。
- 実験コードは作業用のみ。本番ツールへ昇格させない。最初の比較スクリプトは同名ログの再実行をxbで拒否する。

## 確認したソフトウェア問題と修正
1. S41はstopでも時刻取得を30回行う → stop即終了、4 tests。
2. RAM U-Bootレシピがcold USB settle/retryを失っていた → 最大3試行、全3ロード成功時のみbooti。各定義500字未満。実機saveenvは未実行。
3. DT295の正規終了より短い3/5秒KILL、global name-kill、logoの遅い復帰、fcntl inode unlink → S97/simple/logoの所有者・TERM drain35+5秒・desired-stop契約へ修正中。
4. USB resetのunbind後取消で未bindが残る → EXIT cleanupで1回だけbind、mounted-block/swap guard・予算は保持。
5. Buildrootの加算overlayで削除済みS95が残る → CPIO専用finalizerで既知obsolete regular file1件だけ除去。S95はfile-input未生成なら起動しないため『常に二重チューナー』ではない。
6. Linux6.18.62 fixed-regulatorはboot_onがなければGPIOを論理OFF初期値で取得し、その後USB/PHYがONにする。active-low/open-drainなのでOFFは高インピーダンス側。USB起動で既にONの電源へ不要な遷移を入れるsource-proven問題としてDTSにboot-onを追加。always-onは同等でなく、shutdownや意図したoffを禁止しないため選ばない。
7. dwc3 glue removeは自分のvbus参照をdisableしない。したがって既存unbind/bindはcontroller/PHY初期化であり、物理的VBUS断の保証はない。コメント/表示を訂正。無関係なdriver patchやライブ電源操作は追加しない。

## 独立レビューで見つかった未解決P1（deploy禁止）
実装child `9e01c432-4ca0-4ab5-bb9c-3f0b049930eb` が修正を再開。review child `75723a02-7068-4e9a-b6ce-9fa3492346fd` はreadonly。
- S98 manual stopがS97 desired-stopまで設定し、週次ロゴだけ停止したつもりでも次capture終了で録画が永続停止。修正案：S97 `inhibit` と、rcK逆順でS98より先に呼ぶ専用S99 shutdownフック。S98単体stopは独立したcancel/restoreに戻す。
- fetch lock解放→resumeの間に次fetchが取得すると、PIDFILE消失から次fetchが復帰責務を持たず、先resumeもlockで拒否され録画が永久停止。修正案：desired-stopなしならPIDFILEの有無に依らず必ずpauseして借用し、最後のfetchも復帰責務を継承。ロック/PIDではなくdesired-stateを正とする。
- 両方reviewの実fixtureで再現済み。旧ホスト試験は通っていても未検出だった。修正と新回帰試験を待ち、再レビューしてから配備。
- S97 stop単独はactive fetchを取消せず、lock残なら非zeroでresetを禁止する境界。通常shutdownはS99 fence→S98/weekly/fetch取消・reap→S97 stopの順で閉じる。
- 強制KILL/teardown失敗でowner/control/fetch mkdir lockを残すfailclosed仕様は明記が必要。/runは再起動で消える。自動unlinkして多重起動を起こさない。

## ビルド・テスト
- [統合試験](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-integrated-tests.log>)：settime/ramboot/profiles/USBreset 93件 OK、optional archive 9 skip。Startupとrun_auto_loopのfixtureも新しい実exec/所有者へ適応済み。
- childのlogo12+lifecycle11+pipeline10=33件は修正前時点でPASS。上のP1を検出しなかったため完了証拠にしない。
- 並行セッションが変更中のCソースとprogram-boundaryテストを混ぜないため、HEAD a2a60932から隔離した [build source](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-build-source/tools/uboot_boot_linux.py>) に本タスクのproduction12ファイルだけコピーしてビルド。
- Docker `colima-x96max-linux`、`x96max-build:/build`、source snapshotを `/project:ro`、identity=`work/ramboot-identity`、unit2 ramboot profile。
- linux-reconfigure/buildと全image生成はexit0。出力 `output/ramboot-restart-fix` は **レビュー前の中間物であり未配備**。P1修正後にsnapshotの該当scriptを更新し、CPIO/imageを再生成すること。
- [実CPIO試験](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-archive-tests.log>)：9件OK(logo cacheなしで1skip)、S95不在・source一致・private identityを確認。新S99も検査へ追加すること。
- [実DTB逆コンパイル](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/work/restart-built-dtb.dts#L3225-L3234>)でVCC_5Vのboot-onを確認。
- 新Image SHA256 `de2e0459e0626f125da640e90e16e47120d44fcfde110ae6125dafc2d1ba69a4`（旧Imageとは異なる）。カーネルsource変更はDTSのみだが再configureで再生成されているためUSB更新はImageも対象。

## 次の具体手順
1. 上記P1修正・tests、独立review残件、全回帰試験とBusyBox実行を確認。
2. source snapshotへ修正版をコピー→CPIO/image再生成→実archive一致。イメージはprivate SSH identity入り、公開しない。
3. 現在USBが見えるので、既存正常payloadとhashを照合・backup元を保全したうえで起動ファイルを更新可能。FATは128MiBのため全NEW一括では足りない。DTB→Image→CPIOの各NEW hash検証/rename/syncで逐次置換し、全verify→unmount。全block-controller resetは行わない。
4. 旧rootからの切替時は前記old supervisor/captureへTERMし自然drain。新起動後はS97 stop/startの正規動作、NASファイル増加、USB MBR・ファイルhash保持を確認し反復reboot。
5. USBファイルの永続反映とU-Boot saveenvは別。後者はRAM-onlyでレシピを検証し保存先がeMMC offset0x4d400000と再確認してから慎重に行う。過去のblank128Bだけの証拠を再使用しない。

## 未確定・禁止する短絡
- boot-onの効果は支持されたが交絡あり、MBRゼロ化のwriter/電気波形/唯一原因は未確定。
- 外部PHYのpinctrl/tx-delay欠落は現在1Gbps動作。今回原因としてblind修正しない。
- strip済/未strip済のhash差は正しいBuildroot stripオプションで一致済み。並行make由来のstale binary説は撤回済み。
- 同名プロセス数だけで多重起動としない。pipeline関数shell/output子も存在。
- 現在未commit/no push。並行変更の [stb_linux_pipe.c](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/package/stb-streamer/src/stb_linux_pipe.c>) と [test_program_boundary.py](</Users/kenichim/Desktop/work/X96MaxLinuxTSEnc/tests/test_program_boundary.py>) は本タスクで変更・取り消し・commitしない。未追跡dropbear鍵も触らない。
