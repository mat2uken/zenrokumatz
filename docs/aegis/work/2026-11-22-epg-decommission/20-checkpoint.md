# EPG ホスト廃止 - Checkpoint（2026-11-22 09:15、完了候補）

## 現在の状態

| 項目 | 状態 |
| --- | --- |
| 調査 | **完了** |
| 設計 | **完了**。docs/epg-decommission-design.md（lint 合格） |
| 実装 | **完了**。eit_watch / stb_http / stb_linux_pipe / pix-epg-bridge / pix_archive.c --epg / guard adb 除去 / 監査 pix-stb-health 結合除去 |
| テスト | **完了**。288 件 OK（274 + 追加 14）。ramboot-post-image の契約テストを新機構に合わせて更新 |
| 配備 | **完了**。NAS（bridge・pix-archive・guard・監査）+ STB 3 台（再ビルド → 焼き直し → 再起動、ユーザー承認済み） |
| 後処理 | **完了**。epg-window 廃止、pix-stb-health 廃止退避、旧コピー削除（ユーザー承認済み）、guard 状態 JSON の .40 キー除去、last_seq 整理 |
| 検証 | **完了**。収録基準 1〜6 すべて合格（docs/verification-20261122.md §8） |

## 実装の着地点

- STB: eit_watch が EIT schedule を 2048 件まで蓄積し /dev/shm/pix/epg.json（tmpfs）に 30 秒間隔で書く。stb_http が `GET /v1/epg` で返す
- NAS: `nas/src/pix-epg-bridge.py`（systemd pix-epg-bridge.service、127.0.0.1:40775）が 3 台の /v1/epg を 60 秒ごとに取得し mirakc 互換 API で供給。STB 死活時は前回データを継続提供
- pix-archive: `--mirakc` → `--epg` に改名（canonical は nas/src/pix_archive.c）、既定 URL 127.0.0.1:40775
- guard: adb・SMB400・.43 の死んだ復旧経路を除去。SSH S97 再起動と ffprobe 音声無検知は保持
- 監査: pix-stb-health 結合を除去（TIMERS・STB_HEALTH）、canonical は nas/src/pix-recording-audit.py

## 発見・修正したインフラ問題（§8 に詳細）

- NAS docker ボリュームの local パッケージ（stb-streamer-local）が再 rsync されず、旧 eit_watch が rootfs に入った → 作業ディレクトリ削除 + 全機再ビルド。unit1 は余分に 1 回再起動
- ramboot-post-image.sh の DTB 一覧ハードコードが unit3 の sm1.dtb を落としていた → .config の INTREE_DTS_NAME から導出へ修正
- unit3 が unit2 の identity で動いていた（SSH 鍵混線の潜在問題）→ 固有 identity で焼き直し、Mac/NAS known_hosts 更新
- NAS の guard は unit1 への SSH 鍵が無かった（rootfs が鍵作成前に焼かれていた）→ ランタイムで鍵を付与し、再起動後の rootfs で恒久化

## 検証エビデンス（本日実測）

- `/v1/epg`: unit1 222 / unit2 259 / unit3 213 イベント（bridge /health）
- archive.sqlite airings: 3 局 2128 行、max startAt = 2026-11-29 23:45（8 日先）
- 監査: epg_stale 最終出力 08:48 → 08:59 以降なし。summary から stb_health セクション消滅
- manager: 3 ノード recording、dropped 0、channelName = ＢＳ１１イレブン / テレビ静岡 / ＳＢＳ１（新ソースから解決）
- 録画フォルダ: 再起動後も正しい命名（BS_400211_ＢＳ１１イレブン 等）で継続
- 参照ゼロ: unit・cron・pix-cm・pix-stream-20261101 の grep（テキスト+バイナリ）ゼロ。残存ヒットはサードパーティ（.vscode-server、.codex、windr_esender）と CM ロゴ .pgm（画素偶然一致）、deps の civetweb 文書のみ

## 残っていること

- recorder フォールバック命名の実機シミュレーション未実施（tests と自然観察で代替）
- repo は未コミット（指示があれば実施）
- NAS の `~/x96max/` はリポジトリの rsync コピー（次回ビルド時に再 sync する運用）
