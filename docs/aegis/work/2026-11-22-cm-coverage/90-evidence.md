# CM カット対象拡大（マーカーキャッシュ修復）- Evidence

## 収録基準との対応

| 基準 | エビデンス |
| --- | --- |
| 1. 3 局で完走 | 3 局とも `mode=overlay` / `breaks_found` に到達（§3） |
| 2. 同一エラー 0 | 修復後の 3 局で不出現。旧リリースでは再現（§3） |
| 3. 自動回復 | `load_marker` の整合性検証と `alt` 昇格を実装。破損を作る試験を含め 133 件 OK（§4） |
| 4. 分類と文書化 | docs/cm-coverage-expansion-20261122.md（lint 合格） |

基準 1 の実測は §3、基準 2 の対照実験も §3、基準 5 の内訳は §5 にある。
| 5. 退行なし | 当日公開 5 本が `verified=true`、original も存在（§5） |

## 1. 原因の証明

`markers/*.json` の roi と参照 PGM の実測（11/22 15:09 時点）。

| キー | roi | 参照 PGM | 整合 |
| --- | --- | --- | --- |
| `3219535864_1440x1080` | `[56,974,192,80]` | 72x48 | 不一致 |
| `3219435856_1440x1080` | `[56,24,56,64]` | 192x80 | 不一致 |
| `400211_1920x1080` | `[1752,24,64,56]` | 80x64 | 不一致 |

3 件とも `.stats.json` の `alt` は roi と PGM が整合していた（72x48 / 192x80 / 80x64）。

`cm_scan.cpp` の `OverlayMeasure::at()` は roi と同サイズの参照だけを切り抜きとして
扱い、それ以外は全画面参照としてフレームに縮尺する。縮尺された平坦部では勾配が
立たず `mask_count_ = 0` となり、`cm_scan.cpp:560` の `need()` で exit 1 する。
デコード開始前の失敗なので `fail_marker` は呼ばれない。

journal（`pix-cm-worker@*`）に 3 局とも同一手順が残る。昇格の直後に代替保存が
稼働中マーカーの参照画像を上書きしている。

| 局 | 昇格 | 上書きした代替保存 | 結果 |
| --- | --- | --- | --- |
| テレビ静岡 | 11-22 00:01:27 `[56,974,192,80]` | 00:02:32 `[1328,24,72,48]` | 192x80 + 72x48 |
| SBS | 11-21 20:55:59 `[56,24,56,64]` | 20:56:07 / 20:56:55 `[1208,32,192,80]` | 56x64 + 192x80 |
| BS11 | 11-22 03:26:15 `[1752,24,64,56]` | 03:27:56 `[1752,16,80,64]` | 64x56 + 80x64 |

破損時刻と `detection_failed` の発生開始も一致する（11-21 の 5 本は SBS の 20:56 以降）。

## 2. ジョブの分類（11/22 の 89 本）

| 理由 | 本数 |
| --- | ---: |
| `detection_failed`（同一エラー） | 79 |
| `cut_and_verified` | 5 |
| `no_breaks` | 3 |
| `timing_breaks` | 2 |

## 3. 実番組での検証（マーカーの複製を使用、ライブは未変更）

`/home/ku/pix-cm/markers` を複製したディレクトリに対して、新リリースの `cm_service` を
使って検出本体（`inspect_source` → `measure`）を実行した。

| 局 | 番組 | 修復後の roi | 判定 | カット |
| --- | --- | --- | --- | ---: |
| テレビ静岡 | 救命病棟２４時（11/21・60 分） | `[1328,24,72,48]` | overlay / breaks_found | 4 |
| SBS | みなスポ（11/21・30 分） | `[1208,32,192,80]` | overlay / breaks_found | 1 |
| BS11 | Healing Music（11/22・5 分） | `[1752,16,80,64]` | overlay / breaks_found | 1 |

- テレビ静岡の 4 箇所は 11/21 の公開時のカット数と一致（`present_ratio` 0.761、
  `clusters.gap` 0.907、`confirms=1` に更新）
- SBS の 1 箇所も 11/21 の公開時と一致。この番組では学習候補が勝ち、修復した
  `[1208,32,192,80]` は代替として保存された（`fails=1`）
- BS11 は当日失敗した 79 本の 1 本。修復後にカットまで到達した
- 対照: 旧リリース `20261121-cpu-tune-04` を同じ破損コピーに実行すると
  `cm-scan exit=1: ERROR: insufficient reference logo edges` を再現

実行ログの要点（テレビ静岡・救命病棟）:

```
{"event": "marker_cache_inconsistent", "roi": [56, 974, 192, 80], "references": ["alt-0.pgm", ...]}
{"event": "marker_promoted", "reason": "inconsistent cache", "roi": [1328, 24, 72, 48]}
{"event": "decision", "mode": "overlay", "reason": "breaks_found",
 "cuts": [[666,816],[1209,1359],[1925,2075],[2458,2608]], "present_ratio": 0.761}
```

## 3-b. 配備後の本番確認

配備（15:41）後、本番のジョブが 3 局のマーカーを自動修復した。

