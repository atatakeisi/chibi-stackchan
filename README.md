# ちびスタックちゃん (Chibi StackChan)

**ATOMS3R AI Chatbotキット**（AtomS3R + Atomic Echo Base, 8MB PSRAM）で動く、
手のひらサイズのAIスタックちゃん。[guruguru-stackchan](../guruguru-stackchan)（Core2版）からの派生です。

- 🧠 **会話**: Google **Gemini**（`gemini-3.5-flash`）
- 👂 **音声認識(STT)**: OpenAI **Whisper**
- 🗣️ **音声合成(TTS)**: **VoiceVox**（Web API）
- 🙂 **表情**: m5stack-avatar（128×128 に縮小表示・**吹き出しなし**・口パクあり）
- 🔩 **サーボ**: SG90 × 1（Grove接続）。**左右首振りのみ・連続回転なし**
- 📣 **会話スタート**: **呼びかけ方式**（呼びかけワードは Web UI で変更可）
  ＋ 画面クリックでも即スタート
- ☀️ **おまけ**: 今日の天気（Open-Meteo・APIキー不要）と現在時刻（NTP）を会話に反映
- 🔌 **電源**: ATOMS3R の USB-C 給電
- 📶 **設定**: 全て NVS 保存。未設定/接続失敗/起動時画面長押しで **AP設定ポータル**
  （iPhoneのブラウザから入力）。**鍵情報クリアボタン**あり

---

## 必要なもの

| 区分 | 内容 |
|---|---|
| 本体 | M5Stack **ATOMS3R**（8MB PSRAM）+ **Atomic Echo Base**（= AI Chatbotキット） |
| サーボ | **SG90** など PWM ポジションサーボ ×1（180度型。連続回転型は不可） |
| 鍵 | Gemini APIキー / OpenAI APIキー（Whisper用）/ VoiceVox APIキー |
| 電源 | USB-C（ATOMS3R 側に接続） |

## 配線（サーボ）

ATOMS3R 本体側面の **Grove ポート（G1 / G2 / 5V / GND）** に接続:

| SG90 | Grove |
|---|---|
| 信号（橙/黄） | **G2** |
| VCC（赤） | 5V |
| GND（茶/黒） | GND |

> 信号ピンを G1 に変えたい場合は `src/HeadServo.h` の `kServoPin` を `1` に。
> 可動範囲・振幅・速さも同ファイル冒頭の定数で調整できます。
> Echo Base は ATOMS3R の底面にそのまま合体（I2S/I2C は M5Unified が自動設定）。

## ビルド & 書き込み（PlatformIO）

```bash
cp include/secrets.h.example include/secrets.h   # （任意）初期値を書く場合
pio run                  # ビルド
pio run -t upload        # 書き込み
pio device monitor       # シリアルモニタ (115200)
```

- Core2 版と違い esp-sr を使わないため **`framework = arduino` 単独**でビルドできます
  （IRAM 問題・espidf ハイブリッド・setuptools 対策は不要）。
- `secrets.h` は **NVS が空のときの初期値**としてのみ使用。無くても
  （中身がプレースホルダのままでも）AP設定ポータルから全部入力できます。

## 初期設定（AP設定ポータル）

以下のいずれかで設定モード（AP）になります:

1. NVS に SSID が未保存（初回起動）
2. 起動時に Wi-Fi 接続へ 15 秒失敗
3. **起動時に画面（Aボタン）を押したまま** / Wi-Fi 接続試行中に画面長押し
4. 通常運用中に **画面を3秒長押し**

設定モードでは:

1. iPhone で Wi-Fi **`ChibiStackChan-Setup`**（パスワード **`stackchan`**）に接続
2. ブラウザで **http://192.168.4.1** を開く
3. SSID / パスワード / 各APIキー / **呼びかけワード** / 天気の場所 などを入力
   →「保存して再起動」

> ⚠️ ESP32 は **2.4GHz 専用**です（5GHz 非対応）。
> 🗑️ ページ下部の **「鍵情報クリア」** で Wi-Fi・APIキー等の NVS を全消去して
> 再起動→設定モードに戻ります（secrets.h の初期値へのフォールバックも無効化されます）。

## 使い方

