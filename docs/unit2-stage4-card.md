# 2代目: 内蔵カードの PC/SC 化（段4）

2026-11-20。設計の段4。CFW を入れた DT295 で、**内蔵 B-CAS カードから
ATR を取ることに成功**した。設計書が最大のリスクとしていた点の決着。

## 手順と結果

### 1. CFW を入れて ARM コアを起こす

CFW 単体では主 FR のカード経路（`0x50`〜`0x57`）は動くが、**ARM 経路は
ARM イメージを載せないと動かない**。

```sh
frc1_load install /usr/share/cfw/frc1-dt295.frc
arm_probe hold  --dev dt295      # ARM を止める
arm_probe image /tmp/armcore.bin --dev dt295   # ITCM 0 へ 6412 バイト
arm_probe run   --dev dt295      # ARM_RUN、'ARM1' を待つ
```

成功時の表示:

```
mailbox  alive 41524d31 ('ARM1')  loops 0000ca2a
         fault 00000000  lr 00000000
```

`armcore.bin` は公開リリース `v0.0.1` のもの（sha256 `f7229bec...5cd`、
SHA256SUMS と一致）。

### 2. ATR

主 FR 経路（`CFW_CARD_DEV=dt295` のみ）:

```
$ card_tool atr
ATR after 33 ms
ATR 3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
protocols 8002  F 372  D 2  IFSC 124  CWI 5  BWI 4  specific mode
S(IFS 251) answered in 8 ms
```

ARM 経路（`CFW_CARD_ARM=0`）でも**同じ ATR**:

```
ATR after 41 ms
ATR 3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
S(IFS 251) answered in 8 ms
```

**カードの有無も見える**: `CARD_STATUS` で `CARD_EVENT 00000400`
（bit10 = カードあり）。

## 設計書の「極性の矛盾」は誤りだった

設計書 §4.2 で「`docs/card.md` は ARM の ON 引数 0 を極性に使わないと書き、
`svc_card.c` は `pol = q->arg0 ? CARD_PIN_POL : 0u` と読む。DT295 の極性は 1
なので ATR が来ない計算になる」と書き、これを段4 の関門にしていた。

**実機で試したら逆だった。**

| ON の引数 | 結果 |
|---|---|
| **0** | **ATR 取得成功（13 バイト）** |
| 1 | E_TIMEOUT（応答なし） |

つまり **0 が正しい**。`card_link.h` が 0 を渡すのは正しく、文書もコードも
そのままでよい。私の読み違いで、実機が優先する。

（`polarity_test.c` を書いて両方の値を試した。この道具は残しておく価値がある。）

## 現在できていること / いないこと

| 項目 | 状態 |
|---|---|
| 主 FR 経路で ATR | **できる** |
| ARM 経路で ATR | **できる**（ただし ARM イメージのロードが要る） |
| カードの挿抜検出 | できる（`CARD_EVENT` bit10） |
| PC/SC のリーダーとして見えるか | **未着手**（IFD ハンドラと pcscd の設定） |
| `bcasd` からの利用 | 未着手 |
| 録画中の同時利用 | 未着手 |

## 次の段階

1. IFD ハンドラ（`libifd_m2tv.so`）を Linux 向けにビルドし、pcscd に読ませる。
   DT295 は CCID クラスを名乗らないので、ドライバを手動で登録する作業が要る。
2. `bcasd` がそのリーダーを掴めることを確認（`reader.N` 指定）。
3. `torne-b25` から APDU が通ることを確認。
4. そこまで通れば段2（生 TS を外部複合）が成立する。
