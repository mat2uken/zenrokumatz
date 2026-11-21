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

## 追試: NVDEC オフロードの実測（2026-11-21 午前）

CPU チューニング後に残る CPU の約9割がデコードであることを受けて、Quadro K4200 の
NVDEC へのオフロードを実機で検証した。

### 準備

- ドライバ: `nvidia-dkms-470` + `libnvidia-compute-470` + `libnvidia-decode-470` +
  `nvidia-utils-470`（470.256.02）。DKMS が kernel 6.8.0-147 でビルド成功、
  **再起動なしで** nouveau アンロード → nvidia ロードに切り替えられた。
- ffmpeg: 既存 prefix と同構成に `nv-codec-headers`（sdk/11.1 系 11.1.5.5、
  要求ドライバ 470.57+）を足して `prefix-cuvid` をビルド。`h264_cuvid` 等の
  cuvid デコーダは GPU で復号し NV12 をシステムメモリへ渡すため、ツール側の
  変更はデコーダ選択と NV12 受け入れのみ（cm-scan / cm-overlay / cm-verify に実装）。
- フォールバック: cuvid が open できない（ドライバ無し等）場合は無条件で
  ソフトウェアデコーダ。**ドライバ無し状態での出力は現行版とバイト一致**
  （video.csv / audio.csv / logo.json / overlay.csv すべて同一）を確認済み。

### 実測（15分クリップ・1440x1080）

| 項目 | ソフトウェア | NVDEC | 削減 |
|---|---:|---:|---:|
| cm-scan CPU | 252.3 s | **76.3 s** | **−70%** |
| cm-verify CPU | 179.4 s | **39.4 s** | **−78%** |
| 判定（verified / ハッシュ不一致） | true / 0 | true / 0 | 一致 |
| wall（参考） | 68 s / 48.6 s | 120.6 s / 107.2 s | 遅い |

CPU は大きく減るが wall は 2 倍程度かかる（NVDEC 1 セッション約 224fps vs
ソフトウェア 4 スレッド約 400fps）。つまり「速くならない代わりに CPU を手放す」
という、まさにオフロードの性格。NVDEC 出力はソフトウェアとビット同一ではなく
平均輝度差 平均 1.05 / 最大 53.8 のフレームがあり、frame 数も 26,973 → 26,978 と
5 フレーム多い（エラー隠蔽の差）。判定系は勾配相関なのでこの差は許容範囲だが、
e2e でのカット位置一致確認は実施前。

### 発生した問題: Xid 79

**4 ワーカー同時の NVDEC デコードで GPU がバスから脱落（Xid 79）**した。
K4200 は待機時から 78°C / ファン 56% と高温で、カード自体の熱・経年劣化が疑われる。
脱落時、GPU ioctl で待つ cm-scan 4 プロセスがカーネル内で停止して kill 不能になり、
nvidia モジュールもアンロードできなくなった。**NAS の再起動で復旧**
（録画は manager が次パートとして継続、CM ワーカーに影響なし）。

再起動時に潜在バグが顕在化: `/dev/shm/pix_cm`（scratch/queue の tmpfs ディレクトリ）
が boot 時に再作成されず、CM ワーカーが自己検査失敗で再起動ループした。
`/etc/tmpfiles.d/pix-cm.conf` で `pix_cm` / `pix_cm_queue` / `pix_stream` を
boot 時自動作成するよう修正済み。

### 結論と方針

- 単一セッションのオフロード効果は本物（scan −70% / verify −78%）。
- ただし**このカードは 4 同時セッションで落ちる**。本番の 4 ワーカー全員が
  NVDEC を使う構成は不可。
- 採用するなら「同時 1 セッション制限（ファイルロック）＋ GPU 異常時は自動で
  ソフトウェアへフォールバック」のゲート付き構成が最低条件。しかし Xid 79 は
  単一セッションでも再発しうるため、**カードの物理的な清掃・サーマル対応の後に
  再評価する**のが妥当。
