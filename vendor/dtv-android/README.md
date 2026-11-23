# dtv_android

自作 Android プログラムを **Android STB (192.168.99.26)** 上で動かすための開発環境。

STB 制御 (adb) と、ビルド → インストール → 起動 → 実機確認までが完結します。

PIX-SMB400 も同じ pix-stream 構成で動きます。筐体自身がチューナーである点だけが
X96Max と異なり、その差は capture-agent の `PIX_TUNER=smb400` バックエンドが吸収します。
手順・実測値・SMB400 固有の罠は
[docs/smb400-pix-stream-integration.md](docs/smb400-pix-stream-integration.md) にまとめています。
配備は `./tools/deploy-smb400.sh deploy`（自動起動は `boot-install`）。

---

## 1. 対象デバイス

| 項目 | 値 |
| --- | --- |
| IP / ポート | `192.168.99.26:5555` (TCP adb) |
| メーカー / 型番 | Amlogic / `p212` (Google ブランド表記) |
| Android | 6.0.1 (API 23) |
| ABI | `armeabi-v7a` のみ |
| 画面 | 1920 x 1080 / 240dpi |
| RAM / /data 空き | 818 MB / 2.4 GB |
| root | **取得済み** (`test-keys` ビルド、`/system/xbin/su` あり、`adb root` 可) |
| ランチャー | `com.droidlogic.mboxlauncher` |

補足: `android.software.leanback` フィーチャは**ありません**。Android TV ではなく、
TV ボックス用のタブレット風 UI 端末です。`android.intent.category.LAUNCHER` の
通常の Activity として起動します (LEANBACK 用 specification は不要)。

## 2. 必要環境

- JDK 17 以上 (現在 21)
- Android SDK: `~/Library/Android/sdk`
  - platform `android-35` / build-tools `35.0.0`
- Gradle 8.13 (`~/.gradle` にキャッシュ済み)

## 3. 接続と認可

adb over TCP は接続時に **「Allow USB debugging?」ダイアログを TV 画面に表示**します。
これを遥控器で承認するまで `unauthorized` 状态的ままです。

```sh
adb connect 192.168.99.26:5555
# TV 画面でダイアログを承認する
adb -s 192.168.99.26:5555 root     # adbd を root モードに
adb -s 192.168.99.26:5555 shell id # uid=0(root) ... context=u:r:su:s0
```

認可キーは `~/.android/adbkey` (2026-03-17 作成) で、他の STB (PIX-SMB400) と共通です。

補助スクリプトは、接続 → 認可待ち → root 化まで自動で行います。

```sh
./tools/stb.sh connect
```

## 4. ビルド / 配置 / 起動

```sh
./tools/stb.sh run          # build + install + launch
./tools/stb.sh build        # APK のみ
./tools/stb.sh install      # 配置のみ
./tools/stb.sh launch       # 起動のみ
./tools/stb.sh restart-app  # 停止してから起動
./tools/stb.sh status       # 端末状態のサマリ
./tools/stb.sh screenshot   # 画面キャプチャ -> artifacts/stb_screen.png
./tools/stb.sh log          # アプリの logcat
./tools/stb.sh shell <cmd>  # root でシェル実行
```

手動で Gradle を叩く場合:

```sh
cd ~/Desktop/work/dtv_android
~/.gradle/wrapper/dists/gradle-8.13-bin/*/gradle-8.13/bin/gradle assembleDebug
adb -s 192.168.99.26:5555 install -r app/build/outputs/apk/debug/app-debug.apk
adb -s 192.168.99.26:5555 shell am start -n com.example.dtvandroid/.MainActivity
```

### `android` CLI を使う場合

```sh
android install --device 192.168.99.26:5555 --use-delta-install=false \
  --apks app/build/outputs/apk/debug/app-debug.apk
```

`--use-delta-install=false` が必須です。この端末の adbd は Android 6.0 世代で
incremental install に対応しておらず、デフォルトの delta install では
`Protocol message tag had invalid wire type` で失敗します。

## 5. プロジェクト構成

