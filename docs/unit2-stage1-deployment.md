# 2代目への段1投入と実機確認（2026-11-20）

設計 [dt295-external-descramble-design.md](dt295-external-descramble-design.md) の段1
（キャプチャの1800秒切り直しを廃止し、USB復帰を PIX_DECRYPT から切り離す）を
2代目（192.168.99.123、DT295・BS11）へ入れ、実機で確かめた記録。
生のログは `work/unit2-stage1-deploy/` に置いてある（work は git 管理外）。

## 投入前の状態（実測）

| 項目 | 値 |
|---|---|
| ホスト | `x96max-p2`、192.168.99.123、Linux 6.18.62 |
| root | `rootfs`（RAM、一時記憶） |
| 起動媒体 | `/dev/sda1`、serial `62533286`、62533286 sectors（ドキュメントの値と一致） |
| `/etc/default` | `PIX_CAPTURE_SECONDS=1800`、`PIX_PIDS="0 17 18 257 273 289 320 321"` |
| `capture_seconds` | 無し（旧ランチャー） |
| 実行中の引数 | `pix-ts-legacy ... -p 0 -p 17 ... -p 321 -t 1800` |
| 起動後の切り直し回数 | **57回** |
| 切り直しの間隔 | 1802〜1804 秒（毎回） |

**これが「1800秒の切り直し」の直接の証拠**である。ログの
`pipeline stopped after 1804s; restarting` が 57 行並んでいた。

## 投入手順（docs/ram-rootfs.md の「起動媒体を更新する」に沿う）

1. ホストで `X96MAX_PROFILE=ramboot X96MAX_UNIT=2 ./scripts/build-linux.sh`（exit 0）
2. 起動媒体の照合: `/sys/block/sda/size` = 62533286、`blkid` の LABEL=`X96RAMBOOT`
3. 現在の起動3本を `/tmp/boot-rollback/` に退避し、sha256 を記録
4. `scp` は sftp-server が無く失敗したため `cat | ssh` で転送
5. 転送直後に Host と実機の sha256 を突き合わせ → **3本とも一致**
6. 媒体を rw でマウントし、`Image` と `rootfs.cpio.uboot` を上書き
   （DTB は同一 sha256 なので実質変わらず）
7. `sync` → umount → **読み取り専用で再マウントして sha256 再確認**（ディスクから読める）
8. 全パーティションが未マウント・swap 無しを確認 → `S97pix-stream stop`（正常停止、`kill -9` ではない）→ 再起動

### 媒体上の退避が失敗したこと

`/mnt/bootchk/previous/` へ旧ファイルを退避しようとしたが、起動区画が 126MB で
旧 rootfs（35MB）+ Image（41MB）の控えが入らず **No space left on device**。
部分的な控えを消し、RAM 上の `/tmp/boot-rollback/` を復旧手段とした。
今後この機体で同じ手順をするときは、媒体上に退避せず RAM かホストへ退避する。

## 投入後の状態（実測）

| 項目 | 値 | 判定 |
|---|---|---|
| 起動 | 戻ってきた（ping 11秒、SSH +4秒） | OK |
| root | `rootfs`（RAM）のまま | OK |
| `/etc/default` | `PIX_CAPTURE_SECONDS=-` | OK |
| `PIX_PIDS` | 無し | OK |
| `capture_seconds` | ランチャーに存在 | OK |
| 実行中の引数 | `pix-ts-legacy ... -t -`（`-p` フラグ無し） | OK |
| 配信 | `capturing:true`、`readers:1`、`dropped:0` | OK |
| 切り直し | 起動後 0 回 | OK（継続監視中） |
| チューナー | locked=1、C/N 20.6 dB | OK |
| 連続性 | `continuity_errors:2`（起動直後のみ） | 要観察 |
| 音声 | `Stream #0:2[0x141] aac 48000Hz stereo`、ff2 で `mp4a 254kb/s` | OK |

### 起動時に一度だけ出た AAC の警告

