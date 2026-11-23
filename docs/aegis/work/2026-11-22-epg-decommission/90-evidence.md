# EPG ホスト廃止 - Evidence（2026-11-22 09:15）

## 収録基準との対応

| 基準 | エビデンス |
| --- | --- |
| 1. カタログ新鮮 | sqlite airings: 3 局（400211 / 3219535864 / 3219435856）2128 行、max startAt 2026-11-29 23:45。`/api/archive/programs` で現在〜未来の番組を名前付きで返す（本日 ±1h で 13 件確認） |
| 2. フォールバック命名 | `/api/archive/services`（channel/type/id/name）と `/api/archive/programs` が mirakc 無しで応答。manager の channelName が 3 局を新ソースから解決。再起動後の録画フォルダ命名が正しい。tests: test_epg_bridge.py 5 本 + test_eit_sections.py 4 本 |
| 3. epg_stale 消滅 | audit.log: 最終 epg_stale = 2026-11-22 08:48（105.0h）。08:59 と 09:03/09:08 の audit_summary に警告なし |
| 4. 参照ゼロ | 対象範囲（/etc/systemd/system、/var/spool/cron、/etc/cron*、/home/ku/pix-cm、/home/ku/pix-stream-20261101）のテキスト + バイナリ grep で `.40` / mirakc / adb 一致ゼロ |
| 5. 録画継続 | manager /api/nodes: 3 ノード recording、dropped 0。unit1 再起動で part 継続（pushed 回復）を確認。フォルダ名（BS_400211_ＢＳ１１イレブン / GR_3219535864_テレビ静岡 / GR_3219435856_ＳＢＳ１） |
| 6. tests / docs / メモ | tests 288 件 OK（274 + 追加 14。RAMBOOT_UNIT=2 + 11-22 build の CPIO で実行）。docs: epg-decommission-design.md / verification-20261122.md §8 / build-on-nas.md / ram-rootfs.md / nas/README.md。OpenViking メモ記録済み |

## 配置の変更点（正規ソース）

| 種別 | パス |
| --- | --- |
| STB C | package/stb-streamer/src/eit_watch.c（schedule 蓄積 + epg.json）、stb_http.c（/v1/epg）、stb_linux_pipe.c（--epg-file 既定 /dev/shm/pix/epg.json） |
| NAS py | nas/src/pix-epg-bridge.py（新）、pix-stb-guard.py（adb 除去）、pix-recording-audit.py（canonical 新設、pix-stb-health 結合除去） |
| NAS C | nas/src/pix_archive.c（canonical 新設、--epg 改名） |
| ビルド | board/x96max/ramboot-post-image.sh（DTB 一覧を .config から導出） |
| tools | tools/flash-ramboot-over-ssh.sh（新、ram-rootfs.md の手順を実装） |
| tests | tests/test_eit_sections.py（+4）、test_epg_bridge.py（新 10）、test_ramboot_profiles.py（契約更新） |
| docs | epg-decommission-design.md、verification-20261122.md §8、build-on-nas.md、ram-rootfs.md、nas/README.md |

## NAS 上の配置

- /home/ku/pix-epg-bridge/pix-epg-bridge.py + systemd unit（pix-epg-bridge.service、127.0.0.1:40775）
- /home/ku/pix-archive-20261029/repo/{src/archive/pix-archive.c,bin/pix-archive}（--epg で再ビルド、unit は --epg http://127.0.0.1:40775）
- /home/ku/pix-stream-20261101/pix-stb-guard.py（adb 除去版、self-test OK）
- /home/ku/pix-recording-audit/pix-recording-audit.py（pix-stb-health 結合除去版）

## ユーザー承認で実施した破壊的操作（2026-11-22 08:2x 承認）

- STB 再起動 ×3（unit1 は 2 回、unit2/unit3 は 1 回）
- pix-epg-window timer/service 無効化 + epg_window.py 削除
- pix-stb-health timer/service 無効化 + /home/ku/retired-epg-host-20261122/pix-stb-health へ退避
- 旧コピー削除: pix-stream-20261101 の guard .bak 8 本・nodes.json.bak・stale-sources/pix-stb-guard.py・死んだノードの last_seq_*、pix-archive-20261029 の .bak/candidate/staging/miraview/旧バイナリ/tar.gz、pix-archive-control-20261030-stage、pix-record-control-tests-20261029、pix-ts-commit-validation-20261029、pix-ts-tests-20261029
- guard 状態 JSON から .40 ノードの古いキーを除去
