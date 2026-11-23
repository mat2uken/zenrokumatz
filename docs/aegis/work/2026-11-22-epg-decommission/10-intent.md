# EPG ホスト廃止（mirakc/.40 依存の完全除去）- Intent

## TaskIntentDraft

- Requested outcome: 全録システムの EPG ホスト（mirakc / 192.168.99.40 = SMB400）への依存を完全に廃止し、Linux STB 3 台の eit_watch が集めた EIT から NAS がアーカイブ EPG（カタログ API + recorder フォールバック命名）を供給する。設計固め → 実装 → 配備 → 検証まで。
- 元プロンプト: prompts/epg-decommission-20261122.md
- Parent plan/goal: セッションゴール goal-30c014e4-5dcc-42fd-a716-74c5f112a92c
- Success evidence（収録基準）:
  1. `/api/archive/programs` の newest startAt が過去 24 時間以内で、3 局分の番組表が今後数日分入っている
  2. recorder のフォールバック命名が mirakc なしで機能する（テスト + 可能なら実機シミュレーション）
  3. 監査の `epg_stale` 警告が消える
  4. NAS 内の .40 / mirakc / ADB への参照が網羅 grep でゼロになる（systemd unit、/home/ku/pix-cm、/home/ku/pix-stream-20261101、crontab 含む）
  5. 録画 3 台が停止せず、番組名が正しいまま
  6. tests/ 全 OK、docs 更新、OpenViking メモ記録
- Stop condition: 上記収録基準の達成、または破壊的操作のユーザー確認待ちで止まる
- Non-goals: mirakc/.40 本体の復旧、録画・CM 処理経路の変更、11/21/22 の既存修正（unit2 BS11 修正、partId 修正、深夜跨ぎフォルダ修正、pix-stb-guard 再配備、bs-deinterlace ルート修正）の退避
- Risk: high（録画 3 台の稼働系に触る。NAS 再配備は manager 再起動を伴う）

## BaselineReadSet

- prompts/epg-decommission-20261122.md（元依頼）
- docs/verification-20261122.md（直近状態。§1 unit2 再起動検証済、§6 epg_stale 96h）
- nas/README.md（NAS 側正規ソース = nas/src/、ビルドは nas/build.sh --apply）
- docs/ram-rootfs.md（STB イメージ更新手順）
- package/stb-streamer/src/eit_watch.c（EIT 解析。現在番組のみ + 名前キャッシュ 64 スロット。スケジュールは保持していない）
- package/stb-streamer/src/stb_http.c（/v1/program はファイルをそのまま返す。/v1/epg 追加は同パターン）
- nas/src/pix_stream_recorder.c（find_service_by_id / find_service_by_channel / find_program_by_clock は /api/archive/* のみを消費）
- docs/eit-program-names.md
- （要調査）pix-archive 正規ソースの場所、--mirakc の使われ方、/home/ku/pix-cm/epg_window.py、systemd units、crontab、監査 epg_stale

## ImpactStatementDraft

- Compatibility boundary: recorder の API 形（/api/archive/services・/api/archive/programs の JSON スキーマ）は維持する。archive_url 既定値は変えない
- Affected layers: STB 側 eit_watch/stb_http（EPG 収集と公開）、NAS 側 collector/ブリッジ、pix-archive への供給経路、epg_window.py と pix-epg-window.timer の無効化（ユーザー確認）、監査の epg_stale 判定
- Invariants: 録画 3 台は止めない。STB は RAM rootfs・中間ディスク書き込みなし。STB 再起動はユーザー確認必須
- EPG 対象範囲: 各 STB は自トランスポンダしか受けていないため、カタログは録画対象 3 局（テレビ静岡 ch17 / BS11 / SBS ch15）に縮む。録画対象と一致するので要件を満たす（設計文書に明記予定）

## TaskStartSnapshot

- 作業先: /Users/kenichim/Desktop/work/X96MaxLinuxTSEnc（main）
- NAS: ssh ku@192.168.99.80（root 権限は sudo -n）。STB: root@192.168.99.109 / .123 / .56（鍵認証、scp 不可 → ssh cat >）
- .40 は 2026-11-21 10:45 頃から ping 不通。EPG は newest startAt = 11/17 14:50 で stale
- ArchitectureReviewRequired: yes（EPG 供給経路の単一所有者を mirakc から STB eit_watch 経由へ変える、retirement 対象が多い）
- TDD Mode: off。tests/ に回帰を追加する方式（274 件全 OK が現状基準）
