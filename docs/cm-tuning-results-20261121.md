# CMカット処理のチューニング実施記録（2026-11-21）

前回の検討（[cm-gpu-offload-study-20261121.md](cm-gpu-offload-study-20261121.md)）で
洗い出した CPU のみの改善案を実装・検証し、本番へ配備した記録。

## 結論

| 項目 | 変更前 | 変更後 | 改善 |
|---|---:|---:|---:|
| 1本あたりの CPU 時間（マーカー学習あり） | 2,568.5 秒 | 1,112.5 秒 | **−56.7%** |
| 1本あたりの CPU 時間（マーカーキャッシュ利用） | 2,568.5 秒 | 636.1 秒 | **−75.2%** |
| 1本あたりの実時間（50分番組） | 836.6 秒 | 329.5 秒 | **2.5 倍高速** |
| カット結果 | 3箇所 | 3箇所（同一） | 変化なし |

GPU（Quadro K4200）は使っていない。**CPU だけでこの削減に到達した。**

## 実施した変更

### 1. `cm-verify` をパケット照合へ変更（全体の −56%）

**変更前**: 元ファイルとカット後ファイルの**両方**を全編デコードし、全フレームの
画像プレーンを MD5 していた。2時間番組なら 364 GB をハッシュしていた計算になる。

**変更後**: パケットの MD5 で照合し、**出力側だけを全編デコード**する。

`cm-cut` は再エンコードせずパケットをコピーしてタイムスタンプだけシフトするため、
「出力パケットのハッシュが元の部分列と一致する」ことがビット同一性の直接証明になる。
デコード後フレームの MD5 一致は、その帰結を遠回りに確かめているにすぎなかった。

実測（45分番組・実素材）:

```
変更前: wall 1354.5s  CPU 3542.1s
変更後: wall  155.1s  CPU  503.8s      → 7.0 倍の CPU 削減
判定結果: 両者とも verified=true、全カウンタ一致
```

残す保証:
- 出力の全編デコード（`output_video_decoded_frames` で確認）
- パケットのハッシュ・順序・フラグの一致
- タイムスタンプ誤差 1ms 以内

落とした保証と代替: 元ファイルのデコード可否は `cm-scan` が全フレームをデコードして
確認済みなので、`cm-verify` で二重に確かめない。

**副次的な修正**: `--audio-packets-only` は名前と裏腹に映像を常にデコードしていた
（`cm_verify.cpp` の分岐が `packets_only` を見ていなかった）。このオプションは
CPU 削減に一切寄与していなかった。

### 2. 重複した `cm-scan` 実行を削除（−9%）

`cm_service.py` の `measure()` は、キャッシュ不一致時に、数行前と**同一の argv・
同一の出力先**で `cm-scan` を再実行していた。1回目がすでに `logo.json` と参照 PGM を
書き終えているので、それを読むだけでよい。

```python
# 変更前
command(scan_argv, stage / 'scan-no-cache.log')   # 100% 純粋な重複

# 変更後
if not scan_done:
    command(scan_argv, stage / 'scan-no-cache.log')
```

実運用ではマーカーキャッシュがほぼ効いていないため、この重複は毎回発生していた。

### 3. マーカー ROI 測定を `cm-scan` に統合（−9%、キャッシュ時はさらに大）

キャッシュされた全角マークの ROI はスキャン開始前に既知なので、
`cm-overlay` が独立して2回目のフルデコードをする必要はない。
`cm-scan` に `--overlay-roi` / `--overlay-references` / `--overlay-output` を追加し、
デコードループ内で ROI の加算と相関を行う。

**出力は `cm-overlay` と 1 バイトも違わない**ことを実測で確認:

```
$ cm-scan clip.mp4 out ... --overlay-roi 32,48,48,48 --overlay-references ... --overlay-output out/overlay-inline.csv
$ cm-overlay clip.mp4 ... 32 48 48 48 out/overlay-standalone.csv
$ cmp out/overlay-inline.csv out/overlay-standalone.csv
IDENTICAL
```

追加コストは 15分クリップで 1 CPU秒（102.4 → 103.4 秒）。`cm-overlay` の 305 CPU秒が
丸ごと不要になる。

## 検証

### 単体テスト

```
120 passed
```