| 局 | 修復時刻 | 修復後の roi | 実行者 |
| --- | --- | --- | --- |
| テレビ静岡 | 15:49:50 | `[1328,24,72,48]` | 検証実行（ライブ） |
| SBS | 15:49:57 | `[1208,32,192,80]` | 本番ジョブ 208077 |
| BS11 | 16:00:07 | `[1752,16,80,64]` | 本番ジョブ 208105 |

配備後のジョブ（16:09 時点で 4 本、3 局すべて）:

| 時刻 | 局 | 理由 | 判定 |
| --- | --- | --- | --- |
| 15:59:03 | テレビ静岡 | `cut_and_verified` | overlay / 1 箇所 |
| 16:01:57 | テレビ静岡 | `no_breaks` | scenario |
| 16:03:06 | SBS | `timing_breaks` | scenario / 候補 4 箇所 |
| 16:05:43 | BS11 | `no_breaks` | scenario |

`detection_error` は 4 本とも空である。

`insufficient reference logo edges` は配備後の journal で 0 件、例外も 0 件。ライブの
マーカー 3 件はすべて roi と参照 PGM が一致した状態になっている。

16:03:43 には BS11 で `marker_promoted`（`cached marker failed twice`、roi
`[96,40,256,104]`）が記録された。失敗回数が正しく数えられるようになったため、本来の
代替昇格が本番で動作した例である。

## 4. テスト

`dtv_android/experiments/cm-generic` で `python3 -m pytest` → 133 件 OK。
追加した試験（`test_cm_service.py`）:

| 試験 | 確かめること |
| --- | --- |
| `test_inconsistent_cache_is_repaired_from_its_alternative` | 破損状態から `alt` へ自動回復し、ディスクにも書かれる |
| `test_inconsistent_cache_without_alternative_is_relearned` | 代替が無ければ学習へ戻る |
| `test_missing_reference_crop_is_not_a_marker` | 参照欠落をキャッシュとして使わない |
| `test_alternative_cannot_overwrite_a_promoted_marker` | 昇格後の参照を代替保存が上書きしない（回帰） |
| `test_mismatched_candidate_is_not_cached` | サイズの合わない候補を書かない |
| `test_scan_that_stopped_before_the_first_frame_may_be_retried` | 起動失敗時の再実行条件 |

## 5. 公開ゲートの確認

11/22 に公開された 5 本すべてで、`validation.verified=true`、`stored_output` あり、
元 MP4 も存在した。公開判定・カット・検証の経路は今回変更していない。

| 番組 | verified | 出力 | original |
| --- | --- | --- | --- |
| マジカル★エクスプローラー 第８話 | true | あり | あり |
| きみが死ぬまで恋をしたい セレクション放送 | true | あり | あり |
| 生徒会にも穴はある！ 第８話 | true | あり | あり |
| ごりやくさん 第１５回 | true | あり | あり |
| 世界の鉄道 絶景の旅 ＃１６ | true | あり | あり |

## 6. リリース

| 項目 | 値 |
| --- | --- |
| リリース ID | `20261122-marker-integrity-01` |
| 差分 | `cm_service.py` / `cm_logos.py` のみ |
| バイナリ | `cm-scan` / `cm-overlay` / `cm-cut` / `cm-verify` は現行と同一ハッシュ |
| manifest | 9 ファイルすべてハッシュ一致 |
| マーカー退避 | `markers-backup-20261122-marker-integrity`（2.4MB） |

## 7. 拡大施策の計測と採否（第 2 段、2026-11-22 18:35）

### 7.1 計測（20 本）

`experiments/cm-generic/measure_agreement.py` で、同じ録画から本番経路（マーカー）と
拡大候補（時刻・無音）の判定を作り、境界 1.5 秒以内の一致を数えた。

| 群 | 本数 | 結果 |
| --- | ---: | --- |
| 参照（mark が公開した録画） | 10 | 現在のマーカーで mark が決まったのは 2 本。一致 0 |
| 対象（mark が決められなかった録画） | 10 | scenario はすべてで 1〜4 カット。幅は 15 秒の倍数 |

参照群の残り 8 本は `present_ratio 0` で、マーカーがその番組に無かった。番組ごとに
マーカーが変わるためである。比較できた 2 本でも scenario は別の位置を切った。

判断: **scenario 公開は見送り**。`min_cut_confidence` の緩和も見送り（該当 2 本の
`clusters.gap` が 0.251 / 0.256 で分離が弱い）。

### 7.2 採用した拡大（15 秒フロアの公差）

デプロイ済みリリース `20261122-cm-coverage-01` の本番関数で、11/21 の
土曜プレミアム（テレビ静岡・103 分）を再判定した。

| 項目 | 値 |
| --- | --- |
| 判定 | `mode=overlay` / `breaks_found` |
| カット幅 | 195.09 / 195.20 / **14.91** / 265.06 秒 |
| フロア | 15.0 秒（下限 − 公差 1.5） |
| 公開対象 | 4 カットすべて（落とすもの無し） |

14.91 秒の 1 本は、当時の本番では `short_cuts_rejected` で落としていた区間である。
判定そのものは当時と同じ 4 箇所で、フロアだけが変わったことが確認できる。

過去ジョブ 8 件への再適用では 4 カットが公開に変わり、ニュース・物販の 30 秒フロアが
かかる 4 件は従来どおり落ちた。
