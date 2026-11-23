# X96Max_P のLinux起動と全録STB（3台構成）

現在は **3台すべてが稼働中** で、録画からNAS保存・CMカットまで実機で確認済みである。1代目 `192.168.99.109`、2代目 `192.168.99.123`、3代目 `192.168.99.56`。旧README（2026-11-10時点）にあった「チューナー受信は未検証」「SoCエンコードは別途必要」「NAS保存は未検証」はすべて解消済み。録画先はNAS `192.168.99.80`。

## 現状の結論は [docs/README.md](docs/README.md) を先に読む

`docs/` の現行文書が結論の正である。現行／旧（SUPERSEDED）の判断基準、機体別の状態、検証記録への最短導線は [docs/README.md](docs/README.md) にまとめられている。特に [3代目の現状](docs/unit3-status.md)、[2代目の完成状態](docs/unit2-complete.md)、[3台の動作確認](docs/verification-20261122.md) を参照。

## 構成の概要

- **ビルド**: Buildrootの外部ツリー（`BR2_EXTERNAL`）。[固定した取得元とSHA-256](sources.lock.json) はBuildroot 2026.08.1、Linux 6.18.62。
- **起動**: 起動時だけ媒体からLinuxとinitramfsを読み、以後はRAM上で稼働する（RAM rootfs）。再起動時には媒体が必要。手順は [RAM起動の運用手順](docs/ram-rootfs.md)。
- **チューナーと復号**: 1代目はPLEX PX-S1UD（`3275:0080`）とUSBカードリーダー、2・3代目はPIX-DT295のCFW（内蔵B-CAS）。ARIB MULTI2の復号は `torne-b25`。
- **配信**: `eit_watch` → `ffmpeg` ×2 → `stb_streamer` がfMP4を生成し、NAS `192.168.99.80` の録画管理がpullする。番組境界と番組名は放送波のEITから取得する。
- **EPG**: mirakc（`192.168.99.40`）依存を廃止し、3台の `eit_watch` が集めた放送波EITをNASの `pix-epg-bridge` 経由で供給する（[廃止の設計](docs/epg-decommission-design.md)）。
- **CMカット**: NAS側でロゴマーカーと構造ルールで判定し、パケット照合 `verified` のものだけ公開する（[対象拡大の分析](docs/cm-coverage-expansion-20261122.md)、[安全弁の実装](docs/cm-scenario-guards-20261123.md)）。

## 3台の差分

| 項目 | 1代目（.109） | 2代目（.123） | 3代目（.56） |
| --- | --- | --- | --- |
| SoC／RAM | G12A / 2 GiB | G12A / 4 GiB | SM1（S905X3）/ 4 GiB |
| DTB | `meson-g12a-x96-max-p.dtb` | `meson-g12a-x96-max-p-ephy-4g.dtb` | `meson-g12a-x96-max-p-sm1.dtb` |
| 起動媒体 | SD（U-Boot mmc0:1） | USB（U-Boot usb0:1） | SD（U-Boot mmc0:1） |
| チューナー | PX-S1UD + B-CAS | DT295 CFW + 内蔵B-CAS | DT295 CFW + 内蔵B-CAS |
| チャンネル | 地上波 ch17（テレビ静岡） | BS09_0（BS11） | 地上波 ch15（SBS） |

3代目はSM1用DTBが必須で、G12A系DTBを渡すとUSB/vdecが壊れる。2代目はG12A経路なのでSM1用パッチは不要である（[差分の根拠](docs/unit3-status.md)、[機体別の定義](docs/ram-rootfs.md)）。

## ビルド手順

### macOSでビルドする

このMacはApple Silicon、macOS 26.7.1である。既存のDocker／Colimaを使い、専用プロファイル `x96max-linux` でLinux ARM64を動かす。グローバルのDocker contextは切り替えない。

```sh
cd /Users/kenichim/Desktop/work/X96MaxLinuxTSEnc
mkdir -p work/build
./scripts/build-linux.sh > work/build/build.log 2>&1
```

6 CPU・8 GiB RAM・32 GiBディスクのVMを使用する。ビルドツリーはLinux側の名前付きvolume `x96max-build` に置き、macOSのファイルシステムで大文字小文字やLinuxのファイル属性を扱う問題を避ける。ext4もこの環境で生成するため、macOS用のext4ドライバーは不要である。

