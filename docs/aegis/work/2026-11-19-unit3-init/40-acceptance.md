# 3代目の受け入れ基準（2代目と同等の録画を満たす条件）

作成日: 2026-11-19

「2代目と同等」を主観で判定しないため、2代目の実測値と照合して判定する。目標は「Linux 起動 → DT295 で録画 → CM 解析・カット → NAS に正常保存」。

## 2代目の実測ベースライン（2026-11-19 時点、稼働中）

`root@192.168.99.123` から取得。

```json
{"status":"ok","channel":"BS09_0","capturing":true,"readers":1,
 "spoolUsedBytes":0,"spoolMaxBytes":201326592,"fragmentsInSpool":0,
 "totalPushed":30,"totalDropped":0}
```

NAS ノード登録（`http://192.168.99.80:40776/api/nodes`）:

| 項目 | 2代目（X96Max-DT295） |
|---|---|
| チャンネル | `BS09_0`（ＢＳ１１イレブン） |
| codec | `avc` |
| 解像度 | 1440x1080 |
| bitrate | 6,000,000 |
| rateMode | `vbr` |
| gopSec | 2 |
| state | `recording` |

設定（`/etc/default/pix-stream-linux`）:

```
PIX_TUNER=dt295
PIX_DECRYPT=0
PIX_SYSTEM=bs
PIX_CHANNEL=9
PIX_SAT_SLOT=0
PIX_PIDS="0 17 18 257 273 289 320 321"
PIX_PORT=40774
PIX_ALLOWED_IP=192.168.99.80,192.168.99.52
PIX_SPOOL_MB=192
PIX_BITRATE=6000k
PIX_GOP=60
PIX_CAPTURE_SECONDS=1800
```

## 3代目で満たす条件

段階を追って判定する。前段が満たせない限り次段に進まない。

### A. Linux 起動

- `x96max-p3 login:` に到達する
- `cat /proc/cmdline` に `rdinit=/init` があり、`mount` の `/` が `rootfs`（RAM）、`cat /proc/swaps` が空
- カーネル例外・panic が無い
- eth0 が 1Gbps 以上でリンクし、DHCP で `192.168.99.56` を取得する

### B. DT295 受信

- `lsusb` に `06b8:103b Pixela Corp. PIX-DT295` が出る
- `pix-ts-legacy --model dt295 --assets /usr/share/pix-tuner -s terrestrial -c 17 -t 5` が `rc=0`、`locked:1`、`tables_ready:1`、`packets > 0`
- EIT から `service=35864`（SBS）が取れる

### C. 配信

- `wget -qO- http://127.0.0.1:40774/v1/status` が `"capturing":true`
- `totalPushed` が時間とともに増える
- `totalDropped` が 0 のまま
- 映像が 1440x1080、約 6Mbps（`PIX_BITRATE=6000k`）

### D. NAS 保存

- NAS の `/api/nodes` に 3代目（`192.168.99.56`、ch17）が `recording` で現れる
- 録画ファイルが NAS に生成され、サイズが時間に比例して伸びる
- 番組境界で分割される（`PIX_CAPTURE_SECONDS=1800` ではなく番組単位）

### E. CM 解析・カット（2代目と同等）

- NAS 側で CM 解析ジョブが走る
- `_CMカット.mp4` が生成される（例: 2代目の実績 296.3秒 → 178.2秒）
- マーカーキャッシュのキーがサービスIDと一致していること（既知の落とし穴: `400211_1440x1080.json` に対し `211_1440x1080.json` が必要だった例）

### F. 再起動復帰

- 電源再投入だけで Linux → 録画再開まで到達する（`bootcmd` 保存後）
- 媒体（SD）を外しても稼働が継続する

## 障害として扱うもの

- カーネル例外（`Unable to handle kernel`）が出る
- `totalDropped` が増える
- 録画ファイルが生成されない、または NAS で `error` 状態になる

## 既知の前提と注意

- **NAS に `192.168.99.56-17`（X96Max-Plus2、テレビ静岡）という古いノードが残っている。** 3代目が同じ IP を使うため、登録前に削除が必要。放置すると名前・チャンネル割当が衝突する。
- 3代目の設定は意図的に2代目と差分がある（地上波・ch17・衛星オプション無し）。「同等」は画質・安定性・保存の成立で判定し、チャンネルとチューナー系の値は合わせない。
