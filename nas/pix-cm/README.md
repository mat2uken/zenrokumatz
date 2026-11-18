# NAS 側: 局ロゴから CM マーカーを作る

STB の [docs/dt295-station-logos.md](../../docs/dt295-station-logos.md) が配る局ロゴ
(`GET /v1/logos`) を、NAS の CM カット (`pix-cm`) が使うための部分。

NAS 側のコードはバージョン管理の外 (`~/work/cm-general-build` → `releases/`) に
あるので、追跡できるようここに写しを置いている。動いているのは NAS 上のもの。

## 何をするか

学習 (`cm-scan`) は四隅からマークを探すが、短い録画やマークが隠れている番組では
失敗する。局ロゴには形が入っているので、残るのは「画面のどこに出るか」だけ。

1. `GET /v1/logos` から対象サービスの PNG を取る
2. ffmpeg で RGBA に開き、透けている部分をマスクにする
   （完全透明・単色のロゴは「無い」とみなして諦める）
3. 録画の最初の数分からフレームを取り、四隅の帯でマスク付き正規化相関を回す
   （1440x1080 のアナモルフィックも含め、横縦の倍率を変えて探す）
4. 同じ位置が半分以上のフレームで出て、スコアが 0.55 以上なら採用
5. その位置で実フレームを切り出し、学習と同じ形の PGM 参照と
   `markers/<service>_<WxH>.json` を書く（`"source": "logo"` が目印）
6. 失敗しても例外にしない。`logo_seed_skipped` をログに残して学習に任せる

マーカーキャッシュが既にあるときは何もしない（学習済みのものを尊重する）。
判定ルールには触れていない。

## ファイル

| ファイル | 説明 |
|---|---|
| `cm_logos.py` | 取得・PNG 展開・相関・マーカー生成 |
| `test_cm_logos.py` | 相関の総当たり一致、実寸の探索、空ロゴの拒否、マーカー出力 |
| `cm_service-logo.patch` | `cm_service.py` への差分（`seed_marker_from_logo` と `measure()` からの呼び出し） |

## 入れる

```sh
# NAS 上で
cd ~/work/cm-general-build
cp cm_logos.py test_cm_logos.py .
patch -p1 < cm_service-logo.patch        # まだ当てていなければ
python3 -B test_cm_logos.py
python3 -B test_cm_rules.py

python3 -B assemble_release.py . /home/ku/pix-cm/releases/<新しい名前>
# config.json の bundle と ~/pix-cm/current を新しいリリースへ
sudo systemctl restart pix-cm-worker.service pix-cm-worker@{1,2,3}.service
```

`config.json`:

```json
"logo_source": {"enabled": true, "urls": ["http://192.168.99.109:40774"], "timeout": 5}
```

`urls` は STB の stb_streamer。`enabled: false` で従来どおり。

## 戻す

`config.json` の `bundle` と `~/pix-cm/current` を前のリリースへ戻し、
`pix-cm-worker*` を再起動する。新しいマーカーは `"source": "logo"` で見分けられ、
消せば次の録画で学習し直す。

## 実測

- 2026-11-18: BS11 の録画 (1440x1080) に対し、学習済みマーカーを代用ロゴとして
  通したところ `roi [1312,23,96,56]`（学習値 `[1312,24,96,56]`）をスコア 0.94 で再現。
- テレビ静岡の CDT ロゴは完全透明のプレースホルダだったため、
  `logo_seed_skipped / logo is blank` で学習に任せる（誤ったマーカーは作らない）。

## 注意

- numpy は `cm_logos` の中でだけ import する（無効時は読み込まない）。
- ロゴの取得に失敗しても録画処理は止めない（`logo_seed_failed` を残すだけ）。
- ffmpeg は `config['ffmpeg']` を使う。
