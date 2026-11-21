# 引き継ぎ（round 173 時点）

## 電源投入後にやること（これだけ）

```sh
cd ~/Desktop/work/X96MaxLinuxTSEnc
python3 tools/boot-debug-shell.py --vpu-test
```

配信中モジュール: `amvenc_avc.ko` = **`d5825643`**（パッチ 0001〜0008、トレース
18 箇所＋電源レジスタ読み返し 3 箇所入り）。
`cp` → `modprobe meson_vdec` → `modprobe amvenc_avc encode_print_level=0`
→ 合成映像エンコード、まで自動。**`insmod` では依存が解決せず失敗する**
（`Unknown symbol v4l2_m2m_*`）ので `modprobe` を使うこと。

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
# /tmp/amvenc_avc.ko = d5825643 （483,832 バイト、未 strip）
```

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

## 到達点（round 203 時点）

| 項目 | 状態 |
|---|---|
| Linux RAM 起動 | **達成** |
| Ethernet 1Gbps / SSH | **達成** |
| DT295 認識（06b8:103b） | **達成** |
| **VPU H.264 エンコード（機能）** | **達成**（64x64 / 1080p 6Mbps で RC=0、h264 出力） |
| **完了割り込み** | **達成**（`HCODEC_CCPU_INTR_MSK=1` で ISR が動く） |
| TS の PSI 取得 | **達成**（`have_pat:1`、`program_count:5`、`missing_pmts:0`） |
| **TS の映像データ** | **壊れている**（`mpeg2video: Invalid frame dimensions 0x0`） |
| 性能 | 約 11 fps（実時間 30 fps に不足） |
| pix-stream 統合 / NAS 保存 / CM カット | 未達 |

## 決定的だった修正（すべて実機で実証）

1. **U-Boot で `usb start` が必須**
2. **DTB の vdec を `amlogic,sm1-vdec` に**（機体は SM1 / S905X3）
3. **`hantro` を無効化**（Rockchip 専用で Amlogic に非対応）
4. **`ISO0` の `BIT(1)` をクリア**（パッチ 0010。これで HCodec 窓が応答）
5. **`DOS_SW_RESET0` もリセットパルス**（パッチ 0014。ucode の IMEM DMA が完了）
6. **`HCODEC_CCPU_INTR_MSK = 1`**（パッチ 0018。完了割り込みが来る）
7. **`PIX_CHANNEL` を 17 → 15**（SBS は物理 15。17 はテレビ静岡）

## 未解決（優先順）

1. **TS の映像データが壊れている**
   - PSI は正常、`mpeg2video` として検出できるがシーケンスヘッダが 0x0
   - スクランブルではない（TS ヘッダの scrambling control = 00 を実測）
   - `pix-ts-legacy` のハッシュは NAS ビルドと同一（`6df432fe`）で古くない
   - 次: 2代目（`192.168.99.123`、SSH 可）との比較、USB 転送エラーの確認
2. **性能 11 fps**
   - 割り込み・分周（div=0 を実測）・毎フレーム再初期化を除外
   - 解像度にも入力にも依存しない
   - 残る候補: ucode 種別（cabac→cavlc）、エンコード設定
3. **pix-stream 統合**（デコード → CPU コピー → エンコードの分離構成）

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
