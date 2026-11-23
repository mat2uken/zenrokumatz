# 引き継ぎ（round 211 時点）

## 電源投入後にやること（round 211 以降: 何もしない）

**unit3 は U-Boot env に自動起動が焼き込まれた**（`bootcmd=run linux_boot_ram`、
saveenv 済み）。電源投入 → 約 1 分で Linux RAM + pix-stream が立ち上がり、
NAS が自動再接続する。シリアルも SSH も不要。

- env の内容: `board/x96max/ramboot-unit3-uboot.txt`（SM1 用 DTB を選択、
  `usb start` リトライ付き — round 137 の必須知見、panic 自動再起動付き）
- 焼き込みツール: `tools/persist-uboot-env.py <env-file> --boot`
- 失敗時の復旧: SD を抜いて起動すれば素の Android に戻る

### まっさらから手動起動する場合（env が消えたときだけ）

```sh
cd ~/Desktop/work/X96MaxLinuxTSEnc
python3 tools/boot-debug-shell.py --vpu-test
```

配信中モジュール: `amvenc_avc.ko` = **`9939bedb`**（パッチ 0001〜0019）。
`cp` → `modprobe meson_vdec` → `modprobe amvenc_avc encode_print_level=0`
→ 合成映像エンコード、まで自動。**`insmod` では依存が解決せず失敗する**
（`Unknown symbol v4l2_m2m_*`）ので `modprobe` を使うこと。

**CFW 構成（round 214 以降）**: unit3 は 2代目と同じ `PIX_TUNER=dt295-cfw`。
S97 が起動時に frc1_load で CFW を自動インストールし、pcscd → bcasd →
cardrelay → torne-b25 で複合する。PIX_PIDS は設定しない（CFW の生 TS では
ECM を落とせない + pix-cfw-ts にフィルタ引数は無い）。地上波の選局は
PIX_CHANNEL=15 のままでよい。CFW の hook は揮発性で、純正経路を動かすと
戻るが、S97 の再起動で自動的に入れ直る。

これが自動で行うこと:

1. U-Boot に入る（Android なら自動で再起動）
2. `usb start` → `Image` / `meson-g12a-x96-max-p-sm1.dtb` / `rootfs.cpio.uboot` をロード
3. `rdinit=/bin/sh` で起動（init スクリプトを一切走らせない）
4. `eth0` を `192.168.99.56/24` で上げる
5. `0xff63c1e0`（`HHI_VDEC_CLK_CNTL`）を読む ← **vdec_1 ゲート（bit 8）**
6. `http://192.168.99.20:8000/amvenc_avc.ko` から修正版モジュールを取得
7. `insmod /tmp/a.ko encode_print_level=0`
8. `0xff63c1e0` を再度読む ← **bit 8 が立ったか**
9. `dmesg | tail -8`
10. 合成映像（64x64）を `h264_v4l2m2m` でエンコード

**前提**: Mac で HTTP サーバが動いていること。

```sh
cd /tmp && python3 -m http.server 8000 --bind 192.168.99.20 --directory /tmp
# /tmp/amvenc_avc.ko = 9939bedb （490,048 バイト、未 strip）
```

**起動後に SSH を使うには**（round 208 追加。rdinit=/bin/sh なので dropbear が
動いていない）:

```sh
python3 tools/serial-run.py 'mount -t devpts devpts /dev/pts 2>/dev/null; /usr/sbin/dropbear -E -p 22 > /tmp/dropbear.log 2>&1 & sleep 1; pidof dropbear'
# 以後 ssh root@192.168.99.56 で作業できる
```

`tools/serial-run.py` は動作中のシェルに再起動なしでコマンドを送るツール
（round 208 追加）。`boot-debug-shell.py` は必ず U-Boot からやり直すため
/tmp が消える点に注意。

**この HTTP サーバはセッションのバックグラウンドジョブなので、
セッションが終わると止まる。** 再開時は上記で起動し直すこと。
モジュールは NAS の
`/build/output/build/meson-venc-*/venc-h264/amvenc_avc.ko`（未 strip）から
取るのが確実（インストール済みの strip 版は更新されないことがあった）。

## 判定