```
dtv_android/
├── app/
│   ├── build.gradle              # minSdk/targetSdk 23, armeabi-v7a 固定
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── java/com/example/dtvandroid/MainActivity.java
│       └── res/layout/           # activity_main / item_info_row / item_section_header
├── tools/stb.sh                  # build / deploy / 制御の補助スクリプト
├── artifacts/                    # スクリーンショット等
└── settings.gradle, build.gradle, gradle.properties
```

### ビルド設定の要点 (`app/build.gradle`)

- `minSdk 23` / `targetSdk 23` — 端末が API 23。`targetSdk 23` だと権限が
  インストール時に付与され、TV 画面でランタイム権限ダイアログを出す必要がなくなります。
- `abiFilters 'armeabi-v7a'` — 端末が armeabi-v7a のみ対応。
- Kotlin 依存の整列: androidx が `kotlin-stdlib 1.8.22` を、
  `kotlinx-coroutines-android 1.6.4` が `kotlin-stdlib-jdk8 1.6.21` を引き、
  同一クラスが重複するため `resolutionStrategy.force` で jdk7/jdk8 を 1.8.22 に揃えています。
- release ビルドは debug 署名を流用 (sideload 用)。

## 6. 実機確認済みの内容

1. `assembleDebug` 成功。APK は `minSdk 23` / `targetSdk 23`、
   launchable-activity `com.example.dtvandroid/.MainActivity` を含む。
2. `adb install -r` 成功 (`Success`)。
3. `am start` で起動し、`mCurrentFocus` が本アプリの Activity になる。
4. プロセス `com.example.dtvandroid` が常駐 (PID 取得・`ps` で確認)。
5. `screencap` で 1920x1080 のスクリーンショットを取得し、
   実画面上に **"RUNNING" / uptime 00:00:11** とデバイス情報が描画されることを確認。
6. uptime カウンタが 00:00:40 → 00:00:45 → 00:00:51 と増加 = 実机上での
   継続実行を確認 (起動しただけの一瞬の描画ではない)。
7. `AndroidRuntime` の FATAL ログなし。

### 途中で遭遇した不具合

- `String.format("%.1f", dm.densityDpi)` は `Integer` を渡したため
  `IllegalFormatConversionException` で起動時クラッシュ。
  `%d` に修正して解決済み。

## 7. メモ

- 別端末 `192.168.99.43:5555` は PIX-SMB400 (Android 8.0 / API 26) の
  Mirakurun 運用機です。`adb devices` には両方が出るので、作業時は
  `-s <serial>` を必ず付けてください。
的另一台の STB `192.168.99.43:5555` は PIX-SMB400 (Android 8.0 / API 26) の
Mirakurun 運用機です。`adb devices` には両方が出るので、作業時は
`-s <serial>` を必ず付けてください。
- `/system/xbin/su` があるため `adb shell` から root コマンドを直接実行できます。

---

# 付録: STB の整理 (メモリ削減)

自作アプリ以外を削除し、最小限構成にしました。**すべて復元可能です。**

## 整理前 -> 整理後

| 指標 | 整理前 | 整理後 | 変化 |
| --- | --- | --- | --- |
| MemAvailable | 356 MB | **589 MB** | **+233 MB (+65%)** |
| 空き /data | 2.4 GB | **3.7 GB** | +1.3 GB |
| インストール済みパッケージ | 94 | **41** | -53 |
| 常駐アプリプロセス | 25 | **11** | -14 |

## 消したものとその理由

**third-party アプリ (14 本 / 全削除、APK はバックアップ済み)**
Netflix, Kodi, Chrome, Twitter, Skype, Facebook, YouTube TV, Mobdro,
SpiralRoll, AirPlay DMR, ZappTV, InfiniteZoom, ColorsLiveWallpaper,
Barry's Exceptions。`/data/app` 合計 **1.1 GB** を回収。

**Google Play Services 系 (最大の原因)**
`com.google.android.gms` は `privateFlags="8"` (FLAG_PERSISTENT) なので
`pm uninstall --user 0` が効かず、**`pm disable` で停止**した。
起動していない状態で **約 127 MB** を占有していた (persistent 62MB +
通常 36MB + unstable 14MB + Play Store 30MB + instant_app_installer 10MB)。

