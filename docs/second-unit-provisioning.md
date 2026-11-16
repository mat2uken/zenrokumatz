# 2台目の X96Max_P を立ち上げる手順

作成日: 2026-11-14

- 対象: X96Max_P（Amlogic G12A / S905X2、2 GiB）2台目
- チューナー: PX-S1UD、torne、PIX-DT295 のうち**1つだけ**接続する
- カードリーダー: ローカル（この端末のUSBリーダー）／リモート（別ホストのbcasd）の両対応

1台目（192.168.99.109）と同じ手順で作れるが、**MACアドレスとIPアドレスは端末ごとに違う**。先にMACを控えてDHCPサーバへ予約を入れる。

---

## 1. ビルド

作業機（macOS）で実行する。Docker/Colima が必要。

```sh
cd ~/Desktop/work/X96MaxLinuxTSEnc
./scripts/build-linux.sh
```

初回はコンテナイメージの作成を含むため時間がかかる。2回目以降は`stb-streamer-rebuild`などの個別ターゲットも使える。

2026-11-15に作成したイメージは次のとおり。3種類のチューナーと、チューナー選択・時計合わせ・DHCPの設定が入っている。

| 項目 | 値 |
|---|---|
| `sdcard.img` | 672,137,216 バイト |
| `Image` | 41,757,184 バイト |

`output/SHA256SUMS`に各ファイルのハッシュがある。書き込み後は次の値と照合する。

```text
sdcard.img  af91a293c9eb65ae5ab46724fe48a55d35857a994c0203acf7d9a3d3e1b7c229
```

出力は`output/`に揃う。

| ファイル | 用途 |
|---|---|
| `sdcard.img` | SDカードへ書く完成イメージ |
| `Image` / `meson-g12a-x96-max-p.dtb` | 起動ファイル（確認用） |
| `rootfs.ext4` | ルートファイルシステム |
| `SHA256SUMS` | 上記のハッシュ |

ビルド後は必ずハッシュを控える。

```sh
shasum -a 256 output/sdcard.img
```

## 2. SDカードへ書く

`write-sd.py`は**外付けの物理ディスクだけ**を受け付ける（内蔵ディスクを誤って消さないための歯止め）。

```sh
diskutil list            # 書くSDカードの /dev/diskN を確認する
./scripts/write-sd.py output/sdcard.img /dev/diskN
```

書いたら取り出す。

```sh
diskutil eject /dev/diskN
```

### 基板リビジョンによる違い（2026-11-15に判明）

2台目は1台目とEthernetのPHYが違った。同じDTBではリンクしない。

| 基板 | U-Bootのプロンプト | DRAM | PHY | 使うDTB |
|---|---|---|---|---|
| 1台目（u212） | `g12a_u212_v1#` | 2GiB | SoC内蔵（RMII） | `meson-g12a-x96-max-p.dtb` |
| 2台目（u211） | `g12a_u211_v1#` | 3.8GiB | **RTL8211F（RGMII）** | `meson-g12a-x96-max-p-ephy.dtb` |

内蔵PHY前提のDTBでは`NO-CARRIER`のままになる。`dmesg | grep 'eth0: PHY'`で実際のPHY名を確認し、`RTL8211F`なら`-ephy`版を使う。

この筐体のSDスロットは読み取りに失敗した（`mmc read`が`cmd16`タイムアウト）。USBメモリから起動している。

## 3. 初回起動（U-BootでLinuxを指す）

新品の状態では**Androidが起動する**。LinuxはU-Bootのプロンプトで読み込みコマンドを送ったときだけ起動する。

先に待ち受けを走らせてから電源を入れる。

```sh
python3 tools/uboot_boot_linux.py --dev /dev/cu.usbserial-XXXX --wait 900
```

`--dev`は作業機のUSBシリアル。`ls /dev/cu.usbserial*`で確認する。

これでLinuxが起動したら、**既定の起動先をLinuxへ保存する**。次回からは電源を入れるだけでLinuxが立ち上がる。

