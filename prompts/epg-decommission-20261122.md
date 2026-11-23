# 次のセッションへ渡すプロンプト（EPGホスト廃止の完遂）

作業先は `/Users/kenichim/Desktop/work/X96MaxLinuxTSEnc` です。以下をそのまま新しいセッションに貼り付けて使います。

---

全録システムの EPG ホスト（mirakc / 192.168.99.40 = SMB400）への依存を完全に廃止し、Linux STB の eit_watch が集めた EIT からアーカイブ EPG を供給するよう作り替えてください。まず設計を固めてから実装・配備・検証まで進めます。

## 背景と現状（2026-11-22 時点）

全録（録画→NAS保存→CMカット）は Linux STB 3 台で運用中:

| 台 | IP | 役割 | チューナー |
| --- | --- | --- | --- |
| unit1 | 192.168.99.109 | テレビ静岡 ch17 | PX-S1UD（純正経路） |
| unit2 | 192.168.99.123 | BS11（BS9/TSID 0x4090） | DT295-CFW + torne-b25 |
| unit3 | 192.168.99.56 | SBS ch15 | DT295-CFW + torne-b25 |

- 各 STB は HTTP API（port 40774）を持ち、`/v1/status`、`/v1/program`（現在番組）、`/v1/streams/<id>` を提供する。eit_watch（`package/stb-streamer/src/eit_watch.c`）が TS から EIT/SDT を解析し、`/dev/shm/pix/program.txt`（現在番組）とイベント ID ごとの番組名キャッシュを保持する。
- STB は RAM rootfs。ランタイム変更は再起動で消え、STB 側の変更は「NAS の docker で `X96MAX_UNIT=N` してイメージ再ビルド → `docs/ram-rootfs.md` の起動媒体更新手順で焼き直し → **再起動は必ずユーザー確認を取る**」が手順。unit2 は 2026-11-22 に再起動検証済み、unit1/unit3 は未検証。
- NAS（`ssh ku@192.168.99.80`、root 権限は `sudo -n`）で動くもの:
  - `pix-archive.service`: `/home/ku/pix-archive-20261029/repo/bin/pix-archive --root /mnt/xblk/video/zenroku/recordings --listen 192.168.99.80:40773 --interval 300 --mirakc http://192.168.99.40:40772`。番組表を `recordings/_catalog/archive.sqlite` に保持し、`/api/archive/services`・`/api/archive/programs` を公開する。**pix-archive 自体の正規ソースの場所は未確認（要調査）。**
  - `pix-epg-window.timer`（04:00）→ `/home/ku/pix-cm/epg_window.py`: ADB（`GR_HOST=192.168.99.40:5555`）で .40 の mirakc を起こし、地上波は SMB400 の録画ストリーム tee、BS は .123 からダンプして pix-archive を再起動する。
- **問題**: .40 が 2026-11-21 10:45 頃から ping 不通で、EPG が約 96 時間 stale（newest startAt = 11/17 14:50）。監査が `epg_stale` を毎 10 分報告している。
- EPG の消費者は 2 系統:
  1. カタログ API（`/api/archive/programs` 等）と監査の `epg_stale` 判定。
  2. recorder（`nas/src/pix_stream_recorder.c`）の命名フォールバック `find_service_by_id` / `find_service_by_channel` / `find_program_by_clock`（`g_cfg.archive_url`）。STB からの pixb 番組メタデータが無い接続直後などに、時刻とチャンネルから番組を引く。
- 録画の番組名そのものは STB が実受信した EIT から来るので、**録画・CM 処理は EPG 停滞の影響を受けていない**。影響はカタログ表示とフォールバック命名だけ。
- 2026-11-21/22 に入れた修正を退避させないこと（詳細は `docs/verification-20261122.md`）: unit2 の BS11 修正（BS9/0x4090）、CM キュー part 2 消失バグ修正（job_id=partId）、深夜跨ぎフォルダ修正（番組 startAt ベース）、pix-stb-guard 再配備、bs-deinterlace ルート修正。

## 目的

