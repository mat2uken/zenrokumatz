# Amatsukaze / join_logo_scp (JLS) ソース読解報告
## CM自動検出の仕組み — 条件分岐レベルの記述

調査日: 2026-11-23
調査対象コミット:
- nekopanda/Amatsukaze … ff26c44 (2019-09-23「バグ修正」) + 配布7z 0.9.1.4 (2021-01-21)
- yobibi/join_logo_scp … 4107b3e (ver5.1.1)
- nekopanda/join_logo_scp … 0ee68f2 (2018-11-11, ver3.06n系)
- nekopanda/chapter_exe … 浅クローン (改造版)
- rigaya/Amatsukaze … 8c0d54a (2026-11-21) ※後述の通り新GUIラベルはこの系統のもの

記述方針: ソース・同梱文書に書かれた事実は出典パス付きで記載。推測には必ず「推測」と明記。GPLコードの転載は行わず、パラメータ名・コマンド名・アルゴリズム・規則の記述のみ(短い識別子の引用のみ可)。

---

## (a) パイプライン概説 — 各段の入出力・ファイル形式

### a-1. Amatsukaze における実行順序 (nekopanda版 CMAnalyze.hpp / rigaya版 CMAnalyze.cpp)

事実出典: `Amatsukaze/CMAnalyze.hpp`(ff26c44) L32-83, rigaya版 `Amatsukaze/CMAnalyze.cpp` analyze()。両者で骨格は同じ。

