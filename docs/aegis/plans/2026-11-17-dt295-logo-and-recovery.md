# DT295 の取り扱い改善: 局ロゴの受信とファームウェア復旧

2026-11-17。smpn2/pix-dt295 (MIT) の実装を参照し、`vendor/pix-userland` の
PIX-DT295 処理を改善する。

## Goal

1. USB を物理的に切り直さずに、残留したファームウェア状態から復旧できるようにする。
   現状は state 0x11 / 0xFFFF / state 2 が残ると
   「firmware boot requires cold state 0 or unauthenticated state 16」で起動できず、
   xhci のグルードライバ unbind/bind（録画が数十秒止まる）に頼っている。
2. BS-15 のエンジニアリングサービス (929) が送出する局ロゴを受信し、CM カットの
   判定材料として NAS に渡せる形で保存・配信する。

## Architecture

| 層 | 担当 | 変更 |
|---|---|---|
| デバイス制御 | `vendor/pix-userland/src/dt295.c`（ベンダー固定。パッチで変更） | 起動時のソフトリセット、keepalive 周期、ACK 待ちのイベント処理、fw_stall 早期失敗 |
| 受信バッファ | `vendor/pix-userland/src/dt295_stream.c`（同上） | EP 0x81 の在庫を 8×16 KiB → 16×64 KiB に増やす |
| ロゴ抽出 | `package/pix-tuner/logo/`（自前 C、libusb 非依存） | TS から DSM-CC カルーセルを組み立てて PNG を書き出す新規コマンド |
| 配信 | `package/stb-streamer/src/stb_http.c` | `GET /v1/logos`、`GET /v1/logos/<name>.png` |
| 運用 | `board/x96max/overlay/usr/bin/pix-logo-fetch` | パイプラインを止めて 929 を録り、抽出して戻す |

要点: ロゴ抽出はデバイスに触らない純粋な TS パーサにする。録画経路は既存の
`pix-ts-legacy` をそのまま使い、`pix-ts-legacy ... -t N | pix-dt295-logo` で繋ぐ。
これにより実機が無い状態でも、合成 TS で抽出ロジックを検証できる。

## Baseline / Authority Refs

- `vendor/pix-userland` は `rk2kk/pix-userland` の a866c61 をそのまま保持する（sources.lock.json）。
  変更は `board/x96max/patches/pix-userland/*.patch` に置き、ビルド時に当てる。
- 仕様の根拠: smpn2/pix-dt295 の `docs/device.md`（レジスタ・コマンド）、
  `src/dt295/src/fw.rs`（復旧手順）、`src/dt295-core/src/proto.rs`（doorbell と ACK）、
  `src/dt295/src/s1/mod.rs`（KA_INTERVAL=12 秒、KA_FIRST=5 秒）、
  `src/dt295-cli/src/logo/`（カルーセルとロゴの形式）。
- 局ロゴ: BS/CS は 929 のカルーセル（component_tag 0x79/0x7A、stream_type 0x0D）、
  地デジは CDT。NAS 側は `pix-cm/markers/<service>_<WxH>.json` に
  `roi` と参照 PGM を学習キャッシュしている。

## Compatibility Boundary

- `pix-ts-legacy` の CLI と終了コードは変えない（録画経路の呼び出しはそのまま）。
- 復旧手順は「今まで失敗していた状態で成功する」追加であり、0 と 0x10 の経路は変えない。
- keepalive は間隔が短くなるだけで、プロトコルは同じ。
- `/v1/logos` は追加のみで、既存の `/v1/streams`、`/v1/status`、`/v1/program` は変えない。
- ロゴの取得は既定で無効（手動コマンド）。録画を止める判断を勝手にしない。

## TDD Route

- mode: auto（既定）
- decision: strict（復旧ロジックはプロトコル挙動の変更、ロゴ抽出は新規の
  パーサで回帰信号が明確なため。どちらも先にテストを書く）
- authority: この計画。テストは実機ではなく、`dt295_io` を差し替えた
  モックデバイス（レジスタ・doorbell を模擬）と、合成 TS で行う。
- verification: `scripts/test-dt295-host.sh` がホストで全テストを実行する。

## Tasks

### Task 1: 復旧・keepalive・ACK 待ち（strict）

