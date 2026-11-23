# 1代目・2代目・3代目のRAM起動・更新手順

両機とも、起動時だけ媒体からLinuxとinitramfsを読み、以後はRAMだけで動かす。initramfsは、OS全体をメモリ上に展開するアーカイブである。稼働中は起動媒体をマウントせず、swapも使わない。録画先は従来どおりNASとする。**再起動時には起動媒体が必要である。**

| 対象 | IP | 起動媒体 | メモリ・PHY | ビルド時の選択 | DTB |
|---|---|---|---|---|---|
| 1代目 | 192.168.99.109 | SD、U-Boot mmc0:1 | 2GiB・内部PHY | `X96MAX_UNIT=1` | `meson-g12a-x96-max-p.dtb` |
| 2代目 | 192.168.99.123 | USB、U-Boot usb0:1 | 4GiB・ePHY | `X96MAX_UNIT=2` | `meson-g12a-x96-max-p-ephy-4g.dtb` |
| 3代目 | 192.168.99.56 | SD、U-Boot mmc0:1 | 4GiB・外部PHY（SM1） | `X96MAX_UNIT=3` | `meson-g12a-x96-max-p-sm1.dtb` |

3代目は SM1（aml_dt `sm1_ac213_4g`）で、U-Boot が `meson-g12a-x96-max-p-sm1.dtb` を
ロードする（[U-Boot定義](<../output/ramboot-unit3/ramboot-unit3-uboot.txt>)）。G12A 系の
DTB では USB/vdec が壊れるため取り違えない。

1代目はPX-S1UD・テレビ静岡ch17・CM特徴量を維持する。2代目はDT295・BS11で、内蔵カードを使う。
3代目はDT295・CFW・SBS ch15である。

## 媒体の照合

デバイス名は再列挙で変わるため、名前だけで判断しない。起動用 FAT 区画は
`blkid` のラベル `X96BOOT`（1代目）または `X96RAMBOOT`（2・3代目）で探す。

- 1代目SD：CID `02544d53453132388907e3202f01a300`、241745920 sectors。
- 2代目USB：62533286 sectors。
- 3代目SD：CID `035344534c33324780b468426a011a00`（SanDisk SL32G 31.9GB）。

identity は機体ごとに分ける（[build-on-nas.md](build-on-nas.md)）。
`work/ramboot-identity-unit1`、`work/ramboot-identity`（2代目）、
`work/ramboot-identity-unit3`。3代目は 11/19 の構築時に 2代目の identity が
流用されたため SSH ホスト鍵が混線した。2026-11-22 に固有 identity で焼き直した
（docs/verification-20261122.md §8）。identity を替えて焼き直すと SSH ホスト鍵が
変わる。その場合、Mac と NAS の known_hosts の更新が一度だけ必要になる。

## 更新手順（tools/flash-ramboot-over-ssh.sh）

docs/ram-rootfs.md の手順を NAS 側で実行するスクリプト
[tools/flash-ramboot-over-ssh.sh](<../tools/flash-ramboot-over-ssh.sh>) がある。
スクリプトが行うのは次の範囲までである。

- RAM rootfs の確認と媒体照合
- 転送と SHA256 検証、FAT マウント、旧ファイル退避、書き込み
- FAT 上での SHA256 照合、sync・アンマウント

**再起動は行わない**。ハッシュが一致するファイルは書き換えない
（[verification-20261122.md](verification-20261122.md) §1 と同じ方針）。
スクリプトを使わず手で行う場合は以下のとおり。

1. ホストから、対象機の3本をRAM上の一時領域へ転送する。
2. 照合した起動用FAT区画だけをマウントする。元の起動ファイルを退避してから3本を更新する。容量を確認し、元の退避を新しいファイルで上書きしない。媒体全体を初期化せず、既存ext4も消さない。
3. RAM側とFAT側の3本のSHA256がすべて一致することを確かめる。`sync` 後にアンマウントする。不一致やI/Oエラーがあれば再起動せず、RAMで動く状態を保って原因を調べる。
4. 対象機だけを再起動し、次節の確認を行う。**再起動は必ずユーザー確認を取る。**

