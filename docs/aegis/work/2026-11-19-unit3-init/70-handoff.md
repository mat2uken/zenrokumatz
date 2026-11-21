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

## 到達点（round 188 時点）

| 項目 | 状態 |
|---|---|
| Linux RAM 起動 | **達成** |
| Ethernet 1Gbps / SSH | **達成** |
| DT295 認識 | **達成** |
| DT295 から TS 受信（6MB, `tables_ready:1`） | **達成** |
| VPU エンコーダ `probe` → `/dev/videoN` | **達成** |
| CMA 20MB 確保・バッファ設定・キュー登録 | **達成** |
| `vdec_1` クロック 666 MHz | **達成**（実測） |
| **HCodec レジスタ窓へのアクセス** | **達成**（パッチ 0010 でハング解消） |
| **HCodec ucode の IMEM DMA** | **未完了**（`hcodec load mc error`） |
| vdec ファームウェア | **追加済み**（overlay、再ビルド済み） |

## 現在の最重要ポイント: ISO0 の BIT(1)

起動直後の実測（`rdinit=/bin/sh`、ドライバ無し）:

```
AO_RTI_GEN_PWR_SLEEP0 (0xff8000e8) = 0xFFFDFEFF
AO_RTI_GEN_PWR_ISO0   (0xff8000ec) = 0xFFFDFEFF
HHI_VDEC_CLK_CNTL     (0xff63c1e0) = 0x00000000
devmem 0xff624008                  → ハング（HCodec 窓が死んでいる）
```

`compat.h` の `get_cpu_type()` は **G12A** を返す（上流仕様）ため、ISO0 には
`~0x30` が適用され **BIT(1) が残る**。SM1 の分離ビットは BIT(1)
（mainline `GEN_PWR_VDEC_1_SM1`）。**パッチ 0010 で両アームを `~0x33` に
して BIT(1) も落とす**と、`iso0 readback 0xfffdfecc` になり
**`0xff624008` への書き込みが通る**（ハング解消）。

## 次の一手

1. **デコード検証**: vdec ファームを入れた rootfs で
   `ffmpeg -c:v mpeg2_v4l2m2m -i x.ts -f null -` が通るか。
   通れば VPU 電源経路は正常で、問題はエンコーダ固有と確定。
2. **エンコーダの IMEM DMA**: `HCODEC_IMEM_DMA_CTRL` の 0x8000 が落ちない。
   DMA アドレスは `0xbbe0000` で妥当。レジスタ読み値が不自然
   （別オフセットが同値）なので、窓が実値を返していない疑い。
3. デコーダとエンコーダの同時使用は既知のハング要因。録画では
   **デコード → CPU コピー → エンコード** を別プロセスに分ける。

## ハングしないので電源再投入なしで反復できる

```sh
busybox wget -q -O /tmp/b.ko http://192.168.99.20:8000/amvenc_avc.ko
rmmod amvenc_avc ; cp /tmp/b.ko /lib/modules/6.18.62/updates/amvenc_avc.ko
modprobe amvenc_avc encode_print_level=0
```

（ただし HCodec レジスタを動作中に `devmem` で読むとハングするので注意）
| NAS 保存 / CM カット | 未達（VPU 待ち） |
