# 検証証拠の索引

## 目的

Aegis の過程記録は `work/` の実機ログを絶対パスでリンクする。`work/` は Git 管理外である。clone しただけでは根拠ファイルを読めない。

この索引は主要な証拠の要約と SHA-256 を Git 管理側に残す。ファイル本体は `work/` にある。この索引に無い証拠は、過程記録のリンク元を参照する。

## ブートペイロード

3箇所の起動ファイルは別々の世代である。SHA-256 がすべて異なる。削除すると過去の再現手段を失うため、3つとも保持する。

```text
work/backup-unit2-boot-20261122/Image  c424cb5688886cfa7303bfffe09dc458aa24c068540471beda21a6071911e8ae
work/backup-unit2-boot-20261122/rootfs.cpio.uboot  c5fcd763aab3aabc0c247bc0a0461d5ff66a664ef0256fe5e7d87dfe802a2513
work/usbstick/Image  6a9b20e4eee5b07d7ae08aa40eab185a424037cc0460cd4e9d1dbfad9f37cc28
work/usbstick/rootfs.cpio.uboot  ef4964aac16657237e8d623dcc81bcae0c6bc4f4a894342549be84cc46c1f589
work/tftpboot/Image  aa44ffff303a76f4030c8556001d5fdc408b9403fc183aa029cae87d20421ac1
work/tftpboot/rootfs.cpio.uboot  8a83e10c68306dbfcf73ee0596272893f8fa90e6e0b178237d563da63619033e
```

`work/tftpboot/` は 3代目の TFTP 配信用である。`work/usbstick/` は USB 媒体用である。`work/backup-unit2-boot-20261122/` は 2代目の退避である。

詳細は 50-resume-guide.md と verification-20261122.md を参照する。両者は `docs/aegis/work/2026-11-19-unit3-init/` と `docs/` にある。

## 再起動と起動の証拠

```text
work/archive/restart/restart-baseline-console.log  b16d8bcc9803753bd5b4b5caa70a58aafce883aebeb2ed0d0f704d85579932c7
work/archive/restart/restart-boot-on-console.log  389215be4c878913944594717b2ee0f11bff6f1c95d9312bfb63ff6daf489761
work/archive/restart/restart-boot-on-resume.log  71a2ceb09911edb76dd5ef202288552cd4a43535b1d9e22913de96842583e707
work/archive/restart/restart-boot-on-sector0.bin  b433d6dd2e4bdcefbbc6ea9aad766bfef686540a63684c594245237b2753d55a
work/archive/restart/restart-recipe-persist-console.log  db09bb3bb8b5cc6a36b53d659fe370e7d93265d8734ef7b56ddc9a91ce9d1f75
work/archive/restart/restart-native-first-live-steady.log  208ab6b5cbd3a5be92c6cab9652b6d1e2bb339b1f5573bf5d12c8bf7c488cb3d
work/archive/restart/restart-live-lifecycle.log  14f1467f20974fb5ffa4fa11c5000e1bb4836282e907b6c22a6adfd617e2c6ca
work/archive/restart/restart-cold-autoboot-passive.log  4c395d29bce86a1297be8726ac66f7d786cffa549a991e8180e9f846f0a3827a
work/archive/restart/restart-release2-SHA256SUMS  1212ef039401e8663fbc7b1ddd58f578072d01983133859cd01a3c61ea277440
work/archive/restart/restart-nas-recording-after-reboot.json  de56acfc24ab1be85e9b1daf0bdfa3ed277f91574c3171858e207cd8b21fd0be
```

`work/archive/restart/restart-cold-autoboot-console.log` は 0 バイトである。過程記録から参照されているため保持した。内容は無く、参照先の存在確認に使う。

## 映像と TS の実測

```text
work/nas-check/live-pull.mp4  0c60bb88c1951f250793d9a3a4f9f29ce358b01841e238a26e4f7ccc0b37e135
work/ts-samples/clear3.ts  b22f1a85ca9aa2c8e455be591517734923c512b8504cfb83a4bef1746f53a11a
work/encoder-check/hw-pipeline-1440x1080.mp4  21c0111121e979627793cec74c553cdf74e916a7b8d27f46746d4b71f2037d36
work/ramboot/usb-before-ramboot.img.gz  ef38a9b41d570ba8c8a436816c316e1950d22586987d5e854a9c8557d0530dc1
```

`work/ts-samples/synth-1440x1080i-10s.ts` は合成 TS である。ハッシュは未採取である。

## 3代目の起動ログ

```text
work/archive/unit3/unit3-boot-01.log  234eb1ee6b0613d2942c08092c7ec82abbbbbe5de955082c078147160cd38318
```

`work/archive/unit3/unit3-boot-gxl2.log` と `work/archive/unit3/unit3-uboot5.log` は解析用の生ログである。ハッシュは未採取である。3代目の結論は `docs/unit3-status.md` に要約した。

## 整理で削除したファイル

2026-11-23 に `work/` のトップレベルから以下を削除した。

- 0 バイトのファイル 19 件。過程記録から参照されていた `restart-cold-autoboot-console.log` は保持した。
- 内容が重複していたファイル 5 件。元ファイルと SHA-256 が一致した。参照が無いことを確認してから削除した。

```text
restart-expected-mbr.bin
nas-release-cm_service.py
unit3-serial-now.log
unit3-state6.log
unit3-ubootcmds.log
```

過程記録には `work/` のマシン固有絶対パスが残る。新しい記録では相対パスと、必要ならこの索引へのリンクを使う。