```sh
python3 tools/uboot_set_boot.py --target linux --reboot-from-linux --host root@<この端末のIP>
```

`uboot_set_boot.py`は`linux_boot`と`bootcmd_android`を定義したうえで`bootcmd`を切り替えるので、新品でもそのまま使える。Androidへ戻すときは`--target android`。

## 4. ネットワーク

### MACアドレスを控える

```sh
ssh root@<IP> 'ip -o link show eth0 | head -1'
```

`link/ether`の後ろがMACアドレス。この値はハードウェア（efuse）由来で、再起動しても変わらない。

### DHCPサーバへ予約を入れる

MACアドレスに対して、この端末用のIPアドレスを予約する。1台目は`90:0e:b3:0e:17:1b` → `192.168.99.109`で運用している。2台目は別のアドレスを割り当てる。

端末側は設定変更なしでDHCPから取得する（`/etc/network/interfaces`は`iface eth0 inet dhcp`）。

## 5. チューナーとカードリーダーの設定

`provision-second-unit.sh`でまとめて設定する。実機（root）へ置いて実行する。

```sh
scp scripts/provision-second-unit.sh root@<IP>:/tmp/
ssh root@<IP> 'chmod +x /tmp/provision-second-unit.sh'
ssh root@<IP> '/tmp/provision-second-unit.sh --name X96Max-P2 --channel 17 --tuner pxs1ud --bcas local'
```

### 選べる組み合わせ

| チューナー | `--tuner` | 受信 | 復号 | `--bcas` |
|---|---|---|---|---|
| PX-S1UD | `pxs1ud` | `siano-ts` | `torne-b25`（cardrelay経由） | local / remote |
| torne | `torne` | `torne-c` | `torne-b25`（cardrelay経由） | local / remote |
| PIX-DT295 | `dt295` | `pix-ts-legacy` | **チューナー内で復号** | 不要 |

### カードリーダーがローカルの場合

この端末のUSBカードリーダーを使う。

```sh
/tmp/provision-second-unit.sh --name X96Max-P2 --channel 17 --tuner pxs1ud --bcas local
```

`pcscd`と`bcasd`が起動し、`cardrelay`が`/dev/shm/torne/card.sock`を用意する。

### カードリーダーがリモートの場合

別ホストの`bcasd`へ接続する。トークンは接続先の`/etc/bcas/tokens`に登録されているものと同じ64桁hexを渡す。

```sh
/tmp/provision-second-unit.sh --name X96Max-P2 --channel 17 --tuner pxs1ud \
    --bcas remote --host 192.168.99.10 --token <64桁hex>
```

### BS11の現状（2026-11-16 08:40時点）

| 項目 | 状態 |
|---|---|
| 録画 | 稼働中。h264 1920x1080、約6.5Mbps、欠落0 |
| 保存 | 本日12番組をNASへ保存。番組境界で自動分割 |
| CM解析 | 稼働中。ショッピング番組は`no_breaks`で原本保存 |
| CMカット | 仕組みは実証済み。BS11録画ファイルで1299秒→1241秒 |
| 自動復帰 | 電源投入だけでLinux起動→録画開始を確認 |
| 温度 | 55℃、メモリ2.7GB空き |

日中はショッピング番組のためCMがなく、`no_breaks`が正しい判定になる。夜間のアニメ放送で`breaks_found`になればカット済みファイルが自動生成される。

### 衛星（BS/CS）を受ける場合

DT295で衛星を受けるときは`PIX_SYSTEM=bs`にする。トランスポンダを特定するため`PIX_SAT_SLOT`か`PIX_SAT_TSID`が要る。

DT295の出力はスクランブルされたままなので、地上波と同じく`torne-b25`を通す（`PIX_DT295_DESCRAMBLED=0`が既定）。

トランスポンダ番号はBSのチャンネル番号と一致しない。実測ではBS11が`-c 9 --slot 0`だった。`ffprobe`でサービス名を確認して特定する。