rootfsにはBusyBox、libusb、usbutils、Dropbear、`torne-c`、`torne-b25`、`pix-ts-legacy` を組み込む。ffmpeg 6.1.5とH.264ハードウェアエンコーダー（`package/meson-venc`、`/dev/video1`）も組み込む。エンコーダーのucodeはoverlayの `lib/firmware/meson/venc/ga_h264_enc_cabac.bin` に配置した。MPEG-2デコーダーの `gxl_mpeg12.bin` もoverlayに配置している。G12A・SM1の双方に対応するパッチの記録は [sources.lock.json](sources.lock.json) にある。

生成物は `output/` に保存する。

| ファイル | 内容 |
| --- | --- |
| `sdcard.img` | SD全体へ書き込むMBR形式のイメージ |
| `Image` | 非圧縮ARM64 Linux kernel |
| `meson-g12a-x96-max-p.dtb` | 1代目向けのDTB |
| `rootfs.ext4` | Linux rootfs |
| `SHA256SUMS` | イメージの照合値 |
| `buildroot.config`、`linux.config`、`toolchain.txt` | ビルド設定とコンパイラー情報 |

UARTは `ttyAML0`、115200 baudである。初期イメージではUARTからrootでログインでき、パスワードは空である。Dropbearはパスワード認証を無効にしている。SSHを使う場合は、ビルド前に公開鍵を `board/x96max/overlay/root/.ssh/authorized_keys` へ配置し、`.ssh` を700、鍵ファイルを600にする。有線LANはDHCPを使用する。

MacのColima（10 GiB）でカーネルを丸ごと作り直すとOOMで落ちることがある（実測 exit 137）。その場合は [NAS（192.168.99.80）でのビルド手順](docs/build-on-nas.md) を使う。機体ごとに `X96MAX_UNIT=1/2/3` を指定する。専用のidentityは `ramboot-identity-unit1` / `ramboot-identity` / `ramboot-identity-unit3` を使う。RAMビルドの具体的なコマンド例は [RAM起動の再ビルド](docs/ram-rootfs.md) を参照。

### macOSでSDカードを作成する（SD起動の機体）

第1区画は128 MiBのFAT、第2区画は512 MiBのext4である。先頭に1 MiBを置き、U-BootやFIPは含めない。既存eMMCのU-Bootから、SD上のファイルを手動で読み込む構成。**2代目はUSBメモリから起動するため、この手順はSDを使う1・3代目向けである。**

まずmacOSへSDカードを接続し、対象を確認する。

```sh
diskutil list external physical
python3 scripts/write-sd.py --device /dev/diskN
```

`diskN` は表示されたSDの番号へ置き換える。この呼出しは対象とハッシュの確認だけである。表示された機種名・容量がSDと一致したら、次で書き込む。指定したSDの既存データは消去される。

```sh
python3 scripts/write-sd.py --device /dev/diskN --erase
```

スクリプトは内蔵ディスク、パーティション、仮想ディスク、容量不足を拒否する。`sudo dd` で書き込み、SDをアンマウントして読み戻す流れである。全体SHA-256が一致すれば取り出す。macOSがext4を読めないという表示を出すことがあるが、初期化は不要である。

書込み直後にFATが自動マウントされ、`.Spotlight-V100` や `.fseventsd` が追加されるのはそのためである。初期の検証ではこの追加により最初の全体SHA-256検査が不一致になった。現在のスクリプトは、差分が先頭のFAT区画内だけの場合に次を確認する。MBR・rootfsを含む区画外の全バイトが一致すること。`fsck_msdos -n` が成功すること。読み取り専用でマウントした `Image` とDTBのSHA-256が一致することである。どれかが失敗すれば成功とは扱わない。

（初期の実績）`diskutil list external physical` で `/dev/disk8`、`MassStorageClass` を確認した。外部媒体は123,773,911,040 bytesである。許可を受けて元のNTFS「Untitled」を消去し、672,137,216 bytesのイメージを書き込んだ。修正後の検証は書込み済みSDに対して実施し、終了コード0で取り出しまで完了している。[検証ログ](work/validation/sd-verify-terminal.log)と[検証結果](work/validation/sd-verification.json)に記録した。本環境の管理者実行ではrawディスクへの書込みが拒否され、Computer UseもTerminal操作を安全制限で拒否したため、書込みと実媒体の検証はユーザー側のTerminalで実行した。

