# 3代目（unit3 / 192.168.99.56）の現状

2026-11-19 の構築セッションの **round 214 時点**の要約。出典は2ファイル。

[70-handoff.md](aegis/work/2026-11-19-unit3-init/70-handoff.md) は round 211 時点の引き継ぎ。到達点は round 208〜214 の追記を含む。

[50-usb-phy-findings.md](aegis/work/2026-11-19-unit3-init/50-usb-phy-findings.md) の末尾は round 211〜214。

両者は Aegis の過程記録であり助言である。現行の運用手順は [ram-rootfs.md](ram-rootfs.md) を参照。ここに書かれていないことは「未確認」とする。

## 動作したこと（実機で確認済み）

### 起動・ネットワーク・チューナー

**Linux RAM 起動。** 電源投入から約45秒で login。U-Boot env に `bootcmd=run linux_boot_ram`（saveenv 済み）を焼き込む。同 env は SM1 DTB、`usb start` リトライ、`panic_on_oops=1 panic=8` を含む。出典は round 211/212。

**Ethernet 1Gbps / SSH。** 達成。DHCP で `192.168.99.56` を取得。出典は handoff 到達点、round 211。

**DT295 認識。** `06b8:103b` として列挙。出典は handoff 到達点、round 211。

### エンコードと PSI

**VPU H.264 エンコード。** 機能として達成。64x64／1080p 6Mbps で RC=0。完了割り込みも `HCODEC_CCPU_INTR_MSK=1` で動作。配信モジュールは `amvenc_avc.ko` = `9939bedb`。パッチ 0001〜0019。`insmod` は依存解決で失敗するため `modprobe` を使う。出典は handoff 到達点・配信中のモジュール。

**PSI 取得／PID 特定。** `have_pat:1`、`tables_ready:1`。`PIX_PIDS="0 17 18 257 273 274"` を設定。映像 273、音声 274。出典は handoff 到達点・PID 一覧、round 208/209。

**実映像の A/V エンコード。** 達成。round 208 は ch17。round 210 は 29fps／speed 0.924x。出典は handoff 到達点、round 208/210。

### 受信・録画・制御

**ch15（SBS）受信。** 正常。round 205 の「受信不良」は誤診（マルチプログラム mux の誤ストリーム選択）。round 210 で訂正。アンテナ対応は不要と判断。出典は round 205/210。

**NAS 録画。** `192.168.99.56-15` が `recording`。実時間 throughput、dropped 0。旧 `-17` は settings API の channel 変更で自動改称。出典は handoff NAS 登録、round 209/210。

**NAS からの制御。** 合格。`stop` → stopped、`start` → recording。settings で 15↔17↔15 の往復。STB が実際に再選局する。出典は round 211/212。

### 番組締め・CM カット

**番組締め処理・CM カット。** 完走。最初の番組はみなスポ。延長で実境界は 17:31。`p01.mp4` は 1,528,359,146 B。53,878 フレーム／1797.7 秒。CM 解析の後、CMカット mp4 は 1,422,625,084 B。122.1 秒カット。cuts=[980.98, 1103.10]。hash mismatch 0、`verified: true`。STB の feature sideband（signalstats／astats）を CM 解析に使用。出典は round 213。

### CFW 移行と性能

**CFW 構成への移行。** 達成。2代目と同じ `PIX_TUNER=dt295-cfw` + torne-b25。rootfs `bf22ebd5` を SD 配備。S97 が `frc1_load` で CFW を自動インストール。pcscd → bcasd → cardrelay → torne-b25 で複合。録画は実時間、dropped 0。3分間ストール／再選局ゼロ。内容検証で動画 111 フレーム／3.7 秒（30fps）。音声デコードエラーゼロ。出典は round 214。

**性能 11fps。** 解決。debug ブートの `encode_print_level=0` によるシリアル同期 printk が原因。本番 rootfs の既定ログレベルでは発生しない。round 210 の A/B で 80fps 等を実測。出典は round 210、handoff 判定。

## 未解決・運用上の注意