## 再ビルドする

保守担当者はリポジトリのルートで実行する。Dockerの `colima-x96max-linux` contextと、既存の公開鍵入力が必要である。ホスト検査にはPython 3.9以上とCコンパイラも使う。1代目は次を使う。

```sh
X96MAX_PROFILE=ramboot X96MAX_UNIT=1 ./scripts/build-linux.sh
RAMBOOT_UNIT=1 RAMBOOT_CPIO=output/ramboot-unit1/rootfs.cpio.gz \
  /usr/bin/python3 -m unittest discover -s tests -p 'test_*.py' -v
```

2代目は次を使う。`X96MAX_UNIT` を省略したRAMビルドも、従来どおり2代目を選ぶ。取り違えを避けるため、保守時は機体を明示する。

```sh
X96MAX_PROFILE=ramboot X96MAX_UNIT=2 ./scripts/build-linux.sh
RAMBOOT_UNIT=2 RAMBOOT_CPIO=output/ramboot/rootfs.cpio.gz \
  /usr/bin/python3 -m unittest discover -s tests -p 'test_*.py' -v
```

成果物は [1代目用ディレクトリ](<../output/ramboot-unit1/>) と [2代目用ディレクトリ](<../output/ramboot/>) に分ける。起動に必要なのは、カーネル・該当機体のDTB・uImage形式のinitramfsの3本である。

```text
Image
meson-g12a-x96-max-p.dtb           # 1代目
meson-g12a-x96-max-p-ephy-4g.dtb   # 2代目。DTBはどちらか1本
rootfs.cpio.uboot
```

古いU-Bootは生のgzipを拒否するため、標準のuImageヘッダーで包んだ形式を使う。検査用のgzip CPIOを、起動ファイルの代わりにコピーしない。ディスク用ビルドは `X96MAX_PROFILE=disk` で残すが、通常のRAM運用には使用しない。設定の選択元は [ビルド入口](<../scripts/build-linux.sh>)、[共通fragment](<../board/x96max/ramboot.fragment>)、[1代目fragment](<../board/x96max/ramboot-unit1.fragment>)、[2代目fragment](<../board/x96max/ramboot-unit2.fragment>) である。

SSHホスト秘密鍵は [1代目identity](<../work/ramboot-identity-unit1/>) と [2代目identity](<../work/ramboot-identity/>) に分離する。既存機を移行するときは、その機体の鍵を先に取り込み、再ビルドでも再利用する。既存のB-CAS接続設定、ネットワーク設定、SSH公開鍵、初期ロゴキャッシュも機体別に取り込める。処理は [finalizeスクリプト](<../board/x96max/ramboot-finalize.sh>) の明示した入力だけに限り、CPIO専用の作業領域へコピーする。共通targetへ秘密情報を混ぜない。

identityを安全にバックアップする。Gitへ追加したり、ほかの機体へ流用したりしない。**起動アーカイブとディスクイメージにも秘密鍵が含まれる。公開配布しない。** TFTPによる復旧転送は信頼できるLANだけで行い、転送後に臨時サーバーを停止する。

## 起動媒体を更新する

SSHで対象機に接続し、rootがRAM上の `rootfs` であることを確認する。起動用区画は未マウントでなければならない。接続先、媒体の個体番号、DTBのいずれかが不一致なら止める。rootがext4の場合も、この通常更新手順は使わない。初回移行としてUART・退避・一時起動を別途用意する。

媒体の照合は前節の表を使う。デバイス名は再列挙で変わるため、名前だけで判断しない。

1. ホストから、対象機の3本をRAM上の一時領域へ転送する。
2. 照合した起動用FAT区画だけをマウントする。元の起動ファイルを退避してから3本を更新する。容量を確認し、元の退避を新しいファイルで上書きしない。媒体全体を初期化せず、既存ext4も消さない。
3. RAM側とFAT側の3本のSHA256がすべて一致することを確かめる。`sync` 後にアンマウントする。不一致やI/Oエラーがあれば再起動せず、RAMで動く状態を保って原因を調べる。
4. 対象機だけを再起動し、次節の確認を行う。**再起動は必ずユーザー確認を取る。**