- [x] `tests/test_dt295_recovery.c` を先に書く（RED）。
      - state 0x11 / 0xFFFF / 0x02 から `dt295_boot` が 0x10 到達の手順を踏むこと
      - 0xFFFF では EP 0x81 を読み捨ててから cmd3 を送ること
      - 0x11 では cmd(2,1) → cmd(2,0) → 認証の順で進むこと
      - 5 秒無応答が 4 回続いたら、以後のコマンドが時間をかけずに失敗すること
      - keepalive が 12 秒周期で、ストリーム開始時に 1 回出ること
- [x] `board/x96max/patches/pix-userland/0001-dt295-recover-firmware-and-keepalive.patch`
- [x] `scripts/apply-vendor-patches.sh`（ビルドとテストの両方から使う）
- [x] `package/pix-tuner/pix-tuner.mk` でパッチを適用
- 検証: `scripts/test-dt295-host.sh recovery`

### Task 2: EP 0x81 の受信在庫（light）

- [x] `dt295_stream.h` の `DT295_STREAM_TRANSFERS` を 8 → 32 に（URB は usbfs の上限 16 KiB のまま）
- 検証: 既存の `tests/test-dt295-stream.c`（ベンダー分は未同梱のため、
  必要なら本リポジトリに最小の再現テストを置く）

### Task 3: 局ロゴ抽出（strict）

- [x] `tests/test_dt295_logo.c` を先に書く（RED）。合成 TS を組み立て、
      PAT → PMT → DII → DDB → モジュール → PNG まで通す。
      欠損ブロック、バージョン変更、上限超過、壊れた CRC も入れる。
- [x] `package/pix-tuner/logo/{ts_section.c,logo_carousel.c,pix_dt295_logo.c}`:
      - セクション組み立て（pointer_field、long section、CRC32/MPEG-2）
      - DII でモジュール名・サイズ・バージョン・ブロック長を取得
      - DDB をブロック番号で集めて本体を再構成（上限: モジュール 1 MiB、32 個）
      - ロゴ本体（logo_type、サービス三つ組、PNG）を解釈
      - `<network>_<tsid>_<service>.png` と `index.json` を書く（内容不変なら書かない）
- 検証: `scripts/test-dt295-host.sh logo`

### Task 4: 配信と運用

- [x] `stb_http.c`: `GET /v1/logos`（index.json をそのまま返す）と
      `GET /v1/logos/<name>.png`（`..` や `/` を拒否、最大 1 MiB）
- [x] `pix-logo-fetch`: パイプラインを止める → 929 を録る → 抽出 → 再開。
      `--dry-run`、`--wait`、`--out` を持つ。止められないときは何もしない。
- [x] `docs/dt295-station-logos.md`: 使い方、ファイル形式、NAS 側の使い方、制約
- [x] README の DT295 節に追記

### Task 5: 検証と後片付け

- [x] `scripts/test-dt295-host.sh` で全テスト green（復旧・局ロゴとも）
- [x] 実機手順を docs に書く（DT295 が接続されているときに実行する確認項目）
- [x] `git status` が意図した差分のみ

## Follow-up (2026-11-18 午前)

ユーザーの判断で次の 3 つを追加した。

- **地上波 CDT**: `logo_cdt.c` を追加し、`eit_watch --logo-dir` で録画中の TS から
  拾う。チューナーは止めない。実機 (テレビ静岡) で動作確認、ただしこの局の
  ロゴは空だった。
- **週 1 回の自動取得**: `pix-logo-weekly` と `S98pix-logo`。月曜 04:00 JST、
  時計が正しくないあいだは走らない、週 1 回だけ。テレビ静岡の個体で
  発火と前提違いの終了まで確認。
- **NAS 側の消費**: `cm_service.py` の `measure()` から
  `seed_marker_from_logo()` を呼ぶ。実録画で `roi [1312,23,96,56]`
  （学習値 `[1312,24,96,56]`、スコア 0.94）を再現。配備は下の
  「配備のやり直し」のとおり (最初の配備先を誤った)。

### 配備のやり直し (2026-11-18 13:40)

`20261118-logo-01` は古いツリー (619 行の cm_service) から組み立ててしまい、
10:04〜13:38 の 15 本が `original_only / detection_failed` になった。
`20261112-feature-01` (1177 行) を土台に変更分だけを載せた
`20261118-logo-03` (1200 行) を作り直して配備し、4 worker の自己検査と
テストで確認した。古いリリース 2 つは残置 (current ではない)。

