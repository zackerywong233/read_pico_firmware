# Read Pico ファームウェア · Pico OS

**言語:** [English](./README.md) | [简体中文](./README.zh-CN.md) | [日本語](./README.ja-JP.md)

[貢献ガイド](CONTRIBUTING.md) · [サポート](SUPPORT.md) · [セキュリティ](SECURITY.md) · [行動規範](CODE_OF_CONDUCT.md)

[![License](https://img.shields.io/github/license/MindReset/read_pico_firmware?style=for-the-badge&logo=apache&logoColor=white)](LICENSE)
[![Build](https://img.shields.io/github/actions/workflow/status/MindReset/read_pico_firmware/build.yml?branch=main&style=for-the-badge&logo=githubactions&logoColor=white)](https://github.com/MindReset/read_pico_firmware/actions)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-E7352C?style=for-the-badge&logo=espressif&logoColor=white)
![Target](https://img.shields.io/badge/target-ESP32--S3-E7352C?style=for-the-badge&logo=espressif&logoColor=white)

Read Pico は、Read シリーズの開発ボードです。ESP32-S3 と 4.7 インチのモノクロ電子ペーパーディスプレイを搭載しています。
Shenzhen MindReset Technology Co., Ltd. が、電子ペーパー向けのオープンソースファームウェアを
開発する方に向けて提供しています。このリポジトリには、ボードに出荷時から搭載されるファームウェアが含まれています。

本リポジトリは、ボード上で動作する Pico OS ファームウェアを提供します。図書、Wi-Fi 転送、
ウェブ閲覧、ダッシュボード、システム状態などのアプリを搭載しています。

デバイス自己診断はルートメニューにあり、画面、タッチ、センサー、電源、キー、TF カード、
フォント、スリープなどの低レベル診断は「システムツール」にまとめています。BSP、PMU
プロトコルホスト、チップドライバーは Pico OS の拡張にも再利用できます。

AI エージェント向けのディレクトリ構成、`app_desc_t` の仕様、用語集、コメント規約は
[AGENTS.md](AGENTS.md) を参照してください。

## 公式ドキュメントとその他のデバイス

- [Read Pico 公式ドキュメント](https://dot.mindreset.tech/docs/read_0)
- [Dot Open Platform](https://github.com/MindReset/dot_open_platform)：Quote/0 のハードウェア資料や Rand/0 のローカル表示連携など、ほかの Dot デバイスやプロジェクトも試してみてください。ファームウェアのサンプル、ピン配置、ケースの設計ファイルを公開しています。

## ハードウェア

| 項目 | 仕様 |
| --- | --- |
| MCU | ESP32-S3、16 MB flash、8 MB Octal PSRAM。flash と PSRAM はいずれも 120 MHz で動作 |
| ディスプレイ | 4.7 インチのモノクロ電子ペーパー、1216 × 684、16 階調、LCD ペリフェラルで駆動する 16 bit パラレルインターフェース |
| 電子ペーパー電源 | SY7636A。PGOOD は IO エキスパンダー経由で読み取り |
| 電源管理 | CW32L010。独自の I2C プロトコルでバッテリー、充放電、インジケーター LED、RTC、アラーム、電源を制御 |
| タッチ | CST836U、2 点タッチ、割り込み、ディープスリープからの復帰 |
| 加速度センサー | SC7A20H、タップ、向き検出、自由落下、FIFO |
| IO エキスパンダー | FCA9555、電子ペーパー制御ピンとカード検出 |
| ストレージ | TF カード（1 bit SDMMC）。フォントはカードから読み込み |
| その他 | ブザー、3 つの静電容量式キー領域 |

## ビルドと書き込み

デュアル OTA パーティションへ初めて移行するときは、[ブラウザー書き込みページ](https://zackerywong233.github.io/read_pico_firmware/)から USB で書き込み、Flash の消去を選択してください。パーティションテーブルと 7 MB の内蔵ストレージを再作成します。TF カード内のファイルは消去されません。以降の更新は設定 → システム更新から Wi-Fi OTA で行えます。詳しくは[ブラウザー書き込みガイド](docs/WEB_FLASHER.md)をご覧ください。

ESP-IDF v6.1 が必要です。`components/read_pico/read_pico_flash_hpm.c` は、
v6 でのみ提供される `esp_flash_chips/spi_flash_override.h` に依存しています。

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

`sdkconfig.defaults` の 120 MHz flash / PSRAM タイミングは、ボードに搭載された flash の型番に依存します。
CI は `sdkconfig.ci` の標準タイミングを使い、コンパイルできることのみを確認します。
[.github/workflows/build.yml](.github/workflows/build.yml) を参照してください。

開発中や書き込み時に、スリープから復帰したデバイスが認識されない場合は、次の順に試してください。

1. USB Type-A のデータケーブルに交換する。
2. デバイスを再度スリープさせてから復帰させる。
3. ボードを再起動してやり直す。

パネルの共通電圧（VCOM）は工場で校正され、PMU に保存されます。ファームウェアは起動時に
一度だけ読み取り、ドライバーを設定します。ローカルには保存せず、ユーザーによる変更機能もありません。

## 画面

Pico OS はランチャーから起動します。ルートメニューには図書、転送、デバイス状態、ダッシュボード、
デバイス自己診断と「システムツール」があります。ハードウェア診断はシステムツール内にまとめています。

| ルートアプリ | 内容 |
| --- | --- |
| 図書 | TF カードまたは内蔵ストレージの UTF-8 / GBK TXT と EPUB、目次、文字サイズ、本ごとの読書位置、しおり、抜粋。ノートは本の横に保存し、件数と抜粋本文に固定上限はありません（実際の上限はストレージとメモリーによります）。項目をタップして元の位置に戻れます。下部の進捗バーから章をまたいで移動できます。ページ送りは画面全体を GC16 更新して残像を抑制。書棚の絞り込み、並べ替え、検索と管理に対応。EPUB は NCX と nav の目次に対応。 |
| 転送 | 端末のホットスポットまたは既存の WiFi で TXT/EPUB と TTF を転送。TF カードと内蔵ストレージが両方利用可能な場合、容量表示をタップして書籍の保存先を切り替えられます（転送がアイドルのときのみ）。フォントは TF カードに保存します。QR 接続、タッチ画面での WiFi 設定、ブラウザーでの本の管理と置換・削除確認に対応。ページを離れると停止。 |
| ウェブ閲覧 | HTTP/HTTPS のページ本文を抽出して表示。同じ WiFi のブラウザーからブックマークレットで TXT として保存できます。静的抽出のためスクリプト、CSS、画像は読み込みません。ページを離れるか端末をロックすると通信を停止します。 |
| Agent 受信 | 端末の転送ページで MQTT/TLS Broker とトピックを設定し、アプリメニューから JSON テキストまたは HTTPS 画像を受信します。テキストはページ表示し、画像は TF カードに保存してロック画面に設定できます。アプリ起動中のみ通信します。 |
| デバイス状態 | 起動時の I2C スキャン、識別情報、バッテリーと充電状態、ビルド日時 |
| ダッシュボード | 日付、バッテリー、最近読んだ本、個人メモの表示とロック画面テーマ |
| デバイス自己診断 | デバイス応答とコマンド ACK。電源断を伴う項目はバックエンドにのみ結果を記録 |
| システムツール | 電子ペーパー更新、内蔵読書、タッチ、加速度センサーと診断、電源、PMU、キー、スリープ、TF カードとブザー、フォント、IO 拡張 |

図書の KEY1 / KEY2 / KEY3 は前ページ / ツールバー / 次ページです。ツールバーには全画面更新があります。中央キーを500 ms長押しするとアプリメニューを開けます。他のページでは KEY2 の全画面 GC16 更新と KEY3 のメニューを維持します。メニュー項目は指を離すと確定し、外へ滑らせるとキャンセルします。

Agent メッセージは MQTT JSON オブジェクトです。テキストは `{"type":"text","title":"タイトル","text":"本文"}`、画像は `{"type":"image","url":"https://example.com/image.jpg"}` です。Broker、ユーザー名、パスワード、完全一致トピックは端末のローカル転送ページで設定します。パスワードは端末内に暗号化せず保存し、Web ページには再表示しません。画像には TF カードのマウントと有効な HTTPS 証明書が必要です。

## ディレクトリ構成

```text
main/
  app_main.c        起動処理を組み立て、app_loop に引き渡す
  app/              アプリのインターフェース（app.h）、登録テーブル、イベントループ
  apps/             Pico OS アプリとシステムツール。各ファイルは app_desc_t のみ公開
  ui/               ui_kit の描画プリミティブとレイアウト定数、ui_menu の 2 階層メニュー
  font/             stb_truetype のグリフキャッシュ
  factory/          デバイス機能テストと出荷時の VCOM 校正
components/
  read_pico/        ボード BSP：I2C、EPD 定義と走査タイミング、TF カード、ブザー、flash HPM
  read_pico_pmu/    CW32L010 プロトコルのホスト実装
  epdiy/            LCD ペリフェラル経路に限定した電子ペーパーレンダラー
  continuous_du/    複数回の走査にわたり位相を蓄積し、指の動きに追従する連続 DU
  cst836u/ sc7a20h/ fca9555/ sy7636a/    チップドライバー
  e0470_epaper_waveform/                 パネル波形テーブルとトリミング関数
  pwm_audio/        LEDC PWM オーディオ。ブザーのバックエンドのひとつ
assets/             main/assets/*.bin の元画像
tools/              フォントと画像の変換スクリプト
```

ルートアプリを追加するには `main/app/app_registry.c` に登録します。ハードウェア診断は
`main/apps/app_system_tools.c` のツール一覧に追加します。メインループの変更は不要です。

## ピン配置

| 機能 | GPIO |
| --- | --- |
| I2C SCL / SDA | 40 / 39（400 kHz） |
| EPD データ D0–D15 | 4–18, 45 |
| EPD XLE / XSTL / XCL / SPV / CKV | 3 / 46 / 21 / 47 / 48 |
| FCA9555 INT# | 41（ライトスリープからの復帰にも使用） |
| CST836U INT# | 43 |
| SC7A20H INT1 | 1 |
| TF カード CLK / CMD / D0 | 38 / 42 / 44 |
| ブザー | 2 |

電子ペーパーの電源イネーブル、XOE、MODE、VCOM_EN、タッチリセット、カード検出は
FCA9555 の Port-0 に接続されています。[main/apps/app_ioe.c](main/apps/app_ioe.c) のピン定義表を参照してください。

## 転送と制限事項

AP と既存 WiFi の両モードで転送ページの QR コードを表示します。AP では WiFi 接続用とページを開くコードを切り替えられます。端末のビルド日時は UTC と明記します。

転送サーバーは転送画面でのみ動作し、画面を離れると停止します。個別ログインや TLS のないローカル HTTP を使用し、同じネットワークの端末から対象ストレージの本を管理できるため、信頼できるネットワークで使用してください。WiFi 認証情報は端末 NVS に保存され、公開 API やログにパスワードを返しません。NVS/flash 暗号化は有効化しておらず、物理アクセスへの保護は提供しません。

転送を停止すると、入る前の画面またはメニュー位置に戻ります。マウント済みTFカードが使用できなくなると、関連する読書・転送を停止し、内蔵フォントに戻ります。再挿入後はTF画面で明示的に再マウントしてください。書き込み中にカードを抜くとファイルシステムが破損する可能性があります。EPUBを開くと本文と画像プレースホルダーを表示します。タップしたローカルJPEG/PNGだけを読み込み、縦横比を保った別画面で表示します。画像・戻るボタン・3キーのいずれかで、再ページ分割せず元の読書位置に戻ります。章を開く際やページ送り・事前描画では画像をデコードしません。ZIPエントリー、manifest項目、spine章はそれぞれ最大32768件、OPF・ナビゲーションは展開後4 MiB、本文・画像の読み込みは2 MiB、ZIP中央ディレクトリは8 MiBまでです。見出しの合計予算は1 MiBで、超過分は番号見出しになります。未使用エントリーも4 MiBを超えると拒否します。章数とは別にメモリー・リソース上限があり、ZIP64と32768件・章を超える本は非対応です。外部で置き換えた本や別のカードのファイルが同じパス・サイズの場合、以前の読書位置が適用されることがあります。保存失敗後の再試行状態は、電源断後の保持を保証しません。

本棚に入ると前回の読書を再開するか確認します。キャンセルすると本を開かず本棚に留まります（KEY1でキャンセル、KEY3で再開）。目次と現在の章を解析し、最初の2ページまたは保存位置までを先に組版します。残りは2ページずつ追加し、他の章は必要時に読み込みます。未完了の総ページ数は「…」と表示し、前章の末尾へ戻る場合はその章全体の組版が必要です。文字幅は常駐フォント表から計算し、章全体の字形画像を事前生成しません。待機表示と失敗理由（メモリ不足・上限超過・非対応形式・ファイル異常）は引き続き表示します。

スワイプによるページ送りは指を離した時点で確定し、距離は120から64ピクセルに短縮しました。タップ許容範囲の24ピクセルを超え、64未満のドラッグはキャンセルします。画像プレースホルダー上のスワイプは画像を読み込まずページを送ります。

挿絵の上限：ベースラインJPEGはデコード時の縮小を優先し、表示サイズへの拡大を最大2倍まで許容し、元画像16M画素・各辺8192まで。PNGとプログレッシブJPEGは1M画素・デコードヒープ4 MiBまで。出力は648×1000以内のグレースケールで、透過PNGは白背景に合成します。現在の章で最後に閲覧した1枚だけをキャッシュし、同じ画像を開き直す際に再利用します。上限超過・欠落・破損・非対応画像はタップ時に理由を示し、本文の読書は継続できます。同じ正規化リソースパスの繰り返しには「重复图片」と、今回開いてから閲覧した章の最小節番号を表示します（表紙や前書きを含むEPUB順序のため本文の章番号と異なる場合があります）。章頭の画像群に見出しが直続する場合、その画像と同じリソースへの参照は「标题图」と表示し、最初の位置表示を省きます。その他の挿絵は既読の最小節番号を保持します。未読章は走査せず、全書で最初の出現とは断定しません。記録は本を閉じると消去され、別パスの同一内容は照合しません。SVG内のJPEG/PNG参照は対応し、純粋なSVGベクター・CSS背景・外部画像は非対応です。[デコーダーの出典とライセンス](main/book/vendor/README.md)も参照してください。

内蔵フォントはUIとデモ本文用のサブセットです。外部の中国語書籍には、文字を十分に収録したTTFをTFカードの `fonts/` または `assets/fonts/` に置き、「字体 Font」画面で選択してください。既定のパスは `fonts/ChillDuanSansVF.ttf` です。文字が欠ける場合はファイルの有無とフォントの収録文字を確認してください。[TF 配置用フォントとライセンス](sdcard/README.md)を同梱しています。Web の「上传字体」から `/sdcard/fonts` へアップロードできます。TrueType アウトラインの TTF のみ、1ファイル32 MiBまで（OTF/CFF・TTC・WOFFは非対応）。容量を確認し、同名置換には確認が必要です。中断・検証失敗時は元の字体を保持します。転送停止後に端末で字体を選択してください。転送中は内蔵字体を使用し、保存済みの選択は変更しません。

機能の変更は [変更履歴](docs/CHANGELOG.md) を参照してください。拼音データは MIT ライセンスの pypinyin に由来します。[ライセンスと再生成](components/read_pico_search/README.md)を参照してください。

## 謝辞とライセンス

- ファームウェア本体：Apache-2.0。[LICENSE](LICENSE) を参照してください。
- [epdiy](https://github.com/vroland/epdiy)：電子ペーパーのタイミング制御と描画。
  本ボードの LCD 経路に合わせて機能を絞ったフォークです。LGPL-3.0-or-later。
  変更内容は [components/epdiy/LICENSE](components/epdiy/LICENSE) に記載しています。
- [stb_truetype](https://github.com/nothings/stb)：グリフのラスタライズ。パブリックドメイン。
- [pwm_audio](https://github.com/espressif/esp-iot-solution/tree/master/components/audio/pwm_audio)：
  Espressif の LEDC PWM オーディオ。機能を絞ったコピーを Apache-2.0 で使用しています。
  [components/pwm_audio/LICENSE](components/pwm_audio/LICENSE) を参照してください。
- パネル波形テーブルはボードに付属し、現状のまま Apache-2.0 で提供されます。
  [components/e0470_epaper_waveform/LICENSE](components/e0470_epaper_waveform/LICENSE) を参照してください。
- 内蔵フォント `main/assets/builtin.ttf` は、Warren2060 氏の可変フォント
  [ChillDuanSans](https://github.com/Warren2060/ChillDuanSans) を
  `tools/gen_builtin_font.py` でサブセット化したものです。未変更の完全版と原ライセンスは
  `sdcard/fonts/` に同梱し、変更版の内蔵サブセットは Read Pico UI と命名しています。
  両方に SIL OFL-1.1 が適用されます。[配置手順と出典](sdcard/README.md)を参照してください。

開発者の皆さまのご理解とご支援に感謝します。

Shenzhen MindReset Technology Co., Ltd.