**OEM バニップ / テストアプリ**
FOTA 自動更新 (amediatech.fota, adups.fota, sysoper)、Miracast、DLNA、
音楽/カメラ/ギャラリー/時計、内蔵ブラウザ、`dig` (地上波チューナー -
本機にはチューナー硬件が無い)、工場試験アプリ (aging, factorytest, ProductTest)。

**常駐サービス**
- `com.droidlogic.PPPoE` - WAN は通常 DHCP (`ppp0` インタフェースは存在しない)、
  かつ `ro.platform.keepbackground.app` で常駐化されていた。build.prop から
  当該行を削除し、`pm disable` で停止。
- `usbpm` (`/system/bin/usbtestpm`) - crash-loop 状態だったため `stop usbpm`。
- `com.android.documentsui` / `com.android.providers.calendar` - 未使用。

**OEM ランチャー**
`com.droidlogic.mboxlauncher` (約 30 MB) を廃止し、**自作アプリを HOME アプリ
として登録**しました (`AndroidManifest.xml` に HOME/DEFAULT intent-filter)。
起動するとそのまま自作アプリが開き、HOME ボタンでも同じです。

## 実行方法

```sh
cd ~/Desktop/work/dtv_android
./tools/cleanup.sh third       # third-party をバックアップして削除
./tools/cleanup.sh system      # system アプリ整理 + persistent を disable
./tools/cleanup.sh state       # 現在の状態を確認
./tools/restore.sh all         # 元に戻す
```

## 重要な注意点 (実際に踏んだ罠)

### 1. persistent アプリは uninstall するとクラッシュループする

`android:persistent="true"` のパッケージ (GMS, PPPoE, SubTitleService,
mboxlauncher, remotecontrol, inputmethod.remote) に対して
`pm uninstall --user 0` をすると:

- ActivityManager が常駐プロセスとして**自動再起動**するため
  `am force-stop` や `kill -9` は数秒で元に戻ります。
- パッケージが user 0 に対して uninstalled になると、再起動したプロセスが
  自分のパッケージ情報を解決できず**クラッシュ**します:
  `Unable to get package info for <pkg>; is package not installed?`
- そのクラッシュダイアログが **HOME インテントを奪い**、端末が壊れたように
  見える状態になります (PPPoE で実際に発生しました)。

**対策: persistent パッケージは `pm disable` のみ使い、`uninstall` しません。**
`cleanup.sh` は `RETIRE_SYSTEM` (uninstall 可) と `RETIRE_PERSISTENT`
(disable のみ) を分けて管理しています。

### 2. `pm list packages -u` は「インストール済みか」の判定に使えない

`-u` は user 0 に対して uninstalled なパッケージも**列挙**します。
判定には素の `pm list packages` を使ってください
(cleanup.sh / restore.sh の `installed_set` がこれ)。

### 3. 復元には `--user 0` が必須

`pm install -r <apk>` だけでは user 0 の状態は `installed=false` のままになり、
persistent アプリが前述のクラッシュ.loop に入ります。正しい手順は:

```sh
pm install -r --user 0 /system/app/<Name>/<Name>.apk
pm enable --user 0 <pkg>
```

`restore.sh` はこの手順で実装済みで、`com.android.browser` で往復検証済みです。

### 4. `pidof` はこの端末では引数を無視する

`pidof <name>` が**端末上の全 PID** を返します (Android 6 の toybox の挙動)。
`kill -9 $(pidof X)` は unintended に全プロセスを kill します
(実際に出典 adbd まで kill して端末が offline になりました)。
必ず `ps` 出力から PID を抽出してください:

```sh
PID=$(adb shell "ps" | tr -d '\r' | awk '$NF=="<procname>"{print $2}')
```

## build.prop の変更 (2 箇所)

| 変更 | 理由 |
| --- | --- |
| `ro.platform.keepbackground.app=com.droidlogic.PPPoE` を削除 | PPPoE の常駐化の根拠を消した (reboot 後にも効かない) |
| `service.adb.root=1` を追記 | 再起動後も adbd が root で立つことを保証 (`ro.build.fingerprint` が `userdebug` なので通常は自動) |

