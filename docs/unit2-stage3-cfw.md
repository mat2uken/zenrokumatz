# 2代目への CFW（MB8AC031-CFW）導入と生 TS の確認

2026-11-20。設計の段3（[dt295-external-descramble-design.md](dt295-external-descramble-design.md)）を
2代目（192.168.99.123、DT295・BS11）で実施し、**複合前の生 TS が取れることを実測した**。

## なぜこれを先にやったか

段2（外部複合）は純正ファームでは原理的に成立しない。純正ファームは
「カードで復号した後の TS」しか出さず、生のスクランブル TS を取り出す経路が無い
（[dt295-internal-card-feasibility.md](dt295-internal-card-feasibility.md)）。
したがって段の順序を 3 → 4 → 2 に組み替えて、まず生 TS が出せることを確かめた。

## 手順

### 1. 準備

| もの | 取得元 |
|---|---|
| `frc1-dt295.frc` | GitHub Releases `v0.0.1`（SHA256SUMS で照合） |
| `frc1_load` | `sample/frc1_load.c` を Buildroot のクロス GCC で aarch64 向けにビルド |
| `ts_grab` | `sample/ts_grab.c` を同じくクロスビルド |

実機には gcc が無く、libusb のヘッダも無いため、Buildroot の sysroot を使って
コンテナ内でクロスビルドした（`-DCHIPIO_HAVE_LIBUSB -D_POSIX_C_SOURCE=200809L`）。

コンテナは導入前に独立に検査した。

```
magic FRC1 / crc ok / targets 1 / segments 3
target0: vid=06b8 pid=103b hook=0x000ea6fc hook_crc=0x61b6d96b
         state=0x00082008 loader=0x000e9000+0x3910 loader_crc=0xf6ecc1ab
```

### 2. 前提条件の確認（重要）

CFW の `install` は **state == 0**（どのファームも起動していない）を要求する。
録画中は state が `0x02`、正常停止後は `0x10` になるため、どちらも不可。

**USB ポートの電源を切り直して cold にする。**

```sh
P=/sys/bus/usb/devices/usb1/1-0:1.0/usb1-port1
echo 1 > $P/disable; sleep 3
echo 0 > $P/disable; sleep 8
```

これで state が `0x00000000` になった。さらに、ローダの crc32 が
ビルド時の想定と一致することを `peek` で読んで確かめた（`0xf6ecc1ab` で一致）。
**ここが一致しないと、入口が違う命令へ飛ぶので入れてはいけない。**

### 3. 導入

```
$ frc1_load install /tmp/cfw/frc1-dt295.frc
1. state                    read 0x00082008 ok
1b. the vendor's eight bytes read 0x000ea6fc ok   fingerprint 0x61b6d96b as built
2. the loader's crc32       0xf6ecc1ab, as built
3. the vendor dispatcher goes back first          already the vendor's bytes
4. segments
   write 0x000e1000   114 / read back ok
   write 0x00380000 21804 / read back ok
   write 0x003b0000  1012 / read back ok
5. the hook, last                                 stub first / read ok
   answered through the new gate
installed
```

純正の 8 バイトは `/tmp/cfw/frc1-dt295.frc.hook` に退避された。

### 4. 動作確認

```
$ ts_grab probe
hook 0x000ea6fc -> entry 0x000e1000
  IDENTIFY      status OK
     signature 'FR01'   report v3  frame v1  1 receiver(s)
     board     06b8:103b   caps 0x2 card
     build     0x37fbfbad   (main セグメントの crc32 と一致)
```

`caps 0x2 card` はカード機能を広告している（段4 の前提）。

## 生 TS の実測

`BS11` ではなく **`BS15`（IF 1318000 kHz、TSID `0x40f1`）** が BS11 のトランスポンダだった。
`scan --sat` で 24 チャンネル中 21 がロックし、BS15 が `0x40f1` でロックした。

```
$ ts_grab save /tmp/raw.ts --seconds 3 BS15/0x40f1
  1.0 s  4.85 MB (4.84 MB/s)  ring 768 waiting
  2.0 s  9.76 MB (4.87 MB/s)  ring 704 waiting
  STOP          status OK
  firmware  14675968 byte(s) pushed
  host      14675968 byte(s) read, 78063 packet(s) written
  cc        0 jump(s)   PAT 30 packet(s)
  tei       0 packet(s) (0.00e+00)
```

**14.67 MB / 3秒 = 4.87 MB/s、CC の飛び 0、TEI 0。**