```sh
pix-ts-legacy --model dt295 --assets /usr/share/pix-tuner -s bs -c 9 --slot 0 -t 4 -o /tmp/bs.ts
ffprobe -v error -show_entries program_tags=service_name -of csv=p=0 /tmp/bs.ts
```

**BSのトランスポンダは複数サービスが同居する。** そのまま`-map 0:v`で取ると別サービスの映像と音声が混ざり、エンコードに失敗する。サービスのPIDを指定するか、プログラム単位でmapする必要がある。

BS11は実測で`-c 9 --slot 0`、プログラム211、映像PID 320、音声PID 321だった。次の設定でPIDを絞る。

```sh
PIX_SYSTEM=bs
PIX_CHANNEL=9
PIX_SAT_SLOT=0
PIX_PIDS="0 257 320 321"
```

### 復号段の起動順（2026-11-15時点の未解決事項）

`torne-b25`は入力がファイルなら安定して復号する（`card_commands=4 failed=0`）。ところがライブのパイプ越し、とくに下流にエンコーダを繋いだ構成では、カード初期化が`failed=1`で失敗し、`packets=0`のまま終了することがある。

切り分けで分かったこと。

| 構成 | 結果 |
|---|---|
| チューナー → 復号 → ファイル | 成功（27MB、`failed=0`） |
| チューナー → 復号 → ffmpeg（コピーのみ） | 失敗（`packets=0`、`failed=1`） |
| 保存済みTS → 復号 | 成功（`card_commands=4 failed=0`） |

復号器の起動を遅らせても、カードリレーを再起動しても変わらない。DT295は強制終了でファームウェア状態が壊れ、USBの`authorized`トグルでは一覧が`ready`になるだけで実際のキャプチャは復帰しない。物理的な抜き差しが必要になる。

2026-11-15夜の時点で分かったことを追記する。同じライブ入力でも`-c copy`なら通り、デコーダを指定すると`Invalid data found when processing input`で開けない。同じデータをファイルに落とすとHWデコードも通る。1台目は同じ`ffmpeg`と同じパイプ構成で動いているため、入力の先頭に混じる破損がprobeを壊していると見ている。

2026-11-16未明の追加知見。`torne-b25`に`--trace FILE`を付けると、同じカード・同じストリームでも復号が安定して通り、下流のエンコーダまで到達する。付けない場合は`card_commands=2 failed=1`で止まることが多い。トレースは1分あたり数百KBなので`/dev/shm`へ書く。

また、DT295を`kill -9`で止めるとファームウェアが状態2になる。この状態では`firmware boot requires cold state 0 or unauthenticated state 16`となり、使えなくなる。USBの`authorized`トグルでも復帰しないため、物理的な抜き差しが要る。

停止は必ず`kill -TERM`を使い、待ってから強制終了する。

状態2からはUSBポートの無効化で復帰できる。物理的な抜き差しは要らない。

**`ERROR stage=card_init`はカードの故障とは限らない。** チューナーが列挙だけでストリームを出していないと、復号器はデータを待って初期化に失敗し、この表示になる。切り分けは次の順で行う。

1. パイプラインを止める
2. `pix-ts-legacy ... -t 6 -o /tmp/probe.ts`で単体キャプチャし、サイズを確認する
3. 取れなければUSBポートを無効化して再列挙する
4. 取れたファイルを`cat probe.ts | torne-b25 --card-socket ... `へ通し、`failed=0`を確認する

```sh
P=/sys/bus/usb/devices/usb1/1-0:1.0/usb1-port1
echo 1 > $P/disable
sleep 3
echo 0 > $P/disable
sleep 8
```

この操作のあと、`pix-ts-legacy`の短いキャプチャが通ることを実機で確認した（9.1MB）。

2026-11-16未明の到達点を残す。

