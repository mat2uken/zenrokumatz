# 局ロゴの受信と CM カットへの利用

2026-11-18。PIX-DT295 で BS/CS の局ロゴを受信し、NAS の CM カットが使える形で
置く仕組み。実装の根拠は [smpn2/pix-dt295](https://github.com/smpn2/pix-dt295) の
`dt295 logo`（MIT）。

## 何を取るのか

BS / CS の局ロゴは各チャンネルの放送には入っていない。BS-15 のエンジニアリング
サービス（サービス 929、TSID 0x40F1、IF 1318 MHz）が全局分をまとめて送る。
送出は 20 分ほどおきに 1 分弱で、毎回全局分とは限らず、まったく送らない時期もある。
地デジは各局の TS の CDT (Common Data Table, PID 0x29) に入っている。こちらは
録画中の TS にそのまま乗っているので、チューナーを止めずに拾える（後述）。

局ロゴは PNG で、サービスの三つ組 (original_network_id, transport_stream_id,
service_id) と一緒に届く。NAS の CM カットは `service.id`
(= network_id * 100000 + service_id、BS11 なら 400211) で局を引くので、
ファイル名をその ID にしておけばそのまま対応する。

## 地上波: 録画しながら拾う（チューナーを止めない）

地上波の CDT は各チャンネルの TS に乗っているので、パイプラインの `eit_watch`
に `--logo-dir` を渡すだけで、録画を止めずに局ロゴがたまる。

```sh
siano-ts ... | torne-b25 ... | eit_watch --state /dev/shm/pix/program.txt \
    --logo-dir /var/lib/pix/logos | ffmpeg ...
```

- CDT は 1 周に 10 分ほどかかる（1 つの型が約 100 秒おき）。数分で 1 つめ、
  10 分ほどで型がそろう。そろった時点で `<id>.png` と `index.json` を書き、
  以後は内容が変わったときだけ書き直す。
- どのロゴがどのサービスかは SDT の `logo_transmission_descriptor` (0xCF) が示す。
- 実測（2026-11-18、テレビ静岡のマルチプレクス）: type 1 と type 5 が届き、
  index.json に 3 サービス分が載った。ただし**この局のロゴは中身が空**
  （完全透明のプレースホルダ）だった。局によってはロゴを送出していないので、
  NAS 側は空のロゴを「無い」として扱う（後述）。

ファイルから試すこともできる。

```sh
pix-dt295-logo --cdt --out /tmp/logos --wait 600 < ch17.ts
```

## 自動取得（BS/CS はチューナーが要る）

### 取得経路は 2 つある

| 機体の `PIX_TUNER` | 取得方法 |
| --- | --- |
| `dt295`（純正ファーム） | `pix-ts-legacy` がチューナー内で複合して TS を出す |
| `dt295-cfw` | `pix-cfw-grab tune BS15/0x40f1` で選局し、生 TS をそのまま解析する |

CFW 経路は 2026-11-23 に足した。`tune` は選局結果を標準出力へ書く。
パイプへ流す前に捨てること（混ざると TS の 188 バイト同期がずれ、
パーサは PAT を 1 つも読めなくなる）。

### 週 1 回の自動取得

`S98pix-logo` が常駐し、既定では**月曜 04:00 (JST)** に
`pix-logo-fetch --force` を 1 回だけ実行する。

- 設定は `/etc/default/pix-stream-linux` の `PIX_LOGO_WEEKLY*`
  (`PIX_LOGO_WEEKLY=0` で無効、`_DAY` は 1=月 .. 7=日、`_HOUR` は 0-23)。
- 端末に RTC が無いので、時計が 2025 年より前のあいだは何もしない。
- 同じ週に 2 度止めないよう、成否を `$PIX_LOGO_DIR/.weekly` に残す。
- 取得は最大 `PIX_LOGO_WEEKLY_WAIT`（既定 900 秒）待つ。終われば待たずに戻る。
- テレビ静岡のように録画が地上波の個体では `pix-logo-fetch` が前提違いで
  すぐ終わる（ログに残るだけ）。

### 毎晩の再試行（2026-11-24 追加）

929 はカルーセルを宣言するが、モジュールを送っていない時間帯がある。2026-11-23 は
1200 秒の取得を 2 回行っても DII が 1 つも届かず、20 分の窓では捕まえられなかった。
送信再開を待つため、NAS 側で毎日 04:10 に再試行する。

| 項目 | 内容 |
| --- | --- |
| スクリプト | `scripts/logo-retry.sh`（NAS では `/home/ku/pix-cm/logo-retry.sh`） |
| タイマー | `logo-retry.timer`（毎日 04:10、`Persistent=true`） |
| 動作 | PNG が 1 枚でもあれば何もしない。無ければ `--wait 3600` で取得 |
| 上限 | 連続 14 回で停止し、人の判断を待つ |

深夜枠なので、失敗しても録画への影響は 60 分程度に収まる。60 分あれば 20 分周期の
送信を 3 周ぶん観測できる。

ログにはパーサの判定（`logo modules X/Y in N carousel(s)`）を残す。Y が 0 なら
DII が 1 つも届いていない（送信なし）、X<Y なら届いたが不完全、と切り分けられる。

## 使い方

BS/CS のロゴを取りに行くにはチューナーを借りる必要がある。録画中のパイプラインを止めるので、
明示的に `--force` を付けたときだけ動く。

```sh
pix-logo-fetch --dry-run            # 止める・録る・戻す内容だけ表示
pix-logo-fetch --force              # 最大 180 秒待って取得し、パイプラインを再開
pix-logo-fetch --force --wait 600   # 送出の間隔を考えて長めに待つ
```

- ロゴがそろえば待たずに終わる（実測ではなく設計値: 1 回の送出は 1 分弱）。
- 取れなければ終了コード 3。日を置いて再実行する。ロゴはめったに変わらない。
- 録画が途切れるのは停止から再開まで。`/tmp/pix-logo.log` に経過が残る。
- 手動実行と週次の自動実行はロック (`/var/run/pix-logo-fetch.lock`) で直列化する。
  同時に走ると片方が「録画を止めずに」あきらめる。
- 実行中に中断 (SIGTERM/SIGINT) されても、パイプラインを必ず再開してから終わる。
  録り取りは専用のプロセスグループで動かしているので、待ち時間を待たずに止まる。

抽出だけを手で回すこともできる（チューナーが空いているとき）。

```sh
pix-ts-legacy --model dt295 --assets /usr/share/pix-tuner \
    -s bs -c 15 --tsid 0x40f1 -t 300 \
  | pix-dt295-logo --out /var/lib/pix/logos --wait 300
```

## 出力

```
/var/lib/pix/logos/
  400211.png          サービス ID ごとの PNG（放送そのままのバイト列）
  400212.png
  index.json
```

`index.json`:

```json
{
  "schema": "pix-dt295-logo/1",
  "collected_at": "2026-11-18T04:12:33+0900",
  "source": {"system": "bs", "if_hz": 0, "tsid": 16625, "sid": 929},
  "logos": [
    {"id": 400211, "network_id": 4, "transport_stream_id": 16625,
     "service_id": 211, "name": "ＢＳ１１イレブン", "file": "400211.png",
     "bytes": 1234, "width": 96, "height": 56, "logo_type": 5, "logo_id": 1}
  ]
}
```

- `width` / `height` は PNG の IHDR の値。画面上の大きさは受信側の解像度に比例する
  （NAS の既存マーカーは 1440x1080 で roi 96x56 を学習している）。
- `logo_type` は 5 → 2 → 4 → 1 → 3 → 0 の順に好ましい（HD 16:9 の大が最良）。
  同じサービスに複数の型が届いたときは、この順で 1 つだけ書く。
- サイズが 93 バイト以下の PNG は透明のダミーとして捨てる（TVTest と同じ扱い）。
- 内容が変わったときだけ書き換える。何度実行しても同じ結果になる。

`stb_streamer` はこのディレクトリを配る。

```
GET /v1/logos             index.json をそのまま返す
GET /v1/logos/400211.png  PNG を返す（名前は英数と - _ のみ）
```

## CM カットでの使い方

NAS 側 (`pix-cm`) は録画ごとに四隅から常時表示マークを学習し、
`markers/<service>_<WxH>.json` に `roi` と参照 PGM をキャッシュしている。
局ロゴがあると、次の 3 つが学習に頼らずに決まる。

1. **形**: 透けている部分を除いた形が分かるので、相関はロゴの画素だけで取れる。
   背景が動いてもスコアが落ちにくい。
2. **位置**: 四隅のどこに、どの大きさで出るかをテンプレート一致で決められる。
   短い録画や被覆の少ない番組でも学習が要らない。
3. **有無**: 「ロゴが出ているか」がそのまま CM の手がかりになる。局によっては
   CM 中にロゴを消すため、消えた区間が CM の候補になる。

### NAS 側（実装済み、2026-11-18）

`pix-cm` に `cm_logos.py` を置き、`cm_service.py` の `measure()` から、マーカー
キャッシュが無いときに `seed_marker_from_logo()` を呼ぶ。

1. `GET /v1/logos` から対象サービスの PNG を取る（`logo_source.urls`）。
2. PNG を ffmpeg で RGBA に開き、透けている部分をマスクにする。完全透明・
   単色のロゴは「無い」として諦める。
3. 録画の最初の 5 分から数フレーム取り、四隅の帯でマスク付き正規化相関を
   回して位置を決める（アナモルフィックな 1440x1080 も含めて横縦の倍率を変える）。
4. 同じ位置が半分以上のフレームで出て、スコアが 0.55 以上なら採用。
   その位置で実際のフレームを切り出し、学習と同じ形の PGM 参照と
   `markers/<service>_<WxH>.json` を書く（`"source": "logo"` が目印）。
5. 失敗しても例外は投げない。ログに `logo_seed_skipped` を残して、従来どおり
   学習に任せる。

実測: BS11 の録画 (1440x1080) と、学習済みマーカーを代用のロゴにして通した
ところ `roi [1312,23,96,56]`（学習値は `[1312,24,96,56]`）を 0.94 のスコアで
再現した。放送から取ったロゴでの確認は、DT295 の個体を BS に繋いだときに
行う（下の「実機での確認手順」）。

設定（`/home/ku/pix-cm/config.json`）:

```json
"logo_source": {"enabled": true, "urls": ["http://192.168.99.109:40774"], "timeout": 5}
```

`enabled: false` にすれば従来どおり。コードは
`experiments/cm-generic`（dtv_android）に置き、`assemble_release.py` で
`releases/20261118-logo-03` を作って `config.bundle` と `current` を差し替える。
NAS の `~/work/cm-general-build` は 619 行の古い版なので配備元にしない
（この取り違えで 2026-11-18 に一度、15 本を CM カット無しで保存した）。
組み立ては「直前のリリースのファイル + 変更したモジュール」で行う。
戻すときは `config.bundle` と `current` を前のリリースに戻して
`systemctl restart pix-cm-worker*`。

## 仕組み

- `package/pix-tuner/logo/ts_section.c`: TS パケットからセクションを組み立てる。
  pointer_field、adaptation、0xFF の詰め物、CRC-32/MPEG-2 を扱う。
- `package/pix-tuner/logo/logo_carousel.c`: PAT → PMT（stream_type 0x0D、
  component_tag 0x79 が BS、0x7A が CS）→ DSM-CC の DII (0x3B) / DDB (0x3C) →
  モジュール (LOGO-0x / CS_LOGO-0x) → ロゴ本体（logo_type、サービス三つ組、PNG）。
  SDT からサービス名も取る。
- `package/pix-tuner/logo/pix_dt295_logo.c`: 標準入力の TS を読み、そろったら
  書き出して終わる。
- 上限は固定長: モジュール 256 KiB、4096 ブロック、サービス 1024、ロゴ 256 件。
  ヒープはモジュール本体とロゴの出力表だけ。

## 放送ロゴの中身（2026-11-18 実測）

DT295 個体 (192.168.99.123) で実際に取得した結果。

- 929 から **153 サービス分**のロゴが入った（`/var/lib/pix/logos/index.json`）。
  BS11 は `400211`, 48x27, `logo_type 2`, 492 バイト。
- ただし中身は **パレット (PLTE) の無い 8bit パレット PNG**（153 件すべて）。
  通常のデコーダでは透明/黒になる。地デジの CDT も同じ作りだった。
- インデックスを自前で展開してグレースケールで描くと、**「BS11」の文字**が
  はっきり出る。データは本物の局ロゴで、欠けているのは色（パレット）だけ。
- 画素の相関では 0.45 前後しか出ず、そのままでは使えなかった（放送ロゴは
  線画、画面上のマークは塗りで、余白も違う）。そこで**輪郭（シルエット）で
  合わせる**方式に作り替えた: マークの外形を四隅×サイズで走査し、切り出し
  領域の塗りシルエットとの IoU で候補を選ぶ（0.33 未満は棄却）。
- 実測: 学習済みマーカー `[1312, 24, 96, 56]` に対し
  **`roi [1312, 27, 94, 49]`・形の一致 0.506** でシードできた。
  賑やかな背景の誤検出は 0.28〜0.31 で棄却される。
- 配備は NAS のリリース `20261118-logo-04`。`logo_seed` が発火し、
  マーカーの無いサービスでも最初の録画から判定に使える。

改善するなら「シルエット＋位置探索」に作り替えるか、パレットが送出される
ようになるのを待つ。どちらも今すぐの利益は無いので、コードは残しつつ
（空ロゴ・不一致は学習へ戻る）運用は変えない。

## 制約と未確認のこと

- **実データでの確認はまだ**。今この個体には DT295 が挿さっておらず（PX-S1UD の
  地上波構成で稼働中）、929 の実送信を録れていない。カルーセルのバイト配置は
  DSM-CC の規格（12 バイトヘッダ、adaptationLength は 9 バイト目）と参照実装で
  突き合わせたもので、合成 TS のテストは通っている。
- 放送から取ったロゴでの NAS 側の確認はまだ。今の個体は地上波 (PX-S1UD) で、
  その局のロゴは空だった。BS の個体を繋いだときに、BS11 のロゴで
  `logo_seed` のログとマーカーの `roi` を確かめる。
- 地上波の CDT は取れるが、局によっては中身が空。空のロゴは NAS 側で捨てる。
- `pix-logo-fetch` は録画を止める。止めたくない場合は、パイプラインの切れ目
  （30 分ごとの再起動）に合わせるなど、運用で工夫が要る。
- 2 台目の個体で取ったロゴを 1 台目に配る、といった共有はまだ無い。

## ホストテスト

実機が無くても、デバイスを模擬して復旧の手順と、合成 TS からのロゴ抽出を
検証できる。ベンダー固定コミットにパッチを当てたコピーを作り直してから走る。

```sh
scripts/test-dt295-host.sh            # 復旧と局ロゴの両方
scripts/test-dt295-host.sh recovery   # dt295_boot の状態遷移 (モックのレジスタ)
scripts/test-dt295-host.sh logo       # カルーセル → PNG → index.json
```

`tests/test_dt295_recovery.c` は状態 0x11 / 0xFFFF / 0x02 からの復旧、
ACK を返さないファームウェアの打ち切り、keepalive の周期と再試行を見る。
`tests/test_dt295_logo.c` は PAT → PMT → DII → DDB → モジュール結合 → PNG を
通し、モジュールの版が変わったときのやり直しと、透明ダミーだけのときの
終了コード 3 も見る。

## 実機での確認手順

DT295 を挿した状態で:

```sh
# 1. まず抽出だけ (録画を止めずに済む時間帯に)
pix-logo-fetch --dry-run
pix-logo-fetch --force --wait 900
echo $?                      # 0 なら取得、3 なら今回は送出が無かった
ls -l /var/lib/pix/logos/
cat /var/lib/pix/logos/index.json

# 2. 配信
curl -s http://127.0.0.1:40774/v1/logos | head
curl -s http://127.0.0.1:40774/v1/logos/400211.png -o /tmp/400211.png
file /tmp/400211.png         # PNG image data, 96 x 56 (例)

# 3. 復旧の確認 (録画を止めたくない時間帯に)
#    わざと固めた状態から、物理的に抜き差しせずに立ち上がるか
kill -9 $(pidof pix-ts-legacy); sleep 2
cat /dev/shm/pix/dt295.state      # 2 などに残っていれば狙いどおり
pix-ts-legacy --model dt295 --assets /usr/share/pix-tuner \
    -s bs -c 9 --slot 0 -t 5 -o /dev/null
grep dt295_recover /tmp/pix-ts.log   # {"event":"dt295_recover","from":"0x0002"} など
```

`dt295_recover` が出て 5 秒の取得が終われば、ソフトリセットで復旧している。
状態別の目印:

- `0x0002`（ストリーム中）: `dt295_recover_stop` が出て、pause → 排出 →
  state 1 → idle を通る。実機でコマンド3 だけでは戻らなかったための経路。
- `0x0011`: idle イメージの読み直し。
- `0xffff`: 残りの TS を読み捨ててからコマンド3。
出ずに「firmware could not be reset」で失敗するなら、その固体はまだ
`pix-dt295-reset`（unbind/bind）が要る。

## 元に戻す

`/var/lib/pix/logos` を消し、`pix-stream-linux` の `--logo-dir` を外せば
エンドポイントは 404 を返すだけになる。録画経路には一切触れていない。