## Next (2026-11-18 午後)

- Buildroot での実ビルドを確認: `scripts/build-linux.sh pix-tuner-rebuild
  stb-streamer-rebuild` が通り、`pix-ts-legacy` に復旧パッチの文字列
  (`dt295_recover` ほか)、`pix-dt295-logo` に `--cdt` が入った成果物を確認。
  STB へ配備し、局ロゴの採取と `/v1/logos` が動くことを確認。
- `pix-logo-fetch` を中断しても録画を戻すようにした (EXIT トラップ + 専用
  プロセスグループ + ロック)。実機 (busybox ash) で `wait` がシグナルで
  中断されること、グループ kill が効くことを確認。
- NAS 側の変更は dtv_android に保全した (`experiments/cm-generic/cm_logos.py` と
  `cm_service.py` の差分、`docs/cm-station-logos.md`、コミット 47b9a83)。

## 実機確認 (2026-11-18 14:00, 192.168.99.123 / x96max-p2)

- **ファームウェア復旧**: 録画中に `pix-ts-legacy` を kill -9 して state 0x0002 を
  残したところ、当初の実装 (コマンド3) では戻らなかった
  (`dt295_recover_failed`) 。参照実装の to-idle と同じ「pause → EP 0x81 排出 →
  state 1 → idle」を実装し直し、再試験で `dt295_recover_stop` を出して
  **VBUS を切り直さずに復旧**・受信再開 (exit 0, 124,710 パケット) を確認。
- **BS ロゴ**: 929 へ選局し PMT のカルーセル 2 本 (BS 0x79 / CS 0x7A) を認識。
  再起動後に 15 分待って **153 サービス分を取得**（`/var/lib/pix/logos`,
  BS11 = 400211, 48x27, type 2）。
- **ロゴの中身**: 153 件すべて PLTE の無いパレット PNG。インデックスを
  展開すると「BS11」の文字が読める（データは本物）。ただし学習済みの
  画面上マークとの相関は 0.45 前後、シルエット IoU 0.28 で、自動シードの
  しきい値に届かない。→ `logo_seed` は発火せず学習が正のまま。
- **HTTP の不具合**: stb_streamer がリクエストヘッダを読まずに close して
  RST を返し、Python の urllib が応答を読めなかった。close 前に排出する
  修正を入れて両個体へ配備（curl は許容していたため見えていなかった）。

### シルエット照合への作り替え (2026-11-18 午後)

放送ロゴは線画・画面上のマークは塗りで、画素の相関は 0.45 止まりだった。
輪郭を四隅×サイズで走査し、切り出しの塗りシルエットとの IoU で候補を選ぶ
方式に変えたところ、BS11 で `roi [1312, 27, 94, 49]`（学習値は
`[1312, 24, 96, 56]`）・形の一致 0.506 でシードできた。誤検出は 0.28〜0.31
で棄却。NAS はリリース `20261118-logo-04` に配備し、4 worker の自己検査と
テストで確認した。

## Result (2026-11-18)

ホストテストは通った。実機は DT295 が挿さっていない（PX-S1UD の地上波構成で
稼働中）ため未確認。`docs/dt295-station-logos.md` に実機での確認手順を残した。
カルーセルの形式は DSM-CC の規格と参照実装で確認した（12 バイトヘッダ、
adaptationLength は 9 バイト目、DII 本体は downloadId から）。

## Risks

- 実機が今この個体に接続されていない（PX-S1UD の地上波構成で稼働中）ため、
  実機確認は未実施。復旧手順はモックでの検証にとどまる。
- 929 のカルーセルのバイト配置は参照実装の読みで決めている。実データ 1 本での
  確認が望ましい（DT295 再接続時に `pix-ts-legacy ... -t 120 > 929.ts` を残す）。
- パッチはベンダー固定コミットに依存する。上流を上げるときは当て直しが要る。

## Retirement

- 復旧が効くようになっても `pix-dt295-reset`（unbind/bind）は残す。
  ソフトリセットで戻らない個体（0xFFFF のまま）の最後の手段として使う。
- `pix-stream-simple` の自動 unbind/bind は「ソフトリセット失敗時のみ」に縮退させる。