| 項目 | 状態 |
|---|---|
| チューナー受信 | 安定（`-t`付きで数十秒のキャプチャが通る） |
| 復号 | `--trace`と再同期段を入れて `card_commands=48 failed=0` まで確認 |
| コピーモード録画 | **安定**（4分間で再起動0回、560フラグメント、欠落0、約18Mbps） |
| トランスコード | **動作**（`-map 0:v:0`の追加で解決。h264 1920x1080、約7Mbps、4分間で再起動0回） |

重要な設定。

- `PIX_PIDS`には**ECM（289）とPCR（273）を必ず含める**。欠けるとカードセッションが約17秒で切れる
- ffmpegの入力には**`-f mpegts`を明示**する。省略すると`Invalid data found`で開けない
- 復号の前に**ffmpegで再同期**を挟む。チューナーの先頭がパケット境界とずれると`status=-3`（NON_TS_INPUT_STREAM）で拒否される
- 再試行の前に**USBポートを無効化して再列挙**する。状態2からも復帰する
- コピーモードのMPEG-2をfMP4に入れると、CM処理が`trex`欠落で失敗する。CMカットにはh264化が要る
- 1段目のffmpegでは**`-map 0:v:0`を明示する**。`-map 0:a:0`だけだと映像が外れ、2段目が`dimensions not set`で失敗する
- `PIX_START_DELAY`を大きくすると復号の初期化が失敗する。既定の0に戻すこと（6にしたところランチャー経由が全滅し、0で復帰した）
- 復号器の出力をファイルへ書くと成功する（3/3）。パイプでエンコーダへ繋ぐと失敗する場合がある
- `-t`の長さは影響しない（60秒・300秒とも成功）
- 経路に`eit_watch`を入れると復号が失敗する（入れると0/2、外すと3/3）。番組名のために必要だが、録画を優先して外している
- ランチャーは**1つだけ**起動する。手動起動を重ねると複数のパイプラインがチューナーを取り合い、数秒で落ちる連鎖が起きる
- 放送休止中は`PAT contains no programs`が返る。故障ではないので、再試行間隔を延ばして待つ

このため、次のいずれかを検討する。

1. 復号段だけを再試行する（チューナーは維持し、`torne-b25`を起動し直す）
2. チューナーと復号の出力を`/dev/shm/pix/live.ts`へ書き、エンコーダは`tail -f`で読む（起動順の影響を切り離す）

## 6. 起動と確認

```sh
ssh root@<IP> '/etc/init.d/S97pix-stream stop; sleep 3; /etc/init.d/S97pix-stream start'
```

| 確認 | コマンド | 期待 |
|---|---|---|
| 受信できている | `tail -3 /tmp/pix-ts.log` | チャンネルとロックの表示 |
| 復号できている | `tail -3 /tmp/pix-b25.log` | scrambled=0% |
| 配信できている | `wget -qO- http://127.0.0.1:40774/v1/status` | `"capturing":true` |
| 時計が合っている | `date -u` | 現在時刻（起動時にNASから取得） |
| 実時間で出ている | 10秒あたり約300フレーム | `grep -c YAVG /tmp/pix-ff1.log`を10秒間隔で2回 |

DT295では復号もチューナー内で行うため`pix-b25.log`は空になる。受信の記録は`/tmp/pix-ts.log`を見る。

## 7. NASへの登録

NASの管理画面（`http://192.168.99.80:40776/`）でノードを追加する。APIで行う場合:

```sh
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"host":"<IP>","port":40774,"display":"X96Max-P2","channel":"17"}' \
    http://192.168.99.80:40776/api/nodes
```

登録後、ノードが`recording`になっていることを確認する。

```sh
curl -s http://192.168.99.80:40776/api/nodes | python3 -m json.tool | grep -A3 '<IP>'
```

## 8. つまずきやすい点