| 操作 | 動作 |
|---|---|
| **「スタックちゃん」と呼びかける** | ピッと鳴らずに録音→呼びかけワードを含めば会話開始。「スタックちゃん、今日の天気は？」のように続けて話すとそのまま質問扱い。呼びかけだけなら「なあに？」と聞き返してくる |
| 画面クリック | 呼びかけ無しで即録音→会話 |
| 画面3秒長押し | 設定APモードへ |
| 「今何時？」「今日の天気は？」 | NTP時刻 / Open-Meteo の天気を参考情報として Gemini が答える |

### 呼びかけ方式のしくみと注意

マイク音量がしきい値を超えると録音して Whisper に送り、認識テキストに
呼びかけワードが**含まれるときだけ**反応します（含まなければ無言で破棄）。

- 呼びかけワードは Web UI で変更可。**カンマ区切りで複数登録**できます
  （例: `スタックちゃん,すたっくちゃん,スタック`）。認識ゆれ対策に複数登録推奨。
- **物音のたびに Whisper API が呼ばれる**ため、うるさい場所では設定画面の
  「音量トリガ感度」を大きく（鈍く）するか、呼びかけ起動を無効（画面クリックのみ）に。
- 無音時の Whisper 幻聴（「ご視聴ありがとうございました」等）は自動で弾きます。

## 通常運用時の Web UI

Wi-Fi 接続後はポート80でWebサーバが起動。同じWi-FiのiPhoneから
**http://chibi-chan.local/**（または起動時に画面表示されるIP）へ。

| URL | 内容 |
|---|---|
| `/` | メニュー（設定・ロール・しゃべらせる・文字で質問） |
| `/wifi` | **設定フォーム**（Wi-Fi / APIキー / 呼びかけ / 感度 / 音量 / 天気の場所 / 鍵クリア）→ 保存で再起動 |
| `/role` | ロール（キャラ設定＝システムプロンプト）。SPIFFS保存。感情タグのルールは自動追記 |
| `/setting?volume=200&mic=48&speaker=3` | 音量・マイク感度・VoiceVox話者の即時変更（再起動不要） |
| `/speech?say=こんにちは` | しゃべらせる |
| `/chat?text=おはよう` | 文字で質問 |
| `/face?expression=1` | 表情変更（0:Neutral 1:Happy 2:Sleepy 3:Doubt 4:Sad 5:Angry） |

## 構成

```
chibi-stackchan/
├── platformio.ini            # ATOMS3R (arduino単独・PSRAM有効)
├── include/
│   ├── secrets.h.example     # 鍵テンプレート
│   └── secrets.h             # 実際の鍵（git管理外・無くても可）
└── src/
    ├── main.cpp              # 本体（呼びかけ・ポータル・Gemini・Web UI）
    ├── HeadServo.{h,cpp}     # ★SG90 左右首振り（新規）
    ├── WeatherClock.{h,cpp}  # ★NTP時刻 + Open-Meteo天気（新規）
    ├── Whisper / AudioWhisper        # STT（guruguruから移植）
    ├── WebVoiceVoxTTS / AudioOutputM5Speaker / AudioFileSourceHTTPSStream  # TTS
    └── rootCA*.h             # 証明書
```

## メモ / 既知の注意点

- **Atomic Echo Base（ES8311）対応は M5Unified 0.2.x 以降**が必要
  （`cfg.external_speaker.atomic_echo = true`）。本プロジェクトは 0.2.17 でビルド確認済み。
  マイクとスピーカーは同一コーデックのため、録音と再生を `M5.Mic`/`M5.Speaker` の
  begin/end で切り替えています（Core2版と同じ流儀）。
- 顔の縮小は `avatar.setScale(0.4)` + `setPosition(-56, -96)`。ずれる場合はここを微調整。
- サーボはアイドル時 PWM を止めて脱力（省電力・ジー音防止）。ときどき小さく
  キョロッと動く待機モーション入り。会話中は声の大きさに合わせて左右にゆらゆら。
- NVS 名前空間: Wi-Fi=`wifi`、鍵・呼びかけ・天気=`chibi`、音量等=`setting`。
  guruguru（Core2版）の `guruchan` とは独立。
- 実機未検証の初期値（マイク感度 32、VADしきい値 1500、サーボ角度範囲）は
  実機で聞き取り具合を見ながら Web UI / `HeadServo.h` で調整してください。

## ライセンス / クレジット

- 母体: robo8080 系 AI_StackChan2、meganetaaan/M5Stack-Avatar、guruguru-stackchan
- 天気: [Open-Meteo](https://open-meteo.com/)（無料・APIキー不要）