| 観測 | 意味 | 次にやること |
|---|---|---|
| エンコードが完走し `/tmp/o2.ts` ができる | **VPU 解決** | pix-stream に組み込む（下記 A） |
| `start: begin` の後で止まる | `avc_poweron()` 内 | `poweron:` の最後の行を見る |
| `poweron: gen ctrl done` まで出て止まる | 電源投入は完了 | `canvas done` / `mmc ctrl done` / `loadmc done` のどこで止まるか |
| `start: loadmc done` まで出て止まる | ucode 投入後に VPU が応答しない | HCODEC の IRQ（irq 22）が来ているか、`/proc/interrupts` を見る |

`poweron:` の各段階:

```
clock gate enabled → ao begin → ao pwr_cntl done → ao sleep0 done
→ taking lock for dos → dos reset done → enabling hcodec clock
→ hcodec clock done → mem pd done → iso done → gen ctrl done
```

## A. VPU が動いた後に必要なこと

`pix-stream-simple` の `encode()` は 1 つの ffmpeg で
**HW デコード（`mpeg2_v4l2m2m`）と HW エンコード（`h264_v4l2m2m`）を同時に**
使っている。`meson-sm1-codecs` の README はこの組み合わせがハングすると
明記しており、回避策は**デコーダとエンコーダの間に CPU コピーを挟む**こと
（2 プロセスを rawvideo パイプで繋ぐ、または `format=yuv420p`）。

現状は `bwdif`（CPU フィルタ）が入っているので既にコピーされている
可能性もあるが、**実測で確認する必要がある**。

## 現在の構成（確定済み）

| 項目 | 値 |
|---|---|
| 機体 | **SM1 (S905X3)**, `aml_dt=sm1/ac213/4g`, 4 GiB, RTL8211F |
| DTB | `meson-g12a-x96-max-p-sm1.dtb`（`vdec` = `amlogic,sm1-vdec`） |
| パッチ | 0001 vdec codecs / 0002 sm1-vdec 探索 / 0008 num_clks / **meson-venc 0001,0002** |
| カーネル | 6.18.62、`hantro` 無効 |
| 録画ch | **15（SBS）** ← 17 はテレビ静岡だったので修正済み |
| 起動 | U-Boot で **`usb start` が必須**（無いと USB が動かない） |
| NAS | `192.168.99.56-15` として登録される見込み。旧 `-17` は削除する |

## 到達点（round 208 時点）

| 項目 | 状態 |
|---|---|
| Linux RAM 起動 | **達成** |
| Ethernet 1Gbps / SSH | **達成** |
| DT295 認識（06b8:103b） | **達成** |
| **VPU H.264 エンコード（機能）** | **達成**（64x64 / 1080p 6Mbps で RC=0、h264 出力） |
| **完了割り込み** | **達成**（`HCODEC_CCPU_INTR_MSK=1` で ISR が動く） |
| TS の PSI 取得 | **達成**（`have_pat:1`、`tables_ready:1`） |
| **PID 特定 / PIX_PIDS 設定** | **達成**（round 208/209: `0 17 18 257 273 274`） |
| **実映像の A/V エンコード（本番パイプライン）** | **達成**（round 208: ch17。round 210: **29fps / speed 0.924x**） |
| **ch15(SBS) 受信** | **正常**（round 205 の「受信不良」は誤診＝mux の誤ストリーム選択。round 210 訂正） |
| **pix-stream → NAS 録画** | **達成**（NAS が `192.168.99.56-15` を `recording` 認定、実時間 throughput、dropped 0） |
| **rootfs 再ビルド + SD 配備** | **達成**（round 210: ch15 + PIX_PIDS 焼き込み済み `ccb11a7d`。**次の再起動から有効**） |
| **U-Boot env 永続化（電源オフ自動起動）** | **達成**（round 211/212: bootcmd=linux_boot_ram、SM1 DTB、usb start リトライ。電源投入 45 秒で録画再開） |
| **NAS からの制御** | **合格**（round 211: stop/start/ch 変更 15↔17↔15、STB が実際に再選局） |
| **番組締め処理 + CM カット** | **完走**（round 213: みなスポ 30 分番組 → p01.mp4 1.53GB + CM 解析 + p01_CMカット.mp4 1.42GB、122.1 秒カット、検証済み） |
| **CFW 構成への移行** | **達成**（round 214: 2代目と同じ dt295-cfw + torne-b25。rootfs `bf22ebd5`、SD 配備済み、recording 実時間・dropped 0・音声デコードエラーゼロ） |
| 性能 | **解決**（round 210 参照。11fps は debug ブートのコンソール printk 副作用だった） |