1. mirakc / .40 / ADB 依存をすべて除去する。
2. Linux STB の eit_watch が集めた EIT を NAS が収集し、アーカイブ EPG（カタログ + recorder フォールバック）を供給する。
3. 録画 3 台を止めず、既存の修正を壊さずに移行する。

## 設計上の決めるべき論点

- **収集経路**。候補:
  - (A) eit_watch が保持しているイベントキャッシュ/スケジュールを STB の HTTP API（例: `GET /v1/epg`）で公開し、NAS 側ブリッジが定期取得して archive に供給する。STB 側変更 → 3 台分のイメージ再ビルド・焼き直しが要る。
  - (B) NAS が STB の `pix-ring`（`/dev/shm/pix/live.ring`、生 TS）から EIT を解析する。STB 変更不要だが、リング約 32MB（数分分）なので取得できるスケジュールの深さに限りがある。
  - (C) eit_watch が NAS へ直接 POST する push 型。
  まず eit_watch.c のキャッシュ構造（どの程度先のスケジュールまで保持しているか）を調査して決める。
- **EPG の対象範囲**: 各 STB は自トランスポンダしか受けていない（unit2 の BS9 は BS11 単一サービス）。カタログは 3 局分に縮む＝録画対象と一致する。これで要件を満たすかを確認して明記する。
- **archive への供給方法**: pix-archive を改修する（ソースを特定してから）か、mirakc 互換（または新 API）の小さなブリッジを NAS で動かすか。
- **後処理**: `pix-archive` の `--mirakc` フラグ、`epg_window.py` と `pix-epg-window.timer` の扱い（無効化/削除はユーザー確認）、監査 `epg_stale` の新鮮さ基準。

## 制約と方針

- NAS 側 C の正規ソースは `nas/src/`、ビルド・配備は `nas/build.sh`（`--apply` は manager 再起動を伴うが、録画は同一フォルダの次パートとして継続する）。
- STB ストレージ方針: RAM 上処理・tmpfs 完結・中間ディスク書き込みなし。
- テストは `tests/`（`python3 -m unittest discover -s tests -p 'test_*.py'`、`RAMBOOT_UNIT`/`RAMBOOT_CPIO` 付きで実行。274 件、2026-11-22 時点で全 OK）に追加して守る。
- 破壊的な操作（.40 関連 unit の無効化、ソース削除、STB 再起動）は必ずユーザー確認を取る。
- 作業記録は `docs/aegis/work/<日付>-epg-decommission/` と `docs/verification-YYYYMMDD.md` の形式で残す。

## 収録基準（完了の定義）

1. `/api/archive/programs` の newest startAt が過去 24 時間以内で、3 局分の番組表が今後数日分入っている。
2. recorder のフォールバック命名が mirakc なしで機能する（テスト + 可能なら実機シミュレーション）。
3. 監査の `epg_stale` 警告が消える。
4. NAS 内の .40 / mirakc / ADB への参照が網羅 grep でゼロになる（systemd unit、`/home/ku/pix-cm`、`/home/ku/pix-stream-20261101`、crontab 含む）。
5. 録画 3 台が停止せず、番組名が正しいまま。
6. `tests/` 全 OK、docs 更新、メモリ（OpenViking）への要点記録。

## 最初に読むもの・調べるもの

1. `docs/verification-20261122.md`（直近の状態と修正）、`docs/ram-rootfs.md`（STB イメージ更新手順）、`nas/README.md`（NAS 側ソース管理）。
2. pix-archive のソース場所と `--mirakc` の使われ方（`/home/ku/pix-archive-20261029/repo/`、systemd unit）。
3. `package/stb-streamer/src/eit_watch.c` の EIT キャッシュ構造と保持期間。
4. `/home/ku/pix-cm/epg_window.py` の GR/BS ダンプの詳細（mirakc に何を渡しているか）。
5. `/api/archive/*` の呼び出し元の全体把握（recorder 以外に UI/スクリプトが無いか）。
6. NAS 全体の .40 / mirakc / adb 参照の grep。

SSH は `ku@192.168.99.80`（NAS）と `root@192.168.99.109` / `.123` / `.56`（STB、鍵認証）。STB への scp は使えない（sftp-server 無し）ので `ssh cat >` を使うこと。
