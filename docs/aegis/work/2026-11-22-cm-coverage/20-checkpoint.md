# CM カット対象拡大（マーカーキャッシュ修復）- Checkpoint

## 現在の状態

| 項目 | 状態 |
| --- | --- |
| 原因調査 | **完了**。参照画像の自己上書きで roi と PGM が食い違う（§1） |
| 修正 | **完了**。`dtv_android` の `cm_service.py` / `cm_logos.py`、commit `2b0e00b` |
| テスト | **完了**。133 件 OK（破損からの自動回復を含む 6 件を追加） |
| リリース | **完了**。`20261122-marker-integrity-02`（Python 2 ファイルのみ差分） |
| 実番組検証 | **完了**。3 局で完走、公開済み番組と同数のカットを再現 |
| 配備 | **完了**。ユーザー承認のうえ config 切替 + worker 4 つ再起動 |
| 本番ジョブ確認 | **完了**。配備後に 3 局が自動修復し、同一エラー 0 件、公開 1 本 |
| 文書化 | **完了**。拡大検討文書と verification §10 |
| 拡大の計測（第 2 段） | **完了**。`measure_agreement.py` を canonical に追加し 20 本を実測（拡大文書 §8） |
| 拡大の採否（第 2 段） | **完了**。scenario 公開は見送り、15 秒フロアの公差を採用 |
| 追加配備 | **完了**。`20261122-cm-coverage-01`、commit `9342948` |
| 拡大の第 3 段 | **完了**。既知マーク候補と信頼カウンタを配備（`-02`）。瞬断結合は実測で撤回 |
| 拡大の第 4 段 | **完了**。棄却区間からの部分採用（二検出器の一致）を配備（`20261123-cm-corroborate-01`） |

## 原因の要点

- `save_marker` が代替マーカーを固定名 `alt-0..2.pgm` に保存し、`fail_marker` の昇格も
  その固定名をマーカー json に書いていた。昇格後のマーカーは代替スロットを参照し続ける
  ため、次の代替保存が稼働中の参照画像を上書きする。
- `cm-scan` の `OverlayMeasure` は roi と同サイズの参照だけを切り抜きとして扱い、それ以外
  は全画面参照として縮尺する。食い違った組では平坦部を拾って `mask_count_ = 0` となり、
  デコード前に exit 1 する。
- 失敗がルール判定より手前で起きるため `fail_marker` が呼ばれず、`fails` が 2 に届かない。
  代替への自動昇格も起きないので、壊れたキャッシュが以後の全ジョブで再利用された。

## 修正の着地点

- 参照クロップを内容アドレス名 `ref-<sha1>-<i>.pgm` で保存し、保存済みの組を不変にする
- `load_marker` が roi と参照サイズ（roi サイズかフレームサイズ、8bit P5）を検証し、
  不整合なら `stats.alt` を検証して自動昇格。無ければ学習へ戻す
- `save_marker` も候補の整合を検証し、壊れたペアを書かない
- `cm-scan` / `cm-overlay` の起動失敗を `fail_marker` に計上し、その録画はマーカーを
  学習し直して解析を続行（従来はジョブ全損）
- マーカーキー単位の `flock` で stats の更新を直列化（3 worker の更新消失を防ぐ）
- 未参照の参照クロップを `marker_prune_days`（既定 7 日）経過後に削除
- ロゴ seed も同じ命名を使う

## 配備

| 項目 | 値 |
| --- | --- |
| リリース | `/home/ku/pix-cm/releases/20261122-marker-integrity-02` |
| config 退避 | `config.json.before-20261122-marker-integrity-02` |
| マーカー退避 | `/home/ku/pix-cm/markers-backup-20261122-marker-integrity` |
| 再起動 | `pix-cm-worker` / `@1` / `@2` / `@3` すべて active、`self_check_ok` |
| 切り戻し | config 退避を戻し `current` を `20261121-cpu-tune-04` に向けて再起動 |

`deploy-release.sh` は `ku` 権限ではサービス再起動に失敗する（`Interactive
authentication required`）。config と `current` の切替だけが済んだ状態になるため、
`sudo -n systemctl restart` を別途実行する必要がある。

## 次にやること

- 配備後の実ジョブで `insufficient reference logo edges` が出ないことを確認する（完了）
- マーカーが自動修復されたことを journal（`marker_cache_inconsistent` →
  `marker_promoted`）と `markers/*.json` で確認する
- 11/23 以降の公開率が 11/17〜11/21 の水準（21〜43%）に戻るかを見る

## 第 2 段：精度を保ちながら対象を広げる（18:35）

- 拡大提案を「何本増えるか」ではなく「公開中の経路と一致するか」で判定する計測を用意した
  （`experiments/cm-generic/measure_agreement.py`）。同じ録画から本番経路と拡大候補の
  判定を作り、境界 1.5 秒以内の一致・余分・見逃しを数える。
- 実測 20 本の結論は **scenario 公開の見送り**。
- 参照群 10 本のうち、現在のマーカーで mark が決められたのは 2 本だけ。残り 8 本は
  `present_ratio 0`（マーカーがその番組に無い）。
- 比較できた 2 本でも scenario は別の位置を切った（一致 0）。
- `min_cut_confidence` の緩和も見送り。該当 2 本の `clusters.gap` は 0.251 / 0.256 で、
  良好な番組の 0.8〜0.9 に比べて分離が弱い。
- 採用は **15 秒フロアの公差**のみ。検出器のスポットモデル（±`short_break_tolerance`）を
  フロアにも適用する。過去ジョブへの再適用で 4 カットが公開に変わり、3 局では BS11 の
  1 本が録画ごと復帰、テレビ静岡の 2 本に 1 箇所ずつ追加される。
- 30 秒フロア（ニュース・物販）は実測 4 件すべてで従来どおり落ちることを確認した。