## PID 一覧（round 208 実測。ch15/ch17 とも同一布局）

| PID | 内容 |
|---|---|
| 0 | PAT |
| 257 | PMT（主サービス。ch15=program 35856 S） |
| **273** | **MPEG-2 映像 1440x1080（0x111）** |
| **274** | **AAC 音声（0x112）** |
| 256 | PCR（ffmpeg が再スタンプするので PIX_PIDS に入れない） |
| 276/277 | 字幕/データ（CM 解析には不要） |
| 337 (0x151) | ワンセグ H.264 ← **ffmpeg 既定選択がこれを掴んでいたのが round 207 の原因** |

## 決定的だった修正（すべて実機で実証）

1. **U-Boot で `usb start` が必須**
2. **DTB の vdec を `amlogic,sm1-vdec` に**（機体は SM1 / S905X3）
3. **`hantro` を無効化**（Rockchip 専用で Amlogic に非対応）
4. **`ISO0` の `BIT(1)` をクリア**（パッチ 0010。これで HCodec 窓が応答）
5. **`DOS_SW_RESET0` もリセットパルス**（パッチ 0014。ucode の IMEM DMA が完了）
6. **`HCODEC_CCPU_INTR_MSK = 1`**（パッチ 0018。完了割り込みが来る）
7. **`PIX_CHANNEL` を 17 → 15**（SBS は物理 15。17 はテレビ静岡）

## 未解決（優先順）

**定常運用の検証項目はすべて完了（round 213）。CFW 移行も完了（round 214）。**
残るは運用上の注意のみ:

1. **ソフトリブートは使わない**: エンコード動作中の `reboot` はハングする
   （round 212、2 回再現）。再起動は**電源オフ/オン**が正規手順。ハング時の復旧は物理電源のみ（guard の adb reboot は Linux RAM では不可）。なお
   **S97pix-stream を停止してからならソフトリブートは通った**（round 214）
2. **CFW の揮発性**: 純正経路（pix-ts-legacy）を動かすと hook が戻るが、
   S97 が起動時に frc1_load で自動再インストールする（round 214 実測）
3. 長期運用で観察すべき点: SD カードの耐久（rootfs は RAM なので書き込みは
   ほぼ無い）、NAS 側のディスク容量、torne-b25 のカードセッションの長期安定性

## 配信中のモジュール

`amvenc_avc.ko` = `9939bedb`（パッチ 0001〜0019）。
`http://192.168.99.20:8000/amvenc_avc.ko`（Mac で
`cd /tmp && python3 -m http.server 8000 --bind 192.168.99.20 --directory /tmp`）

## 反復のコツ

ハングしなくなってからは **電源再投入なしで** モジュールを差し替えられる:

```sh
busybox wget -q -O /tmp/b.ko http://192.168.99.20:8000/amvenc_avc.ko
rmmod amvenc_avc ; cp /tmp/b.ko /lib/modules/6.18.62/updates/amvenc_avc.ko
modprobe amvenc_avc encode_print_level=0
```

ただし **DOS/HCodec のレジスタを動作中に `devmem` で読むとハング**する。

**性能に関する注意（round 210）**: `--vpu-test` は
`modprobe amvenc_avc encode_print_level=0`（全ログ）でロードする。この状態で
パイプラインを動かすと、フレーム毎の ~9 行のログが 115200bps の同期コンソール
に流れて **10-11fps に落ちる**（本番 rootfs の modules-load.d は既定
LOG_ERROR なので問題ない）。パイプラインの速度測定や実運用の前に:

```sh
modprobe -r amvenc_avc && modprobe amvenc_avc   # 既定レベルで再ロード → 80fps
```

## NAS 登録の現状（round 210）

| ノード | 状態 |
|---|---|
| `192.168.99.56-15`（unit3） | **recording**（旧 `-17` は settings API の channel 変更で自動改称） |
| `192.168.99.123-BS09_0`（unit2） | recording |
| `192.168.99.109-17`（unit1） | recording |
| `192.168.99.209-15`（X96Max-2） | disabled |
