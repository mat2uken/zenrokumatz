# NAS 側プログラムの管理

NAS（sweedmaster02 / 192.168.99.80）で動く録画系プログラムの正規ソースと、その
ビルド・配備手順。

## 正規ソース

| ファイル | 役割 |
| --- | --- |
| `nas/src/pix_stream_recorder.c` | STB から fMP4 を受け取り、番組単位で保存する |
| `nas/src/pix_stream_manager.c` | STB の登録、録画セッションの起動と監視 |
| `nas/src/pix-stb-guard.py` | STB の死活監視と復旧（SSH で S97 を再起動） |
| `nas/src/pix-epg-bridge.py` | STB の `/v1/epg` を mirakc 互換 API で pix-archive に供給する |
| `nas/src/pix_archive.c` | pix-archive の canonical。NAS では `/home/ku/pix-archive-20261029/repo/src/archive/pix-archive.c` に置いて `repo/scripts/build_pix_archive.sh` でビルドする |
| `nas/src/pix-recording-audit.py` | 録画・CM・EPG の監査（NAS では `/home/ku/pix-recording-audit/` に配備） |
| `nas/src/fmp4_to_mp4.c` / `.h` | fMP4 から通常の MP4 への変換 |
| `nas/src/cm_queue.c` / `.h` | CM ワーカーへの fMP4 受け渡しキュー |
| `nas/src/feature_sidecar.c` / `.h` | STB 特徴量 sidecar の読み取り |

**ここが唯一の正である。** NAS の `/home/ku/pix-stream-20261101/*.c` や
`dtv_android/nas_src/` のコピーは更新されない古いスナップショットで、実行中の
バイナリのソースではない。2026-11-20 時点で、NAS の `pix_stream_manager.c` は
11/3 版、`pix_stream_recorder.c` は 11/8 版で、どちらもバイナリより数版古かった。
古いソースを直してビルドすると、新しい機能（`pixn`、`seeded from STB`、
`programme window ended`、`--max-segment-sec`、`/api/time`）が失われる。

## ビルドと配備

```
nas/build.sh            # nas/out にビルドするだけ
nas/build.sh --apply    # NAS に配備してサービスを再起動する
```

NAS には `libcjson-dev` が必要（ヘッダは `/usr/include/cjson/cJSON.h`、
実行時は `libcjson.so.1`）。未導入なら `sudo apt-get install libcjson-dev`。

`--apply` は NAS 上でビルドし直してから入れ替える。コンパイラと libc が実行環境と
同じになるようにするため。旧バイナリは `*.before-<年月日>-<時刻>` として残る。

### 再起動の影響

`--apply` は `pix-stream-manager` を再起動する。manager がレコーダーを所有して
いるので、これで両方のバイナリが入れ替わる。書きかけの区間はそこで切れるが、
レコーダーは同じ番組を次のパートとして同じフォルダに続けて保存するので、番組が
別フォルダに分かれることはない。

## 過去の管理から外れたコピー（参照のみ）

- `dtv_android/nas_src/` — 2026-11 中旬の系統。`cm_queue.c` の `features` 引数など、
  現在の `pix_stream_recorder.c` と組み合わせられない差分がある。参照専用で編集しない。
  `scripts/deploy-cm-features.sh` は 2026-11-23 に C ソースの取得元を
  `X96MaxLinuxTSEnc/nas/src/` へ切り替えた。Python モジュールは引き続き
  `dtv_android/experiments/cm-generic/` を使う。
- NAS の `/home/ku/pix-stream-20261101/*.c` — 参考のために置かれているだけ。
  編集しない。