- **ソフトリブートは使わない。** エンコード動作中の `reboot` でハングする（round 212 で2回再現）。シリアル／ネットワーク沈黙。BL2 バナーも出ない。1回目は約7分後に自発復帰。2回目は15分以上復帰せず物理電源で回復。再起動は電源オフ／オンが正規手順。ハング時の復旧は物理電源のみ（guard の adb reboot は Linux RAM では不可）。S97pix-stream を停止してからならソフトリブートは通った（round 214）。
- **CFW の揮発性。** 純正経路（pix-ts-legacy）を動かすと hook が戻る。S97 が起動時に `pix-cfw-grab probe` で FR01 シグネチャを確認する。無ければ `frc1_load` する。通常の再起動で自動的に入れ直る（round 214 実測）。
- **番組境界。** EIT の実切替で検出する。中継延長では予定時刻を過ぎても実際の境界で分割される（round 213）。
- **日常範囲の警告。** round 213 の CM 検証で音声に `decode_band_types` 警告が1件発生。キャプチャ先頭の中途 PES 由来、2代目と同じ。`audio_decode_warning: true` で記録される。staging の 0 バイト残骸は蓄積しても無害とされる。

## 他機体との差分

### SoC／DTB

3代目は **SM1（S905X3）**。`aml_dt=sm1_ac213/4g`、4 GiB、RTL8211F。DTB は `meson-g12a-x96-max-p-sm1.dtb`。vdec は `amlogic,sm1-vdec`。1代目は G12A／2 GiB／内蔵 PHY で `meson-g12a-x96-max-p.dtb`。2代目は G12A／4 GiB／ePHY で `meson-g12a-x96-max-p-ephy-4g.dtb`。round 204 で 2代目が G12A であることを確認。

### カーネル／バイナリ

3代目は Linux 6.18.62。`pix-ts-legacy` の SHA-256 は `6df432fe`。2代目と同一（round 204）。版差ではない。

### 起動媒体

3代目は SD（U-Boot mmc0:1）。2代目は USB メモリ（round 204）。3代目も `usb start` が必須。round 137 の知見を U-Boot env のリトライに反映。2代目の実績レシピと同じ。

### チューナー／チャンネル

3代目は DT295 CFW。地上波 SBS **ch15**（service 35856）。NAS ノードは `192.168.99.56-15`。1代目は PX-S1UD・テレビ静岡 ch17（`192.168.99.109-17`）。2代目は DT295 CFW・BS11（`192.168.99.123-BS09_0`）。`192.168.99.209-15` は disabled（handoff NAS 登録）。

### 複合設定

3代目は `PIX_TUNER=dt295-cfw`。`PIX_PIDS` は設定しない。`PIX_CHANNEL=15`。2代目と同じ構成（round 214）。3代目は以前 `PIX_PIDS="0 17 18 257 273 274"` を使っていた。CFW 移行で削除。

### 決定的だった修正

U-Boot の `usb start`。SM1 DTB。`hantro` 無効化。`ISO0` の BIT(1) クリア（パッチ0010）。`DOS_SW_RESET0` リセットパルス（0014）。`HCODEC_CCPU_INTR_MSK=1`（0018）。`PIX_CHANNEL` 17→15。2代目は G12A 経路のため SM1 用 DTB／パッチを必要としない。

## 未確認

- 長期運用の安定性は round 214 時点で観察項目として挙がっているのみ。結果は未確認。観察項目は SD カードの耐久、NAS 側ディスク容量、torne-b25 のカードセッションの長期安定性。
- 本要約の出典は round 211〜214 の記録。
- 11/22 の EPG ホスト廃止に伴う STB 3台の再ビルド・焼き直し・再起動が 3代目の現行イメージにどう反映されたかは未確認。
- 11/23 の監視修正が 3代目の現行イメージにどう反映されたかも未確認。
- いずれもこの2ファイルの範囲外。[verification-20261122.md](verification-20261122.md) §8、[stb-stall-20261123.md](stb-stall-20261123.md) を参照。
- ソフトリブートが通る条件は round 214 の「S97 停止後は通った」一例のみ。運用手順として定着しているかは未確認。

## 参照（長いログ）

- 要約の出典（1）: `docs/aegis/work/2026-11-19-unit3-init/70-handoff.md`
- 要約の出典（2）: `docs/aegis/work/2026-11-19-unit3-init/50-usb-phy-findings.md`
- USB PHY 解釈の更新元: `docs/aegis/work/2026-11-19-unit3-init/30-usb-phy-findings.md`
- 実機ログ（`work/` は Git 管理外）: `work/unit3-boot-gxl2.log`（round 37 の xhci 到達ログ）
- 実機ログ: `work/restart-recipe-persist-result.log`（2代目の `usb start` → 録画のレシピ）
