# nas_src は現在の正規ソースではない

このディレクトリは 2026-11 中旬の NAS 側プログラム（CM 特徴量オフロード作業で
使った系統）のコピーである。**現在動いている NAS の録画系プログラムのソースは
ここではない。**

## 正規ソース

`X96MaxLinuxTSEnc/nas/src/` を見ること。

- `pix_stream_recorder.c` — 実行中のバイナリ（2026-11-17 以降）に対応するのは
  こちらの系統。`pixn`・`seeded from STB`・`programme window ended`・同じ番組を
  複数パートにまとめる処理を含む。
- `pix_stream_manager.c` — `--max-segment-sec` と `/api/time` を含む版。
- `pix-stb-guard.py` — SSH 経由の STB 復旧（`restart_stream`）を含む版。

このディレクトリの `pix_stream_recorder.c` は、番組境界の扱いが現在の実装と
異なる系統で、そのままビルドして配備すると録画の挙動が変わる。

## このディレクトリとの差分を統合するとき

`cm_queue.c` / `.h` の `features` 引数と `feature_sidecar.c` / `.h` は、
`X96MaxLinuxTSEnc/nas/src/` にまだ取り込まれていない。CM ワーカーへの特徴量
受け渡しを再開するときは、`nas/src/pix_stream_recorder.c` にこの差分を移す。
`scripts/deploy-cm-features.sh` は今もこのディレクトリからビルドするので、
その際に合わせて直すこと。