`service.adb.tcp.port=5555` と `ro.adb.secure=1` は**変更していません**
(これがあると adb over TCP への接続経路が失われます)。
元の build.prop は `artifacts/backup/build.prop` にバックアップ済みで、
`./tools/restore.sh prop` で戻せます。

> 注意: 端末の toybox `sed` は置換文字列中の `\n` を**リテラル NUL バイト**
> として書き込みます。build.prop は Mac 側で編集してから push してください
> (NUL 混入は Android の property parser を壊します)。

## 再起動後検証 (2 回実施)

1. 再起動 → `adb` で再接続 → **root が自動で復帰** (`uid=0`, `u:r:su:s0`)
2. HOME ボタン → **自作アプリが起動** (クラッシュダイアログなし)
3. `logcat` に `FATAL EXCEPTION` / `Unable to get package info` **0 件**
4. `pm list packages` = 41、`pm list packages -d` = 6 (意図した disable のみ)
5. `su -c id` → `uid=0(root)` 取得可
6. ネットワーク: `ping 8.8.8.8` 応答あり

## 残しているもの (意図的)

`com.android.systemui` (ステータスバー)、`com.android.tv.settings` (設定画面)、
`com.droidlogic.service.remotecontrol` (IR リモコン)、
`com.droidlogic.inputmethod.remote` (リモコン用 IME)、
`com.thirdparty.superuser` (root 管理 UI)、`com.google.android.webview`、
framework 系 provider 群、`com.android.providers.media` (MediaStore)。
いずれも 4 MB 前後で、削っても得られるメモリ LanguageGain は小さく、破壊のリスクが
はるかに大きいため残しています。

---

# 付録2: 追加の RAM 削減 (2 巡目)

1 巡目 (356MB → 589MB) の後もう一段階進め、**MemAvailable 611MB** まで到達しました。

| 追加で停止したもの | 効果 | 備考 |
| --- | --- | --- |
| `com.android.providers.media` (メディアスキャナ) | ~8 MB | `pm disable` が効く (persistent ではない) |
| `com.thirdparty.superuser` (Superuser UI) | ~4 MB | **`su -c id` は引き続き root を取得できる**ことを実測確認 |
| `com.android.defcontainer` (既定の「開く」ハンドラ) | ~3 MB | 機能損失は小さい |
| `com.android.inputmethod.latin` (AOSP IME) | ~8 MB | 既定 IME を **RemoteIME** に切替済み (IR リモコン運用なので AOSP IME は不要) |

合計で **約 23 MB**。2 巡目以降の追加効果はこれ以上大きくならない見込みです。

## なぜこれ以上は「難しい」のか (実測した理由)

残りの 3 大プロセスは **Android フレームワーク本体**で、通常の手段では消せません:

| プロセス | PSS | 理由 |
| --- | --- | --- |
| `surfaceflinger` | 50 MB | 描画。要素は画面バッファ (EGL mtrack 42MB) |
| `system` (system_server) | 47 MB | Android フレームワーク本体 |
| `com.android.systemui` | 35 MB | `privateFlags="8"` + system_server が **service として起動**するため、disable でも自動再起動 |

### SystemUI を消そうとして失敗した記録

- `pm disable --user 0 com.android.systemui` → **再起動する**
- `packages.xml` の `privateFlags="8"` を 0 に書き換えて `system_server` を再起動
  → **PMS が再スキャンして 8 に戻す** (フラグは設定ファイルではなく
  プラットフォーム側が付けるため、ファイルを編集しても無効)
- Gravit: 起動要因は `Start proc ... for service com.android.systemui/.SystemUIService`。
  **system_server が Service を bind している**ので、サービスを起動しないと
  状態バー/通知シェード/入力パイプライン が壊れます。

→ SystemUI を消すには framework 側の改変が必要で、リスクに見合いません。

### 解像度を下げると graphics メモリは減る (でも非推奨)