### エンドツーエンド（実録画から作った fMP4・25分・1440x1080）

同じ入力・同じ設定で新旧リリースを実行。

| | 変更前 | 変更後（学習） | 変更後（キャッシュ） |
|---|---:|---:|---:|
| CPU 秒 | 2,568.5 | 1,112.5 | 636.1 |
| 実時間（秒） | 836.6 | 329.5 | 184.4 |
| 検証 | verified=true | verified=true | verified=true |
| カット位置 | 3箇所 | **同一の3箇所** | **同一の3箇所** |

カット位置は完全一致:

```
[[312.325, 370.383], [692.705, 900.913], [1191.203, 1339.351]]
```

### バイナリの等価性

再ビルドしたバイナリが本番版と同じ出力を出すことを確認:

- `cm-scan`: `video.csv` / `audio.csv` / `logo.json` / `structure-*.pgm` / 参照 PGM が
  **すべてバイト一致**
- `cm-cut`: 生成 MP4 が**バイト一致**、レポート JSON も一致
- `cm-overlay`: 統合版の出力が**バイト一致**

## 配備

```
release: /home/ku/pix-cm/releases/20261121-cpu-tune-01
config.json の bundle を切り替え、current シンボリックリンクを更新
pix-cm-worker / @1 / @2 / @3 を再起動 → 4ワーカーすべて active
```

配備後の実績: 実ジョブが新リリースで完走（`release: 20261121-cpu-tune-01`）。

実処理で正常に CM カットされることの確認: CM を含む既知の実録画を fMP4 にして
本番キューへ投入し、本番ワーカーが処理することを確かめた。

```
job:      tunetest-...        release: 20261121-cpu-tune-01
status:   completed           reason:  cut_and_verified
validation: verified=true  full_decode_ok=true  verification_mode=packet_hashes
            expected_video_frames=30222  output_video_frames=30222  video_hash_mismatches=0
            expected_audio_packets=47269  output_audio_packets=47269  audio_hash_mismatches=0
            video_max_pts_error_ms=0.0    audio_max_pts_error_ms=0.0
cuts:     4 箇所
出力:     1440x1080 H.264 / 30,222 フレーム / 1008.4 秒（ffprobe で再生可能を確認）
```

確認後、このテストで作られた録画フォルダ・`.media` の実体・ジョブ状態は削除し、
ライブラリは元の状態に戻した。

### ロールバック

```
cp /home/ku/pix-cm/config.json.before-20261121-cpu-tune-01 /home/ku/pix-cm/config.json
ln -sfn /home/ku/pix-cm/releases/20261118-logo-04 /home/ku/pix-cm/current
systemctl restart pix-cm-worker pix-cm-worker@1 pix-cm-worker@2 pix-cm-worker@3
```

## 見送ったもの

| 案 | 判断 |
|---|---|
| GPU（NVDEC）によるデコード移譲 | 削減できるのは残りのデコード分のみ。ドライバ導入＋ffmpeg 再ビルド＋3ツール改修のコストに見合わない |
| フル解像度 `sws_scale` の窓内限定 | −1.9%（実測）。画素値は不変なので、ついでの整理としてなら可 |
| `video.csv` の行集約 | **非推奨**。205,800行で 0.3 秒。判定のフレーム粒度を失うリスクの方が大きい |
| Python 側の処理 | 無視できる（1回あたり約 1.1 秒） |

## 今後の検討課題

- **`field_coded_unsupported` が 125 件**（全体の 5.8%）。パケット照合方式にすると
  「AU数≠フレーム数」問題が消えるため、この制約を外せる可能性がある。要検証。
- **マーカーキャッシュのヒット率**。今回の統合でキャッシュヒット時の価値が大きく
  なった（CPU −75%）。ヒット率を上げれば効果がさらに広がる。

## 変更したファイル

`dtv_android/experiments/cm-tune/`（正規ソースは `cm-generic/` と `cm-compare/`）

| ファイル | 変更内容 |
|---|---|
| `cm_verify.cpp` | パケット照合＋出力のみデコードへ全面書き換え |
| `cm_scan.cpp` | `OverlayMeasure` クラスと `--overlay-roi` 系オプションを追加 |
| `cm_service.py` | 重複スキャン削除、ROI 測定の統合、`run_parallel` 削除 |