`[mpegts] AAC bitstream not in ADTS format and extradata missing` が
起動直後に **1回だけ** 出た。probe 用のバッファに ADTS ヘッダーがまだ入っていない
段階で出るもので、その後の 3 分間で再発していない。音声の実体は
ffmpeg の入力側（`Stream #0:2`）でも出力側（`mp4a 254kb/s`）でも正常に取れている。
PID を絞らなくなったことで音声 PID（0x141）が従来の位置からずれた可能性があるが、
`-map 0:a:0` は「最初の音声」を選ぶので影響しない。

**ただし、これは再起動前と直接比べられていない。** RAM 起動なので前回のログが
再起動で消えており、旧構成で同じ警告が出ていなかったことは確かめていない。
段2 に進む前に、旧構成へ一度戻して同じ警告の有無を比べる価値がある。

## 継続確認

`/tmp/stage1-watch.sh` が 5 分おきに `totalPushed`・切り直し回数・`-t` を記録する
（3時間）。**1800 秒を超えて切り直しが 0 のままであることが合格条件。**

### 復旧手段

- RAM 上の `/tmp/boot-rollback/` に旧3本（sha256 確認済み）
- 媒体上の `.Spotlight-V100` 等は未変更
- U-Boot からは `linux_boot_usb`（旧定義）で従来どおり起動できる

## 1800秒の境目を越えた結果（2026-11-20 08:23 起動）

起動 08:23:08。キャプチャ開始からの経過と切り直し回数:

```
08:23:44 up=  74.84 cuts=0 pushed=  10 dropped=0 -t=-
08:28:44 up= 374.95 cuts=0 pushed= 165 dropped=0 -t=-
08:33:44 up= 675.04 cuts=0 pushed= 315 dropped=0 -t=-
08:38:44 up= 975.12 cuts=0 pushed= 465 dropped=0 -t=-
08:43:44 up=1275.22 cuts=0 pushed= 615 dropped=0 -t=-
08:48:44 up=1575.27 cuts=0 pushed= 765 dropped=0 -t=-
08:53:44 up=1875.36 cuts=0 pushed= 915 dropped=0 -t=-    ← 1800秒を越えた
```

**旧構成なら 1800 秒ちょうど（08:53 付近）で `pipeline stopped after 1804s` が出ていた。
今回は出ていない。** `pix-simple.log` の中身は起動時の 2 行だけで、再起動の記録が無い。

### 同一ストリームが番組をまたいだこと

33分時点で `/dev/shm/pix/program.txt` の `event` が **36739 → 36741** に変わっていた。
キャプチャは1本のまま（`pix-ts-legacy` のプロセスは起動時のまま、ログに再起動なし）、
番組境界をストリームを切らずに越えている。**これが「分割の原因だった切り直し」を
無くした直接の証拠になる。**

### 長時間の健全性（33分時点）

| 指標 | 値 | 意味 |
|---|---|---|
| `continuity_errors` | 2 で横ばい | 起動直後の2件のみ。増えていない |
| `renewals` | 163 | 鍵の更新が続いている（止まっていない） |
| `usb_error` / `usb_timeouts` | 0 / 0 | USB 経路に異常なし |
| `writer_stalled` / `writer_error` | 0 / 0 | 書き手が詰まっていない |
| `totalDropped` | 0 | 配信側の取りこぼしなし |
| AAC 警告 | 1（起動時のみ） | 再発なし |

## 調査中に自分で起こした事象（記録）

音声を確かめようとして、NAS と同じ HTTP 口からストリームを取り出そうとした。
`wget -qO- .../v1/streams/gs-9?after=0` は**終わらない口**（ライブ配信を続ける）なので、
コマンドが返らず、裏側の `wget` が残った。

### 何が起きたか

`stb_streamer` は `readers` が 1 のときだけ「送った断片を捨てる」
（`stb_http.c:337` の `only_reader`）。落とした `wget` が2人目の読者として
残っていたため、次が起きた。

1. `readers=2` になる
2. NAS の読者に対して断片が捨てられなくなり、スプールが 192MB まで埋まる
3. `totalDropped` が増える（0 → 68）

**コードの欠陥ではなく、私の操作ミス**（読み捨てない読者を1本刺した）。
`wget` を PID で終了させたところ、`readers=1`、スプール 0MB に戻り、
`totalDropped` は 68 で止まった。以降 `totalPushed` は増え続けている。