`wm size 1280x720` を試すと **EGL mtrack が 42MB → 19MB** に減りました。
ただし画面解像度が落ちるため**実アプリの見た目が劣化します**。
実測のみで、確認後に `wm size reset` で戻しています。

### `ro.*` プロパティは実行時に変更できない

`ro.hwui.texture_cache_size` (64MB/プロセス) や `ro.platform.keepbackground.app` は
`ro.` = read-only のため `setprop` が**無視されます**。build.prop を編集して
**再起動**しないと反映されません。

### そもそも残量はほぼキャッシュ

最終状態の内訳:

```
MemTotal:     837 MB
MemAvailable: 611 MB   <- 自由に使える
MemFree:       88 MB
Cached:       505 MB   <- ファイルキャッシュ (必要に応じて回収される)
AnonPages:     98 MB   <- 実際のアプリ使用量
```

**実アプリが本当に使っているのは AnonPages 98MB しかありません。**
MemAvailable の 611MB の大半は再利用可能なページキャッシュです。
つまり「空き RAM 356MB→589MB→611MB」という数字は、**アプリが実際に
競合できる匿名メモリが 98MB しかない**という実態と分けて考える必要があります。

## 本当のボトルネックは CPU

```
Hardware : Amlogic gxl
AArch64 Processor rev 4 (aarch64)   <- cortex-a9 (32bit 互換ビルド)
```

818MB / 4 コアの 2015 年製 SoC です。RAM をさらに削っても、CPU が遅ければ
アプリの動作は改善しません。**RAM はほぼ十分** (98MB 使用) なので、
これ以上は削るより **アプリの CPU 負荷と I/O を減らす** のが費用対効果の高い投資先です。

## 再起動後検証 (2 巡目でも実施)

- root 自動復帰 (uid=0) / HOME → 自作アプリ / FATAL EXCEPTION 0 件
- 既定 IME が RemoteIME に保持されている
- disable セット (8 件) が保持されている
- 復旧経路の健全性: `com.droidlogic.otaupgrade` (USB ファームウェア更新) と
  `com.android.tv.settings` (ネットワーク/開発者オプション) は
  **再有効化済み・起動確認済み**。テスト中に disable してしまったため戻しました。

---

# 付録3: 再起動後の再現性検証 (2026-11-01)

`tools/fingerprint.sh` で状態を機械比較可能な指紋化し、**実際に 4 回再起動**して
再現性を検証しました。

## 結果: 設定系は完全再現

```
before_reboot.txt  vs  after_reboot1.txt   -> 設定系 diff 0 件
after_reboot1.txt  vs  after_reboot2.txt   -> 設定系 diff 0 件
```

差分はメモリ変動 (MemFree/MemAvailable/Cached) と、ディスク使用量の 100KB 単位の
書き換えのみ。**以下は起動後も完全に保持されます**:

- インストール済みパッケージ一覧 (41 → 40 へ GMS 完全除去で微減)
- disable 済みパッケージ一覧
- third-party アプリ一覧 (自作アプリのみ)
- 既定 IME (`com.droidlogic.inputmethod.remote/.RemoteIME`)
- preferred HOME アクティビティ (`com.example.dtvandroid/.MainActivity`)
- build.prop の adb 関連行 (`service.adb.tcp.port=5555`, `service.adb.root=1`, `ro.adb.secure=1`)
- FLAG_PERSISTENT 付与パッケージ一覧
- 常駐アプリプロセス一覧

パッケージ状態は `/data/system/packages.xml`、IME と HOME は
`/data/system/settings.xml` に記録されます。どちらも **persistent** な
`/data` パーティション上にあるため、設計上、再起動後も保持されます。
## 再現を壊していた2つの罠 (この検証で見つかった)

### 1. `com.android.defcontainer` を disable すると **APK インストールが壊れる**

2巡目で `defcontainer` (~3MB) を optional として disable したところ、
`pm install` が**全滅**しました:

```
Failure [INSTALL_FAILED_INTERNAL_ERROR]
logcat: PackageManager: Failed to bind to media container service
```