- 本番は現行のソフトウェア版（`20261121-cpu-tune-01`）のまま運用する。
  cuvid 対応ビルド（`/home/ku/work/cm-nvdec-20261121/out`）はフォールバック
  検証済みであり、カード安定後の投入に備えて保持する。

## ゲート付き NVDEC の実装（2026-11-21 午後）

「カードの物理メンテ後に GPU オフロードを有効化できるようにする」ため、
同時1セッション制限・温度監視・自動フォールバックを実装した
（dtv_android commit `80211a0` に続き、ゲート部分を追加）。

### 実装

- **cm-scan / cm-overlay / cm-verify 共通の GpuGate**:
  1. サービスが `CM_GPU_DECODE` を設定していないなら GPU に一切触れない
  2. `/dev/shm/cm-gpu.lock` の flock（非ブロッキング）で同時 1 セッション
  3. `nvidia-smi` で温度を確認し、`CM_GPU_MAX_TEMP`（既定 85°C）以上なら拒否
  4. カードが応答しない場合も拒否
  5. すべての拒否は無条件でソフトウェアデコードへフォールバック
- **cm_service.py**: `decode_gpu: true` のときだけツール環境へ
  `CM_GPU_DECODE=1` を渡す（`tool_env()`）。既定は未設定 = GPU 非接触。

### 検証結果（全経路テスト済み）

| 経路 | 結果 |
|---|---|
| フラグ無し | GPU 非接触、ログに gate 行なし |
| `CM_GPU_DECODE=1`（健康なカード） | NVDEC 使用、CPU 100→29 秒（5分クリップ・−71%） |
| ロックが他プロセスに保持されている場合 | 「another decoder holds the GPU; using software」でフォールバック |
| 温度しきい値超過 | 「GPU at 89C >= 85C; using software」でフォールバック |
| フォールバック出力 | 純ソフトウェアと **バイト一致**（video/audio/logo 全て） |
| e2e（decode_gpu 無効） | カット位置がベースラインと完全一致、verified=true |

### カードの最終判定: 物理メンテ必須

e2e 中、**単一セッションでも Xid 79 が再発**した。時系列は:

```
74°C（待機）→ ゲート通過（<85°C）→ NVDEC デコード開始
→ 負荷で 89〜96°C まで急上昇 → 後続デコードは温度ゲートが正しく拒否
→ 最初のセッションがカードを熱暴走させ、バスから脱落
```

温度ゲートは「次のセッションを防ぐ」働きは果たしたが、**走行中の熱暴走は
防げない**。ioctl で停止したプロセスは kill 不能になり、モジュールの
アンロードも不可能（再起動でのみ回復）。カードは清掃・サーマル処理を
してもなお再発する可能性があり、状況によっては寿命です。

### 配備

- `20261121-cpu-tune-02` を配備（`decode_gpu` 未設定 = GPU オフ）。
  動作は `-01` と同一で、ゲートと cuvid 経路が休眠状態で載っている。
- **GPU を有効化する条件**: カードの物理メンテ完了後、
  1. `config.json` に `"decode_gpu": true` を追加
  2. `systemctl restart pix-cm-worker pix-cm-worker@1 pix-cm-worker@2 pix-cm-worker@3`
  3. 翌日の `journalctl | grep "gpu gate"` で拒否理由の頻度を確認
- 残作業: 今回の e2e で kill できずに残った 1 プロセスがあり、次回再起動で消える
  （本番に影響なし）。

## 運用決定（2026-11-21）

**K4200 の物理メンテが完了するまで、ソフトウェアデコードのみで運用する。**

- 本番リリース `20261121-cpu-tune-02` は `decode_gpu` 未設定（GPU 非接触）で稼働。
  チューニングの効果（CPU −56.7%、キャッシュ時 −75.2%）はこの状態で得られている。
- cuvid 経路と GpuGate はリリースに同梱済みで休眠中。カード交換・清掃後は
  `config.json` に `"decode_gpu": true` を足してワーカー再起動するだけで有効化できる。
- 有効化の前提条件: Xid 79 が再発しないことを単一セッションで確認できた後。
  発生した場合は即座に `decode_gpu` を外し、ワーカーを再起動する。