1. **前段**: TS解析→映像フォーマット別に分離→フィルタ用映像 `i%d.mpg` と音声 `audio.wav` を一時フォルダに生成 (TranscodeManager.hpp L545-556)。CM解析は映像ファイルごと(フォーマット切替ごと)、かつ**300フレーム(約10秒)以上あるファイルのみ**実行 (TranscodeManager.hpp L565-567「短すぎるとエラーになることがあるので」)。
2. **AVS生成** `amtsN.avs`: `AMTSource("amtsN.dat")` を読む1行AVS (CMAnalyze.hpp makeAVSFile L246-260)。rigaya版は更に chapter_exe 用に 8bit 化した `amtsN_8bit.avs` を別途生成 (rigaya CMAnalyze.cpp)。
3. **ロゴ解析** (第1段): **logoframe.exe は使わない**。Amatsukaze 内蔵の `logo::LogoFrame` クラス (LogoScan.hpp, **MITライセンス**) が AviSynth を直接呼んで全フレーム評価し、複数ロゴ候補から最良1個を選ぶ (CMAnalyze.hpp L273-317)。結果を logoframe 互換形式で `logofN.txt` に出力。
   - 複数ロゴはサービスID+録画時刻で自動選択 (EncodeServerData.cs LogoSetting.CanUse: From<=時刻<=To のロゴのみ使用。**「### NO LOGO ###」(ロゴなし)設定も期間付きで登録可能** — EncodeServerData.cs L526)。
   - ロゴ合致判定: `logoRatio < threshold` で「この区間はマッチするロゴはありませんでした」。threshold は **looseLogoDetection 時 0.03、通常時: 再生時間7分以下 0.03 / 7分超 0.1** (CMAnalyze.hpp L301)。
   - 一致しなかった場合: nekopanda版 → NoLogoException でジョブ保留(ロゴ生成待ち) (TranscodeManager.hpp L600-605「マッチするロゴが見つかりませんでした」)。rigaya版 → **自動ロゴ検出を試行** (後述 d-2)。
4. **無音・シーンチェンジ解析** (第2段): **chapter_exe.exe をサブプロセスで実行**。引数: `-v amtsN.avs -o chapter_exeN.txt` + `--chapter-exe-options` (GUIの「chapter_exeオプション」) (CMAnalyze.hpp L319-337)。標準出力は `chapter_exe_oN.txt` に保存され、Amatsukaze はここからシーンチェンジ位置 (SCPos 行) を読む (L411-439)。
5. **CM解析** (第3段): **join_logo_scp.exe をサブプロセスで実行**。引数: (ロゴ一致時のみ) `-inlogo logofN.txt` `-inscp chapter_exeN.txt` `-incmd "JL/JL_標準.txt"` `-o trimN.avs` `-oscp jlsN.txt` `-odiv divN.txt` + `--jls-option` (L339-354)。**ロゴ不一致時は -inlogo を渡さない** → JLS はロゴなしモードで動作。
6. **結果読み込み** (L367-459): `trimN.avs` の `Trim(a,b)` 列 = **残す区間**(本編) を正規表現で読み取り、trims に格納。**trims の補集合が CM ゾーン** (makeCMZones L441-459)。`divN.txt` は分割点、`jlsN.txt` は構成詳細(後述)。
7. **PMT更新によるCM追加認識** (applyPmtCut L107-195): `--pmt-cut 頭%:尾%` 設定時、TS中のPMT更新(PID構成変化)時刻と**60フレーム以内に一致するシーンチェンジ**を「CM境界」と認識(L135 `diff < 30 * 2`)。前頭%以内にあった PMT 変化位置まではじめ側を、末尾%以内にあった位置から終わり側を本編trimsから削除(=CM扱い)。PMT認識は「設定区間(前後カット率の範囲)内」に限定 (L154-163)。シーンチェンジ情報がないTSでは利用不可 (L112-114)。
8. **チャプター生成** (MakeChapter L462-679): `jlsN.txt` (-oscp出力) の1行 `開始frm 終了frm 秒 誤差frm ロゴ秒 :ラベル` を構成単位で読み、trims 上の中点でカット/残し判定 (L543 isCut=!(trimIdx%2))、ラベル CM の部分をチャプター「CM」、Trailer/Sponsor/Endcard/Edge/Border は「A/B/…+秒数」チャプターにする (L573-601)。**2秒以下のチャプターは結合** (L625「2秒以下のチャプターは消す」)。

### a-2. 中間ファイル一覧 (TranscodeSetting.hpp L927-962)

| ファイル | 生成者 | 内容 |
|---|---|---|
| `amtsN.dat` / `amtsN.avs` | Amatsukaze | AMTSource 用デコード済み映像+音声 / 読み込みAVS |
| `logofN.txt` | Amatsukaze内蔵LogoFrame | ロゴ区間。行形式 `bestFrm S/E 0 ALL startFrm endFrm` (LogoScan.hpp writeResult L1818-1819)。S/E交互、fade欄=0、ALL=インタレース評価なし |
| `chapter_exeN.txt` | chapter_exe -o | `CHAPTERnn=hh:mm:ss.mmm from:frm` と `CHAPTERnnNAME=seriフレーム mark SCPos:pos pre` の対、最終行 `# SCPos:lastfrm lastfrm` (chapter_exe_utf8.cpp L45-47, L312, L732) |
| `chapter_exe_oN.txt` | Amatsukazeリダイレクト | chapter_exe標準出力(無音区間一覧)。JLSの読み取り対象 |
| `trimN.avs` | JLS -o | `Trim(a,b) ++ Trim(c,d) …` 1行 = **残す区間** |
| `jlsN.txt` | JLS -oscp | 構成詳細 `%6d %6d %4d %3d %4d :ラベル` = 開始frm / 終了側frm / 期間sec / 15秒(整数秒)からの誤差frm / ロゴ表示sec / 構成ラベル (JlsDataset.cpp outputResultDetailGetLine L3627-3660) |
| `divN.txt` | JLS -odiv | DivFile コマンドによる分割フレーム位置 (1行1値) |

ラベルの種類 (JlsNameSpace.hpp ComLabelType L305-325): `:CM` `:Nologo` `:Nologo(cut)` `:N-Edge(cut)` `:L-Edge(cut)` `:Trailer(cut)` `:Sponsor(cut)` `:Border` `:Border15s` `:Mix` `:L` `:N-Edge(add)` `:L-Edge(add)` `:Trailer(add)` `:Trailer` `:Trailer(cut-cancel)` `:Sponsor(add)` `:Endcard(add)`。

### a-3. chapter_exe (nekopanda改造版) の検出仕組み

事実出典: `chapter_exe/src/chapter_exe.cpp` (iconv UTF-8複製 /tmp/amatsukaze-research/chapter_exe_utf8.cpp)、`mvec.cpp`、`改造版_読んでね.txt`、`変更ファイル.txt`。

- 改造系譜: ru(オリジナル) → Yobi (精度向上・2候補出力・SCPos前フレーム出力・最終フレーム出力) → sysuzu (-b 追加) → Yobi mod3 (-e 追加) → mod4 (判定改修) → **nekopanda (VFW非依存・AviSynthスクリプト直接読み)** (README.md「VFWに依存しないでAvisynthスクリプトを読めるように改造」)。
- **無音検出**: 音声はフレーム単位で読み、`|サンプル| > -m` (既定 **50**, 範囲1..2^15) が1サンプルでもあれば非無音 (L258-279)。**連続 -s (既定10) フレーム**無音が続いた時点で無音区間として記録 (L283-285 `seri >= setseri`)。探索高速化のため非無音中は setseri フレーム先読みでスキップ (L253-267)。FAW (音声内欠落フレームマーク) 検出に対応 (L215-235)。
- **シーンチェンジ検出** (proc_scene_change L325-748): 無音区間 [start-`-e`, start+seri+`-e`] (既定 -e=**1**) の各フレームで mvec() を実行。mvec は 16x16 ブロック単位の動きベクトル探索 (ツリー探索→最大±32px全探索、Y8輝度) を元にした IIR_3DNR (H_Kasahara氏) 由来の改修アルゴリズム (mvec.cpp 冒頭コメント)。
  - 判定値 `rate_sc = cnt_sc*1000/cnt_total`、**rate_sc >= 100 (RATE_SCENE_CHANGE)** でシーンチェンジフラグ (mvec.cpp L250, L392-399)。
  - 特殊ケース: 画面が**空白(ベタ)寄り** (RATE_SCENE_JDGBLK=850/1000) のときは閾値を 20/8 に下げる (RATE_SCENE_CHGBLK/CHGCBK)。**表示物の消滅/出現**は 50 (CHGLOW)。**固定ライン**(画面の上下左右から MAX_LINEOBJ=20 ブロック以内の縦横直線で半分以上が固定)は総数から除外 (L84, L252-296) — ロゴ/黒帯縁による誤検出対策。
  - **インタレース**: フィールド別 (T/B) に独立評価し、動きの多い側/少ない側を別カウント (L428-440)。
  - 候補確定ルール: シーンチェンジ直後 5フレーム (space_sc1) は再検出しない。ただし「前回より大幅に変化大」(THRES_RATE1=**300** 以上かつ前回値2-3倍)なら上書き (L482-499)。直前候補から10/30フレーム以内の上書きは、より明確な転換のみ許可 (L462-474)。**変化なし** (rate_sc < THRES_RATE2=**7**) は still としてマーク「＿」 (L707-708)。
  - **長無音**: -b (既定 **60**) フレーム毎に候補を強制確定し、無音区間内に最大2箇所まで SCPos を出力 (readme.txt「1つの無音期間にシーンチェンジがあれば２つまで出す」)。
  - マーク出力 (L652-728): 「＿」=変化なし/still、「○」=長無音内の強制区切りでマーク対象外、「★」1-4個=**前回無音のマーク位置から15/30/45/60秒(30fps換算フレーム)の距離で一致した候補**、「＠」=第2候補。
- JLS 側の読み方 (join_logo_scp/src/JlsIF.cpp readScpos L452-): `CHAPTERnn=` から無音開始時刻、`NAME=` の `seriフレーム` から無音幅と `SCPos:位置 前位置` を解析。still「＿」マークは DataScpRecord.still=1 (JlsIF.cpp L543-551)。連続無音区間は統合 (L555-566)。**0フレームと最終フレームもSCPosとして常に存在** (L456-458, chapter_exe L312)。

### a-4. join_logo_scp 本体の内部構造 (yobibi ver5.1.1)

- 入力: `-inscp` (chapter_exe -o 出力) 必須、`-inlogo` (logoframe互換形式) 任意、`-incmd` (JLスクリプト) 必須、`-o` `-oscp` `-odiv` 出力3種、その他 `-CutMrgIn/-CutMrgOut/-CutMrgWI/-CutMrgWO/-lastcut/-flags/-set/-F/-setup/-sublist/-syscode` 等 (readme.txt)。
- **-inlogo 未指定/ロゴ区間なし時は全フレームロゴありとみなす** (readme.txt)。さらに実行時、**有効ロゴ区間の合計 < WLogoSumMin (既定20秒) なら自動で LogoOff** ($NOLOGO=1) し「warning: LogoOff is set due to insufficient logo period」を出す (JlsScrFuncReg.cpp setSystemRegNologo L1235-1281, JlsDataset.cpp L34)。**ロゴなし/ロゴ無効/ロゴ合計不足の3条件すべてがロゴなし経路(AutoCM)に落ちる**。
- `JL_common.txt` (-setup, ver5.0+) が常に最初に読まれ、文字コード設定・内部メモ出力・Amatsukazeロゴ検出結果の判別 (`intype_logo_amt`: ロゴファイル名が `logof[0-9]` を含むかで Amatsukaze 由来か判定, JL_common.txt L16-46) を行う。
- 実行はスクリプト上から1行ずつ。検索コマンドは前の確定済みカット位置を変更しない、未確定のまま終わった位置はカットされない (JLコマンド説明_全般.txt L6-15)。

---

## (b) JL_*.txt (実行内容スクリプト) の規則 — コマンドごと

### b-1. スクリプト言語の要素 (JLコマンド説明_全般.txt)

- 変数: `Default`(未定義時のみ)/`Set`、`$変数名` で展開、リスト変数 `$name[n]`、`$#name`=要素数。システム変数 `$MAXFRAME $MAXTIME $NOLOGO $LASTEXE $HEADTIME/$TAILTIME` 等 (L95-131)。
- 制御: `If/ElsIf/Else/EndIf` (フラグ定義・数値比較・文字列一致・`defined()`)、`Repeat/EndRepeat`、`Break`、`{ … }` ローカル変数領域、`;` 併記 (L574-612, L1428-1470)。
- ファイル/関数: `Call`、`Function/Fcall` (参照渡し `ref()` 可)、`Memory/MemSet/MemCall` (識別子ごとに遅延実行、システム識別子 `START`(次の検索コマンド直前)/`AUTO`(Auto系直後)/`END`/`FINALIZE`/`OUTPUT`)、`LazyStart/-lazy_s/-lazy_a/-lazy_e` (L617-741)。
- 実行タイミング制御が肝: `LazyAuto` 宣言ファイル内の検索コマンドは自動遅延期込となり、`MemCall mbase_*` で**実行場所フック**(検索開始前/Auto推測直前/直後/前半提供処理前後/予告処理前後/最後) を差し込める (JL_標準.txt L173-320 の各 `MemCall` 行、補助解説ver5)。

### b-2. 従来方式カットコマンド (ロゴ端を無音SC位置へ調整する方式)

| コマンド | 動作 | 出典 |
|---|---|---|
| `Find S/E/B 中心 範囲先頭 範囲末尾` | ロゴ位置基準で範囲内の無音SCのうち中心最近傍をCMカット位置に確定 | 全般 L48-51 |
| `Force S/E/B 位置` | 無音SCを使わず位置補正だけで確定 | L52-54 |
| `Abort S/E/B` | そのロゴ端のカットを不確定(=残す)にする | L56-57 |
| `MkLogo S/E/B … -EndLen …` | 無音SCから仮想ロゴ区間を作る (Auto系にも反映可) | L59-71 |
| `DivLogo` | 同一ロゴ内の分割 | L74-78 |
| `Select S/E/B …` | ロゴ位置が自動検出不能なとき範囲内無音SCでロゴ位置を選択 | L80-86 |
| `GetPos/GetList` | 検索結果を変数に取得(確定しない) | L272-286 |

主なオプション: `-N/-NR 番号`(全体/Abort除いた何番目のロゴ、負=末尾から)、`-Nlogo/-Nauto/-NF*` (実際のロゴ/推測ロゴ扱い構成の区別)、`-LenP/-LenN 最小 最大` (前/後のロゴ端までの距離制限)、`-F/-Fhead/-Ftail/-Fmid` (範囲限定、先頭・末尾基準は自動で半分までに制限)、`-SC/-NoSC/-SM/-SMA/-AC` (基準位置からの無音SC/無音期間/推測区切りの有無制限)、`-wide` (ロゴ可能性範囲まで拡大)、`-noedge` (0/最終フレームを除外)、`-fromabs/-fromhead/-fromtail/-fromTR/-fromSP/-fromEC/-fromBD/-fromMX/-fromCM/-fromNL` 等 (基準位置変更)、`-force` (無音SCがなければ強制作成)、`-autochg` (推測構成に反映) (全般 L745-1296)。

従来方式のカット位置は「**ロゴ表示終了から CutMrgOut 補正した場所=CM開始、次のロゴ表示開始から CutMrgIn 補正した場所=CM終了**」が基本 (全般 L11-12)。**CutMrgIn/CutMrgOut 未指定時は自動検出**: 全ロゴ端と確定無音SCの距離ヒストグラム(30fpsフレーム換算200本)を作り、ロゴ端の不確定度で重み付け(不確定端は重み3、ファイル端50ms内は0)、フェード有無で±2/±1フレーム平滑、**標準マージン範囲(-2〜+36フレーム)は2倍加重、2秒超は1/4**、最多ビンを採用。2巡目は候補不足時に確定度を緩めて再試行 (JlsReformData.cpp detectCutmrgSub L586-720)。`-CutMrgWI/-CutMrgWO` = ロゴ端にシーンチェンジがある場合 CutMrg を使わない(=補正0)モード。0=常に使用/1=指定値と0の2点から判断/2=自動判断 (readme.txt、JL_標準.txt L166-168)。

### b-3. Auto系コマンド (構成推測) — ここが判定規則の実体

事実出典: `JLコマンド説明_Autocutメモ.txt`、`JlsAutoScript.cpp`、`JlsAutoReform.cpp`。

**構成推測の前提**: chapter_exe の無音SC列に対し、
1. **候補境界スコアリング** (`getScore` JlsAutoReform.cpp L4849-4960): 各無音SC位置から、±30秒を5秒刻み16点、±3分を15秒刻みで見て、各刻み位置に無音SC/無音があれば加点。15秒単位(60秒内)は基本21点+近さ (≤100ms:+10、≤180ms:+7、≤350ms:+4)、5秒単位は7点+近さ(静止画は近さ1に減)、60-120秒15点、120秒超3点。**合計≥50なら5秒単位点を切り捨て15秒単位のみ、遠い一致は1/3減点、最低50に頭打ち**。
2. **候補ランク付け** (`setScore` L4134-4180): score≥30→`SCP_CHAP_CPOSQ`(15秒単位可能性保持)、≥10→`SCP_CHAP_CPOSIT`(5秒単位可能性)、それ以外は候補外。700ms以内の隣接候補は間引き (msecValLap1)。
3. **構成区切り確定** (`setChap/setChapGuess` L4192-4578): 「継続候補」「新候補」の2バッファを持ち、**各候補から5/15秒単位の距離 (±1200ms許容、mod05 は180ms) にある候補を chain として蓄積**、最初の候補から**180秒以上**継続候補が伸びなければ確定 (L4240-4256「3分以上経過した場合」)。確定区切りは `SCP_CHAP_DFIX`。距離チェーンから外れた途中の15秒単位候補は除外もする (L4404-4426)。

**AutoCM (ロゴなし時の15秒単位CM検出)** `AutoCM B -code 1-10 -scope 秒 -scopex 秒 -maxprd 秒`:
- ロゴなし時のみ動作 (ロゴ使用レベル<4 または LogoOff 時) (JlsAutoReform.cpp startAutoCM L52-62, JlsScript.cpp L2110-2130)。
- `detectCM` (L1023-1326) の状態遷移: 構成区切り列を先頭から走査し、連続2区切りの秒数差 `secdif` が
  - **(secdif%15==0 かつ secdif≤maxprd[既定60秒])** → CM単位、または **(secdif%30==0 かつ ≤120秒 かつ cutlevel≥10)** → CM単位。
  - `autocm_sub` +10 (既定10) なら **45秒 (30秒単位でない 30<secdif≤60) は CM から除外** (L1098-1100 is_sub_off45s)。
  - **checkMuteMany** (L5078-5127): 区間内に「どの境界にも属さない無音」が多い場合 (14.5秒内に2超/29.5秒内に3超/59.5秒内に5超/全体8超 = type2、その半分でtype1) は type≥2 で CM 判定を取り消し。SCなし無音 (still+TypeNoSc=1) はさらに+4して棄却。
  - 末尾救済: `-scope 秒` (autocm_scope, 既定40秒): 最後から指定秒以降の短時間構成は**手前がCM扱いならCM継続** (L1110-1115)。`-scopex` (既定0): 末尾指定秒内の**60/90/120秒構成は無条件CM扱い** (L1116-1119)。`-maxprd`: 指定秒超の構成は15秒単位でもCMにしない。
  - **cutlevel (-code 値) と確定条件** (L1183-1226 のコメントが正): 1=4構成以上CM / 2=4構成以上+3構成の一部(周囲構造少) / 3=4+3の一部 / 4=3構成以上 / 5=3+2の一部 / 6=**3構成以上CM、2構成の一部** (既定) / 7=2構成以上 / 8=2+1の一部 / 9=1構成以上 / 10=1構成以上+120秒構成許可。「一部」は d15/d30 (ちょうど15/30秒の区間数) 条件付き (L1201-1210)。先頭位置は条件を±3段調整 (AUTOFIRST_STRICT=-3 / NOCUT=0 / LAX=+3, L1146-1170)、`is_sub_cmhead` (autocm_sub 1桁目=1, 既定10→false) なら先頭を cutlevel≥9 に引き上げ (L1172-1176)。**最後の区間は強制 cutlevel=10** (L1178-1181)。
  - 状態機械は「CM候補蓄積→ロゴ区間蓄積」の交互で、確定時に直前のロゴあり部分が 15秒以上構成 (dov15) を含むかで再配置 (detectCMSetLogo / detectCMCancelShort L1374-1475)。4構成判断 (S4N_JUDGE) では「ロゴあり累積≥120秒かつ4構成以下」なら CM 側へシフト (L1190-1210)。
- ロゴなし全体推測 (mkReformAllNologo L336-354): まず mkRangeCM を全範囲で実行し初期CM構成を作り、その上で detectCM を全範囲 (reform_all) で実行。

**ロゴあり構成推測** (mkReformAllLogo L173-261): 各ロゴ区間 [rise,fall] ごとに、
- 前ロゴ終了〜次ロゴ開始の間で `mkRangeCM` (L1541-1601): 探索範囲 cmscope はロゴ端の可能性範囲 (WLogoSftMrg=JL_標準3.5秒拡張, L1602-1645)。
- `setCMForm` (L3056-3196): 範囲内の15秒単位候補 (≥SCP_CHAP_CDET) から `setCMFormDetect` (L3203-3350) で**対象候補から15秒刻みに手前へ最大120秒遡って**別の15秒単位区切りを探し、CM構成の start を確定。途中の既存確定区切り (DFIX) が15秒位置に無ければ探索打切り。**15秒位置が無音で無音SCが無い場合、スコアが高ければ無音SCを新規挿入**できる (L3265-3290 insertScpos)。
- `setCMFormDetRevise` (L3350-3520): CM と認めた各区間の誤差 (15秒単位からのずれ) が **MgnCmDetect (JL_標準 0.25秒)** 超なら: 次区間と併せて誤差が半分以下になるなら中間区切りを削除して無視 / CM側に5秒・10秒構成があれば DINT 区切りを作って分離 / 先頭・末尾の区切りなら削除 (ロゴあり時は極端な端のみ) / **AddUC+10 (LogoUCGapCm) なら誤差大の区間を残す側へ** (L3480-3505)。ロゴなし時は LogoUCGapCm が常時有効 (JlsDataset.cpp L216-218)。
- `setCMFormByLogo` (L3522-3807): LogoRevise (+20: ロゴ外広域削除) 時、CM境界を隣接ロゴ端の限界まで15秒単位構成で**外側へ拡張**。
- ロゴ期間自体が1構成になるか: `mkReformAllLogoUnit` — ロゴ期間の長さが LogoLevel 相応の単位 (15秒単位必須、レベル5/6で5秒単位、7で秒単位、8で無条件+180-600秒の30秒単位も) なら残し側に1構成追加 (isLengthLogoLevel L5293-5331)。
- ロゴ内の微調整: `addLogoEdge` (ロゴ扱い部分でロゴなし割合が多い所の補正)、`addLogoBorder` (無音SCのないロゴ切替境界=Border構成)、`addLogoMix` (ロゴ区間内にロゴなし期間があるが無音SCがない=Mix) (L430-801)。
- 先頭処理 `setFirstArea` (L1647-1841) と末尾処理 `setFinalArea` (L2030-2331): 下記 b-5。

**ロゴ使用レベル** `SetParam LogoLevel` (Autocutメモ L334-343):
- 1: ロゴなしで推測、ロゴ全不使用 / 2: なしで推測後、ロゴを補助(追加のみ) / 3: 同(追加・削除) / **4: ロゴ優先度低、15秒単位のみ認識** / 5: +5秒単位を一部 / 6: **+5秒単位を追加認識 (JL_標準既定)** / 7: 秒数単位 / 8: 秒数無関係。未設定時は 7 (USE_HIGH) (JlsScript.cpp L2119-2121)。ロゴデータが無い場合は強制 0 (UNUSE_ALL) (L2114-2116)。
**ロゴ補正** `SetParam LogoRevise` (Autocutメモ L344-351): 0-4 = ロゴ端のCM化の強さ (0=不明確ならCM化しない … 4=できるだけCM化)、+10 = ロゴ内部の複数15秒構成CM化、+20 = **ロゴ外部の未CM認識広域構成のCM化**。JL_標準既定 **23** (2+20)。

**AutoCut TR (予告位置認識+予告後カット)** `AutoCut TR -Ftail 起 終 -code … -limit … -trscope … -tr1stprd … [-trsumprd …]`:
- -code 1桁目 (c_from): 0=実行しない / 1=**CM明けから数える** / 2=**CM明け(CM直前の自動判定含む)** / 3=**CM直前の指定秒以内に開始する構成から数える** / 4=CM直前の5秒単位構成 / 5=CM直前の15秒単位構成 (JlsAutoScript.cpp setParamCutTR L91-107)。
- -code 10の位 (c_cutst): カット開始条件 0=15秒単位の構成 / 1=15秒以上 / 2=無制限 (L1100-1107)。
- -code 100の位 (c_lgpre): CM明け直前のロゴなし部分を予告候補に追加するか (1/2) (JL_標準の cuttr_addcode=100 に対応)。
- 予告開始位置特定 (subCutTRGetLocSt/Sub L1201-1441): 各ロゴ区間で (a) ロゴ終了が探索範囲内、(b) ロゴ長≤WLogoTRMax (JL_標準 2:10)、(c) 各構成≤WCompTRMax (1:10)、(d) **ロゴ直前の構成がCM15秒単位でない場合、2-3個前がCMなら予告開始に含める**(番組提供期間外 6-13秒のみ除外して追加、tr1stprd 以下) (L1295-1340)、(e) ロゴ内で WCompTRMax 超の構成があれば本編と判定し候補クリア (L1385-1388)、(f) **tr1stprd (既定45秒) より大きい構成が現れるまで予告にしない** (L49, L1407-1410)。
- 予告確定後 (startAutoCutTR L1082-1199): 予告位置から -limit 構成数 (JL_標準1) と -trsumprd 秒までを **TRKEEP=予告として残す**。以降、ロゴ付きの構成は条件 (ロゴ2分以内、構成60秒以内、範囲内) を満たす限り **TRCUT=カット番宣** にする。ロゴなし不明構成 (N_OTHER) は N_TRCUT。カット開始直後の Border 構成は TRRAW (エンドカード判断待ち)。
**AutoCut EC (エンドカード分離)**: TR カット手前の15秒未満部分を切り分ける。-code 1-5 (構成期間 15秒未満/5秒/5秒以下/5-13秒/5-13秒+上限)、+10 最後の1構成強制カット、+20 period秒構成は強制カット、+100 提供・既存ECより前も許可 (Autocutメモ L54-78, JlsAutoScript.cpp setParamCutEC L120-153)。
**AutoAdd SP/EC/TR (配置情報追加)**: 条件を満たす構成を SP=番組提供/EC=エンドカード/TR=予告として**残す側**に登録。候補は `subAddSearch`→`subAddGetPriority` (L2535-2858) で順位付け:
- 構成期間: SP既定 6-13秒 (+code 2/4/5/6 で 5秒/5-13秒/15秒指定、-period/-maxprd で調整)、EC/TR既定 13秒以下 (setParamAdd L161-197)。
- 探索範囲 (-code 10の位): 0/1=予告(なければロゴ)隣接のみ、2=予告から90秒以内(予告の手前側は隣接のみ)、3=90秒の所、4/5/6=ロゴ基準の同列 (Autocutメモ L96-103, L146-152)。90秒は -scope/-scopen で変更。
- 追加条件 (-code 100の位〜): +100 ロゴなしのみ / +200 ロゴありのみ / +300 両隣もロゴなし / +400 AutoCut後も検索 / +1000 予告前側除外 / +2000 予告後側除外 / +3000 予告と提供の間以外除外 / +4000 直後が提供・EC時のみ / +10000 **構成13秒以下で最初の候補位置限定** / +20000 既存予告≥trsumprdなら無効 / +100000 合併15秒CM検出を無効化 / +200000 強制有効化 (Autocutメモ L154-167)。
- 優先度加点 (subAddGetPriority): 予告間隙間+40、**番組提供直後隣接+130**、予告直後隣接+30 (ロゴなし+15)、予告前隣接+20 (ロゴなし+15)、提供から90秒内+30、ロゴ端基準は type_logo_prior (1-9) など。**合併15秒単位CM (N_AUNIT/N_BUNIT: 例えばロゴなし5秒+10秒が15秒CMを构成) は原則CMのままで、検出分割 (divideCm) 時のみ最下位優先で候補** (L2782-2800) — 「5秒+10秒=15秒」を番組提供と誤認しないための規則。
- **JL_標準の先頭番組提供** (L201-231): `AutoAdd SP -Fhead 0 00:13:30 -code 1001/21/1004/206 …` — 先頭13.5分以内で、code1=1001 (6-13秒、ロゴ終了側隣接・前側除外)、code2=21 (6-13秒、予告から60秒内)、code3=1004 (5-13秒隣接)、code4=206 (15秒・ロゴ付き隣接)。**先頭はロゴ/予告に隣接する6-13秒構成を番組提供として残す**規則。
- **JL_標準の末尾番組提供** (L311-316): code1=421 (6-13秒、範囲90秒・AutoCutカット位置後含む)、code2=1004、code3=206、code4=**4001 (予告と認識した最初の内容を番組提供化+予告を後ろにずらす)**。
- **JL_標準のエンドカード** (L58-70 + L155-160): `If $NOLOGO` 時のみ code1=11003 (制約緩和)。通常 code1=**31303** (提供直後隣接でロゴなし、13秒以下候補限定+予告30秒未満) period5、code2=**11223** (予告・提供から90秒内のロゴあり最初の短期間) period5、code3=11023 (90秒内最初の短期間) period3、code4=11203 (予告・提供直後の隣接でロゴあり) period10。

**AutoEdge S/E** (各ロゴ端のカット/延長): -code 1-5 は構成期間 3-10秒/5秒/3-5秒/5-10秒、+10 桁で既存カット後基準/ロゴ切替基準、+100の位でロゴ部分カット/ロゴなし部分カット/ロゴ部分残す/ロゴなし残す、+1000 全構成実施 (Autocutメモ L188-204)。JL_標準では cut1st (最初のロゴ先頭部分カット: 局タイトル想定 3秒, -code 2=指定秒構成時カット) と edgest/edgeed (既定0=無効) に使用。

**AutoUp B -code 1/2**: 従来方式で確定した結果を推測構成のロゴ有無に反映 (1=ロゴ有無のみ、2=推測構成も変更) (Autocutメモ L266-274)。JL_NHK で使用 (`AutoUp B -code 2` L199)。

### b-4. 15秒単位の扱い・最短/最長制約の全体像

- 「15秒単位」判定は整数秒で `%15==0` (isCmLengthMsec L5285-5291)。候補境界の許容誤差は mod15 ≤ **1200ms** (msecValNear3)、5秒単位は mod05 ≤ **180ms** (msecValNear1) (calcDifMod0515 L5243-5275)。
- 15秒単位CMの長さ上限: AutoCM -maxprd 既定 60秒 (autocm_maxprd, JL_標準 L177「指定秒を超える構成は15秒単位でもCM扱いにしない」)。cutlevel≥10 で30秒単位≤120秒も許可。
- 構成推測チェーンの打切り距離: 候補から **180秒** (setChapUpdate「3分以上経過した場合」)。15秒刻み手前探索は最大 **120秒** (setCMFormDetect `sec_dif15 > 120`)。
- 予告関連の最長: WLogoTRMax=**2:10** (ロゴ期間がこれ超なら本編)、WCompTRMax=**1:10** (1構成がこれ超なら本編)、cuttr_tr1stprd=**45秒** (この秒数以下の構成が現れるまで予告としない)、acmtr_period=45秒 (ロゴなし拡張予告の構成上限)。
- CM構成で15秒単位から外れたとみなす誤差: MgnCmDetect=**0.25秒**、CM内部の分割を許す誤差: MgnCmDivide=**0.1秒** (JL_標準 L194-195)。
- ロゴなし不明構成の「不明」判定: **WLogoRevMin=125秒** (JL_標準) — 「CM構成ではないロゴなし期間が不明構成と認識する構成最小期間」。AddUC=1 ならこれ以上のロゴなし構成は残す。
- 本編/CM境界の最終決定: Auto系では構成区切り (SCP_CHAP_DFIX/DINT) が境界。Trim出力は構成区切りのまま (outrev_cutframe=0 既定で補正なし、JL_common.txt L9)。

### b-5. 本編先頭・末尾付近・番宣の扱い (重点項目)

**先頭 (setFirstArea L1647-2030)**:
1. 先頭優先位置 (base/param_pos_first, LvPosFirst) と最初のロゴ立ち上がり位置 (nrf_1st) を取得。立ち下がりが zone_first 期間内なら次のエッジへ送る (L1690-1696)。
2. `setFirstAreaUpdate` (L1842-2028): 30秒+base の範囲 (msecEnd1st, L1668) 内の候補位置を順位付け。順位 = 位置指定 ($base から近いほど高、3秒以内) + **ロゴ最近似位置 npoint=3001優先** + 区切りによる優先 (+15000: 確定区切りか15秒単位距離 / +10000: 5秒単位・確定区切りとの距離 / +2000)。「ロゴ位置から15秒未満は正確に秒数単位でなければ候補から外す」(gap>msecValExact(100ms)) (L1873-1878)。**確定構造 (nscDecide) から3秒未満の候補は除外** (L1881-1888)。
3. 先頭CM化 (L1794-1841): 確定した先頭区切り位置が msecLogo1st (最初のロゴ位置) 以前 または msecLgCut1st (**LgCutFirst=12秒**: 最初からロゴありのとき先頭12秒まではCM扱いを許容) 以下なら先頭構成を N_OTHER (ロゴなし=カット側) に。**zone_first (ZoneFirst) 内に全体が収まる構成はロゴなし扱い**。以降、確定区切り列を msec_nologo まで順に N_UNIT (15秒単位) / N_OTHER に変更 (L1813-1833) — **先頭の「ロゴなし区間」は区切り毎に15秒単位なら N_UNIT、それ以外 N_OTHER で連続的にCM化していく**。
4. NOCUT (先頭からロゴ直近=カットしない) 判定: msec1stSel==0 (base=0指定) かつ最初のロゴがほぼ先頭 (early≤350ms) なら先頭カットなし (L1772-1782)。

**末尾 (setFinalArea L2030-2331)**:
1. WCompLast (JL_標準 30.5秒) より後ろに最終確定区切りがある場合、最終構成を N_OTHER に (L2265-2270)。
2. ZoneLast 内に全体が収まる構成はロゴなし扱い (L2185)。
3. **最後のロゴ期間が長い (60秒超) 場合は、内部候補から calcDifSelect (10/15/30/45/60/90/120秒 or 3-25秒の秒単位) に合う区切りを探して構成を確定** (L2053-2137)。
4. 最後付近の15秒単位構成追加 (L2140-2230): 確定位置の後に15秒単位 (mod15≤1200ms) 候補があれば DFIX+N_UNIT として追加、さらに「最後の2構成が15秒単位」なら両方確定 (L2160-2211)。
5. **zone_last または最終ロゴ終了位置より後は全部ロゴなし (N_*) に変換** (L2235-2263)。
6. 予告認識 (AutoCut TR -Ftail 00:00:00-00:07:00) はこの後、構成ラベルとして Trailer を残し、**予告後のロゴ付き15秒単位構成を Trailer(cut) で切る**。

**番宣 (他番組宣伝) の扱いのまとめ**:
- 末尾: **「予告 (Trailer)」は残す対象として保護**される。保護量 = cuttr_limit 構成数 (既定1) + trsumprd、開始認識は「CM明け/CM直前」基準 (cuttr_code=2 自動)。予告の**後**に続くロゴ付き15秒単位構成 (番宣) は CutTR (既定0=残す) または AutoCut TR が Trailer(cut) として切る。ロゴなし番宣は N_TRCUT (JL_標準 param_cuttr=0 ではこの自動カット後の部分は Trim に出る点に注意 — TRCUT ラベルは CutTR=1 時にカット扱い)。
- 先頭: **保護されるのは「番組提供」(6-13秒、fhead 0-13:30) と「局タイトル」(cut1st 3-10秒) のみ**。先頭のロゴあり番宣は「ロゴあり=本編側」として残り、ロゴなし番宣は CM の一部として切られる。つまり **JLS の規則上、先頭に現れる番宣を個別に識別・保護するコマンドは存在しない** (Autocutメモの TR 系は全て「予告・番組提供から90秒以内」=末尾基準、-Fhead系は SPのみ)。先頭番宣を切る方向のフラグとして HCCM (最初にロゴ付番宣 15/30秒から始まる場合は AutoEdge S でカット) が JL_set_フラグ指定標準.txt にあるのみ。
- NHK (CM無し局) (JL_NHK.txt): 最初のロゴ以前・最後のロゴ以降を `Find S/E 0 ±1.2 -wide -noedge` → ±10.2秒 → `Force` で強制カット (L177-189「必須設定」)。末尾のロゴ付き宣伝は **lastcut_num=6回** `Abort S -NR -1 -Ftail 0-10:00 -LenN 00:01:01 -WithN -AC ±1.2` で「1分以内のロゴ付き短区間」を順に無効化 (L146-150)、10分版もあり (L153-157)。ロゴなし時は cuttr_code=3+addtr_code1=3 (隣接50秒以下) で予告認識 (L185-200)。
- AT-X (JL_ATX.txt): **ロゴは無効化** (「ロゴは無効化」コメント、LogoOff 相当) し、放送休止お知らせ対策として「先頭の20秒/15秒以上の無音区間をカット」(len_infohead_list) (yobibi現行版)。Amatsukaze同梱版は「ロゴは無効化」のみの旧版。
- **番宣がCM区間の始端に含まれる問題へのJLSの態度**: 末尾では「tr1stprd=45秒: 45秒以下の構成が現れるまで予告にしない」により、CM直前の45秒超構成は本編側に置かれる。先頭では同等の保護は存在しない — ただし setFirstArea は「ロゴ位置/指定位置/確定構造」を強く優先し、ZoneFirst・LgCutFirst(12秒) で先頭ロゴなし区間のCM化を制限する。**「ロゴあり番宣」はロゴ期間として本編側、「ロゴなし番宣」はCM側に分類される**ので、ロゴ検出の精度がそのまま番宣の運命を決める構造。

### b-6. 局別・状況別スクリプトの差分

| スクリプト | 特徴 (出典: 各JLファイル) |
|---|---|
| JL_標準.txt | 汎用。LogoLevel=6, LogoRevise=23, autocm_code=6, autocm_sub=10(45秒除外), cuttr_code=2+limit=1, CutTR=0, AddUC=0, 先頭提供 1001/21/1004/206, 末尾提供 421/1004/206/4001, EC 31303/11223/11023/11203, fhead 0-13:30, ftail 0-7:00 |
| JL_フラグ指定.txt | 標準をCallしつつ、構成結合フラグ (fAMcN15/30/60/90/120 系: ロゴなし60秒=15+45秒構成の結合等) と末尾n分系フラグ (vTC120C1M等) を追加 |
| JL_ロゴあり番組提供.txt | ALSP フラグのみ追加 (番組提供はロゴありに限定) |
| JL_NHK.txt | CMなし局向け。最初のロゴ前/最後のロゴ後を強制カット、末尾短ロゴ区間6回Abort、長時間(10分)番宣対応、TRLONG/TCTR/TRTR/TPTRLIM2 フラグ、cuttr_code=3。LogoLevel/LogoRevise は設定しない (ロゴ情報は位置確認のみに使用) |
| JL_MBS.txt | param_cuttr=1 (予告終了後のロゴ付き15秒単位構成をカット — 他番組宣伝対策)、WLogoTRMax=2:50、提供は6-14秒、末尾提供 code1=21/1004 |
| JL_ATX.txt | ロゴ無効化+放送休止告知カット (現行版は複数話分割機能を含む; Amatsukaze同梱は旧版) |
| JL_従来標準.txt | Auto系を使わない、Find/Abort/Select の羅列。最初のロゴ直後 3/5秒、ロゴ前 8/10秒、最後のロゴ前後 15/30/45秒列挙等、局タイトル・提供・エンドカードを**固定秒数の距離パターン**で処理 |
| JL_構成30分単位.txt | 30分枠複数話の分割 (DivFile/-odiv)。fdiv 23:00-30:30、分割位置候補はロゴ立上り優先/推測構成先頭優先 |
| JL_ANYSEL.txt | 環境変数 (TS_IN_PATH/SERVICE_ID) から 1. `{ts}.trim.avs` 2. `{ts:r}.keyframe` 3. ChList.csv+ChParamJL1/2.csv (放送局×タイトル正規表現 → JL_RUN/OPTIONS/FLAGS/COMMENT) でスクリプトを選択 |
| JL_位置読込.txt | .trim.avs/.keyframe 直接読込。**type_trim_in: 0=なし 1=list 2=avs-Trim 3=keyframe** (既定2)。keyframe は先頭末尾カット+内側1フレーム。use_autodet=0 なら `LogoDirect`+`Force B`+`AutoUp -code 1` で位置をそのまま確定、1 なら FINALIZE で OUTDIRECT 反映+チャプター追加 (L89-113) |

### b-7. nekopanda のチューニング (本家JLS対する差分)

- **nekopanda/join_logo_scp (2018, ver3.06n系)** の変更 (修正内容.txt): **DivFile コマンド追加** (→ -odiv、Amatsukazeの複数話分割で使用) のほか、ver3.05-3.06 として「開始マージン不具合修正 (ZoneFirst)」「AddUCパラメータが機能していなかった」「ロゴ有無と認識方法の初期内容不一致」「音声シーンチェンジカウントの変更」「EOF時の最後の構成追加 (`param_zone_last`)」「最初からロゴがある時の開始マージン最大期間 (`param_lgcut_first`)** 等。**現在の yobibi master (v5.1.1) にも DivFile・param_zone_last・param_lgcut_first は取り込まれている** (join_logo_scp/src/JlsScript.cpp L2399 DivFile、JL_標準.txt に該当変数あり) — つまり nekopanda の追加機能は本家に統合済み。
- Amatsukaze 0.9.1.4 同梱の JL 一式 (13ファイル、JL_common.txt 無し) は **JLS v3.06n 世代のスクリプト**: JL_標準/NHK/MBS/従来標準/ロゴあり番組提供の**設定値は現行 yobibi 版と同一** (diffで確認: 差分は MemCall フック・-NR 表記という世代差のみ)。JL_構成30分単位/JL_ATX のみ現行版が大きく進化。同梱 exe のバナー表示は "join_logo_scp ver3.0" (strings による確認; **正確な版数は推測**: JL ファイルの表記世代と DivFile 収録から ver3.06n 系と判断)。
- **Amatsukaze が JLS に渡す独自オプション**: -oscp / -odiv を常に渡す (本家単体利用では任意)。ロゴは「logoframe.exe -oa 互換だが fade=0 固定・ALL の簡易形式」を渡すため、JL_common.txt の `intype_logo_amt` 判別 (ファイル名 logof[0-9]) は ver5.1 でのみ有効。
- rigaya版 Amatsukaze (2026) は同梱スクリプトを JL_ANYSEL.txt + ChList.csv (サービスID→局→スクリプト) に拡張し、`--copy-trimavs`、`--min-output-duration`、`--cm-cut-offset`、`--no-logo-in-cm`、自動ロゴ検出等を追加 (後述 c-2, d-2)。

---

## (c) パラメータ表

### c-1. JLS エンジン内部既定値 vs JL_標準.txt の上書き値

SetParam変数 (JlsDataset.cpp L30-53 既定値、JL_標準.txt L188-214 上書き):

| 変数 | エンジン既定 | JL_標準の値 | 意味 (Autocutメモ L296-356) |
|---|---|---|---|
| CutTR | **1** | 0 (param_cuttr) | 予告終了後のロゴ付き15秒単位構成 0:残す 1:カット |
| CutSP | 0 | 0 | 番組提供カット |
| AddUC | 0 | 0 (param_adduc) | ロゴなし不明構成 0:制御なし 1:残す (不明=≥WLogoRevMin のロゴなし) |
| TypeNoSc | 0(自動) | 0 | SCなし無音位置のCM判断 0:自動(ロゴあり時のみ使用) 1:不使用 2:使用 |
| CancelCntSc | 0 | 0 | 無音SC数が多い構成内の分離 (0:間引き有効 1:無効) |
| WLogoTRMax | 120秒 | **2:10** | ロゴ期間が超えたら本編扱い (予告とみなす上限) |
| WCompTRMax | 60秒 | **1:10** | 1構成が超えたら本編扱い |
| WCompFirst | 0 | **30.0秒** | 先頭構成ロゴなし扱いの最大期間 |
| WCompLast | 0 | **30.5秒** | 最後構成ロゴなし扱いの最大期間 |
| WLogoSftMrg | 4200ms | **3.5秒** | ロゴ期間が通常より少し短い場合の延長補正上限 |
| WLogoSumMin | **20秒** | 20秒 | ロゴ期間合計がこれ未満ならロゴなし扱い (自動LogoOff) |
| WLogoLgMin | **4.5秒** | **20.0秒** | 推測時のロゴ有情報確定期間 (ロゴ長がこれ以上なら次のCM探索をロゴ終了から開始, mkReformAllLogo L262) |
| WLogoCmMin | 40秒 | 40.0秒 | 推測時のロゴ無情報確定期間 |
| WLogoRevMin | **185秒** | **125.0秒** | CM構成ではないロゴなし期間が不明構成と認識する最小期間 (AddUC=1対象) |
| MgnCmDetect | 1500ms | **0.25秒** | CM構成で15秒単位ではない可能性と認識する誤差 |
| MgnCmDivide | 500ms | **0.1秒** | CM構成内部分割を許可する誤差 |
| WCompSpMin/Max | 6秒/13秒 | 6/13 | 番組提供認識の標準秒数範囲 (内部処理用) |
| LogoLevel | 0(未設定→7) | **6** | 構成推測時のロゴ使用レベル 1-8 |
| LogoRevise | 0 | **23** | ロゴ端CM化2 + ロゴ外広域20 |
| AutoCmSub | 0 | **10** | ロゴなし時補助: +1 先頭15秒単位は少数でもCM化 / +10 45秒除外 |
| ZoneFirst/ZoneLast | -1 | 0.0 / 0.0 | 先頭/末尾から指定期間内に全体が収まる構成はロゴなし扱い |
| LvPosFirst/PosFirst | 0/-1 | cutbaselogo(1)/$base | 先頭位置優先設定 |
| LgCutFirst | -1 | **12.0秒** | 最初からロゴありの時の開始マージン最大期間 |
| msecValLap1/Lap2/Spc/Exact/Near1/Near2/Near3 (固定値) | 700ms/2500ms/1200ms/100ms/180ms/350ms/1200ms | - | 候補間引き/探索窓/精度の各種閾値 (JlsDataset.hpp L393-400) |

### c-2. Amatsukaze GUI日本語ラベル → 内部パラメータ対応

| GUIラベル (ProfileSettingPanel.xaml ほか) | 内部/CLI (EncodeServer.cs L1279-1531, AmatsukazeCLI.hpp) | 意味・既定値 |
|---|---|---|
| チャプター・CM解析を無効にする | DisableChapter → `--chapter` を付けない | 解析の ON/OFF |
| JoinLogoScpコマンドファイル | JLSCommandFile → `--jls-cmd "JL\*.txt"` | 使用するJLスクリプト。「チャンネル設定に従う」= ServiceSettingElement.JLSCommand (局別指定可) |
| (同)オプション / 「チャンネル設定に従う」チェック | JLSOption/EnableJLSOption → `--jls-option "…"` | JLS 起動オプション (例 `-CutMrgIn 150 -flags TRTR`) |
| chapter_exeオプション | ChapterExeOptions → `--chapter-exe-options "…"` | chapter_exe への `-m -s -b -e` 等。既定空 (= -m 50 -s 10 -b 60 -e 1) |
| ロゴ検出判定しきい値を低くする | LooseLogoDetection → `--loose-logo-detection` | ロゴ一致率しきい値 0.1→0.03 (CMAnalyze.hpp L301)。tooltip「ロゴ区間が短いなどでロゴ無し判定を食らう場合」 |
| ロゴ検出に失敗しても処理を続行する | IgnoreNoLogo → `--ignore-no-logo` | NoLogoException を抑制しロゴなしモードで続行 (rigaya版は自動ロゴ検出も試行) |
| ロゴ消ししない | NoDelogo → `--no-delogo` | AMTEraseLogo を掛けない |
| 最初からN%と最後からN%にPMT更新がある場合はCMとする | EnablePmtCut/PmtCutHeadRate/PmtCutTailRate → `--pmt-cut 0.1:0.2` 等 [0:0] | a-1-7参照。前頭/末尾の指定%以内のPMT変化+60フレーム内SC一致位置で本編区間を刈り取り |
| ロゴ最長フェードフレーム数 | EnableMaxFadeLength/MaxFadeLength → `--max-fade-length` [16] | ロゴ消しのフェード追従幅 (2秒=60, 3秒=90) |
| 追加ロゴ消し用ロゴリスト | AdditionalEraseLogo → `--erase-logo` 複数可 | CM解析メインロゴとは別にロゴ消しのみへ適用 |
| チャンネルごとの設定「ロゴなし」 | LogoSetting "### NO LOGO ###" (+From/To期間) | 該当サービス・期間はロゴファイル不使用=ロゴなし経路固定 |
| Trim情報入力 (`*.ts.trim.avs`) | `--trimavs` (AmatsukazeCLI.hpp L78, L403) / JL_ANYSEL では JLS 内から直接読む | CM位置を手動指定。メインファイルのCMカット出力のみに適用 |
| --- 以下 rigaya版 (ProfileSettingPanel.xaml L811-888) --- | | |
| CM解析でロゴを使用しない | NoLogoInCM → `--no-logo-in-cm` | ロゴ解析をスキップしロゴなしモードでCM解析。ロゴ消しする場合はCM解析**後**にロゴ解析 (CMAnalyze.cpp L99-106)。**JLスクリプト内に LogoOff 記述があれば自動的に同じ挙動** (logoOffInJL) |
| 出力する最短区間 (最短区間の設定 N秒) | EnableMinOutputDuration/MinOutputDuration → `--min-output-duration 秒` [5, 最小1] | 指定秒未満の分割出力区間を作らない (音声フォーマット切替による細分化抑制) (AmatsukazeCLI.hpp L64。rigayaブログ「1.0.9.0の更新告知」) |
| CMカット点をNフレーム内側にする | EnableCmCutOffset/CmCutOffsetFrames → `--cm-cut-offset` | CM境界で本編を指定フレーム多めにカット (trimavs読込時は無効) (AmatsukazeCLI.hpp L72-73) |
| 並列ロゴ解析 | ParallelLogoAnalysis → `--parallel-logo-analysis auto/数値` | ロゴ解析のマルチスレッド化 |
| (ログ上) CM解析でロゴを使用: する/しない | 上記 NoLogoInCM のログ表現 | |
| 自動ロゴ検出 (既定ON) | `--auto-logo-detect 1` + `-search-frames 10000 -div-x 5 -div-y 5 -block-size 32 -threshold 12 -margin-x 6 -margin-y 6` | ロゴ不一致時の自動ロゴ生成試行 (後述 d-2) |

注意: **logo_level / param_adduc は Amatsukaze のGUI項目ではなく JL スクリプト (JL_標準.txt L56, L74) の `Default` 変数**。GUIから変えるには「JoinLogoScpオプション」に `-set logo_level 7` 等を渡すか、JL ファイルを編集する。**CutMrgIn/CutMrgOut/CutMrgWI/CutMrgWO/LogoOff は JLS 起動オプション/スクリプトコマンド** (readme.txt L57-74, 全般 L512-521)。type_trim_in は JL_位置読込.txt L23 の変数。

### c-3. chapter_exe / ロゴ解析のしきい値パラメータ

| パラメータ | 既定 | 意味 |
|---|---|---|
| -m | 50 | 無音判定サンプル絶対値閾値 (1..32768) |
| -s | 10 | 最低無音フレーム数 (これ未満の無音は無音区間にならない) |
| -b | 60 | 長無音内でシーンチェンジを確定する間隔フレーム数 |
| -e | 1 | 無音区間の前後に探索を拡げるフレーム数 |
| (固定) space_sc1/2/3 | 5/10/30f | SC候補間の最低間隔 |
| (固定) THRES_RATE1/RATE2 | 300/7 | 明確SC / 変化なしの判定値 (rate_sc 0-1000) |
| AMTLogo系 maskratio | 0.35 (CMAnalyze.hpp L291), 0.1 (ReMakeLogo L931), GUIロゴ確認 35 (AMTAnalyzeLogo Create L1232) | ロゴ評価に使う特徴点画素比率 |
| LogoFrame THRESH | 0.2 (LogoScan.hpp L1538) | corr評価の不明/ありなし境界 |
| LogoFrame threshL | 0.5 (L1695) | MinMax判定用しきい値 |
| LogoFrame avgDur/medianDur | 1.0秒/0.5秒 (L1697-1698) | 移動平均/メディアン窓 |
| logoRatio しきい値 | 0.03 / 0.1 (CMAnalyze.hpp L301) | ロゴ一致判定 (loose/短時間で 0.03) |
| maxFadeLength | 16f | ロゴ消しフェード追従幅 (AMTEraseLogo Create L1517) |

---

## (d) ロゴ自動生成の方法

### d-1. nekopanda (0.9.x) のロゴ採取 — 半自動 (矩形手動指定)

事実出典: README.md L151-171, LogoScan.hpp LogoAnalyzer L794-1098, LogoAnalyzeModel.cs/ViewModel。

- **必要素材**: ユーザがフレーム上でロゴを囲む矩形を選択 (2の倍数に丸め)。しきい値 thy を {8,10,12,15} (既定12) から選択、最大フレーム数を {3000,5000,10000,20000,50000} (既定20000) から選択 (LogoAnalyzeViewModel.cs L115-152)。「**ロゴ周辺の背景が単一色になっているフレームが必要**。失敗する場合はアニメとかで試してください」 (README.md L251-253)。途中で映像フォーマットが変わるTSは「ロゴ生成(TS解析あり)」(サービス絞り込みTS) を推奨 (L254-259)。
- **アルゴリズム** (LogoScan.hpp):
  1. `AddFrame` (L594-659): 各フレームで**ROI周辺1画素枠** (上下左右の縁) の輝度・U・Vそれぞれの min-max 差が **thy 以下なら「単一色背景」**と判定し、枠の med_average (中央寄り平均, L414-428) を背景色として回帰サンプルに追加。**単一色でないフレームは学習から完全に除外**される — これが空・壁・走査線混入への第一の防壁。
  2. `LogoColor`/`GetAB` (L344-396): 画素ごとに (前景値, 背景色) の線形回帰 (XY入れ替え両方向の平均, approxim_line — MakKi氏 delogo-aviutl 拝借, ファイルヘッダ L8-10)。ロゴモデルは**1画素あたり A, B の2係数** (観測値 fg と背景 bg から `fg = A*bg + B` … 実装は `bg = a*fg + b*maxv` 形式、EvaluateLogo L242-254 でロゴ除去に使用)。
  3. `InitialLogoCreator`: 有効フレーム (単色背景フレーム) を UtVideo (ULH0) 可逆圧縮で `logotmp*.dat` に一時保存しつつ初期ロゴを生成 (L814-915)。
  4. `ReMakeLogo` を**2回**実行 (ScanLogo L1058-1079): 仮ロゴ (maskratio 0.1) で各保存フレームを fade 0.0-1.9 (0.1刻み20段階) で評価し、**|評価値|最小の fade をそのフレームのロゴ強度**と推定 (L962-973)。**fade > 0.8 のフレーム (=ロゴが乗っていると推定されたフレーム) だけで再回帰**してロゴを作り直す (L1012-1031)。2回目はさらにそのロゴで再選別。
  5. `GetLogo(clean)` (L490-566): 回帰後、calcDist (A,B からの距離指標) を 3回の maxfilter で膨張し、**dist < 0.3 の画素を「ロゴなし」に戻す**クリーンアップ (L548-562) — ノイズ成分除去。
  6. .lgd として保存: **AviUtl .lgp 互換ヘッダ (LOGO_FILE_HEADER + LOGO_HEADER + LOGO_PIXEL配列) + Amatsukaze 拡張部 (独自ヘッダ magic 0x12345 + Y/U/V の A/B float配列)** (AMTLogo.hpp Save/Load L169-279)。ToOutLGP 変換も MakKi氏由来 (L8-10)。
- **失敗条件** (そのまま例外になるもの): 回帰不能 (A が NaN/Inf/0) で GetLogo が nullptr →「Insufficient logo frames」 (L846-849, L1033-1035)。有効フレーム数 < 要求で上記に至る。
- **信頼度判定**: 採取直後には無い。**使用時**に LogoFrame の selectLogo (残渣×検出率逆数スコア) と logoRatio≥0.03/0.1 で判定し、不一致ならロゴ生成待ち (nekopanda) または自動再生成 (rigaya)。サーバは .lgd の serviceId 埋め込みで logo フォルダを監視し自動でチャンネル割当 (EncodeServer.cs L1852-1948, 既定期間 2000-2030年)。

### d-2. rigaya (2026) の全自動ロゴ検出 — 問題1への多数の専用対策を含む

事実出典: `Amatsukaze/LogoScan.cpp` (12583行), `LogoScanEdgeAreaPolicy.h`, `LogoScanWeakSideRescuePolicy.h`, `AmatsukazeGenLogo/AmatsukazeGenLogo.cpp`, `Amatsukaze/CMAnalyze.cpp` tryAutoDetectAndRetryLogo。

- ロゴ不一致時に自動実行 (CMAnalyze.cpp L507-693): `AutoDetectLogoRect` (最大10000フレーム、ブロック32px、分割5x5、マージン6px、しきい値12、スレッド=コア数-2上限16) で矩形を検出し、その矩形でロゴ生成 → 再評価、logoRatio がしきい値 (0.03/0.1) を超えれば採用。
- **pass1 (全フレーム粗推定)**: 画素ごとに前景/背景統計 (前景=ブロック値、背景=周辺推定 TryEstimateBg、AutoDetectStats)。**サンプルは「fg値の近いものごとに束ねて binAccum + Gompertz 重みで代表点化」** — コメント: 「静止した不透明枠や固定文字が大量に混ざると、そのままでは同じfgサンプル数が多いだけで回帰を引っ張ってしまうため」 (L3527-3531)。**時間方向パーセンタイルマップ (p5)**: 「シーン内容はシーン転換で時間p5が落ちる」のに対し持続オーバーレイは落ちない、を利用 (L3379-3393)。**セグメントコンセンサス**: 時間セグメントごとの回帰で ON/OFF 混合で薄まった α を復元、「常時静止シーンは lift が立つがゼロセグメント条件側で弾かれる」「散発的な採用はシーン内容の擬似コンセンサスとして棄却 (半径5px内に同傾向≥12近傍要)」 (L3423-3452)。**lift候補シャドウ**: pass1が本物ロゴの部分矩形にロックする問題へ、別候補を IoU≥0.85 重複排除で評価 (L3400-3416, L5884-5937)。
- **矩形化**: 2値化→連結成分 (CCL)→最良成分→サイズ/位置異常検査 (RectSizeAbnormal: 幅2-15%/高さ1.5-10%の制約はエッジエリアポリシー側、RectPositionAbnormal: マージン6px)。**細線除去 (LineTrim)**: 「文字の範囲からはみ出した細線(テロップ枠の横線など)を矩形化の前に削る」 (L3454-3455) — **走査線・帯への直接対策**。サイズ異常時のみ行帯拡張 (RowExtend)。
- **pass2 (フレームゲート)**: pass1 矩形で仮ロゴを作り、全フレームの corr0/corr1 (本体LogoFrameと同じ評価式, kThresh 0.2/kThreshL 0.5, L5572-5579) で「ロゴありフレーム」だけ再収集して矩形を再推定。**受入フレーム不足 (TooFewAcceptedFrames) なら pass1 結果に rescue blending して戻す**; pass2矩形が大きくズレた (Pass2RectDiverged) は rescue 無効 (L4436-4453 のコメント)。さらに「弱いpass2フォールバック」を明示的に棄却 (rejectWeakPass2FallbackIfNeeded)。
- **エッジエリア・フォールバック** (LogoScanEdgeAreaPolicy.h, PickEdgeArea L161-218): 通常成分が選べなかったとき、**「縁(エッジ)強度の出現率」の塊**で矩形を救済:
  - 強い縁 = 強度上位 **1%** (kEdgeAreaTop=0.01)。コメント: 「上位2%では中身の滲みを拾ったため上位1%だけを縁とする」。
  - **棄却条件1 (WeakEdge)**: 上位1%値 < 0.15 (床上げ) または **< 中央値×3倍** (kEdgeAreaRatio=3.0, L175-177) — 「ロゴなしでも上位1%が取れる」平坦な画像 (壁・空) を比で弾く。
  - blob化は 4px 膨張+上下左右4連結、**サイズ制限: 幅2-15%・高さ1.5-10%** (kEdgeAreaMin/MaxWidth/Height) — テロップ帯・大型帯を弾く。
  - **棄却条件2 (InsufficientShare)**: 最良塊 (16px以内の近傍塊は併合) が全強縁の **65%未満**なら不採択 (kEdgeAreaShare=0.65, L209-213) — 「静止背景との競合を比だけで分離できず、強い縁の65%以上を必要とした」。
  - 各辺から強縁2.5%ずつ trim して滲みを除去 (kEdgeAreaTrim)。
- **弱点成分救済** (LogoScanWeakSideRescuePolicy.h): アンカー矩形の側面の弱い細線成分を、**アンカー面積比 (area*8 ≤ anchorArea)、アスペクト3-16、面積≤256px、距離≤16px、6種のゲート (bgVarGain≥0.80, presenceGain≥0.45, magGain≥0.05, consistencyGain≥0.20 等)** すべて満たす時のみ戻す — 側面の走査線・別ロゴ・背景成分を弾く。
- この一連の定数は実局 (denpa=電波系チャンネル?, テレビ朝日, BS日テレ) での試験で調整されたことが各コメントに記されている。

### d-3. ロゴ評価 (照合) の方法 — logoframe相当部の詳細

事実出典: LogoScan.hpp LogoDataParam/LogoFrame (nekopanda版), rigaya版 LogoScan.cpp は同算法。

- **.lgp/.lgd との演算**: ロゴの A/B 係数を使い、評価対象フレーム (ROIをY抜き出し→インタレ解除 DeintY: 縦3タップ (a+2b+c)/4) に対して
  - `corr0 = EvaluateLogo(fade=0)` = **ロゴを除去しないフレームそのままと、ロゴ合成済み単色背景との 5x5 カーネル相関**。ロゴがあれば相関≈1。
  - `corr1 = EvaluateLogo(fade=1)` = **ロゴを完全除去したフレームとの相関**。ロゴがあれば除去後は平坦になって相関≈0、**ロゴがなければ逆に「負のロゴ」が現れて相関<0**。
  - 相関は「ロゴを32段階の単色背景に乗せた画像群」で作った**5x5カーネル**と注目画素の一致度 (CalcCorrelation5x5)、注目点は「ロゴ合成画像の分散上位 maskratio 割合」の画素 (CreateLogoMask L112-229)。背景明度ごとに正規化 (scales)、黒背景スコアで最終正規化 (blackScore L226-228)。**corr>1 は打ち切り、相関下限 0.2×平均以下は弱める** (corrLowerLimit=0.2, L121)。
- **しきい値 (logo_level 相当)**: 評価値そのものは0-1に正規化され、Amatsukaze では **THRESH=0.2 を「あり/なし/不明」の境界**、**threshL=0.5 を MinMax判定**に使用 (L1536-1538, L1695)。すなわち「ロゴ検出判定しきい値」はロゴファイル側ではなく**この評価スコアに対する固定値**であり、ユーザ調整項目は maskratio (0.35) と一致率しきい値 (0.03/0.10) のみ。
- **フェードの扱い**: 学習時は fade 0-1.9 の20段階で最良値を推定 (ReMakeLogo)。照合時 (AMTAnalyzeLogo) は 0.0-1.0 の **11段階**を評価して LogoAnalyzeFrame (p/t/b 各11) に保存 (L1150-1155)、AMTEraseLogo が前後4フレームの最良フェード列を見て「急変ならフィールド別 (T/B)、緩変ならフレーム単位」でフェード値を決定 (CalcFade2 L1263-1315)。**maxFadeLength (既定16フレーム)** は、大域的なロゴあり/なし結果 (frameResult) があるとき、切り替わり境界の判定に使う窓幅 (CalcFade L1317-1141)。ロゴ検出 (writeResult) 側は corr0/corr1 のみでフェードを考慮しない。
- **ロゴ区間出力の平滑化** (writeResult L1695-1827): rawScores = max(0,corr0)+min(0,corr1)。**±0.5秒の移動最大値の前後min (MinMax)** — 「動きの多い映像でロゴがかき消されることがあるので、それを救済」— と **±0.5秒の移動平均**の2系統で tri-state (0:なし/1:不明/2:あり) を作り、**両者が不一致なら不明**。不明区間は両側が同じ値なら埋める (L1752-1765)。**境界精緻化**: 0.5秒窓メディアン score に対し ±THRESH(0.2) を前後探索して、移動平均の遅れを相殺し best/start/end の3点を出力 (L1773-1819)。

---

## (e) 依頼者 4問題点それぞれへの示唆

### e-1. ロゴ学習が空・壁・走査線を誤拾いする問題

JLS/Amatsukaze の事実としての防衛手段 (転載不可だが方式は自由に再現可):
1. **学習フレームの資格判定を「候補ROIの周辺縁」で行う** (AddFrame): 枠1pxのmin-max差が thy (既定12/255) 以下のフレームだけ学習。空・壁が ROI 内にあっても、その周辺縁が単色でなければそのフレームは使われない。ただし **「背景が本当に単色なだけのフレーム (壁全体など)」は通過してしまう** — これに対して Amatsukaze は次の2段で対処する。
2. **生成ロゴの自己評価による再学習**: 仮ロゴで全フレームの fade を評価し、**ロゴが乗っていると推定されたフレーム (fade>0.8) のみで回帰し直す**のを2回反復。誤学習ロゴでは「乗っている」フレームが少なく実質的に失速する。
3. **学習と照合の評価関数共通化** (依頼者済み対応の先): Amatsukaze は学習・照合とも同じ EvaluateLogo 相関カーネルで、**さらに「ロゴ候補採用」自体に (平均残渣)×(検出率の逆数) スコア** (selectLogo) を使い、logoRatio 3%/10% を下げればロゴなし扱い。**JLS 側にも WLogoSumMin=20秒 (ロゴ合計20秒未満なら全體ロゴなし化) という最終保護**がある (JlsScrFuncReg.cpp L1250)。
4. **rigaya 版の直接的対策リスト** (d-2): 走査線→細線除去 (LineTrim)、帯→blobサイズ上限 (幅2-15%/高さ1.5-10%) と縁独占率65%、壁/空→強縁上位1%が中央値×3倍必要、常時静止画→時間p5リフトとゼロセグメント棄却、散発擬似→空間整合 (近傍12画素)、部分ロゴ誤拾い→lift候補シャドウ (IoU重複排除)。「実局で調整した値と理由がコメントとして残っている」点が参考になる。
5. **(推測)** 依頼者の「画面上下の帯から時間的に安定した画をロゴ候補として学習」方式は、Amatsukaze 流の「単色背景フレーム限定」に相当する事前フィルタを持たないため、安定した非ロゴ領域がそのまま回帰に入る。JLS 系の最重要ポイントは「**ロゴは fg と背景の回帰直線として推き、ロゴでない安定画は回帰の傾きが1/切片0 (すなわちdist計算で消える)**」ことにある: GetLogo の clean 処理 (calcDist<0.3 → A=1,B=0 に戻す) は実質「安定画をロゴから削除する」工程。依頼者の勾配テンプレート相関でも「テンプレートの重みを回帰分布 (FG-BG 平面上での線形性) で棄却する」段を入れるのが定石との示唆 (推測として記載)。

### e-2. ロゴなし経路の精度が低い (完全一致2/境界ずれ6/誤提案5/14)

JLS AutoCM の事実としての差分ポイント:
1. **境界候補の相互距離スコア**: 依頼者の「無音+SCだけで15秒単位のスポット連鎖」に対し、JLS は各無音SC位置から **±30秒5秒刻み・±3分15秒刻みに無音SCがどれだけ揃っているか**を加点 (getScore: 15秒単位21点+近さボーナス、5秒単位7点) し、≥30点のみ15秒単位候補、≥10点のみ5秒候補、700ms内は間引き。**単独の区間長判定ではなく「刻み位置の相互支持」で候補を作る**のが精度の芯。
2. **候補チェーン確定**: 継続候補/新候補の2バッファで5/15秒距離チェーンを管理し、**180秒伸びなければ確定、途切れれば除外**。局所的な1個だけの15秒一致では構成区切りにならない。
3. **連続数条件 (cutlevel)**: 既定6 = **3構成以上連続でCM、2構成は「ちょうど15/30秒」の時のみ**。1個や2個の偶発一致ではCMにならない。誤提案 (CM無いのに提案) 抑制の中核。末尾は cutlevel=10 (1構成でCM化) に自動緩和 — **本編末尾だけ甘い**設計。
4. **無音多すぎ棄却 (checkMuteMany)**: 14.5秒内に境界無所属の無音が2超等あれば CM 判定取消。BGM中のCM等で無音区間が列挙される場合の誤爆対策。TypeNoSc=2 で「SCなし無音」をCM判断から外すことも可能。
5. **45秒除外 (autocm_sub+10)**: 45秒 (30秒単位でない) は CM 外。**60/90/120秒の末尾救済 (scopex)** と「手前がCMなら継続 (scope 40秒)」。
6. **不明構成保護**: ≥WLogoRevMin(125秒) のロゴなし長区間は「不明構成」→ AddUC=1 で残す方針が取れる。
7. **境界ずれ対策**: CM確定後、各区間の15秒からの誤差 >0.25秒なら (a) 両隣と足して誤差半減するなら中間区切りを無視、(b) CM側端の5秒/10秒構造があれば**内部分割 (DINT)**、(c) 端の区切りは削除、(d) AddUC+10 なら残す側へ反転 (setCMFormDetRevise)。**「境界は必ずしも無音SCに置かない」**—— 15秒位置が無音なら **無音位置に無音SCを新規挿入** してでも刻みに乗せる (setCMFormDetect L3265-3290)。
8. **(推測)** 依頼者実測の「境界ずれ6/14」は、上記 1-2-3 (相互スコア+チェーン+連続数) を入れない限り、単発15秒区間と偶発無音の区別が構造的に難しいというJLSの設計思想から見てもっとも改善余地が大きい箇所。

### e-3. 始端が最大30秒早い (番宣をCM区間に含める)

JLS の事実:
1. **番宣を独立クラスとして持つ**: Trailer(予告/番宣・残す) / Trailer(cut) (カット番宣) / Sponsor(番組提供) / Endcard。番宣の**保護は末尾のみ** (AutoCut TR -Ftail, cuttr_limit, tr1stprd=45秒) で、先頭は「番組提供 (6-13秒, fhead 13.5分内)」「局タイトル (cut1st 3-10秒)」だけが保護対象。**先頭番宣を保護するコマンドは存在しない** — つまり JLS は「先頭のロゴなし番宣はCMの一部として切ってよい」「先頭のロゴあり番宣は本編側に残る」という立場を取る。依頼者 (常時録画でCM区間抽出が主目的) とは目的が逆の部分。
2. **先頭CM区間の始端決定** (setFirstArea): 始端は「CM列の最初の15秒単位境界」ではなく、(a) 開始マージン指定 ($base / -fromabs) に最も近い候補、(b) **最初のロゴ立ち上がり位置** (npoint=3001最優先)、(c) 確定構造・15秒単位距離 (+15000/+10000点) の順位で選ぶ。さらに (d) **ZoneFirst: 先頭から指定期間内に全体が収まる構成はロゴなし扱い**、(e) **LgCutFirst=12秒: 最初からロゴがある場合は12秒まではCM化を許容**、(f) ロゴ位置から15秒未満の候補は「正確に秒数単位 (誤差≤100ms)」でなければ候補外、(g) 確定構造から3秒未満の候補は候補外。
3. **NHK版の方式** (CMなし局): 「最初のロゴ開始以前を強制カット」を明示的に `Find ±1.2秒→±10.2秒→Force` で行う。**ロゴ先端を「本編の始まり」の定義として使う**アプローチであり、依頼者の「始端が早い」問題への最も単純な答えはこれ (自局はロゴありCM=本編にロゴがあるので、本編先頭のロゴ立ち上がりを始端锚にする方式が転用可能)。
4. **PMT更新CM認識** (Amatsukaze --pmt-cut): 番組切替 (PMT構成変化) は番宣と本編の間に高確率で起きる。前頭%/末尾%内に限定して「PMT変化+60フレーム内シーンチェンジ」を本編境界として刈り取る。**内容特徴で番宣とCMを区別できない (依頼者実測) という制約に対し、TS構造側の情報 (EPG/PMT) で始端を打つ**のが Amatsukaze の答え。
5. **(推測)** 依頼者の CM 区間始端が30秒早い現象は、JLS の用語で言えば「先頭のロゴなし番宣数個が CM 候補列に連結し、cutlevel 判定でその列がまとめて CM 化」された形。JLS が依頼者の用途 (CM 区間を切り出す) で同問題にどう対処するかという問いへの答えは (i) 末尾と同じ「tr1stprd 相当: 45秒以下の構成が現れるまでは CM 列とみなさない」を**先頭にも適用**する、(ii) ZoneFirst/LgCutFirst 相当の「先頭ロゴありなら12秒までしかCM化しない」制限、(iii) PMT/PAT 変化併用、の3方向。JLS 標準スクリプトは (i)(ii) を先頭に持たないため、必要なら自作ルールとして追加するのが妥当 (いずれも仕組みは JLS スクリプトで表現可能な範囲)。

### e-4. ロゴあり/なし両経路の相補性 (同一録画で別の正しいCM区間)

JLS/Amatsukaze の事実:
1. **同一実行内での経路併合はしない**: LogoLevel 4-8 (ロゴ使用) か 1-3/LogoOff (ロゴ不使用) の**どちらか一方**で構成推測する (mkReformAll の分岐, JlsAutoReform.cpp L37-51)。ロゴデータがあれば既定でロゴ経路 (JL_標準 LogoLevel=6)。ロゴ合計<20秒や -inlogo なしで自動的にロゴなし経路へ**切替**。Amatsukaze も「ロゴ一致するTSはロゴ経路、不一致TSはロゴなし経路」で、**同じTSで両方走らせて結果をマージする機能はない**。
2. **部分的な併合に相当する仕組みは3つ**: (a) **LogoLevel 2/3**: ロゴなしで構成推測した**最後**にロゴ情報で追加/削除のみ行う (Autocutメモ L336-338)。(b) **ロゴあり経路内のロゴなし処理**: addLogoEdge/Mix/Border で「ロゴ区間内のロゴなし部分」を分離、mkRangeCM はロゴ端±(1.2秒+3.5秒)の可能性範囲から探索。(c) **AutoUp**: 従来方式 (Find/Forceで確定したロゴ境界) の結果を推測構成に反映する橋渡し。
3. **複数ロゴの扱い**: 同チャンネルに複数 .lgd がある場合、selectLogo スコアで**最良1個のみ**採用 (CMAnalyze.hpp L298)。ただしロゴ消し用の「追加ロゴ」(--erase-logo) は別枠で全数適用。**経路口ではなくロゴ単位での並行評価**は Amatsukaze にある (複数ロゴの全部を評価し、後から最良を選ぶ — evalResults は全ロゴ分保持)。
4. **(推測)** 依頼者の「経路相補性」観測 (同一録画でロゴ経路・ロゴなし経路が別々の正しいCM区間を出す) に対し、JLS の設計から言えるのは: (i) 両経路を走らせて結果レベルでマージ (例: ロゴ経路の区間を優先し、ロゴなし経路の区間でロゴ経路CMと重ならないものを追加) は JLS に存在しない独自機能になる、(ii) ただし AutoCM はロゴあり経路と**同一の getScore/setChap 境界インフラ**を使うため、「ロゴあり区間を確定済み境界として固定し、残りをロゴなし推測する」実装は LogoLevel 2/3 の動作に近く、JLS 的には (ロゴを補助に回すことで) 半実現している。(iii) CM境界の相互検証 (両経路で一致した境界のみ採用) という方向は JLS に無い (推測)。

---

## (f) 出典一覧

### リポジトリ内 (クローン: /tmp/amatsukaze-research/)

- Amatsukaze (nekopanda): `Amatsukaze/README.md` (チャプター・CM解析 L135-176, L239-299, Trim入力 L350-365, 同梱ライブラリ L570-590) / `AmatsukazeServer/Server/EncodeServer.cs` (L1279-1283 ツールパス, L2007-2015 JLフォルダ列挙, L1400-1531 CLI引数構築) / `EncodeServerData.cs` (LogoSetting L497-528, ServiceSettingElement L530-558) / `Amatsukaze/CMAnalyze.hpp` (パイプライン・PMTカット・MakeChapter) / `Amatsukaze/LogoScan.hpp` (ロゴ学習・評価・区間出力) / `Amatsukaze/AMTLogo.hpp` (.lgd形式) / `Amatsukaze/TranscodeSetting.hpp` (Config L502-560, getter L745-807) / `Amatsukaze/AmatsukazeCLI.hpp` (ヘルプ L62-110, 既定 L180-197, PMT L97-99) / `Amatsukaze/TranscodeManager.hpp` (L540-613, NoLogo判定 L600-605) / `AmatsukazeGUI/Views/ProfileSettingPanel.xaml` (L88-95, L423-477) / `AmatsukazeGUI/ViewModels/LogoAnalyzeViewModel.cs` (L120-152, L170-215) / `AmatsukazeGUI/Models/LogoAnalyzeModel.cs` (L269-300) / 配布7z `amatsukaze-release/Amatsukaze/` (Readme.txt 更新履歴, JL/ 13ファイル, exe_files/)
- join_logo_scp (yobibi): `readme.txt` / `修正内容.txt` / `JL/JL_標準.txt` / `JL/JL_NHK.txt` / `JL/JL_MBS.txt` / `JL/JL_ATX.txt` / `JL/JL_従来標準.txt` / `JL/JL_ロゴあり番組提供.txt` / `JL/JL_フラグ指定.txt` / `JL/JL_位置読込.txt` / `JL/JL_ANYSEL.txt` / `JL/JL_構成30分単位.txt` / `JL/common/JL_common.txt` / `JL/common/JL_set_フラグ指定標準.txt` / `JL/common/JL_set_フラグ指定標準2.txt` / `JL/common/JL_out_divfile.txt` / `JL/common/JL_ini_選択読込.txt` / `JL/doc/JLコマンド説明_全般.txt` / `JL/doc/JLコマンド説明_Autocutメモ.txt` / `JL/doc/JLコマンド説明_補助解説ver5.txt` / `src/JlsAutoReform.cpp` (L37-357 推測全体, L1023-1326 detectCM, L1475-1645 mkRangeCM, L1647-2030 setFirstArea, L2030-2331 setFinalArea, L3056-3350 setCMForm系, L4134-4578 setScore/setChap, L4849-5127 getScore/checkMuteMany, L5243-5331 単位判定) / `src/JlsAutoScript.cpp` (L91-299 code解析, L1082-1510 AutoCut TR/EC, L1995-2921 AutoAdd系) / `src/JlsDataset.cpp` (L25-70 既定値, L586-720 CutMrg自動検出はJlsReformData.cpp, L3627-3680 -oscp出力) / `src/JlsReformData.cpp` (L524-720 detectCutmrg) / `src/JlsScrFuncReg.cpp` (L1235-1281 自動LogoOff) / `src/JlsScript.cpp` (L2110-2130 LogoLevel適用) / `src/JlsIF.cpp` (L353-560 入力解析) / `src/JlsNameSpace.hpp` (L18-31 ScpChapType, L36-75 ScpArType/Ext, L78-108 ConfigVarType, L305-325 ComLabelType)
- join_logo_scp (nekopanda fork): `修正内容.txt` (ver3.06n DivFile等), `src/*` (yobibi現行との差分確認用)
- chapter_exe (nekopanda fork): `README.md` / `改造版_読んでね.txt` (Yobi/sysuzu/mod3/mod4変更履歴) / `src/chapter_exe.cpp` (L51-143 オプション, L242-312 無音走査, L325-748 proc_scene_change) / `src/mvec.cpp` (L16-30 閾値定数, L80-442 判定本体) / `src/変更ファイル.txt`
- Amatsukaze (rigaya): `AmatsukazeGUI/Views/ProfileSettingPanel.xaml` (L811-888) / `Amatsukaze/AmatsukazeCLI.hpp` (L64-140 新オプション) / `Amatsukaze/CMAnalyze.cpp` (L96-140 no-logo-in-cm, L470-560 自動ロゴ検出, L686-693) / `Amatsukaze/LogoScan.cpp` (L2280-2430 失敗分類, L3374-3460 対策定数群, L4340-4460 2-passフロー, L12503- entry) / `Amatsukaze/LogoScanEdgeAreaPolicy.h` / `Amatsukaze/LogoScanWeakSideRescuePolicy.h` / `AmatsukazeGenLogo/AmatsukazeGenLogo.cpp` (L303-337 usage)

### ウェブ

- rigayaの日記兼メモ帳「Amatsukaze 改造版 1.1.0.0」: https://rigaya34589.blog.fc2.com/blog-entry-2177.html?pc= (「出力する最短区間」の由来と既定5秒、「CM解析でロゴを使用: する/しない」ログ表記、JL_ANYSEL運用例)
- rigaya/Amatsukaze README: https://github.com/rigaya/Amatsukaze/blob/master/README.md (環境変数 CLI_IN_PATH/TS_IN_PATH/SERVICE_ID のJLSへの受け渡し、Linux対応)

---

## (g) 不明点リスト

1. **logoframe.exe 単体 (yobibi配布のロゴ検出ツール) のソース**: 本次のクローン対象外 (Amatsukaze は内蔵実装で代替しており、JLS は -inlogo のファイル形式のみを要求するため影響なし)。ロゴ評価自体は Amatsukaze 内蔵 LogoScan.hpp (MIT) で代用記載したが、logoframe.exe 独自の閾値 (logo_level 相当のオプション名が yobibi 配布のlogoframeに存在するか等) は未確認。
2. **Amatsukaze 同梱 join_logo_scp.exe の正確な版数**: バナー文字列は "ver3.0"、DivFile 収録・JLスクリプトの表記世代 (MemCall無し・-N表記) から **nekopanda fork ver3.06n 系と推測** (推測)。バイナリの詳細バージョン確認は未実施。
3. **Amatsukaze 0.9.1.4 同梱exeが ver3.0x 系であることの影響**: 現行 yobibi v5.1.1 とは MemCall フック・JL_common.txt (-setup)・-syscode 等の機能差がある。**本報告の b 章の規則記述は現行 v5.1.1 ソースを主典拠としており、Amatsukaze 同梱版ではこのうち MemCall 系・ver5 専用機能が無い点に注意** (設定値・Auto系コマンドの中核は diff により同一と確認済み)。
4. **ChList.csv / ChParamJL1/2.csv の実配布内容**: リポジトリでは空テンプレート。rigaya 配布7zには実データが入っていると推測されるが未取得 (制約により /tmp 外参照不可という程でもないが、今回の成果物に必須でないため未取得)。
5. **rigaya版の「AVFrame直接ロゴ解析」の内部差**: ログ表記は確認したが、従来AVS経由に対しFFmpeg AVFrame を直接評価する部分の実装詳細 (LogoScan.cpp 内にある) は今回詳細追跡していない。判定アルゴリズム本体 (THRESH 0.2 / threshL 0.5 / MinMax+移動平均) は同一であることを確認済み。
6. **JL_set_フラグ指定標準2.txt** (フラグ指定の後半) は今回一覧記載にとどめ、個別フラグ全解説は未実施 (フラグ→パラメータ対応の主要部は JL_set_フラグ指定標準.txt で確認済み)。
7. **「出力する最短区間」の実装箇所** (rigaya): `--min-output-duration` のヘルプ・ブログによる機能説明 (分割出力の最小秒数, 既定5) は確認したが、TranscodeManager 側の適用コード行の精査は未実施 (説明に必要な水位は確保済みと判断)。
8. **「PMT更新によるCM認識」のGUI表記** は nekopanda/rigaya 両版で確認済みだが、依頼者が挙げた「CM解析でロゴを使用しない」が nekopanda 0.9.1.4 に**存在しない**ことは確認済み (exe内文字列探索で不在)。同ラベルは rigaya 版由来。依頼者の環境がどちらの版かは本調査では確定不能。
