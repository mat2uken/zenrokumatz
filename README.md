# X96Max_P のLinux起動とUSBチューナー

既存eMMCのU-BootからSD上のLinuxを起動し、`dtv_android` で使っているチューナー処理をLinux ARM64向けにビルドする構成です。2026年11月10日に、macOSからUARTで実機情報を読み取りました。

SD上のLinux 6.18.62 ARM64を起動し、UARTからrootログインできました。チューナー受信は未検証です。SoCによるH.264エンコードは、G12A向けの外部ドライバー移植が別途必要です。

次のセッションでは[残作業のレビューと引継ぎ](docs/handoff-20261110.md)を読み、[再開用プロンプト](prompts/resume.md)を使用してください。

## Android起動時に確認したこと

| 項目 | UARTでの観測 |
| --- | --- |
| 接続 | `/dev/tty.usbserial-0001`、115200 baud、8N1。採取には対応する `/dev/cu.usbserial-0001` を使用 |
| Androidの機種名 | `X96Max_P`、board/device は `franklin` |
| SoC系列 | Android DTのcompatibleが `amlogic, g12a`。S905X2系という想定と整合するが、チップの刻印は未確認 |
| CPU | Cortex-A53相当の4コア。現在のAndroidは32bit |
| RAM | `MemTotal=2056328 kB`、DTのusable-memoryは2 GiB |
| OS | Android 9、Linux 4.9.113、`armv7l` |
| eMMC | 30,777,344 sectors、約15.76 GB |
| Ethernet | DTの `internal_phy=1`、起動ログのPHY IDは `01803301`。内蔵100Mb PHYに一致 |
| 接続状態 | SDとUSBチューナーは未列挙。有線LANはNO-CARRIER |

Androidの採取ログは [work/serial-probe-20261110](work/serial-probe-20261110/) にあります。その後、許可を受けて一度再起動し、[U-Bootの情報](work/uboot-probe-20261110/serial.log)も採取しました。既存の `bootcmd` でAndroidへ復帰し、`X96Max_P` のrootプロンプトを確認しています。永続的な起動設定は変更していません。

## この個体向けのDTB

