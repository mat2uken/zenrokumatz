# 2代目: 全段が動作した状態（2026-11-21 完成）

CFW の生 TS を、**DT295 の内蔵 B-CAS カード**で複合し、H.264 の fMP4 として
NAS へ配信するところまで通った。録画の全経路が動いている。

## 構成

```
DT295（CFW 差し替え）
  │ 生 TS（複合前、4.87 MB/s、スクランブル 82%）
  ▼
pix-cfw-ts            ts_grab を回し続ける常駐の口（止まったら選局し直す）
  ▼
torne-b25             ARIB MULTI2 の複合（内蔵カード）
  ▼
pix-ring              32 MB のリングで下流の詰まりを吸収
  ▼
eit_watch             番組名・境界（program.txt）
  ▼
ffmpeg ×2             MPEG-2 → H.264、fMP4 へ
  ▼
stb_streamer          /v1/streams/bs-9 で NAS へ
```

## 実測

| 項目 | 結果 |
|---|---|
| クライアントが読んだ量 | **210 MB**（1 回の接続で） |
| 形式 | fMP4、**h264 1440x1080 + aac** |
| 番組情報 | `service=101 event=17436 duration=3600000` |
| `totalDropped` | 読者がいない間だけ増え、繋ぐと止まる（設計どおり） |
| 切り直し | 5 分間で **0 回** |
| カード初期化 | 連続 3 回すべて成功（`sw=2100`） |

## 完成までに直したこと

すべて実測で確認した。推測で直したものは無い。

| # | 症状 | 原因 | 直し方 |
|---|---|---|---|
| 1 | カードが 2 回目以降応答しない | bcasd が接続済みカードに RESET をしていなかった | `SCardReconnect(SCARD_RESET_CARD)` |
| 2 | カードの ATR が交互に失敗 | CFW の初回 `CARD_ON` が無視される（既知の未解決事項） | IFD ハンドラで 2 回まで再試行 |
| 3 | 再起動するとカードが見えない | IFD バンドルを手で置いていた（RAM 起動で消える） | `package/cfw` に取り込んでイメージに同梱 |
| 4 | 取得が `LIBUSB_ERROR_BUSY` | 落とし損ねた `pix-cfw-*` が USB を握っていた | 入れ直す前に必ず落とす（手順化） |
| 5 | 録画に番組名が付かない | バッファ経路に `eit_watch` が無かった | 直結経路と同じ位置に入れた |
| 6 | CFW 経路が選ばれない | S97 が `dt295-cfw` を旧ランチャーに割り当てていた | `pix-stream-simple` に向けた |

## 運用

```sh
# 起動
/etc/init.d/S96bcas restart      # pcscd → bcasd → cardrelay
/etc/init.d/S97pix-stream start  # 取得 → 複合 → 配信

# 状態
curl http://127.0.0.1:40774/v1/status
curl http://127.0.0.1:40774/v1/program
```

CFW を入れ直すときは、先に S97 と S98 を止め、USB を電源切り直してから
`frc1_load install /usr/share/cfw/frc1-dt295.frc`。

## 残る注意

- **純正ファームと CFW は共存しない。** `pix-ts-legacy` を動かすと CFW の
  hook が消える。切り替えるときは cold から入れ直す。
- 読者がいないとスプールが 192 MB で頭打ちになり、古い断片から捨てる
  （`totalDropped` が増える）。NAS が読んでいる間は 0 のまま。
- ガード（NAS 側 `pix-stb-guard`）は `enabled=false` のノードを復旧しない。
  CFW で作業するときは管理画面で止めておく。