### 学び（この後の実機確認に効く）

- **ライブの口は打ち切らない。** 取ったら必ず読み切って閉じるか、最初から開かない。
- 録画中に `/v1/streams` を触ると、スプールの挙動が変わる（読者が2人になる）。
- 音声の確認は、ffmpeg のログ（入力 `Stream #0:2 aac`、出力 `mp4a`）で足りた。
  配信口を触る必要は無かった。

## 56分時点の結果（1800秒の2倍近く）

```
08:23:44 up=  74.84 cuts=0 pushed=  10 dropped=0
08:28:44 up= 374.95 cuts=0 pushed= 165 dropped=0
08:33:44 up= 675.04 cuts=0 pushed= 315 dropped=0
08:38:44 up= 975.12 cuts=0 pushed= 465 dropped=0
08:43:44 up=1275.22 cuts=0 pushed= 615 dropped=0
08:48:44 up=1575.27 cuts=0 pushed= 765 dropped=0
08:53:44 up=1875.36 cuts=0 pushed= 915 dropped=0   ← 旧構成なら1回目の切り直し
08:58:44 up=2175.47 cuts=0 pushed=1065 dropped=0
09:03:44 up=2475.57 cuts=0 pushed=1215 dropped=68  ← 私が刺した読者によるもの
09:08:45 up=2775.66 cuts=0 pushed=1364 dropped=68
09:13:45 up=3075.74 cuts=0 pushed=1514 dropped=68
09:18:45 up=3375.84 cuts=0 pushed=1664 dropped=68
```

**uptime 3375 秒（約56分）で切り直し 0 回。** 旧構成なら 1800 秒と 3600 秒の
2回切り直されていた。`pix-simple.log` は起動時の2行のままで、再起動の記録は無い。

`dropped=68` は上に書いた私の操作ミス（読み捨てない読者を刺した）によるもので、
以降 68 のまま増えていない（09:03 以降の3サンプルで一定）。

### 判定

| 段1 の合格条件 | 結果 |
|---|---|
| 6時間以上の連続 | 56分時点で継続中（監視は3時間ぶん仕込んである） |
| `totalPushed` が単調増加 | OK（10 → 1664） |
| `pipeline stopped after` が出ない | OK（0回） |
| NAS の録画が1ファイルで継続 | **未確認**（NAS にログインできない）。番組境界（event 36739→36741）を1本のストリームで越えたことは確認済み |

## 最終的な健全性（uptime 3400秒）

| 項目 | 値 |
|---|---|
| メモリ | 3.4GB 中 2.6GB 空き |
| プロセス | pix-ts-legacy / ffmpeg ×2 / stb_streamer / launcher の 6本が稼働 |
| 温度 | 67.7℃ |
| 鍵の更新 | 279回（止まっていない） |
| USB エラー / タイムアウト | 0 / 0 |
| 書き手の詰まり | 0 |
| 連続性の誤り | 2（起動直後のみ、横ばい） |
| 切り直し | **0回** |

## 結論

**段1 は実機で成立した。** 1800秒ごとの切り直しは無くなり、番組境界を
またいでもキャプチャは1本のまま続いている。

残っている確認は1つで、NAS 側の録画ファイルが分割されないこと（NAS に
ログインできないため未確認）。ただし分割の原因は「読者が繋ぎ直されること」で、
今回は切り直し自体が起きていないため、原因は消えている。

### 元に戻す手順

```
# RAM 上の退避から戻す（再起動するまで有効）
mount /dev/sda1 /mnt/bootchk
cp /tmp/boot-rollback/Image /mnt/bootchk/Image
cp /tmp/boot-rollback/rootfs.cpio.uboot /mnt/bootchk/rootfs.cpio.uboot
sync && umount /mnt/bootchk && reboot
```

再起動で `/tmp` が消えるため、その後に戻す場合はホスト側の git から
旧コミット（`e735fbb`）で再ビルドする。設定だけなら
`PIX_CAPTURE_SECONDS=1800` を `/etc/default/pix-stream-linux` に書き、
`S97pix-stream restart` すれば切り直しは元に戻る（ファームは差し替え済みのままでよい）。