| 症状 | 原因 | 対処 |
|---|---|---|
| Androidが起動する | `bootcmd`がAndroidを指している | 手順3で`--target linux`を実行する |
| IPアドレスが変わる | DHCPの予約が無い | 手順4でMACを予約する |
| `siano-ts`が見つからない | PX-S1UDが認識されていない | `lsusb`で`3275:0080`を確認。抜き差しする |
| カードソケットが無い | `bcasd`か`cardrelay`が落ちている | `ps`で確認し`/etc/init.d/S96bcas restart` |
| DT295が状態`0x11`のまま | 前回の終了処理が残っている | USB給電を切って再接続する |
| 録画が始まらない | NASのノード登録が無い | 手順7で登録する |

## 9. 2台目を運用に加えるときの注意

- **同じチャンネルを2台で録ると重複する。** NASのノードごとにチャンネルを分けるか、片方を待機にする。
- `PIX_PORT`（既定40774）は端末ごとに独立しているため、IPが違えば競合しない。
- バンドル（`release_id`）はNAS側で共通。端末ごとの差は`/etc/default/pix-stream-linux`だけに閉じ込める。

### 復号器が起動直後に消えるとき（2026-11-16 判明）

`torne-b25`は`--trace`のファイルを**`O_EXCL`で開く**。前回のファイルが残っていると、何も出力せずに即終了する。ログが空のまま復号器だけが消えるときは、まずこれを疑う。

```
rm -f /dev/shm/pix/b25.trace    # 起動前に必ず消す
```

pix-producer は毎回消してから起動する。

### B-CASのカードが割り当てられても切断されるとき

`bcasd`側で`client stb1 ... -> card1`の直後に切断される場合、**カード1（2台目のリーダー）が応答していない**。同じサーバのcard0は正常で、1台目は動き続ける。トークンをcard0側（`/etc/bcas/tokens`の別エントリ）へ替えると復号できる。恒久対策はリーダーの再列挙。

### 現在の構成

```
pix-producer  チューナー → 再同期 → 復号 → /dev/shm/pix/live.ts
pix-reader    tail -f live.ts → h264 1440x1080 → fMP4 → stb_streamer
pix-stream-simple  上記2つを回し続ける（S97pix-stream が起動）
```

復号結果をいったんファイルへ出すのは、直接エンコーダへ繋ぐと復号器の書き込みが止まってカードのやり取りが途切れるため。

### BS11のCMカットが成立した条件（2026-11-16）

マーカーキャッシュのキーがサービスIDと一致していなかった。BS11のキャッシュは`400211_1440x1080.json`だが、うちのストリーマはサービス`211`を名乗るため、マーカーが見つからず`scenario`（サイドバンド無しの弱い判定）に落ちていた。

`/home/ku/pix-cm/markers/`に**同じ内容を`211_1440x1080.json`と`211_1440x1080/`として複製**すると`overlay`が選ばれ、CMを検出してカットまで進む。

実測: `20261116_131635_中国時代劇 折腰` 296.3秒 → 178.2秒（118秒を除去、`_CMカット.mp4`）。

出力は`recordings/.media/<job>.mp4`に置く。番組ディレクトリには
シンボリックリンクが見える。`_CMカット.mp4`のサイズが小さくても異常ではない。

### PIX-DT295は内蔵カードスロットで復号する（2026-11-16 判明）

DT295は**内蔵スロットのB-CASで復号したクリアTSを出力**する。外部リーダーは要らず、`torne-b25`も不要になる。

実測（10秒、124,971パケット）:

```
scrambling=0: 124971        ← すべてクリア
mpeg2video 1920x1080 / aac / arib_caption
```

これまでは`torne-b25`を経路に入れていたが、クリアTSをそのまま通していただけだった。外しても録画は成立する。

参照実装（Khronos31/pix-userland）の文書には、内部カードは`card-status`による
挿入状態だけを確認すると書かれている。外部ATR/reset/APDUやPC/SCは製品機能外で、
ホスト側にAPDU転送APIは無い。復号はデバイス内部で完結している。

設定は`PIX_DECRYPT=0`（既定）。クリア出力でないチューナー（PXS1UDなど）を使う場合だけ1にする。