`defcontainer` は Android の**既定コンテナサービス**で、パッケージインストーラが
これに bind して初めてインストール処理を続けられます。3MB の節約のために
**APK を入れられなくなる** ことを意味していたため、**有効に戻しました**。
`cleanup.sh` の KEEP リストに明示的に追加済み。

> 教訓: 「消費量が小さい = 消してよい」ではありません。**他の機能の前提に
> なっているコンポーネント는지**を必ず確認해야 합니다。

### 2. GMS は `pm uninstall --user 0` で完全除去できる (PPPoE とは別)

GMS は `privateFlags="8"` (FLAG_PERSISTENT) ですが、**manifest 上は
`android:persistent="true"` ではありません**。この差が重要です:

| | PPPoE | GMS |
| --- | --- | --- |
| manifest persistent | **あり** | **なし** |
| uninstall 後の挙動 | ActivityManager が再起動 → **crash-loop** | 再起動されない → 安全 |
| 正しい除去方法 | `pm disable` のみ | `pm uninstall --user 0` で完全除去可 |

GMS を `pm install -r --user 0` で戻すと `installed=true` になり、
system_server が GMS の broadcast / content provider / service を起動しようとして
**3プロセス・約157MB を再消費**しました (MemAvailable 610MB → 447MB)。

正しい手順 (device admin 登録が uninstall を阻むため先に解除が必要):

```sh
dpm remove-active-admin --user 0 com.google.android.gms/.auth.managed.admin.DeviceAdminReceiver
pm uninstall --user 0 com.google.android.gms    # -> Success, installed=false
```

これで **GMS は完全に消え、プロセス0・クラッシュ0・MemAvailable 610MB** を維持。
再起動後も crash-loop しないことを確認済み。

## 冪等性

`./tools/cleanup.sh system` を**適用済み端末で再実行**しても状態が壊れません
(既に retired のものはスキップ、disable は冪等)。
ただし GMS は `uninstall` 済みの状態で再実行すると `installed` リストから
`disabled` リストへ移動するため、fingerprint 上は差分が出ます。
**最終的な状態 (GMS 完全除去) は 1 巡目・2 巡目どちらの経路でも同じ**です。

# 付録4: 2 台目端末 (192.168.99.209) のセットアップ (2026-11-02)

1 台目 (`192.168.99.57`) と同じ手順をそのまま適用した記録。

## 端末の違い

| | 192.168.99.57 (1 台目) | 192.168.99.209 (2 台目) |
| --- | --- | --- |
| Model | X96Max_Plus2 | X96Max_Plus_2101W |
| build | `20200901` | `20230626` |
| SoC / OS | franklin / Android 9 (API 28) / userdebug | 同じ |
| /data | 32 GB 級 | 53 GB 表示 |
| 初期パッケージ | 85 | **87** |
| Bluetooth HAL | 故障 (HCI init 失敗) | **正常動作** (MAC あり・crash 0) |

SoC と Android 版が同じため `cleanup.sh` の KEEP / RETIRE リストがほぼ
そのまま使えた。差分は 3 点だけ:

- 2023 build にしか無い GMS 補助パッケージ 3 本
  (`carriersetup` / `configupdater` / `apps.restore`) を `RETIRE_SYSTEM` に追加
- `com.android.documentsui` を `OPTIONAL_DISABLE` に追加 (1 台目と同じ最終状態)
- `com.droidlogic.overlay` (RRO リソース) は 1 台目に無いが、
  RAM を消費しないため**残した** (パッケージ一覧の差として残る)

## 実行コマンド

2 台目はバックアップ先を分けないと 1 台目の APK を上書きするため、
`STB_BACKUP` を追加した。

```sh
cd ~/Desktop/work/dtv_android
export STB_SERIAL=192.168.99.209:5555
export STB_BACKUP=$PWD/artifacts/backup/209

adb connect $STB_SERIAL && adb -s $STB_SERIAL root
./tools/cleanup.sh all        # third-party 削除 + system 退役 + disable + IME + BT
./tools/cleanup.sh optional   # providers.media / documentsui
./tools/stb.sh install        # 自作アプリ
adb -s $STB_SERIAL shell cmd package set-home-activity com.example.dtvandroid/.MainActivity
```

