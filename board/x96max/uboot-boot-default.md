# X96Max_P の起動先をLinuxへ固定した記録

作成日: 2026-11-14

- 対象: X96Max_P（192.168.99.109、Amlogic G12A / S905X2、2 GiB）
- シリアル: `/dev/cu.usbserial-0001`（115200 8N1）

---

## 1. 結論

U-Bootの`bootcmd`をLinux起動へ書き換えて`saveenv`した。電源投入から何も押さずにLinuxが立ち上がる。

Androidへ戻す手段は残してある。`bootcmd_android`に元の起動コマンドを保存し、`bootcmd`をそれに向け直せば戻る。

## 2. 変更前の状態

`bootcmd`はAndroid（eMMC）を起動する設定だった。

```text
bootcmd=ddr_auto_fast_boot_check 6 0 0 50;run storeboot
```

Linuxは、U-Bootのプロンプトで`board/x96max/uboot-manual.txt`の読み込みコマンドを送ったときだけ起動していた。そのため電源を入れるだけではAndroidが立ち上がっていた。

## 3. 変更後の環境変数

| 変数 | 値 | 役割 |
|---|---|---|
| `bootcmd_android` | `ddr_auto_fast_boot_check 6 0 0 50;run storeboot` | 元のAndroid起動（そのまま保存） |
| `linux_boot` | SDカードからImageとDTBを読む一連のコマンド | Linux起動 |
| `bootcmd` | `run linux_boot` | 電源投入時に実行される |

`linux_boot`の中身は次のとおり。

```text
setenv sddev 0; setenv kernel_addr_r 0x01080000; setenv fdt_addr_r 0x08000000;
setenv dtb_mem_addr 0x08000000; setenv fdtaddr 0x08000000; mmc dev ${sddev};
mmc rescan; fatload mmc ${sddev}:1 ${kernel_addr_r} Image;
fatload mmc ${sddev}:1 ${fdt_addr_r} meson-g12a-x96-max-p.dtb;
setenv bootargs console=ttyAML0,115200n8 earlycon root=PARTUUID=96320001-02
rootfstype=ext4 rootwait rw; booti ${kernel_addr_r} - ${fdt_addr_r}
```

## 4. 手順（実行したこと）

シリアルでU-Bootのプロンプトに入り、次の順に送った。`;`を含む値は引用符で囲む。囲まないと、その場で後半が実行される。

```text
setenv bootcmd_android 'ddr_auto_fast_boot_check 6 0 0 50;run storeboot'
setenv linux_boot '...（上記の一連のコマンド）...'
setenv bootcmd 'run linux_boot'
printenv bootcmd
saveenv
```

`saveenv`は`Writing to MMC(1)... done`で完了した。

## 5. 検証

`reset`（電源投入と同じ）を実行し、何も押さずに次の順で進むことを確認した。

| 段階 | 確認 |
|---|---|
| U-Boot起動 | `U-Boot 2015.01` |
| SDカード読み込み | `reading Image` |
| カーネル起動 | `Starting kernel ...` → `Booting Linux` |
| ログイン到達 | `x96max-linux login:` |

ネットワークと録画も復帰を確認している。

| 項目 | 結果 |
|---|---|
| アドレス | `192.168.99.109`（固定設定） |
| 時計 | 起動時にNASの`/api/time`から設定 |
| パイプライン | channel 17、`capturing: true` |

## 6. Androidへ戻す手順（実機で確認済み）

シリアルでU-Bootのプロンプトに入り、次を送る。

```text
setenv bootcmd 'run bootcmd_android'
saveenv
reset
```

`tools/uboot_set_boot.py --target android --reboot-from-linux`でも同じことができる。Linuxへ戻すときは`--target linux`。

2026-11-14に実機で往復を確認した。

| 手順 | 結果 |
|---|---|
| `--target android` → `reset` | `## Booting Android Image` が出てAndroidが起動 |
| Androidのシェルから`reboot` | U-Bootのプロンプトを捕捉 |
| `--target linux` → `reset` | `reading Image` → `Starting kernel` でLinuxが起動 |
| 起動後 | `192.168.99.109`、時計正常、パイプライン稼働 |

Android側のシェルは`console:/ #`で、`id`が`uid=0(root)`を返す。`reboot`はそこから実行できた。

U-Bootのプロンプトに入るには、電源投入前に`tools/uboot_boot_linux.py`を走らせてEnterを送り続ける。ただし`bootcmd`がLinuxを指している状態では、自動起動が始まる前に割り込む必要がある。

## 7. U-Bootコンソールの扱い

実際に触って分かった注意点。

- `boot`というコマンドは無い。保存済みの設定で起動するときは`run bootcmd`を送る。
- 改行を送り続けると、環境変数の表示が止まらなくなる。その場合は`Ctrl+C`（0x03）でプロンプトへ戻す。
- `;`を含む値を`setenv`するときは必ず引用符で囲む。囲まないと、その場で後半が実行される。

## 8. 復旧に使える材料

| 材料 | 場所 |
|---|---|
| 変更前の環境変数（86項目） | `work/archive/docs/artifacts/uboot-env-x96max-p-20261114.txt`（ローカルアーカイブ） |
| Linux起動の手動手順 | `board/x96max/uboot-manual.txt` |
| Android起動を維持する変数 | `bootcmd_android`（上記） |
| 起動先の切り替え | `tools/uboot_set_boot.py` |

環境変数を丸ごと控え直すときは`tools/uboot_set_boot.py --dump<file>`を使う。以前の控えは8,000バイトで切れているため、完全な控えが必要なら取り直す。

## 9. 注意

- `bootcmd`がLinuxを指しているため、**SDカードを抜くと起動しない**。その場合はU-Bootのプロンプトから`setenv bootcmd 'run bootcmd_android'; saveenv`で戻す。
- `saveenv`はeMMCの環境領域へ書く。`board/x96max/uboot-manual.txt`のメモは「RAM上だけの設定なのでsaveenvしない」となっているが、今回の既定起動の変更は意図的に保存している。