配信設定は [1代目defaults](<../board/x96max/ramboot-unit1-overlay/etc/default/pix-stream-linux>) または [2代目defaults](<../board/x96max/ramboot-overlay/etc/default/pix-stream-linux>) へ反映して再ビルドする。RAM上で変更した設定、ログ、追加パッケージ、ロゴ更新は再起動で消える。PX-S1UDは既存B-CAS経路を使い、DT295の内蔵カード設定では外部B-CASサービスを待たない。

## 起動後を確認する

SSHで `mount`、`cat /proc/cmdline`、`cat /proc/swaps` を実行する。合格条件は、rootが `rootfs` であり、SD・USBのroot指定、ブロックデバイスのマウント、swapがないことである。NASから対象機を確認する。

```sh
# 1代目。2代目はIPを192.168.99.123へ変更する。
curl -fsS http://192.168.99.109:40774/v1/status
curl -fsS http://192.168.99.109:40774/v1/program
```

`capturing:true` だけで判断せず、時間を置いたときに `totalPushed` が増え、NASの録画が再開することを確かめる。映像・音声、EITの番組情報、1代目のCM特徴量、局ロゴも確認する。起動後に媒体を外す試験は、先に全区画の未マウントとswapなしを確認する。媒体を外した状態でもSSH・配信が継続すれば、稼働中の媒体非依存を確認できる。

1代目へ4GiB DTBを適用しない。2代目でもLinuxのメモリ量はファームウェアの予約を除いた値になる。4GiB全部を強制公開しない。使用範囲の根拠は [2代目専用DTS](<../board/x96max/dts/amlogic/meson-g12a-x96-max-p-ephy-4g.dts>) にある。

## 起動・チューナーを復旧する

起動ファイルを読めなければU-Bootで停止する。古いRAM内容を使った起動は行わない。[1代目U-Boot定義](<../board/x96max/ramboot-unit1-uboot.txt>) または [2代目U-Boot定義](<../board/x96max/ramboot-uboot.txt>) を一時実行する。ログイン・同一鍵SSH・配信・媒体非依存を確かめるまで `saveenv` しない。1代目のUARTは `ku@192.168.99.10` のUSBシリアルで利用できる。読み書き権限を確認し、ほかのシリアル利用者と同時接続しない。

区画を読めない場合は、UARTから [一時起動ツール](<../tools/uboot_boot_linux.py>) を使う。`--ramboot --unit 1` または `--ramboot --unit 2` に、`--tftp-server` と転送元IPv4を指定する。ツールはファームウェアの `aml_dt` が選択機体と一致することを確認してからロードする。媒体の初期化やMBR書込みを繰り返さない。

元のディスク起動へ戻す場合は、UARTのU-Bootで退避した定義を確認する。

```text
# 1代目はlinux_boot、2代目はlinux_boot_usbが旧定義。
printenv bootcmd_before_ramboot
setenv bootcmd ${bootcmd_before_ramboot}
saveenv
```

元のext4の健全性は別途確認する。1代目でカーネル・DTBも戻す場合は、RAMで動く状態からFAT上の退避を復元し、SHA256を照合してから切り替える。戻せない場合も、TFTPからRAM起動して読取り専用で調査できる。

DT295の復旧が必要なら、全USB区画をアンマウントしてから `pix-dt295-reset` を実行する。配信は数十秒止まる。共通の [リセット処理](<../board/x96max/overlay/usr/bin/pix-usb-reset-common>) は、USB上のroot・データマウント・swap・不明なブロック構成を拒否する。制限は1起動につき最大3回、間隔300秒である。拒否や再列挙失敗時に、sysfsへ直接書いたり制限ファイルを消して連打したりしない。ログとマウント状態を調べる。

カーネルは、コントローラーの切断時に `_regulator_put` の警告を出すことがある。RAM起動は、この警告自体を修正するものではない。警告が連続する場合は復旧を止め、ログを保存する。