`/vendor/build.prop` に `service.adb.root=1` を追記 (1 台目と同じ)。
2 台目は `service.adb.tcp.port=5555` が `/vendor/build.prop` に最初から入っていた。

## 結果

| 項目 | 前 | 後 | 1 台目 (参考) |
| --- | --- | --- | --- |
| インストール済み | 87 | 53 | 53 |
| disabled | 0 | 14 | 15 (SubTitleService 込み) |
| third-party | 6 | 1 (自作アプリ) | 1 |
| AnonPages | 319 MB | **177 MB** | 215 MB |
| MemAvailable | 3.00 GB | **3.18 GB** | 3.08 GB |
| /data 使用量 | 741 MB | 299 MB | 1.6 GB |

再起動後、`adb root` 無しで `uid=0`・HOME=自作アプリ・`FATAL EXCEPTION` 0 件を確認。

## root 経路: 2 台目は setuid では取れない (init 起動に変更)

2 台目の `su` は 1 台目と実装が違い、`-c` を解釈しないうえ、アプリの uid では
実行権限すら無い。そこで両方の呼び出し規約を受け付ける `c_src/su_shim.c` を
`/system/xbin/su` に据えた (`tools/install-su-shim.sh`)。

| | 192.168.99.57 | 192.168.99.209 (導入前) |
| --- | --- | --- |
| `/system/xbin/su` | SuperSU (79 KB, `-c` 対応) + `daemonsu` | Amlogic 版 (16 KB, `-c` **非対応**) |
| 権限 | `755 root:shell` | `4750 root:shell` |
| serial からの `su -c id` | `uid=0` | **`Permission denied` (rc=126)** |

シムの配置で serial (root) と `run-as` 経由は `su -c` が通る。

### ただしアプリのプロセスは root になれない (実測)

`BootReceiver` は `/system/xbin/su -c "nohup sh <watchdog>"` を呼ぶが、
**アプリのプロセスから setuid バイナリを実行しても実権限は得られない**:

```text
enter uid=10059 euid=0 gid=10059 egid=10059 caps=0 argc=3
mkdir: '/dev/shm': Permission denied
Uid:	10059	10059	10059	10059
```

euid は 0 になるが CapEff は 0 のまま (user namespace 内の root)。
`setuid(0)` で real uid を書き換えるには CAP_SETUID が要るため失敗し、
次に非 setuid の `sh` を exec した時点で kernel が euid を real uid へ戻す。
結果、ウォッチドッグはアプリ uid で動き `/dev/shm` を作れずに落ちる。
1 台目は SuperSU の `daemonsu` (常駐 root) が代行するため成立していた。

**対策: パイプラインの起動を init に移した。** `scripts/stb/pix-stream.rc` を
`/vendor/etc/init/pix-stream.rc` に置くと、init が real root で
ウォッチドッグを起動する (`init.svc.pix-stream=running`)。アプリは起動に
関与しない。アプリ側の試行は無害化するため、ウォッチドッグが先頭で
「`/dev/shm` に書けるか」を確認し、書けなければ
`/data/local/tmp/pix-stream/refused.log` に残して即終了する。

> `id -u` は userns 内でも 0 を返すため、権限判定には使えない。
> 実際に書けるかで判定している。

`su` シムは serial / ツール用として残している。元のバイナリは
`artifacts/backup/209/su.orig`、`./tools/install-su-shim.sh restore` で戻せる。

> 注意: 4755 なので端末上の任意のアプリが euid 0 (実権限なし) には入れる。
> 1 台目も含め、貸与や共用を想定する端末では `chmod 4750` に締めること。

# 付録5: 3 台目端末 (192.168.99.124) の起動復旧とセットアップ (2026-11-02)

起動ロゴで停止していた X96Max_V311 を復旧し、1・2 台目と同じ最小構成にした。
復旧の経緯 (U-Boot 環境変数) は `docs/x96max-v311-boot-recovery.md` を参照。

## 端末の違い