### スクランブルされていることの確認

`transport_scrambling_control`（各パケットの byte3 の上位2ビット）を数えた。

| 値 | パケット数 | 意味 |
|---|---|---|
| 0 | 8,509 | スクランブル無し（PSI とパケット空き） |
| 2 | 43,680 | **偶数鍵でスクランブル** |
| 3 | 25,874 | **奇数鍵でスクランブル** |

**78,063 中 69,554（89%）がスクランブルされている。** 純正ファームでは
絶対に得られなかった「複合前の生 TS」である。

### 複合していないことの確認

```
$ ffmpeg -i /tmp/raw.ts -map 0:v:0 -frames:v 5 -f null -
Cannot determine format of input stream 0:0 after EOF
Invalid data found when processing input
```

映像は復号できない。PSI（MPEG-2 映像、AAC、ARIB 字幕）は読めるが、
ES はスクランブルされたまま。**外部デスクランブラに渡すべき状態**になっている。

## 戻し方

```
$ frc1_load restore /tmp/cfw/frc1-dt295.frc
```

または USB の抜き差し（ローダは RAM にあり、電源を切れば flash から戻る）。

## まだ確かめていないこと

- 常駐（継続）キャプチャ。今は `ts_grab save --seconds N` で有限時間だけ。
  `-t -` 相当の連続運用は、ファーム側の常駐ループ（段5）かホストのポーリング常駐化が要る。
- 選局の作り直し。今は `ts_grab` 内蔵の選局を使っている。本番のランチャーに
  組み込むには、この選局列を `pix-userland` の代替として実装し直す必要がある。
- 内蔵カードの ATR（段4）。

## 重要な発見: 純正経路を動かすと CFW の hook が消える

CFW 導入後に、念のため「今までの録画経路が壊れていないか」を確かめた。

```
$ pix-ts-legacy --model dt295 --assets /usr/share/pix-tuner -s bs -c 9 --slot 0 -t 5
rc=0 / 11.5 MB 撮れた / locked=1
```

**純正経路は動く。** しかしその直後に状態を見ると:

```
0x000ea6fc  hook   00 49 8b 5a 8b 94 d9 80    ← 純正のバイトに戻っている
$ ts_grab probe
the hook at 0x000ea6fc does not hold a jump of ours: 00498b5a8b94d980
```

**`pix-ts-legacy` は純正ファームをチップに載せ直し、そのときにローダの hook も
純正の値へ戻す。** つまり CFW と純正ファームは共存しない。

### 含意（段4・段2 に効く）

| 状況 | 状態 |
|---|---|
| CFW を入れ、純正経路を一度も動かさない | CFW が生きている |
| 純正経路（`pix-ts-legacy`）を動かす | **CFW の hook が消える。入れ直しが必要** |
| 入れ直し | `frc1_load install` が「別のファームが載っている」で止まるので、先に USB を cold にする |

**切り替えは排他**なので、段4 以降に進むときは次を守る。

1. 本番のランチャーから `pix-ts-legacy` の起動を外す（`S97pix-stream` を止める）
2. USB ポートを電源切り直しして cold にする
3. `frc1_load install` で CFW を入れ直す
4. その状態で CFW 経路だけを使う

元に戻すときは、USB を抜き差しすれば純正経路がそのまま使える（CFW は RAM なので消える）。

### 入れ直しが失敗したときの落とし穴（実測）

CFW を入れた状態で `S97pix-stream stop` を実行しただけでは足りなかった。

- `S98pix-logo`（週1回のロゴ取得）が待機していて、`pix-logo-weekly` が
  `S97pix-stream start` を呼び、`pix-ts-legacy` が純正ファームを載せ直す。
- その結果、USB を電源切り直ししても、**すぐに純正ファームが載って state が 0x02 に戻る**。
- `install` は「state is 0x00020000, not 0」で正しく止まる。

**CFW の入れ直しは次の順で行う。**

```sh
/etc/init.d/S98pix-logo stop      # 週次ジョブを先に止める
/etc/init.d/S97pix-stream stop
# tuner プロセスが残っていないことを確認してから
P=/sys/bus/usb/devices/usb1/1-0:1.0/usb1-port1
echo 1 > $P/disable; sleep 3; echo 0 > $P/disable; sleep 8
frc1_load install /tmp/cfw/frc1-dt295.frc
```

この順で通ることを確認済み（state 0 → installed → IDENTIFY FR01）。