Linux標準の [meson-g12a-x96-max.dts](https://github.com/torvalds/linux/blob/v6.18/arch/arm64/boot/dts/amlogic/meson-g12a-x96-max.dts) は、外部RGMII PHYと初期値1 GiBのRAMを記述しています。今回の2 GiB・内蔵PHYの個体に、そのまま適用する前提にはできません。

[専用DTS](board/x96max/dts/amlogic/meson-g12a-x96-max-p.dts) は標準DTSを読み込み、RAMを2 GiB、Ethernetを `internal_ephy` / RMIIへ変更します。内蔵PHYの接続方法は [upstreamのG12A U200](https://github.com/torvalds/linux/blob/v6.18/arch/arm64/boot/dts/amlogic/meson-g12a-u200.dts) に合わせています。Wi-FiとBluetoothは初回起動の対象から外しました。

SD、USB、電源などは標準X96 Max用DTSを引き継ぎます。これらの配線と実機の一致、U-BootによるRAM記述の書換え、Linuxでの2 GiB認識は起動試験で確認します。DTBのコンパイル成功だけでは実機との適合を判断できません。

G12共通DTSはBL31用の `0x05000000` から3 MiB、BL32用の `0x05300000` から32 MiBを予約しています。後者はAndroid DTのsecos記述と一致します。ロードアドレスを決める際の確認事項は、これらに加えて実機U-Boot自身とDTBの使用範囲です。

## macOSでビルドする

このMacはApple Silicon、macOS 26.7.1です。既存のDocker／Colimaを使い、専用プロファイル `x96max-linux` でLinux ARM64を動かします。グローバルのDocker contextは切り替えません。

```sh
cd /Users/kenichim/Desktop/work/X96MaxLinuxTSEnc
mkdir -p work/build
./scripts/build-linux.sh > work/build/build.log 2>&1
```

6 CPU・8 GiB RAM・32 GiBディスクのVMを使用します。ビルドツリーはLinux側の名前付きvolume `x96max-build` に置き、macOSのファイルシステムで大文字小文字やLinuxのファイル属性を扱う問題を避けます。ext4もこの環境で生成するため、macOS用のext4ドライバーは不要です。

構成はBuildrootの `BR2_EXTERNAL` で管理しています。[固定した取得元とSHA-256](sources.lock.json) はBuildroot 2026.08.1、Linux 6.18.62です。rootfsにはBusyBox、libusb、usbutils、Dropbear、`torne-c`、`torne-b25`、`pix-ts-legacy`、ffmpeg 6.1.5、G12A向けH.264ハードウェアエンコーダー（`package/meson-venc`、`/dev/video1`）を組み込みます。エンコーダーのucodeはoverlayの `lib/firmware/meson/venc/ga_h264_enc_cabac.bin`、デコーダーの `gxl_mpeg12.bin` も同様に配置しています。

生成物は `output/` に保存します。

| ファイル | 内容 |
| --- | --- |
| `sdcard.img` | SD全体へ書き込むMBR形式のイメージ |
| `Image` | 非圧縮ARM64 Linux kernel |
| `meson-g12a-x96-max-p.dtb` | この個体向けのDTB候補 |
| `rootfs.ext4` | Linux rootfs |
| `SHA256SUMS` | イメージの照合値 |
| `buildroot.config`、`linux.config`、`toolchain.txt` | ビルド設定とコンパイラー情報 |

UARTは `ttyAML0`、115200 baudです。初期イメージではUARTからrootでログインでき、パスワードは空です。Dropbearはパスワード認証を無効にしています。SSHを使う場合は、ビルド前に公開鍵を `board/x96max/overlay/root/.ssh/authorized_keys` へ配置し、`.ssh` を700、鍵ファイルを600にしてください。有線LANはDHCPを使用します。

## macOSでSDカードを作成する

第1区画は128 MiBのFAT、第2区画は512 MiBのext4です。先頭に1 MiBを置き、U-BootやFIPは含めません。既存eMMCのU-Bootから、SD上のファイルを手動で読み込む構成です。

まずmacOSへSDカードを接続し、対象を確認します。

```sh
diskutil list external physical
python3 scripts/write-sd.py --device /dev/diskN
```

`diskN` は表示されたSDの番号へ置き換えます。この呼出しは対象とハッシュの確認だけです。表示された機種名・容量がSDと一致したら、次で書き込みます。指定したSDの既存データは消去されます。

```sh
python3 scripts/write-sd.py --device /dev/diskN --erase
```

スクリプトは内蔵ディスク、パーティション、仮想ディスク、容量不足を拒否します。`sudo dd` で書き込み、SDをアンマウントして読み戻す流れです。全体SHA-256が一致すれば取り出します。macOSがext4を読めないという表示を出すことがありますが、初期化は不要です。

書込み直後にFATが自動マウントされ、`.Spotlight-V100` や `.fseventsd` が追加されるのはそのためです。今回もこれらの追加を確認し、最初の全体SHA-256検査は不一致になりました。現在のスクリプトの確認項目は、差分が先頭のFAT区画内に限られる場合に、MBR・rootfsを含む区画外の全バイトが一致すること、`fsck_msdos -n` が成功すること、読み取り専用でマウントした `Image` とDTBのSHA-256が一致することです。どれかが失敗すれば成功とは扱いません。

今回の作成対象は `/dev/disk8`、`MassStorageClass`、123,773,911,040 bytesの外部媒体です。許可を受けて元のNTFS「Untitled」を消去し、672,137,216 bytesのイメージを書き込みました。修正後の検証は書込み済みSDに対して実施し、終了コード0で取り出しまで完了しています。[検証ログ](work/validation/sd-verify-terminal.log)と[検証結果](work/validation/sd-verification.json)に記録しました。

本環境の管理者実行ではrawディスクへの書込みが拒否され、Computer UseもTerminal操作を安全制限で拒否したため、書込みと実媒体の検証はユーザー側のTerminalで実行しました。通常の作成手順は上記のとおりです。書込み済みの媒体を再検証する場合は、`diskutil list external physical` で接続後の番号を確認してから次を実行します。

```sh
python3 scripts/write-sd.py --device /dev/diskN --verify-only
```

`--verify-only` はSDを書き直しません。検証に成功した場合はSDを取り出します。

## 最初のLinux起動

[U-Boot用の手順](board/x96max/uboot-manual.txt) に、この個体の手動起動コマンドを用意しています。既存U-Bootは2015.01（2019-10-09ビルド）、ボード表示は `u212`、DRAMは2 GiBでした。`booti` とFAT読み込みに対応しています。MMC 0がSDIO Port B、MMC 1がSDIO Port CのeMMCで、MMC 0はカード未挿入と表示されました。`bdinfo`、`ext4load`、独立した `part` コマンドはありません。

Imageは既存の `loadaddr=0x01080000` に読み込みます。DTBは `0x08000000` に読み込み、RAM上の `fdt_addr_r`、`dtb_mem_addr`、`fdtaddr` も同じ値にします。今回のImageは41,757,184 bytes、DTBは51,071 bytesです。初回は旧DTBアドレス `0x01000000` を使いましたが、`booti` の処理後にDTBマジックが消え、起動に失敗しました。Imageの再配置による上書きと整合する結果です。DTBの移動後は正常なヘッダーを確認でき、Linuxが起動しました。読込み直後の非重複だけを調べた以前の検査では、この問題を検出できていませんでした。

SDを本体へ移したらUARTでautobootを止め、手順の `mmc dev 0`、`mmc rescan`、`fatls` を実行します。`fatload` の成功をそれぞれ確認してから `booti` へ進みます。初回は `saveenv` を使わず、既存の起動設定を保存し直しません。[U-Bootのbooti仕様](https://docs.u-boot.org/en/latest/usage/cmd/booti.html)では、initramfsを渡さない場合に第2引数へ `-` を指定します。

rootfsの指定は `root=PARTUUID=96320001-02` です。U-BootのMMC番号とLinuxのデバイス番号を同一と仮定しません。起動後は `uname -a`、`cat /proc/meminfo`、`mount`、`ip addr`、`lsusb` を確認し、SDのrootfs、RAM、UART、有線LAN、USBの認識を記録します。

Androidへ戻す際、既存 `bootcmd` の先頭で `ddr_auto_fast_boot_check` が未定義という表示が出ました。後続の既存処理でAndroidへ復帰できたため、この起動設定は変更していません。

## チューナー実装の流用

`dtv_android` のHEADは `4fe3b1358c98d25794175798310e44cc935488f1` でした。既存の未コミット変更は保持しています。

| 既存の処理 | Linuxでの扱い |
| --- | --- |
| `torne-c`／`torne-b25` | C/C++・libusb実装をLinux ARM64向けに再ビルド。受信とB25処理の2プロセス構成を維持 |
| `pix-ts-legacy` | `dt295/transcode-hooks` の同じCLIを再ビルド。`DT295_CAPTURE_TESTING` とXREGフックを維持 |
| `capture-agent.sh` の選局・FIFO接続 | 起動コマンドとプロセス構成を参照。Androidのパスやサービス起動を含む全体移植は後段 |
| `c_src` のfMP4・HTTP・バッファ処理 | POSIX部分は再利用候補。初期受信イメージには未組込み |
| `LiveTsStreamer.java` | MediaCodec、Android Surface、EGLに依存。LinuxではV4L2／FFmpeg等の処理を実装する必要がある |

`dtv_android` の作業手順にはtorneの実行ファイルを稼働機から配布すると記載されており、受信側のソースは同リポジトリに含まれていません。関連する `pix-smb400/repo` の履歴 `861d55040d81ba5d009c589ebada7144eff760ee` から必要なC/C++コードとLICENSE・NOTICEを [vendor/torne](vendor/torne/) に取り出しました。稼働中Androidバイナリーとの同一性までは確認していません。

PIX-DT295は `rk2kk/pix-userland` の `a866c61067f03557701914a2e48a3a11bfbb706e` を [vendor/pix-userland](vendor/pix-userland/) に保存しています。Android用の `-llog` とNDKコンパイラー指定をLinux向けに置き換えます。`/tmp` が存在するため、Android固有の一時ディレクトリー用パッチは不要です。

**既存文書との相違:** `dtv_android/docs/dt295-integration.md` は古い `assets/dt295.assets` と外部の `firmware/dt295.bin` を比較しています。今回取得した参照ブランチ自身に、衛星対応の `firmware/dt295.bin` が含まれていました。SHA-256の先頭は同文書の成功側と同じ `3cabe15f` で、完全な値は `sources.lock.json` に記録しています。このファイルをrootfsの `/usr/share/pix-tuner/dt295.assets` に配置しました。メーカー由来のデータには同梱の表記が適用され、ソースコードのGPL表記だけで再配布条件を判断しません。

取得したCLIのヘルプには、DT295を `macOS target 0` とする古い表示が残っています。今回は既存Androidビルドと同じ `DT295_CAPTURE_TESTING` によって実際のUSB受信処理を有効にしており、Linuxでの受信結果は今後確認します。

接続後の最初の確認は次の形です。チャンネル番号は設置場所の物理チャンネルへ置き換えます。

```sh
# torne: USB認識と短い未復号TS取得
lsusb
torne-c --probe
torne-c --channel 20 --duration 10 --raw > /tmp/torne-raw.ts

# PIX-DT295: 内蔵B-CASを使う既存CLI
pix-ts-legacy --model dt295 --assets /usr/share/pix-tuner --list-json
pix-ts-legacy --model dt295 --assets /usr/share/pix-tuner -c T17 -t 10 -o /tmp/dt295.ts
```

torneは続けて、`capture-agent.sh` と同じカード用Unix socket＋`torne-b25`の組合せで復号後TSを取得します。受信側と復号側の終了コード、TEI・CC・スクランブル表示は別々に確認します。ファイルができただけでは受信成功とみなしません。映像・音声のデコードと停止後の再開始まで確認する必要がありますが、これらの実機試験はまだ実施していません。

DT295の既存実装はfirmware state `0x00` または `0x10` からの開始だけを扱っていました。
`board/x96max/patches/pix-userland/` のパッチで、前回の残り（`0x11` / `0x12` /
ストリーム中 `0x01`・`0x02`・`0x04` / エラー `0xFFFF`）からも、USBを切り直さずに
ソフトリセットで `0x10` へ戻すようにしています（コマンド3。`0x11` は前のセッションが
残らないようアイドルイメージを読み直し、駄目ならサスペンド→レジューム）。同じパッチで、
セッションの keepalive を12秒周期（従来30秒）にし、
ACKを待たないファームウェアへのコマンドは4回で打ち切るようにしました。
それでも戻らない個体向けに `pix-dt295-reset`（USBコントローラの unbind/bind）は
残しています。

局ロゴの受信は `pix-dt295-logo` で行い、`stb_streamer` の `/v1/logos` で NAS に
配ります。地上波は各局の TS の CDT から、`eit_watch --logo-dir` が録画を止めずに
拾います。BS/CS は 929 のカルーセルからで `pix-logo-fetch` がチューナーを借り、
`S98pix-logo` が週 1 回（既定 月曜 04:00 JST）自動で実行します。NAS 側は
`cm_logos.py` がロゴからマーカーの初期値を書き、CM カットは従来どおり
それを測って判定します。詳細と使い方は
[docs/dt295-station-logos.md](docs/dt295-station-logos.md)。

## SoCエンコードまでの順序

[参照会話](chatgpt-conversation://6af26e0b-e484-83e8-b514-321cc0042f39) の方針を引き継ぎ、最終的には1番組の1080i入力から29.97pのH.264/AAC MP4を作り、NASへ保存する構成を検討します。今回の初期イメージは起動とチューナー受信までを扱います。

候補の [meson-sm1-codecs](https://github.com/ray38080289/meson-sm1-codecs/tree/eb7a44464af1c7793f793698e309abe4e6ad82da) は、対象と動作確認がSM1／S905X3です。固定コミットの `compat.c` は `amlogic,sm1-vdec` を探し、`compat.h` はCPU種別をSM1固定で返します。G12A用DTBを渡すだけではこの個体のエンコーダーを利用できません。

SD起動とUSBホストの認識は確認できました。次は有線LAN通信と未解決の初期化警告を確認し、torneまたはDT295で復号後TSを取得します。SoCのH.264エンコーダーはG12Aの識別・電源・クロック・firmwareを対応させ、720p30の合成入力で単体試験を行います。受信とエンコードがそれぞれ成立してから、MPEG-2デコード、デインターレース、音声、MP4、NASを接続する順序です。DT295内蔵H.264の成功はSoCエンコードの成功とは別に記録します。

## PX-S1UD と B-CAS による実放送の受信

USBホストの修正後、PLEX PX-S1UD（`3275:0080`）と USB カードリーダー（`2ce3:9563`）が使える。起動時に `S96bcas` が pcscd → bcasd → cardrelay を立ち上げ、`torne-b25` が使う `/dev/shm/torne/card.sock` を用意する。

```sh
siano-ts --list          # bus:address を確認する（再起動で変わる）
siano-ts --device 1:3 --firmware /lib/firmware/isdbt_rio.inp -c 20 -t 30 \
  | torne-b25 --card-socket /dev/shm/torne/card.sock > /tmp/clear.ts
```

復号後の TS は `scrambled=0`、`tei=0`、`cc_errors=0` になる。B-CAS の応答は `cardprobe /dev/shm/torne/card.sock` で確認でき、正常なら `card_status=0x2100` と B-CAS ID が返る。

### USB ホストが動かなかった原因

`meson-g12a-x96-max.dts` は `vcc_5v`（GPIOH_8 制御）を定義しているが、USB コントローラへ配線していない。そのため `dwc3` が `regulator_enable()` を呼ばず、VBUS が供給されず、xhci のポートは `PP=0`、`CCS=0` のままだった。同じ G12A で動作実績のある `meson-g12a-u200.dts` に合わせて、次の 3 つを追加している。

```dts
&usb       { vbus-supply = <&vcc_5v>; };
&usb2_phy0 { phy-supply = <&vcc_5v>; };
&usb2_phy1 { phy-supply = <&vcc_5v>; };
```

`dwc2` は GHWCFG2 が `0x8`（デバイス専用）なので、このボードの USB2.0 ポートはホストにならない。USB3.0 ポート側（xhci）を使う。

### NAS へ配信する

`S97pix-stream` が `siano-ts` から `stb_streamer` までの 5 段を起動する。NAS 側は
既存の dtv_android と同じ経路で pull する。

```sh
/etc/init.d/S97pix-stream start        # 端末側
curl "http://192.168.99.109:40774/v1/status"
curl "http://192.168.99.109:40774/v1/streams/gr-20?after=0" -o rec.mp4   # NAS 側
```

設定は `/etc/default/pix-stream-linux`（チャンネル、ポート、許可 IP、ビットレート）。

段を分けている理由は 2 つある。`h264_v4l2m2m` が extradata を出さないため、
1 本の ffmpeg で fMP4 をパイプへ書くと avcC が空になり再生できない。2 段目に
MPEG-TS を渡して SPS/PPS を解析させる。中間を TS にしているのは、FIFO を
2 本使うと ffmpeg 同士が出力のオープンで待ち合うため。

起動直後に取りこぼさないよう、`siano-ts` の TS キューは 1024 スロット（約 8 秒）
にしてあり、選局は 4 秒遅らせている。読者がいないあいだスプールは 192 MB で
頭打ちになり、古い断片から捨てる。

### NAS の録画管理に登録する

`stb_streamer` は `_pixstb._tcp.local` を広告するので、NAS の録画管理
（`http://192.168.99.80:40776/`）の「検出された端末」に現れる。名前は
`/etc/default/pix-stream-linux` の `PIX_DISPLAY`、チャンネルは `PIX_CHANNEL`。

番組として区切るには `pixb` ボックスが要る。`eit_watch` が `torne-b25` と
`ffmpeg` の間で TS を素通ししながら EIT（PID 0x12、table_id 0x4E）を読み、
現在の番組を `/dev/shm/pix/program.txt` に書く。`stb_streamer` は `moof` の
直前でこれを読み、番組が変わっていれば `pixb` を差し込む。NAS はこれで区切り、
アーカイブ EPG から番組名を付けて保存し、CM カットを走らせる。

番組名も放送波から取る。`eit_watch` は short_event_descriptor（0x4D）を
ARIB STD-B24 から UTF-8 に直して `program.txt` に `name=` として書き、
`stb_streamer` は `pixb` の直後に `pixn` を置く。NAS はこの名前でファイルを作る
ので、EPG サービスが止まっていても録画名が局名に落ちない。接続途中から始まる
録画のために `GET /v1/program` も用意してある。詳細は
[docs/eit-program-names.md](docs/eit-program-names.md) を参照。

端末に RTC が無いため起動直後の時計は 1970 年を指す。EIT の開始時刻と現在時刻を
比べる判定を入れると放送中の番組を全部捨ててしまうので、時計が合っているときは
放送時間に現在時刻が入るイベントを選び、合っていないときは前へ進む方向で
いちばん早いものを選ぶ。

## トランスコードの1コマンド経路

G12AのVDECでMPEG-2をデコードし、HCodecでH.264へエンコードしてAACとMP4へまとめます。ライブTSは実時間で届くため `-re` を付けます。

```sh
ffmpeg -re -c:v mpeg2_v4l2m2m -i input.ts -vf "setpts=N/FR/TB,copy,format=nv12" \
       -c:v h264_v4l2m2m -b:v 6000k -g 60 -c:a aac -b:a 192k out.mp4
```

- `copy` は必須です。ffmpegのv4l2m2mエンコーダーは入力フレームを元のlinesizeのままコピーしますが、エンコーダーは32境界のストライドで読むため、デコーダーの64境界フレームをそのまま渡すと1行32バイトずつずれます。
- `setpts=N/FR/TB` は、キャプチャ済みファイルの最終フレームに飛んだPTSが付き、muxerがフレームを水増しして数百MBに膨れるのを防ぎます。
- 1080iは `bwdif=mode=send_frame` を挟むと29.97pになります。この場合は新規フレームになるため `copy` は不要です。

実放送の1440x1080i MPEG-2＋AACから、110フレームの1080p29.97 H.264＋AACのMP4を作れることを確認しています。

## 1代目・2代目のRAM起動

両機とも起動時だけOS全体をinitramfsへ読み込み、動作中はRAM上で稼働します。1代目はSD/mmc、2代目はUSBから読み込みます。再起動時には媒体が必要です。機体別ビルド、起動媒体の更新、揮発する設定、SSHホスト鍵、復旧時の停止条件は [RAM起動の運用手順](<docs/ram-rootfs.md>) を参照してください。

## 検証状況

UARTによるAndroid・U-Boot情報の採取とAndroidへの復帰、Linux・DTB・チューナーのビルドが完了しました。ビルドの終了コードは0です。[イメージ検査結果](work/validation/image-layout.json)ではARM64 Image形式、メモリー範囲、MBR、区画サイズ、rootfsの格納位置、SHA-256を確認しています。DTBのRAMは2 GiB、EthernetはRMIIで、PHYの参照先も `0180.3301` と一致しました。

[rootfsとCLIの検査](work/validation/rootfs-and-cli.log)では、FAT32とext4の読取り専用検査が終了0でした。Linux ARM64のrootfsをchrootで使用し、CLI 3本が起動して使用法を表示することも確認しています。torneの2本は `--help` が正式な引数ではなく、使用法を表示して終了2になる既存仕様です。これはUSB受信の確認ではありません。

SD書込みスクリプトのモック試験は9件成功しました。FAT内の差分がある場合の検証、MBR・rootfs・起動ファイルの不一致やFAT検査の失敗を含みます。実媒体でもMBR・rootfsの全バイト、FATの整合性、起動ファイル2本のSHA-256を確認し、終了0で取り出しました。SD・UART・USB・Ethernetに必要な22項目もカーネル組込みになっています。

実機ではLinux 6.18.62／aarch64でUART rootログインまで確認しました。SDは `/dev/mmcblk0`、ext4 rootfsは `/dev/mmcblk0p2` からrwでマウントされ、eMMCは `/dev/mmcblk1` として認識されています。内部Ethernet PHYはRMIIで登録されましたが、ケーブルを差し直すまではNO-CARRIERでした。差し直した後は100 Mbps/Fullでリンクし、DHCPで `192.168.99.109/24` を取得しています。USB 2.0／3.0 root hubを認識し、チューナーは未接続でした。同日の追加セッションでPWM組込み版のカーネル#2へ更新し、U-Bootは `Image` を `0x01080000`、DTBを `0x08000000` に読み込む手順のまま起動できています。[起動結果](work/linux-boot-20261110/boot-result.json)、[UARTログ](work/linux-boot-20261110/serial.log)、[追加セッションの結果](work/serial-20261110/session-result.json)を参照してください。

メモリー初期ログは1,966,080 KiB、`MemTotal` は1,851,464 kBで、U-Bootの2 GiB表示との差は未解決です。CPU電圧regulatorのPWM取得失敗は、実機で `modprobe pwm_meson` により解消することを確認しました（`pwmchip0/1` 登録、`cpufreq-dt` が1.0〜1.8 GHz、`VDDCPU` 730 mV、`cpu-thermal` 41.8 °C）。Buildrootはudevデーモンを持たずplatform modaliasで自動読込みされないため、`board/x96max/linux.fragment` に `CONFIG_PWM_MESON=y` を追加して組込みに変えました。SDの `Image` を差し替えて再起動し、モジュールを読まずに `pwmchip0/1` と `cpufreq-dt`（`scaling_cur_freq` が負荷に応じて1.2→1.6 GHzへ変化）が現れ、`cpufreq-dt`／`regulator-vddcpu` の遅延プローブ警告が消えることを実機で確認済みです（カーネル#2）。wifi32k・SDIO電源、HDMI/VPUの同期待ちは残っています。vendor U-Bootの予約メモリー処理はmainline DTBに対して `FDT_ERR_NOTFOUND` を表示しますが、DTB移動後はLinuxへ制御が渡りました。有線LANは、PHY ID `0180:3301`・AN有効・50 MHz clock有効・PHY IRQ登録済みという正常な状態でもリンクせず、BMSRのlink statusが0、ANLPARにリンク相手の能力が無く、RJ45のリンクLEDも消灯していました。ケーブルを差し直すとLEDが点灯し、100 Mbps/FullでリンクしてDHCPから `192.168.99.109/24`、default via `192.168.99.254`、DNS `192.168.99.250` を取得しています。ゲートウェイ、ホスト、本体方向のいずれもpingで疎通しています。ホストの公開鍵を `/root/.ssh/authorized_keys` へ配備し、鍵認証のSSHと `scp` が使えます（この配備はrootfsを作り直すと消えるため、恒久化はoverlayへの配置が必要です）。チューナー受信、SoCエンコード、NAS保存は未検証です。

```sh
python3 -m unittest discover -s tests -p 'test_write_sd.py' -v
```

今回使ったColima環境は検証後に停止しました。`./scripts/build-linux.sh` で再開でき、同じビルドvolumeを利用します。