| | .57 (1 台目) | .209 (2 台目) | .124 (3 台目) |
| --- | --- | --- | --- |
| Model | X96Max_Plus2 | X96Max_Plus_2101W | **X96Max_V311** |
| SoC / OS | S905X3 / Android 9 (API 28) | 同じ | **S905X2 / Android 8.1 (API 27)** |
| build | `20200901` | `20230626` | `X96Max_V311_20190302-1355` |
| WebView | com.google.android.webview | 同じ | **com.android.webview** |
| `/system/xbin/su` | SuperSU (79 KB, `-c` 可) | Amlogic 版 (`-c` 不可) → シム | 75 KB, アプリ uid から `-c` 可 |
| Bluetooth HAL | 故障 | 正常 | 正常 |
| 一括セットアップ | `cleanup.sh` (franklin) | 同じ | `cleanup.sh` (u211) |

## プロファイル化

SoC も Android の版も違うため、パッケージ一覧を `tools/profiles/<profile>.sh` に
分離し、`STB_PROFILE` で選ぶようにした (既定は `franklin` なので 1・2 台目の
挙動は変わらない)。

- `tools/profiles/franklin.sh` … 1・2 台目 (従来のリストをそのまま移設)
- `tools/profiles/u211.sh` … 3 台目 (Android 8.1 のパッケージ構成に合わせた)

## 実行コマンド

```sh
cd ~/Desktop/work/dtv_android
export STB_SERIAL=192.168.99.124:5555
export STB_BACKUP=$PWD/artifacts/backup/124
export STB_PROFILE=u211

adb connect $STB_SERIAL && adb -s $STB_SERIAL root
./tools/stb.sh install                      # 自作アプリ
adb -s $STB_SERIAL shell cmd package set-home-activity com.example.dtvandroid/.MainActivity
./tools/cleanup.sh all                      # third-party 削除 + 退役 + disable + IME + BT
./tools/cleanup.sh optional                 # providers.media / documentsui
adb -s $STB_SERIAL shell 'mount -o rw,remount /vendor; \
  grep -q "^service.adb.root=1" /vendor/build.prop || echo service.adb.root=1 >> /vendor/build.prop'
```

退避は `artifacts/backup/124/` (APK 5 本、`vendor_build.prop.before`、
`system_build.prop.before`、パッケージ一覧、getprop 全量)。
戻すときは `STB_BACKUP` を同じにして `./tools/restore.sh all`。

## 結果

| 項目 | 前 | 後 |
| --- | --- | --- |
| インストール済み | 83 | **52** |
| disabled | 0 | 15 |
| third-party | 5 | 1 (自作アプリ) |
| MemAvailable | 3.14 GB | **3.35 GB** |
| `/data` 使用量 | 1.3 GB | 463 MB |
| `FATAL EXCEPTION` | - | 0 |

再起動後、`adb root` を打たずに `uid=0`、HOME とフォーカスは自作アプリ、
IME は RemoteIME。`su -c` はアプリ uid (10056) から実行でき、`uid=0` を返す。

## 3 台目で判断を変えた点

- **GMS 系 8 本は disable で止めた (uninstall しない)。** この ROM は
  `pkgFlags` に `PERSISTENT` が立っていないが、GMS のプロセスは persistent
  なので、1 台目のように uninstall すると crash-loop の危険がある。
  戻すのは `pm enable` だけで済む。
- `com.android.phone` は残した (SIM 無しでも platform persistent のため)。
- `com.homingsystems.hswatch` / `hswatchd` は停止状態で RAM を消費しないため残した。
- Bluetooth は故障していないが、他 2 台と状態を揃えて無効化した。
  この端末は IR リモコンのみで運用し BT リモコンは使わないため、無効のまま運用する。
  戻すときは `pm enable --user 0 com.android.bluetooth`。
- AC 電源再投入 (コールドブート) 後の状態: `uid=0` (adb root 不要)、
  HOME/フォーカス=自作アプリ、52 パッケージ / 15 disabled、
  MemAvailable 3.34 GB、`FATAL EXCEPTION` 0 件。
