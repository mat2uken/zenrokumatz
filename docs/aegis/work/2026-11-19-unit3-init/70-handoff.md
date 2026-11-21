# 引き継ぎ（round 173 時点）

## 電源投入後にやること（これだけ）

```sh
cd ~/Desktop/work/X96MaxLinuxTSEnc
python3 tools/boot-debug-shell.py --vpu-test
```

配信中モジュール: `amvenc_avc.ko` = **`a8c333c5`**（トレース 18 箇所入り）。
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
# /tmp/amvenc_avc.ko = d73bd5f7
```

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

## 到達点

| 項目 | 状態 |
|---|---|
| Linux RAM 起動 | **達成** |
| Ethernet 1Gbps / SSH | **達成** |
| DT295 認識 | **達成** |
| DT295 から TS 受信（6MB, `tables_ready:1`） | **達成** |
| VPU エンコーダ `probe` → `/dev/video0` | **達成** |
| VPU エンコードの初期化（CMA 20MB 確保・バッファ設定・キュー登録） | **達成** |
| **VPU エンコードの実行** | **ハング中**（`avc_poweron` 以降をトレースで特定中） |
| NAS 保存 / CM カット | 未達（VPU 待ち） |