書込み済みの媒体を再検証する場合は、`diskutil list external physical` で接続後の番号を確認してから次を実行する。`--verify-only` はSDを書き直さず、検証に成功した場合はSDを取り出す。

```sh
python3 scripts/write-sd.py --device /dev/diskN --verify-only
```

### 最初のLinux起動（U-Boot）

[U-Boot用の手順](board/x96max/uboot-manual.txt) に手動起動コマンドを用意している。既存U-Bootは2015.01（2019-10-09ビルド）、ボード表示は `u212`、DRAMは2 GiBだった。`booti` とFAT読み込みに対応する。MMC 0がSDIO Port B、MMC 1がSDIO Port CのeMMCである。`bdinfo`、`ext4load`、独立した `part` コマンドはない。

Imageは既存の `loadaddr=0x01080000` に読み込む。DTBは `0x08000000` に読み込み、RAM上の `fdt_addr_r`、`dtb_mem_addr`、`fdtaddr` も同じ値にする。初期に旧DTBアドレス `0x01000000` を使ったところ、`booti` の処理後にDTBマジックが消えて起動に失敗した。Imageの再配置による上書きと整合する結果である。DTBの移動後は正常なヘッダーを確認でき、Linuxが起動した。

SDを本体へ移したらUARTでautobootを止め、手順の `mmc dev 0`、`mmc rescan`、`fatls` を実行する。`fatload` の成功をそれぞれ確認してから `booti` へ進む。初期設定の確認時は `saveenv` を使わず、既存の起動設定を保存し直さない。[U-Bootのbooti仕様](https://docs.u-boot.org/en/latest/usage/cmd/booti.html)では、initramfsを渡さない場合に第2引数へ `-` を指定する。現在のRAM起動では各機体の `bootcmd` をU-Boot envに保存済みである（[ram-rootfs.md](docs/ram-rootfs.md)）。

## チューナー・配信の仕組み

### PX-S1UDとB-CASによる実放送の受信

USBホストの修正後、PLEX PX-S1UD（`3275:0080`）とUSBカードリーダー（`2ce3:9563`）が使える。起動時に `S96bcas` がpcscd → bcasd → cardrelayを立ち上げ、`torne-b25` が使う `/dev/shm/torne/card.sock` を用意する。

```sh
siano-ts --list          # bus:address を確認する（再起動で変わる）
siano-ts --device 1:3 --firmware /lib/firmware/isdbt_rio.inp -c 20 -t 30 \
  | torne-b25 --card-socket /dev/shm/torne/card.sock > /tmp/clear.ts
```

復号後のTSは `scrambled=0`、`tei=0`、`cc_errors=0` になる。B-CASの応答は `cardprobe /dev/shm/torne/card.sock` で確認でき、正常なら `card_status=0x2100` とB-CAS IDが返る。

USBホストが動かなかった原因は、`vcc_5v`（GPIOH_8制御）がUSBコントローラへ配線されていなかったためである。`dwc3` が `regulator_enable()` を呼ばず、VBUSが供給されなかった。動作実績のある `meson-g12a-u200.dts` に合わせて次を追加している。`dwc2` はデバイス専用なので、USB3.0ポート側（xhci）を使う。

```dts
&usb       { vbus-supply = <&vcc_5v>; };
&usb2_phy0 { phy-supply = <&vcc_5v>; };
&usb2_phy1 { phy-supply = <&vcc_5v>; };
```

### DT295の内蔵カードとCFW

2・3代目はDT295のCFW経路で、生TSを内蔵B-CASカードで復号する。`pix-cfw-ts` → `torne-b25` → `pix-ring` → `eit_watch` → `ffmpeg` ×2 → `stb_streamer` の経路でNASへ配信する（[2代目の完成状態](docs/unit2-complete.md)）。純正ファームとCFWは共存せず、`pix-ts-legacy` を動かすとCFWのhookが消える。S97が起動時に `pix-cfw-grab probe` でシグネチャを確認し、無ければ `frc1_load` して入れ直す。DT295のファームウェア状態からの復帰には、`board/x96max/patches/pix-userland/` のパッチ（ソフトリセットとkeepalive周期の変更）を使う。どうしても戻らない個体向けに `pix-dt295-reset`（USBコントローラのunbind/bind）を残している。

### NASへ配信する

`S97pix-stream` が `siano-ts` から `stb_streamer` までの5段を起動する。NAS側は既存のdtv_androidと同じ経路でpullする。

```sh
/etc/init.d/S97pix-stream start        # 端末側
curl "http://192.168.99.109:40774/v1/status"
curl "http://192.168.99.109:40774/v1/streams/gr-20?after=0" -o rec.mp4   # NAS 側
```

設定は `/etc/default/pix-stream-linux`（チャンネル、ポート、許可IP、ビットレート）にある。段を分けている理由は2つある。`h264_v4l2m2m` がextradataを出さないため、1本のffmpegでfMP4をパイプへ書くとavcCが空になり再生できない。2段目にMPEG-TSを渡してSPS/PPSを解析させる。中間をTSにしているのは、FIFOを2本使うとffmpeg同士が出力のオープンで待ち合うためである。起動直後に取りこぼさないよう、`siano-ts` のTSキューは1024スロット（約8秒）で選局は4秒遅らせる。読者がいないあいだスプールは192 MBで頭打ちになり、古い断片から捨てる。

### NASの録画管理に登録する

`stb_streamer` は `_pixstb._tcp.local` を広告するので、NASの録画管理（`http://192.168.99.80:40776/`）の「検出された端末」に現れる。名前は `/etc/default/pix-stream-linux` の `PIX_DISPLAY`、チャンネルは `PIX_CHANNEL` である。

番組として区切るには `pixb` ボックスが要る。`eit_watch` が `torne-b25` と `ffmpeg` の間でTSを素通しし、EIT（PID 0x12、table_id 0x4E）を読む。現在の番組を `/dev/shm/pix/program.txt` に書く。`stb_streamer` は `moof` の直前でこれを読み、番組が変わっていれば `pixb` を差し込む。NASはこれで区切り、EPGから番組名を付けて保存し、CMカットを走らせる。番組名も放送波のshort_event_descriptor（0x4D）から取得し、`stb_streamer` は `pixb` の直後に `pixn` を置く。接続途中から始まる録画のために `GET /v1/program` も用意してある（[docs/eit-program-names.md](docs/eit-program-names.md)）。EPGは mirakc ではなく3台のEITをNASのbridge経由で供給する（[docs/epg-decommission-design.md](docs/epg-decommission-design.md)）。

端末にRTCが無いため起動直後の時計は1970年を指す。EITの開始時刻と現在時刻を比べる判定を入れると、放送中の番組を全部捨ててしまう。そのため、時計が合っているときは放送時間に現在時刻が入るイベントを選ぶ。合っていないときは前へ進む方向でいちばん早いものを選ぶ。

### トランスコードの1コマンド経路

G12AのVDECでMPEG-2をデコードし、HCodecでH.264へエンコードしてAACとMP4へまとめる。ライブTSは実時間で届くため `-re` を付ける。

```sh
ffmpeg -re -c:v mpeg2_v4l2m2m -i input.ts -vf "setpts=N/FR/TB,copy,format=nv12" \
       -c:v h264_v4l2m2m -b:v 6000k -g 60 -c:a aac -b:a 192k out.mp4
```

- `copy` は必須である。ffmpegのv4l2m2mエンコーダーは入力フレームを元のlinesizeのままコピーする。エンコーダーは32境界のストライドで読むため、デコーダーの64境界フレームをそのまま渡すと1行32バイトずつずれる。
- `setpts=N/FR/TB` は、キャプチャ済みファイルの最終フレームに飛んだPTSが付き、muxerがフレームを水増しして数百MBに膨れるのを防ぐ。
- 1080iは `bwdif=mode=send_frame` を挟むと29.97pになる。この場合は新規フレームになるため `copy` は不要である。

実放送の1440x1080i MPEG-2＋AACから、110フレームの1080p29.97 H.264＋AACのMP4を作れることを確認している。3代目（SM1）では `amvenc_avc.ko`（`/dev/video1`）によるVPU H.264エンコードも動作確認済みで、`modprobe` で読み込む（[3代目の現状](docs/unit3-status.md)）。

## RAM起動

3台とも起動時だけOS全体をinitramfsへ読み込み、動作中はRAM上で稼働する。再起動時には起動媒体が必要である（1・3代目はSD、2代目はUSB）。機体別ビルド、起動媒体の更新、揮発する設定、SSHホスト鍵、復旧時の停止条件は [RAM起動の運用手続](docs/ram-rootfs.md) を参照。

## 未解決・未確認の事項

- **3代目のソフトリブート**: エンコード動作中の `reboot` でハングする（再現2回、15分以上復帰せず物理電源で回復）。再起動は電源オフ／オンが正規手順。`S97pix-stream` を停止してからなら、ソフトリブートが通った例を1件確認した。ただし、運用手順として定着しているかは未確認（[unit3-status.md](docs/unit3-status.md)）。
- **ロゴ検出**: 装置の局ロゴ置き場が空になり、CFW経由でのBS15選局は成功したものの929のカルーセルからロゴモジュールが送出されず、ロゴ検出は未解決である。NAS側に毎日04:10の再試行を設定済み（[試行記録](docs/dt295-logo-cfw-20261123.md)）。なおCMカット用のロゴマーカー自体は3局で動作中である。
- **長期安定性**: SDカードの耐久、NAS側ディスク容量、`torne-b25` のカードセッションの長期安定性は観察項目として挙がっているのみで、結果は未確認（[unit3-status.md](docs/unit3-status.md)）。
- **CM判定の精度**: 時刻・無音から推定するscenario経路は、本編を含む誤りが残るため公開を見送っている。安全弁は合成データと実素材7本で確認済みだが、公開率の推移は未評価（[cm-scenario-guards-20261123.md](docs/cm-scenario-guards-20261123.md)）。
- **3代目イメージへの反映**: 11/22のEPG廃止に伴う再ビルド・焼き直しと11/23の監視修正が3代目の現行イメージにどう反映されたかは、[unit3-status.md](docs/unit3-status.md) の出典範囲外で未確認。
- **番組分割の公開レース**: 2つのstagingがほぼ同時にpublishされる。先方のフォルダがまだ無い。このため、同一番組が別フォルダに分かれることがある（[verification-20261122.md](docs/verification-20261122.md)）。

## 検証状況（補足）

イメージ検査・rootfsとCLIの検査・SD書込みスクリプトのモック試験は終了0で完了している。書込みスクリプトの回帰は次で実行できる。

```sh
python3 -m unittest discover -s tests -p 'test_write_sd.py' -v
```

3台の動作確認と修正の記録は [verification-20261122.md](docs/verification-20261122.md) に、現状の索引は [docs/README.md](docs/README.md) にある。初期調査のログは [work/](work/) に残している。

## 旧（2026-11-10時点）の記録

以下は初期調査の事実記録である。当時は受信・エンコード・NAS保存が未検証だったが、現在は上記のとおり解消済み。

| 項目 | UARTで確認した内容 |
| --- | --- |
| 接続 | `/dev/tty.usbserial-0001`、115200 baud、8N1 |
| Androidの機種名 | `X96Max_P`、board/device は `franklin` |
| SoC系列 | Android DTのcompatibleは `amlogic, g12a` |
| CPU | Cortex-A53相当の4コア。当時のAndroidは32bit |
| RAM | `MemTotal=2056328 kB`、DTのusable-memoryは2 GiB |
| OS | Android 9、Linux 4.9.113、`armv7l` |
| eMMC | 30,777,344 sectors、約15.76 GB |
| Ethernet | DTの `internal_phy=1`、起動ログのPHY IDは `01803301`。内蔵100Mb PHYに一致 |
| 接続状態 | 当時はSDとUSBチューナー未列挙、有線LANはNO-CARRIER |

ログの採取には `/dev/tty.usbserial-0001` に対応する `/dev/cu.usbserial-0001` を使った。SoC系列はS905X2系という想定と整合するが、チップの刻印は未確認である。Androidの採取ログは [work/serial-probe-20261110](work/serial-probe-20261110/) に、U-Bootの情報は [work/uboot-probe-20261110/serial.log](work/uboot-probe-20261110/serial.log) にある。当時は既存の `bootcmd` でAndroidへ復帰し、永続的な起動設定は変更していなかった。初期のDTBは標準の `meson-g12a-x96-max.dts`（外部RGMII PHY・初期値1 GiB）を元に、2 GiB・内蔵PHYへ変更した [専用DTS](board/x96max/dts/amlogic/meson-g12a-x96-max-p.dts) を使った。現在の機体別DTBは [3台の差分](#3台の差分) と [ram-rootfs.md](docs/ram-rootfs.md) を参照。初期の起動結果は [work/linux-boot-20261110/boot-result.json](work/linux-boot-20261110/boot-result.json) と [work/serial-20261110/session-result.json](work/serial-20261110/session-result.json) に記録がある。
