// =============================================================================
// Chibi StackChan - ATOMS3R AI Chatbot Kit (AtomS3R + Atomic Echo Base)
//
//   LLM : Google Gemini (gemini-3.5-flash)
//   STT : OpenAI Whisper
//   TTS : VoiceVox Web API
//   顔  : m5stack-avatar (128x128 に縮小表示、吹き出しなし)
//   首  : SG90 PWM サーボ x1 (Grove)。左右首振りのみ・連続回転なし
//   起動: 呼びかけワード (Web UI で変更可) または 画面(Aボタン)クリック
//   設定: NVS 保存。未設定/接続失敗/起動時に画面長押し → APポータル
//         (SSID: ChibiStackChan-Setup / pass: stackchan / http://192.168.4.1)
//   おまけ: 今日の天気(Open-Meteo) と 現在時刻(NTP) を会話に反映
//
// guruguru-stackchan (Core2) からの派生。BLE/バスサーボ/esp-srウェイクワードは
// 使わず、呼びかけ判定は「音量トリガ → Whisper → 呼びかけワード照合」方式。
// =============================================================================
#include <Arduino.h>
#include <SPIFFS.h>
#include <M5Unified.h>
#include <nvs.h>
#include <Avatar.h>

#include <AudioOutput.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceSPIFFS.h>
#include <AudioGeneratorMP3.h>
#include "AudioFileSourceHTTPSStream.h"
#include "AudioOutputM5Speaker.h"
#include "WebVoiceVoxTTS.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "rootCACertificate.h"
#include "rootCAgoogle.h"
#include "rootCAanthropic.h"
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <deque>
#include <algorithm>
#include <vector>

#include "AudioWhisper.h"
#include "Whisper.h"
#include "HeadServo.h"
#include "WeatherClock.h"

// NVS が空のときの初期値 (include/secrets.h, git管理外)
#include "secrets.h"

#define MDNS_HOST "chibi-chan"

// 保存する質問と回答の最大数
const int MAX_HISTORY = 5;
std::deque<String> chatHistory;

/// set M5Speaker virtual channel (0-7)
static constexpr uint8_t m5spk_virtual_channel = 0;

using namespace m5avatar;
Avatar avatar;
const Expression expressions_table[] = {
  Expression::Neutral,
  Expression::Happy,
  Expression::Sleepy,
  Expression::Doubt,
  Expression::Sad,
  Expression::Angry
};

WebServer server(80);

//---------------------------------------------
String GEMINI_API_KEY = "";    // Google Gemini (LLM / conversation)
String CLAUDE_API_KEY = "";    // Anthropic Claude (会話をClaudeにする場合)
String AI_PROVIDER = "gemini"; // 会話AI: "gemini" | "claude" (Web UIで切り替え)
String OPENAI_API_KEY = "";    // OpenAI (Whisper speech-to-text)
String VOICEVOX_API_KEY = "";  // VoiceVox (text-to-speech)
String CFG_WIFI_SSID = "";
String CFG_WIFI_PASS = "";

// 呼びかけ (ウェイクワード)。カンマ/読点区切りで複数登録可。
String WAKE_PHRASE = "スタックちゃん";
// 呼びかけモード: 0=無効(画面タッチのみ) 1=呼びかけワードで起動 2=なんでも返事(ワード不要)
uint8_t wake_mode = 1;
bool   wake_enable = true;  // wake_mode != 0 の派生値
int    vad_threshold = 5000;   // 音量トリガのしきい値 (int16 peak, 200-8000)。実機調整済みの値

// 天気の場所 (Web UI で変更可)
String WEATHER_PLACE = "東京";
float  WEATHER_LAT = 35.6812f;
float  WEATHER_LON = 139.7671f;

// 最新の返答から選んだ表情 (発話中に適用)
Expression g_reply_expression = Expression::Neutral;
String TTS_SPEAKER_NO = "3";
String TTS_SPEAKER = "&speaker=";
String TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;

bool portal_mode = false;      // AP設定ポータル動作中か
bool avatar_ready = false;     // avatar.init() 済みか (描画タスクが動いているか)
//---------------------------------------------

static const char HEAD_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html><html lang="ja"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ちびスタックちゃん</title></head>)KEWL";

String speech_text = "";
String speech_text_buffer = "";
DynamicJsonDocument chat_doc(1024 * 10);
String InitBuffer = "";
int GEMINI_LAST_HTTP_CODE = 0;

static const char GEMINI_API_URL[] = "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash:generateContent";
// Claude Messages API。証明書は rootCAanthropic.h (GTS Root R4/R1) で検証する
static const char CLAUDE_API_URL[] = "https://api.anthropic.com/v1/messages";
static const char CLAUDE_MODEL[]   = "claude-haiku-4-5";  // 短い会話用途向けの最安モデル

static const char kBaseChatJson[] = "{\"messages\":[]}";
static const char kExpressionTagRule[] =
    "返答の先頭に感情タグを1つだけ付けてください: [happy] [sad] [angry] [sleepy] [doubt] [neutral]";
static const char kTimerTagRule[] =
    "タイマーを頼まれたら返答の最後に [timer:秒数] を付けてください (例: 3分なら [timer:180])。"
    "タイマーの中止を頼まれたら [timer:cancel] を、残り時間を聞かれたら [timer:status] を付けてください。"
    "タイマーと関係ない話ではこれらのタグを付けないでください。";
// 返答は音声合成(VoiceVox Web API)で読み上げるため、長文だと合成に時間がかかり
// タイムアウトやAPIポイントの浪費につながる。ロールに関わらず長さの上限を自動追記する
static const char kSpeechLengthRule[] =
    "返答は音声で読み上げられます。話し言葉で120文字以内に収めてください。";

// タイマー ([timer:秒] タグでセット、満了時に読み上げ)
uint32_t timer_end_ms = 0;    // 0 = 未設定
uint32_t timer_total_sec = 0;
static const char kDefaultRole[] =
    "あなたは「ちびスタックちゃん」という小さくてかわいいロボットです。"
    "一人称は「ぼく」。日本語で、話し言葉で短く(60文字以内で)答えてください。";

bool init_chat_doc(const char *data) {
  DeserializationError error = deserializeJson(chat_doc, data);
  if (error) {
    Serial.println("DeserializationError");
    return false;
  }
  return true;
}

// システムプロンプト(ロール)を組み立てて InitBuffer に反映
void applyRole(const String& roleText) {
  init_chat_doc(kBaseChatJson);
  JsonArray messages = chat_doc["messages"];
  JsonObject m = messages.createNestedObject();
  m["role"] = "system";
  m["content"] = roleText + "\n" + kExpressionTagRule + "\n" + kTimerTagRule +
                 "\n" + kSpeechLengthRule;
  InitBuffer = "";
  serializeJson(chat_doc, InitBuffer);
}

// ====================================================================
//  NVS helpers
// ====================================================================
static String nvs_get_string(const char* ns, const char* key) {
  String out = "";
  uint32_t h;
  if (ESP_OK == nvs_open(ns, NVS_READONLY, &h)) {
    size_t len = 0;
    if (ESP_OK == nvs_get_str(h, key, nullptr, &len) && len > 0) {
      char* buf = (char*)malloc(len);
      if (buf) {
        if (ESP_OK == nvs_get_str(h, key, buf, &len)) out = String(buf);
        free(buf);
      }
    }
    nvs_close(h);
  }
  return out;
}

static void nvs_set_string(const char* ns, const char* key, const String& v) {
  uint32_t h;
  if (ESP_OK == nvs_open(ns, NVS_READWRITE, &h)) {
    nvs_set_str(h, key, v.c_str());
    nvs_close(h);
  }
}

static uint8_t nvs_get_u8_or(const char* ns, const char* key, uint8_t def) {
  uint8_t v = def;
  uint32_t h;
  if (ESP_OK == nvs_open(ns, NVS_READONLY, &h)) {
    nvs_get_u8(h, key, &v);
    nvs_close(h);
  }
  return v;
}

static void nvs_set_u8v(const char* ns, const char* key, uint8_t v) {
  uint32_t h;
  if (ESP_OK == nvs_open(ns, NVS_READWRITE, &h)) {
    nvs_set_u8(h, key, v);
    nvs_close(h);
  }
}

// 設定は専用の名前空間 "chibi" に保存 (Wi-Fi は "wifi", 音量等は "setting")
void loadConfigFromNvs() {
  // 「鍵情報クリア」直後は secrets.h へのフォールバックもしない
  bool nofallback = nvs_get_u8_or("chibi", "nofallback", 0) != 0;

  CFG_WIFI_SSID = nvs_get_string("wifi", "ssid");
  CFG_WIFI_PASS = nvs_get_string("wifi", "pass");
  if (CFG_WIFI_SSID == "" && !nofallback) {
    CFG_WIFI_SSID = String(WIFI_SSID);
    CFG_WIFI_PASS = String(WIFI_PASS);
    if (CFG_WIFI_SSID == "YOUR_WIFI_SSID") CFG_WIFI_SSID = "";
  }

  GEMINI_API_KEY   = nvs_get_string("chibi", "gemini");
  CLAUDE_API_KEY   = nvs_get_string("chibi", "claude");
  OPENAI_API_KEY   = nvs_get_string("chibi", "openai");
  VOICEVOX_API_KEY = nvs_get_string("chibi", "voicevox");
  if (!nofallback) {
    if (GEMINI_API_KEY == "")   GEMINI_API_KEY   = String(GEMINI_APIKEY);
    if (OPENAI_API_KEY == "")   OPENAI_API_KEY   = String(OPENAI_APIKEY);
    if (VOICEVOX_API_KEY == "") VOICEVOX_API_KEY = String(VOICEVOX_APIKEY);
#ifdef CLAUDE_APIKEY
    if (CLAUDE_API_KEY == "")   CLAUDE_API_KEY   = String(CLAUDE_APIKEY);
#endif
  }

  String prov = nvs_get_string("chibi", "provider");
  if (prov != "") AI_PROVIDER = prov;
  printf("[AI] provider=%s (claude key %s)\n", AI_PROVIDER.c_str(),
         CLAUDE_API_KEY == "" ? "未設定" : "設定済み");

  String w = nvs_get_string("chibi", "wake");
  if (w != "") WAKE_PHRASE = w;
  wake_mode = nvs_get_u8_or("chibi", "wake_en", 1);
  if (wake_mode > 2) wake_mode = 1;
  wake_enable = (wake_mode != 0);
  String v = nvs_get_string("chibi", "vad");
  if (v != "") vad_threshold = constrain(v.toInt(), 200, 8000);
  // NOTE: この基板では Serial(HWCDC) の出力がUSBに届かないため printf(IDFコンソール)を使う
  printf("[WAKE] モード=%s / 有効ワード=[%s] / VADしきい値=%d\n",
         wake_mode == 0 ? "無効" : wake_mode == 2 ? "なんでも返事" : "呼びかけワード",
         WAKE_PHRASE.c_str(), vad_threshold);

  String place = nvs_get_string("chibi", "place");
  if (place != "") WEATHER_PLACE = place;
  String lat = nvs_get_string("chibi", "lat");
  String lon = nvs_get_string("chibi", "lon");
  if (lat != "" && lon != "") {
    WEATHER_LAT = lat.toFloat();
    WEATHER_LON = lon.toFloat();
  }
}

// ====================================================================
//  Web UI (iPhone のブラウザから設定)
// ====================================================================
static const char CONFIG_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html><html lang="ja"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ちびスタックちゃん設定</title><style>
 body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:18px}
 h1{font-size:20px} .card{background:#1d1d22;border-radius:12px;padding:16px;margin:12px 0;max-width:480px}
 label{display:block;font-size:13px;color:#aaa;margin-top:10px}
 input,select{width:100%;box-sizing:border-box;font-size:16px;padding:10px;border-radius:8px;border:1px solid #444;background:#222;color:#fff}
 button{font-size:17px;padding:14px 18px;margin-top:16px;border:0;border-radius:10px;background:#2d6cff;color:#fff;width:100%;max-width:480px;display:block}
 button.danger{background:#c0392b}
 small{color:#888}
</style></head><body>
{{mode}}
<h1>ちびスタックちゃん 設定</h1>
<p><a style="color:#7fb0ff" href="/">&larr; メニューへ戻る</a></p>
<form onsubmit="save(event)">
 <div class="card">
  <b>Wi-Fi (2.4GHz のみ)</b>
  <label>SSID</label><input id="ssid" name="ssid" placeholder="{{ssid}}">
  <label>パスワード</label><input id="pass" name="pass" type="password">
 </div>
 <div class="card">
  <b>&#9888;&#65039; APIキー (重要設定)</b><br>
  <small style="color:#e6b34d">この子が聞いたり喋ったりするための鍵です。<b>通常は変更不要です。</b><br>
  間違ったキーを保存すると喋らなくなります。</small>
  <details style="margin-top:10px">
   <summary style="cursor:pointer;color:#7fb0ff;padding:6px 0">自分のAPIキーに変更する (取得済みの人向け)</summary>
   <small>空欄の項目は変更されません。保存時に確認が表示されます。</small>
   <label>Gemini API Key (会話)</label><input id="gemini" name="gemini">
   <label>Claude API Key (会話をClaudeにする場合)</label><input id="claude" name="claude">
   <label>会話AI (現在: {{provider}})</label>
   <select id="provider"><option value="">(変更しない)</option><option value="gemini">Gemini</option><option value="claude">Claude</option></select>
   <label>OpenAI API Key (音声認識 Whisper)</label><input id="openai" name="openai">
   <label>VoiceVox API Key (音声合成)</label><input id="voicevox" name="voicevox">
  </details>
 </div>
 <div class="card">
  <b>呼びかけ (ウェイクワード)</b>
  <label>呼びかけワード (カンマ区切りで複数可)</label>
  <input id="wake" name="wake" placeholder="{{wake}}">
  <label>呼びかけ起動</label>
  <select id="wake_en"><option value="1" {{wen1}}>呼びかけワードで起動</option><option value="2" {{wen2}}>なんでも返事 (ワード不要・誤反応は増える)</option><option value="0" {{wen0}}>無効 (画面タッチのみ)</option></select>
  <label>音量トリガ感度 (200=敏感 〜 8000=鈍感)</label>
  <input id="vad" name="vad" type="number" min="200" max="8000" placeholder="{{vad}}">
 </div>
 <div class="card">
  <b>声・音</b>
  <label>VoiceVox 話者 (現在の声が選択されています)</label>
  <select id="speaker">
   <option value="0">0: 四国めたん (あまあま)</option>
   <option value="1">1: ずんだもん (あまあま)</option>
   <option value="2">2: 四国めたん (ノーマル)</option>
   <option value="3">3: ずんだもん (ノーマル)</option>
   <option value="4">4: 四国めたん (セクシー)</option>
   <option value="5">5: ずんだもん (セクシー)</option>
   <option value="6">6: 四国めたん (ツンツン)</option>
   <option value="7">7: ずんだもん (ツンツン)</option>
   <option value="8">8: 春日部つむぎ</option>
   <option value="9">9: 波音リツ</option>
   <option value="10">10: 雨晴はう</option>
   <option value="11">11: 玄野武宏 (ノーマル)</option>
   <option value="12">12: 白上虎太郎 (ふつう)</option>
   <option value="13">13: 青山龍星</option>
   <option value="14">14: 冥鳴ひまり</option>
   <option value="15">15: 九州そら (あまあま)</option>
   <option value="16">16: 九州そら (ノーマル)</option>
   <option value="17">17: 九州そら (セクシー)</option>
   <option value="18">18: 九州そら (ツンツン)</option>
   <option value="19">19: 九州そら (ささやき)</option>
   <option value="20">20: もち子さん</option>
   <option value="21">21: 剣崎雌雄</option>
   <option value="22">22: ずんだもん (ささやき)</option>
   <option value="23">23: WhiteCUL (ノーマル)</option>
   <option value="24">24: WhiteCUL (たのしい)</option>
   <option value="25">25: WhiteCUL (かなしい)</option>
   <option value="26">26: WhiteCUL (びえーん)</option>
   <option value="27">27: 後鬼 (人間ver.)</option>
   <option value="28">28: 後鬼 (ぬいぐるみver.)</option>
   <option value="29">29: No.7 (ノーマル)</option>
   <option value="30">30: No.7 (アナウンス)</option>
   <option value="31">31: No.7 (読み聞かせ)</option>
   <option value="32">32: 白上虎太郎 (わーい)</option>
   <option value="33">33: 白上虎太郎 (びくびく)</option>
   <option value="34">34: 白上虎太郎 (おこ)</option>
   <option value="35">35: 白上虎太郎 (びえーん)</option>
   <option value="36">36: 四国めたん (ささやき)</option>
   <option value="37">37: 四国めたん (ヒソヒソ)</option>
   <option value="38">38: ずんだもん (ヒソヒソ)</option>
   <option value="39">39: 玄野武宏 (喜び)</option>
   <option value="40">40: 玄野武宏 (ツンギレ)</option>
   <option value="41">41: 玄野武宏 (悲しみ)</option>
   <option value="42">42: ちび式じい</option>
   <option value="43">43: 櫻歌ミコ (ノーマル)</option>
   <option value="44">44: 櫻歌ミコ (第二形態)</option>
   <option value="45">45: 櫻歌ミコ (ロリ)</option>
   <option value="46">46: 小夜/SAYO</option>
   <option value="47">47: ナースロボ タイプT (ノーマル)</option>
   <option value="48">48: ナースロボ タイプT (楽々)</option>
   <option value="49">49: ナースロボ タイプT (恐怖)</option>
   <option value="50">50: ナースロボ タイプT (内緒話)</option>
   <option value="51">51: 聖騎士 紅桜</option>
   <option value="52">52: 雀松朱司</option>
   <option value="53">53: 麒ヶ島宗麟</option>
   <option value="54">54: 春歌ナナ</option>
   <option value="55">55: 猫使アル (ノーマル)</option>
   <option value="56">56: 猫使アル (おちつき)</option>
   <option value="57">57: 猫使アル (うきうき)</option>
   <option value="58">58: 猫使ビィ (ノーマル)</option>
   <option value="59">59: 猫使ビィ (おちつき)</option>
   <option value="60">60: 猫使ビィ (人見知り)</option>
  </select>
  <label>スピーカー音量 (0-255)</label><input id="volume" type="number" min="0" max="255" placeholder="{{volume}}">
  <label>マイク感度 (1-255)</label><input id="mic" type="number" min="1" max="255" placeholder="{{mic}}">
 </div>
 <div class="card">
  <b>天気の場所</b>
  <label>地名 (読み上げ用)</label><input id="place" placeholder="{{place}}">
  <label>緯度</label><input id="lat" type="number" step="0.0001" placeholder="{{lat}}">
  <label>経度</label><input id="lon" type="number" step="0.0001" placeholder="{{lon}}">
 </div>
 <button type="submit">保存して再起動</button>
</form>
<button class="danger" onclick="clearKeys()">鍵情報クリア (Wi-Fi・APIキーを消去)</button>
<script>
 document.getElementById("speaker").value="{{speaker}}";
 function save(e){
  e.preventDefault();
  const danger=["gemini","claude","openai","voicevox","provider"].some(id=>document.getElementById(id).value!=="");
  if(danger && !confirm("APIキー(重要設定)を変更しようとしています。\n\n・新しいAPIキーは取得してありますか？\n・キーの管理(残高や有効期限)はできていますか？\n\n不用意な変更は動作不能につながります。\nこのまま保存してよいですか？")) return;
  const f=new FormData();
  for(const id of ["ssid","pass","gemini","claude","provider","openai","voicevox","wake","wake_en","vad","speaker","volume","mic","place","lat","lon"]){
   const v=document.getElementById(id).value;
   if(v!=="") f.append(id,v);
  }
  fetch("/wifi_set",{method:"POST",body:f}).then(r=>r.text()).then(t=>{
   document.body.innerHTML="<h1>保存しました</h1><p>再起動します。数秒後にWi-Fiへ接続します。</p>"+
    "<p><a style='color:#7fb0ff;font-size:17px' href='/'>&larr; メニューへ戻る（再起動が終わる10秒後くらいにどうぞ）</a></p>";
  });
 }
 function clearKeys(){
  if(!confirm("⚠️ Wi-Fi情報とAPIキーをすべて消去します。\n\n消去すると、APIキーを再入力するまで一切動かなくなります。\nキーを再入力できる人 (APIキー管理者) だけが実行してください。\n\n本当に消去しますか？"))return;
  fetch("/clear_keys",{method:"POST"}).then(r=>r.text()).then(t=>{
   document.body.innerHTML="<h1>消去しました</h1><p>再起動して設定モード(AP)で立ち上がります。<br>"+
    "Wi-Fi「ChibiStackChan-Setup」(パスワード: stackchan) に接続して http://192.168.4.1 を開いてください。</p>";
  });
 }
</script>
</body></html>)KEWL";

// 設定モード(APポータル)専用のシンプル画面。初めて使う人が「今どっちの設定を
// しているのか」「何を入力すればよいのか」で迷わないよう、Wi-Fi の2項目だけに絞る。
// APIキー管理者が鍵情報クリア後にキーを入れ直す場合は末尾のリンクから全設定を開ける
static const char SETUP_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html><html lang="ja"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ちびスタックちゃん はじめの設定</title><style>
 body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:18px}
 .badge{background:#dc6e00;color:#fff;font-weight:bold;padding:11px 14px;border-radius:8px;
        max-width:480px;box-sizing:border-box}
 h1{font-size:22px;margin:18px 0 6px} p{color:#bbb;line-height:1.7}
 .card{background:#1d1d22;border-radius:12px;padding:16px;margin:14px 0;max-width:480px}
 label{display:block;font-size:14px;color:#aaa;margin-top:12px}
 input{width:100%;box-sizing:border-box;font-size:17px;padding:12px;border-radius:8px;
       border:1px solid #444;background:#222;color:#fff}
 button{font-size:18px;padding:16px 18px;margin-top:18px;border:0;border-radius:10px;
        background:#2d6cff;color:#fff;width:100%;max-width:480px;display:block}
 small{color:#888} a{color:#7fb0ff}
</style></head><body>
<div class="badge">&#128295; SETUP MODE / はじめの設定</div>
<h1>Wi-Fi につなぐ</h1>
<p>ちびスタックちゃんを、おうちの Wi-Fi につなぎます。<br>
入力するのは、下の<b>2つだけ</b>です。</p>
<form onsubmit="save(event)">
 <div class="card">
  <label>Wi-Fi の名前 (SSID)</label>
  <input id="ssid" required placeholder="例: aterm-a1b2c3-g">
  <label>Wi-Fi のパスワード</label>
  <input id="pass" type="password">
  <p><small>&#9888;&#65039; <b>2.4GHz</b> の Wi-Fi のみ対応です。名前の最後が
  「-a」「-5g」などの5GHz用にはつながりません。</small></p>
 </div>
 <button type="submit">保存してつなぐ</button>
</form>
<p style="margin-top:22px"><small>音量・声・キャラなどの設定は、つながったあとに
できます。<br><br><a href="/wifi?full=1">すべての設定を表示（APIキー管理者向け）</a></small></p>
<script>
 function save(e){
  e.preventDefault();
  const f=new FormData();
  f.append("ssid",document.getElementById("ssid").value);
  f.append("pass",document.getElementById("pass").value);
  fetch("/wifi_set",{method:"POST",body:f}).then(r=>r.text()).then(t=>{
   document.body.innerHTML="<h1>保存しました</h1>"+
    "<p>再起動して Wi-Fi につなぎます。<br>"+
    "スマホの Wi-Fi を、<b>おうちの Wi-Fi に戻してください</b>。</p>"+
    "<p>うまくつながると、10秒ほどで「こんにちは」と挨拶します。<br>"+
    "挨拶しないときは、入力し直してもう一度お試しください。</p>";
  });
 }
</script>
</body></html>)KEWL";

static const char ROLE_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html><html lang="ja"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ロール設定</title>
<style>body{font-family:sans-serif;background:#111;color:#eee;padding:18px}
textarea{width:100%;height:200px;font-size:15px;background:#222;color:#fff;border-radius:8px}
button{font-size:17px;padding:12px 18px;margin-top:12px;border:0;border-radius:10px;background:#2d6cff;color:#fff}</style>
</head><body>
<h1>ロール設定</h1>
<p><a style="color:#7fb0ff" href="/">&larr; メニューへ戻る</a></p>
<p>キャラ設定(システムプロンプト)を入力。空で送ると既定に戻ります。<br>
<span style="color:#e6b34d">感情タグ・タイマータグのルールは自動で追記されます (書かなくてOK)。</span></p>
<form onsubmit="postData(event)">
 <textarea id="textarea"></textarea><br>
 <button type="submit">送信</button>
</form>
<script>
function postData(e){
 e.preventDefault();
 const xhr=new XMLHttpRequest();
 xhr.open("POST","/role_set",true);
 xhr.setRequestHeader("Content-Type","text/plain;charset=UTF-8");
 xhr.onload=()=>{document.open();document.write(xhr.responseText);document.close();};
 xhr.send(document.getElementById("textarea").value.trim());
}
</script></body></html>)KEWL";

void handle_config();  // forward (handleNotFound から使う)

// 結果表示などの簡易ページ。必ず「メニューへ戻る」リンクを付ける
static String htmlPage(const String& inner) {
  return String(HEAD_HTML) +
    "<body style=\"font-family:sans-serif;background:#111;color:#eee;padding:18px;"
    "font-size:17px;line-height:1.9\">" + inner +
    "<p style=\"margin-top:24px\"><a style=\"color:#7fb0ff;font-size:17px\" href=\"/\">"
    "&larr; メニューへ戻る</a></p></body></html>";
}

void handleNotFound() {
  if (portal_mode) {  // ポータル中はどのURLでも設定ページへ (captive-portal風)
    handle_config();
    return;
  }
  server.send(404, "text/html", htmlPage("ページが見つかりません"));
}

void handle_config() {
  // 設定モード(AP)中は Wi-Fi 入力だけの簡易画面を出す。
  // 通常時の設定画面と見分けが付かず、初めての人が迷うのを防ぐため。
  // APIキー管理者が全項目を触りたいときは /wifi?full=1 で従来の画面を開ける
  if (portal_mode && !server.hasArg("full")) {
    server.send(200, "text/html", SETUP_HTML);
    return;
  }
  String html = FPSTR(CONFIG_HTML);
  // いまどちらのモードで開いているかを最上部に明示する
  html.replace("{{mode}}", portal_mode
      ? "<div style=\"background:#dc6e00;color:#fff;font-weight:bold;padding:11px 14px;"
        "border-radius:8px;max-width:480px;box-sizing:border-box\">"
        "&#128295; SETUP MODE（設定モードで接続中）</div>"
      : "<div style=\"background:#16301c;color:#8fd89f;padding:11px 14px;border-radius:8px;"
        "max-width:480px;box-sizing:border-box\">"
        "&#9989; 通常モード（おうちの Wi-Fi に接続中）</div>");
  html.replace("{{ssid}}", CFG_WIFI_SSID == "" ? "(未設定)" : CFG_WIFI_SSID);
  html.replace("{{provider}}", AI_PROVIDER == "claude" ? "Claude" : "Gemini");
  html.replace("{{wake}}", WAKE_PHRASE);
  html.replace("{{wen1}}", wake_mode == 1 ? "selected" : "");
  html.replace("{{wen2}}", wake_mode == 2 ? "selected" : "");
  html.replace("{{wen0}}", wake_mode == 0 ? "selected" : "");
  html.replace("{{vad}}", String(vad_threshold));
  html.replace("{{speaker}}", TTS_SPEAKER_NO);
  html.replace("{{volume}}", String(M5.Speaker.getVolume()));
  html.replace("{{mic}}", String(M5.Mic.config().magnification));
  html.replace("{{place}}", WEATHER_PLACE);
  html.replace("{{lat}}", String(WEATHER_LAT, 4));
  html.replace("{{lon}}", String(WEATHER_LON, 4));
  server.send(200, "text/html", html);
}

void handle_config_set() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "POST only");
    return;
  }
  String ssid = server.arg("ssid");
  if (ssid != "") {
    nvs_set_string("wifi", "ssid", ssid);
    nvs_set_string("wifi", "pass", server.arg("pass"));  // 空=オープンネットワーク
  }
  if (server.arg("gemini") != "")   nvs_set_string("chibi", "gemini", server.arg("gemini"));
  if (server.arg("claude") != "")   nvs_set_string("chibi", "claude", server.arg("claude"));
  if (server.arg("provider") != "") nvs_set_string("chibi", "provider", server.arg("provider"));
  if (server.arg("openai") != "")   nvs_set_string("chibi", "openai", server.arg("openai"));
  if (server.arg("voicevox") != "") nvs_set_string("chibi", "voicevox", server.arg("voicevox"));
  if (server.arg("wake") != "")     nvs_set_string("chibi", "wake", server.arg("wake"));
  if (server.arg("wake_en") != "")  nvs_set_u8v("chibi", "wake_en", constrain(server.arg("wake_en").toInt(), 0, 2));
  if (server.arg("vad") != "")      nvs_set_string("chibi", "vad", server.arg("vad"));
  if (server.arg("place") != "")    nvs_set_string("chibi", "place", server.arg("place"));
  if (server.arg("lat") != "")      nvs_set_string("chibi", "lat", server.arg("lat"));
  if (server.arg("lon") != "")      nvs_set_string("chibi", "lon", server.arg("lon"));

  uint32_t h;
  if (ESP_OK == nvs_open("setting", NVS_READWRITE, &h)) {
    if (server.arg("volume") != "") {
      uint32_t vol = constrain(server.arg("volume").toInt(), 0, 255);
      nvs_set_u32(h, "volume", vol);
    }
    if (server.arg("speaker") != "") {
      nvs_set_u8(h, "speaker", constrain(server.arg("speaker").toInt(), 0, 60));
    }
    if (server.arg("mic") != "") {
      nvs_set_u8(h, "micgain", constrain(server.arg("mic").toInt(), 1, 255));
    }
    nvs_close(h);
  }

  // 何か保存された = 意図した設定があるので secrets.h フォールバック禁止を解除
  nvs_set_u8v("chibi", "nofallback", 0);

  Serial.println("[CFG] settings saved, restarting...");
  server.send(200, "text/plain", "OK");
  delay(1200);
  ESP.restart();
}

// 鍵情報クリア: Wi-Fi と APIキー等 (NVS) を全消去して再起動 → APポータルへ
void handle_clear_keys() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "POST only");
    return;
  }
  uint32_t h;
  if (ESP_OK == nvs_open("wifi", NVS_READWRITE, &h)) {
    nvs_erase_all(h);
    nvs_close(h);
  }
  if (ESP_OK == nvs_open("chibi", NVS_READWRITE, &h)) {
    nvs_erase_all(h);
    nvs_close(h);
  }
  // secrets.h の初期値でも繋がらないように (次の保存で解除される)
  nvs_set_u8v("chibi", "nofallback", 1);
  Serial.println("[CFG] keys cleared, restarting...");
  server.send(200, "text/plain", "OK");
  delay(1200);
  ESP.restart();
}

// ====================================================================
//  Gemini
// ====================================================================
String https_post_json_gemini(const char* url, const char* json_string,
                              const char* root_ca, const char* api_key) {
  String payload = "";
  GEMINI_LAST_HTTP_CODE = 0;
  WiFiClientSecure *client = new WiFiClientSecure;
  if (client) {
    client->setCACert(root_ca);
    {
      HTTPClient https;
      https.setTimeout(65000);

      String requestUrl = String(url) + "?key=" + String(api_key);
      Serial.print("[HTTPS] begin...\n");
      if (https.begin(*client, requestUrl)) {
        int httpCode = -1;
        for (int retry = 0; retry < 3; retry++) {
          Serial.print("[HTTPS] POST...\n");
          https.addHeader("Content-Type", "application/json");
          httpCode = https.POST((uint8_t *)json_string, strlen(json_string));

          if (httpCode > 0) {
            GEMINI_LAST_HTTP_CODE = httpCode;
            Serial.printf("[HTTPS] POST... code: %d\n", httpCode);
            if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
              payload = https.getString();
              break;
            }
            String errorPayload = https.getString();
            if (errorPayload != "") {
              Serial.println("[HTTPS] error payload:");
              Serial.println(errorPayload);
            }
            if (httpCode == HTTP_CODE_TOO_MANY_REQUESTS && retry < 2) {
              int waitMs = 1200 * (retry + 1);
              Serial.printf("[HTTPS] 429 Too Many Requests. retry in %d ms\n", waitMs);
              delay(waitMs);
              continue;
            }
            if (httpCode == HTTP_CODE_NOT_FOUND) {
              break;
            }
          } else {
            Serial.printf("[HTTPS] POST... failed, error: %s\n",
                          https.errorToString(httpCode).c_str());
            break;
          }
        }
        https.end();
      } else {
        Serial.printf("[HTTPS] Unable to connect\n");
      }
    }
    delete client;
  } else {
    Serial.println("Unable to create client");
  }
  return payload;
}

// OpenAI 形式の messages JSON を Gemini 形式へ変換
String buildGeminiRequestFromChatJson(const String& chatJson) {
  DynamicJsonDocument inDoc(1024 * 10);
  if (deserializeJson(inDoc, chatJson) != DeserializationError::Ok) {
    return "";
  }

  DynamicJsonDocument outDoc(1024 * 12);
  JsonArray contents = outDoc.createNestedArray("contents");
  bool hasSystem = false;

  JsonArray inMessages = inDoc["messages"].as<JsonArray>();
  for (JsonObject msg : inMessages) {
    const char* role = msg["role"] | "";
    const char* content = msg["content"] | "";
    if (!content || strlen(content) == 0) {
      continue;
    }

    if (String(role) == "system") {
      if (!hasSystem) {
        JsonObject systemInstruction = outDoc.createNestedObject("systemInstruction");
        JsonArray parts = systemInstruction.createNestedArray("parts");
        JsonObject part = parts.createNestedObject();
        part["text"] = content;
        hasSystem = true;
      }
      continue;
    }

    JsonObject contentObj = contents.createNestedObject();
    if (String(role) == "assistant") {
      contentObj["role"] = "model";
    } else {
      contentObj["role"] = "user";
    }

    JsonArray parts = contentObj.createNestedArray("parts");
    JsonObject part = parts.createNestedObject();
    part["text"] = content;
  }

  if (contents.size() == 0) {
    JsonObject contentObj = contents.createNestedObject();
    contentObj["role"] = "user";
    JsonArray parts = contentObj.createNestedArray("parts");
    JsonObject part = parts.createNestedObject();
    part["text"] = "こんにちは";
  }

  String result;
  serializeJson(outDoc, result);
  return result;
}

// 返答テキストから表情を選ぶ ([happy]等のタグ優先、なければキーワード)
Expression pickExpression(const String& textIn) {
  String t = textIn;
  t.toLowerCase();
  if (t.indexOf("[happy]") >= 0)   return Expression::Happy;
  if (t.indexOf("[sad]") >= 0)     return Expression::Sad;
  if (t.indexOf("[angry]") >= 0)   return Expression::Angry;
  if (t.indexOf("[sleepy]") >= 0)  return Expression::Sleepy;
  if (t.indexOf("[doubt]") >= 0)   return Expression::Doubt;
  if (t.indexOf("[neutral]") >= 0) return Expression::Neutral;

  const String& s = textIn;
  if (s.indexOf("ごめん") >= 0 || s.indexOf("残念") >= 0 || s.indexOf("悲し") >= 0 ||
      s.indexOf("つら") >= 0 || s.indexOf("さみし") >= 0)
    return Expression::Sad;
  if (s.indexOf("怒") >= 0 || s.indexOf("だめ") >= 0 || s.indexOf("ダメ") >= 0 ||
      s.indexOf("こら") >= 0)
    return Expression::Angry;
  if (s.indexOf("眠") >= 0 || s.indexOf("ねむ") >= 0 || s.indexOf("おやすみ") >= 0)
    return Expression::Sleepy;
  if (s.indexOf("？") >= 0 || s.indexOf("?") >= 0 || s.indexOf("わからない") >= 0 ||
      s.indexOf("かな") >= 0 || s.indexOf("はて") >= 0)
    return Expression::Doubt;
  if (s.indexOf("！") >= 0 || s.indexOf("!") >= 0 || s.indexOf("嬉し") >= 0 ||
      s.indexOf("楽し") >= 0 || s.indexOf("ありがと") >= 0 || s.indexOf("やった") >= 0 ||
      s.indexOf("すごい") >= 0 || s.indexOf("好き") >= 0)
    return Expression::Happy;
  return Expression::Neutral;
}

// 読み上げ前に "[emotion]" タグを除去
String stripExpressionTag(String s) {
  const char* tags[] = {"[happy]", "[sad]", "[angry]", "[sleepy]", "[doubt]", "[neutral]"};
  for (const char* tag : tags) {
    int idx;
    while ((idx = s.indexOf(tag)) >= 0) {
      s.remove(idx, strlen(tag));
    }
  }
  s.trim();
  return s;
}

// max_tokens で途中で切れた返答を、最後の文末 (。！？) までで切り詰める。
// 尻切れの文をそのまま読み上げると不自然なため
String trimToLastSentence(String s) {
  const char* ends[] = {"。", "！", "？", "!", "?"};
  int best = -1;
  for (const char* e : ends) {
    int i = s.lastIndexOf(e);
    if (i >= 0) {
      int end = i + strlen(e);
      if (end > best) best = end;
    }
  }
  if (best > 0) s = s.substring(0, best);
  return s;
}

// 秒数を「◯分◯秒」の読み上げ形式に
String formatDuration(uint32_t sec) {
  String m = "";
  if (sec >= 60) {
    m = String(sec / 60) + "分";
    if (sec % 60) m += String(sec % 60) + "秒";
  } else {
    m = String(sec) + "秒";
  }
  return m;
}

// Gemini返答中の [timer:...] タグを実行し、文面から除去する
String processTimerTag(String s) {
  int i = s.indexOf("[timer:");
  if (i < 0) return s;
  int e = s.indexOf(']', i);
  if (e < 0) return s;
  String arg = s.substring(i + 7, e);
  arg.trim();
  s.remove(i, e - i + 1);
  s.trim();
  if (arg == "cancel") {
    timer_end_ms = 0;
    if (s == "") s = "タイマーをやめたよ";
    printf("[TIMER] cancel\n");
  } else if (arg == "status") {
    if (timer_end_ms == 0) {
      s = "いまタイマーは動いてないよ";
    } else {
      uint32_t rem = (timer_end_ms - millis() + 999) / 1000;
      s = "あと" + formatDuration(rem) + "だよ";  // 残りはこちらで計算して答える
    }
  } else {
    long sec = arg.toInt();
    if (sec > 0) {
      if (sec > 12L * 3600) sec = 12L * 3600;  // 上限12時間
      const bool replaced = (timer_end_ms != 0);  // 動作中のタイマーを上書き
      timer_total_sec = (uint32_t)sec;
      timer_end_ms = millis() + (uint32_t)sec * 1000UL;
      if (timer_end_ms == 0) timer_end_ms = 1;  // 0 は「未設定」の意味なので回避
      if (s == "") s = formatDuration(sec) + "のタイマーをセットしたよ";
      else if (replaced) s += "。前のタイマーはやめてセットし直したよ";
      printf("[TIMER] set %ld sec%s\n", sec, replaced ? " (replaced)" : "");
    }
  }
  return s;
}

// Claude (Anthropic) 用 HTTPS POST。APIキーはURLではなくヘッダで渡す
String https_post_json_claude(const char* url, const char* json_string,
                              const char* root_ca, const char* api_key) {
  String payload = "";
  GEMINI_LAST_HTTP_CODE = 0;
  WiFiClientSecure *client = new WiFiClientSecure;
  if (client) {
    client->setCACert(root_ca);
    {
      HTTPClient https;
      https.setTimeout(65000);
      printf("[HTTPS] Claude begin...\n");
      if (https.begin(*client, url)) {
        for (int retry = 0; retry < 3; retry++) {
          https.addHeader("Content-Type", "application/json");
          https.addHeader("x-api-key", api_key);
          https.addHeader("anthropic-version", "2023-06-01");
          int httpCode = https.POST((uint8_t *)json_string, strlen(json_string));
          if (httpCode > 0) {
            GEMINI_LAST_HTTP_CODE = httpCode;
            printf("[HTTPS] Claude POST code: %d\n", httpCode);
            if (httpCode == HTTP_CODE_OK) {
              payload = https.getString();
              break;
            }
            String errorPayload = https.getString();
            if (errorPayload != "") printf("[HTTPS] error: %s\n", errorPayload.c_str());
            // 429=レート制限 / 529=過負荷 は少し待ってリトライ
            if ((httpCode == 429 || httpCode == 529) && retry < 2) {
              delay(1200 * (retry + 1));
              continue;
            }
            break;
          } else {
            printf("[HTTPS] Claude POST failed: %s\n", https.errorToString(httpCode).c_str());
            break;
          }
        }
        https.end();
      }
    }
    delete client;
  }
  return payload;
}

// chat_doc ({"messages":[{role,content},...]}) から Claude Messages API のリクエストを作る。
// Claude は system をメッセージ列と別枠で渡す点が Gemini と異なる。
String buildClaudeRequestFromChatJson(const String& json_string) {
  DynamicJsonDocument inDoc(1024 * 10);
  if (deserializeJson(inDoc, json_string) != DeserializationError::Ok) return "";
  DynamicJsonDocument outDoc(1024 * 12);
  outDoc["model"] = CLAUDE_MODEL;
  outDoc["max_tokens"] = 300;  // 返答は60文字程度なので十分
  JsonArray msgs = outDoc.createNestedArray("messages");
  for (JsonObject m : inDoc["messages"].as<JsonArray>()) {
    const char* role = m["role"];
    const char* content = m["content"];
    if (!role || !content) continue;
    if (strcmp(role, "system") == 0) {
      outDoc["system"] = content;
    } else {
      JsonObject o = msgs.createNestedObject();
      o["role"] = (strcmp(role, "assistant") == 0) ? "assistant" : "user";
      o["content"] = content;
    }
  }
  if (msgs.size() == 0) return "";
  String out;
  serializeJson(outDoc, out);
  return out;
}

String chatClaude(String json_string) {
  String response = "";
  avatar.setExpression(Expression::Doubt);  // 考え中
  if (CLAUDE_API_KEY == "") {
    avatar.setExpression(Expression::Neutral);
    return "クロードのエーピーアイキーが設定されていません。設定画面から入力してね。";
  }
  String req = buildClaudeRequestFromChatJson(json_string);
  if (req == "") {
    avatar.setExpression(Expression::Sad);
    delay(1000);
    avatar.setExpression(Expression::Neutral);
    return "エラーです";
  }
  String ret = https_post_json_claude(CLAUDE_API_URL, req.c_str(),
                                      root_ca_anthropic, CLAUDE_API_KEY.c_str());
  avatar.setExpression(Expression::Neutral);
  printf("%s\n", ret.c_str());
  if (ret != "") {
    DynamicJsonDocument doc(8000);
    if (deserializeJson(doc, ret.c_str()) != DeserializationError::Ok) {
      avatar.setExpression(Expression::Sad);
      response = "エラーです";
      delay(1000);
      avatar.setExpression(Expression::Neutral);
    } else {
      const char* data = doc["content"][0]["text"];
      if (data) {
        response = String(data);
        std::replace(response.begin(), response.end(), '\n', ' ');
        g_reply_expression = pickExpression(response);
        response = stripExpressionTag(response);
        response = processTimerTag(response);
        const char* stop = doc["stop_reason"];
        if (stop && strcmp(stop, "max_tokens") == 0) {
          response = trimToLastSentence(response);
          printf("[AI] max_tokensで途切れたため文末まで切り詰め\n");
        }
      } else {
        response = "わかりません";
      }
    }
  } else {
    avatar.setExpression(Expression::Sad);
    if (GEMINI_LAST_HTTP_CODE == 429 || GEMINI_LAST_HTTP_CODE == 529) {
      response = "ただいま混み合っています。少し待ってからもう一度話しかけてね。";
    } else if (GEMINI_LAST_HTTP_CODE == 401 || GEMINI_LAST_HTTP_CODE == 403) {
      response = "クロードのエーピーアイキーが正しくないみたい。設定を確認してね。";
    } else {
      response = "わかりません";
    }
    delay(1000);
    avatar.setExpression(Expression::Neutral);
  }
  return response;
}

String chatGemini(String json_string) {
  String response = "";
  avatar.setExpression(Expression::Doubt);  // 考え中
  String geminiRequest = buildGeminiRequestFromChatJson(json_string);
  if (geminiRequest == "") {
    avatar.setExpression(Expression::Sad);
    delay(1000);
    avatar.setExpression(Expression::Neutral);
    return "エラーです";
  }
  String ret = https_post_json_gemini(GEMINI_API_URL, geminiRequest.c_str(),
                                      root_ca_google, GEMINI_API_KEY.c_str());
  avatar.setExpression(Expression::Neutral);
  Serial.println(ret);
  if (ret != "") {
    DynamicJsonDocument doc(5000);
    DeserializationError error = deserializeJson(doc, ret.c_str());
    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.f_str());
      avatar.setExpression(Expression::Sad);
      response = "エラーです";
      delay(1000);
      avatar.setExpression(Expression::Neutral);
    } else {
      const char* data = doc["candidates"][0]["content"]["parts"][0]["text"];
      Serial.println(data);
      if (data) {
        response = String(data);
        std::replace(response.begin(), response.end(), '\n', ' ');
        g_reply_expression = pickExpression(response);
        response = stripExpressionTag(response);
        response = processTimerTag(response);
        const char* finish = doc["candidates"][0]["finishReason"];
        if (finish && strcmp(finish, "MAX_TOKENS") == 0) {
          response = trimToLastSentence(response);
          printf("[AI] max_tokensで途切れたため文末まで切り詰め\n");
        }
      } else {
        response = "わかりません";
      }
    }
  } else {
    avatar.setExpression(Expression::Sad);
    if (GEMINI_LAST_HTTP_CODE == HTTP_CODE_TOO_MANY_REQUESTS) {
      response = "ただいま混み合っています。少し待ってからもう一度話しかけてね。";
    } else if (GEMINI_LAST_HTTP_CODE == HTTP_CODE_NOT_FOUND) {
      response = "Geminiのモデルまたはエーピーアイ設定が見つかりません。設定を確認してください。";
    } else {
      response = "わかりません";
    }
    delay(1000);
    avatar.setExpression(Expression::Neutral);
  }
  return response;
}

// 質問に「現在日時」+ (天気の話題なら)「今日の天気」を参考情報として添付し、
// Gemini に投げる。返答は speech_text 経由で読み上げられる。
// 数字列 + 時間/分/秒 の並びから合計秒数を取り出す (見つからなければ0)。
// 「三分」等の漢数字と「三十」のような十の位も簡易対応。
uint32_t parseDurationSec(const String& in) {
  String t = in;
  const char* kanji[] = {"〇", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  for (int d = 0; d <= 9; d++) t.replace(kanji[d], String(d));
  int i;
  while ((i = t.indexOf("十")) >= 0) {  // a十b -> a*10+b (aは省略で1, bは省略で0)
    int a = 1, b = 0, start = i, end = i + 3;
    if (i >= 1 && isDigit(t[i - 1])) { a = t[i - 1] - '0'; start = i - 1; }
    if ((int)t.length() > i + 3 && isDigit(t[i + 3])) { b = t[i + 3] - '0'; end = i + 4; }
    t = t.substring(0, start) + String(a * 10 + b) + t.substring(end);
  }
  uint32_t total = 0;
  const int n = t.length();
  i = 0;
  while (i < n) {
    if (isDigit(t[i])) {
      long v = 0;
      int j = i;
      while (j < n && isDigit(t[j])) { v = v * 10 + (t[j] - '0'); j++; }
      if (t.startsWith("時間", j))     { total += v * 3600; j += 6; }
      else if (t.startsWith("分", j))  { total += v * 60;   j += 3; }
      else if (t.startsWith("秒", j))  { total += v;        j += 3; }
      i = j;
    } else {
      i++;
    }
  }
  return total;
}

// タイマー系の定型コマンドを本体だけで処理する (Gemini混雑・圏外でも動くように)。
// 処理したら speech_text をセットして true を返す。
bool tryLocalTimerCommand(const String& text) {
  const bool mentionsTimer = (text.indexOf("タイマー") >= 0) || (text.indexOf("たいまー") >= 0);
  if (mentionsTimer && (text.indexOf("やめ") >= 0 || text.indexOf("止め") >= 0 ||
                        text.indexOf("ストップ") >= 0 || text.indexOf("キャンセル") >= 0 ||
                        text.indexOf("中止") >= 0)) {
    timer_end_ms = 0;
    g_reply_expression = Expression::Neutral;
    speech_text = "タイマーをやめたよ";
    printf("[TIMER] cancel (local)\n");
    return true;
  }
  if (mentionsTimer && (text.indexOf("あと") >= 0 || text.indexOf("残り") >= 0 ||
                        text.indexOf("何分") >= 0 || text.indexOf("何秒") >= 0)) {
    g_reply_expression = Expression::Neutral;
    speech_text = (timer_end_ms == 0)
        ? String("いまタイマーは動いてないよ")
        : "あと" + formatDuration((timer_end_ms - millis() + 999) / 1000) + "だよ";
    return true;
  }
  const bool asksMeasure = mentionsTimer || text.indexOf("はかって") >= 0 ||
                           text.indexOf("計って") >= 0 || text.indexOf("測って") >= 0;
  if (asksMeasure) {
    uint32_t sec = parseDurationSec(text);
    if (sec > 0) {
      if (sec > 12UL * 3600) sec = 12UL * 3600;
      const bool replaced = (timer_end_ms != 0);  // 動作中のタイマーを上書き
      timer_total_sec = sec;
      timer_end_ms = millis() + sec * 1000UL;
      if (timer_end_ms == 0) timer_end_ms = 1;
      g_reply_expression = Expression::Happy;
      speech_text = formatDuration(sec) + "のタイマーに" +
                    (replaced ? "セットし直したよ" : "セットしたよ");
      printf("[TIMER] set %u sec (local%s)\n", (unsigned)sec, replaced ? ", replaced" : "");
      return true;
    }
  }
  return false;
}

String exec_chat(String text) {
  // ローカルコマンド: IPアドレス読み上げ (画面表示が小さくて読めない時用)。
  // Whisper は「アイピー」を "IP" とアルファベットで書き起こすことが多いため、
  // 表記ゆれ (アイピー/あいぴー/IP/ip) を広めに拾う
  String lower = text;
  lower.toLowerCase();  // ASCII のみ小文字化 (日本語はそのまま)
  const bool mentionsIp = text.indexOf("アイピー") >= 0 ||
                          text.indexOf("あいぴー") >= 0 ||
                          lower.indexOf("ip") >= 0;
  const bool asksIp = mentionsIp &&
      (text.indexOf("アドレス") >= 0 || text.indexOf("教え") >= 0 ||
       text.indexOf("何") >= 0 || text.indexOf("なに") >= 0 ||
       text.indexOf("いくつ") >= 0);
  if (asksIp) {
    String ip = WiFi.localIP().toString();
    ip.replace(".", "、てん、");
    g_reply_expression = Expression::Happy;
    speech_text = "ぼくのアイピーアドレスは、" + ip + "、だよ";
    return speech_text;
  }
  // タイマー系の定型はGeminiを介さず即応答 (混雑時でも確実に動く)
  if (tryLocalTimerCommand(text)) {
    return speech_text;
  }
  init_chat_doc(InitBuffer.c_str());
  chatHistory.push_back(text);
  if (chatHistory.size() > MAX_HISTORY * 2) {
    chatHistory.pop_front();
    chatHistory.pop_front();
  }

  JsonArray messages = chat_doc["messages"];
  for (int i = 0; i < (int)chatHistory.size(); i++) {
    JsonObject m = messages.createNestedObject();
    m["role"] = (i % 2 == 0) ? "user" : "assistant";
    m["content"] = chatHistory[i];
  }

  // 最新のユーザ発話に参考情報を添付 (履歴には残さない)
  String ctx = "";
  String now = wc::nowJa();
  if (now != "") ctx += "現在日時: " + now + "。";
  if (wc::wantsWeather(text)) {
    avatar.setExpression(Expression::Doubt);
    String weather;
    if (wc::fetchTodayWeather(weather)) ctx += weather;
  }
  if (ctx != "") {
    messages[messages.size() - 1]["content"] = text + "\n(参考情報: " + ctx + ")";
  }

  String json_string;
  serializeJson(chat_doc, json_string);

  String response = (AI_PROVIDER == "claude") ? chatClaude(json_string)
                                              : chatGemini(json_string);
  speech_text = response;
  chatHistory.push_back(response);
  return response;
}

// ====================================================================
//  Web handlers (通常運用時)
// ====================================================================
void handleRoot() {
  if (portal_mode) {
    handle_config();
    return;
  }
  String html = String(HEAD_HTML) +
    "<body style=\"font-family:sans-serif;background:#111;color:#eee;padding:18px\">"
    "<h1>ちびスタックちゃん</h1><ul style=\"line-height:2.2;font-size:18px\">"
    "<li><a style=\"color:#7fb0ff\" href=\"/wifi\">設定 (Wi-Fi / APIキー / 呼びかけ / 天気)</a></li>"
    "<li><a style=\"color:#7fb0ff\" href=\"/role\">ロール (キャラ設定)</a></li>"
    "<li><a style=\"color:#7fb0ff\" href=\"/role_get\">現在のロール確認</a></li></ul>"
    "<form action=\"/speech\"><input name=\"say\" placeholder=\"しゃべらせる\" style=\"font-size:16px;padding:8px\">"
    "<button style=\"font-size:16px;padding:8px\">送信</button></form>"
    "<form action=\"/chat\"><input name=\"text\" placeholder=\"文字で質問する\" style=\"font-size:16px;padding:8px\">"
    "<button style=\"font-size:16px;padding:8px\">送信</button></form>"
    "</body></html>";
  server.send(200, "text/html", html);
}

void handle_speech() {
  String message = server.arg("say");
  String speaker = server.arg("voice");
  if (speaker != "") {
    TTS_PARMS = TTS_SPEAKER + speaker;
  }
  Serial.println(message);
  if (message != "" && speech_text == "" && speech_text_buffer == "") {
    g_reply_expression = Expression::Happy;
    speech_text = message;
    String safe = message;
    safe.replace("<", "&lt;");
    server.send(200, "text/html", htmlPage("しゃべります: " + safe));
  } else if (message == "") {
    server.send(200, "text/html", htmlPage("say パラメータがありません"));
  } else {
    server.send(200, "text/html",
                htmlPage("いま再生・処理中みたいです。少し待ってからもう一度どうぞ"));
  }
}

void handle_chat() {
  String text = server.arg("text");
  String speaker = server.arg("voice");
  if (speaker != "") {
    TTS_PARMS = TTS_SPEAKER + speaker;
  }
  String response;
  if (text == "") {
    response = "text パラメータがありません";
  } else if (speech_text == "" && speech_text_buffer == "") {
    response = exec_chat(text);
  } else {
    response = "いま再生・処理中みたいです。少し待ってからもう一度どうぞ";
  }
  String safe = response;
  safe.replace("<", "&lt;");
  server.send(200, "text/html", htmlPage(safe +
    "<form action=\"/chat\" style=\"margin-top:18px\">"
    "<input name=\"text\" placeholder=\"続けて質問する\" style=\"font-size:16px;padding:8px\">"
    "<button style=\"font-size:16px;padding:8px\">送信</button></form>"));
}

void handle_face() {
  String expression = server.arg("expression");
  int idx = expression.toInt();
  const int n = sizeof(expressions_table) / sizeof(expressions_table[0]);
  if (idx >= 0 && idx < n) avatar.setExpression(expressions_table[idx]);
  server.send(200, "text/html", htmlPage("表情を変えました"));
}

void handle_setting() {
  String value = server.arg("volume");
  String speaker = server.arg("speaker");
  String mic = server.arg("mic");

  size_t speaker_no = 0;
  if (speaker != "") {
    speaker_no = constrain(speaker.toInt(), 0, 60);
    TTS_SPEAKER_NO = String(speaker_no);
    TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
  }

  size_t micgain = 0;
  if (mic != "") {
    micgain = constrain(mic.toInt(), 1, 255);
    auto micConfig = M5.Mic.config();
    micConfig.magnification = (uint8_t)micgain;
    M5.Mic.config(micConfig);
  }

  size_t volume = constrain(value.toInt(), 0, 255);
  uint32_t nvs_handle;
  if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
    if (value != "") nvs_set_u32(nvs_handle, "volume", volume);
    if (speaker != "") nvs_set_u8(nvs_handle, "speaker", speaker_no);
    if (mic != "") nvs_set_u8(nvs_handle, "micgain", (uint8_t)micgain);
    nvs_close(nvs_handle);
  }
  if (value != "") {
    M5.Speaker.setVolume(volume);
    M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
  }
  server.send(200, "text/html", htmlPage("設定を反映しました"));
}

bool save_json() {
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return false;
  }
  File file = SPIFFS.open("/data.json", "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
    return false;
  }
  serializeJson(chat_doc, file);
  file.close();
  return true;
}

void handle_role() {
  server.send(200, "text/html", ROLE_HTML);
}

// 現在のロール本文をスマホでも読みやすく表示する。
// 自動追記される固定ルール (感情タグ・タイマータグ) は琥珀色で区別する。
String buildRoleViewHtml(const String& title) {
  init_chat_doc(InitBuffer.c_str());
  const char* content = chat_doc["messages"][0]["content"];
  String roleText = content ? String(content) : "(未設定)";
  roleText.replace("<", "&lt;");
  String rules = String(kExpressionTagRule);  // 自動追記はここから始まる
  rules.replace("<", "&lt;");
  int idx = roleText.indexOf(rules);
  if (idx >= 0) {
    roleText = roleText.substring(0, idx) +
               "<span style=\"color:#e6b34d\">" + roleText.substring(idx) + "</span>";
  }
  String html = String(HEAD_HTML) +
    "<body style=\"font-family:sans-serif;background:#111;color:#eee;padding:18px\">"
    "<h1 style=\"font-size:20px\">" + title + "</h1>"
    "<div style=\"font-size:17px;line-height:1.9;background:#1d1d22;border-radius:12px;"
    "padding:16px;white-space:pre-wrap;word-break:break-word\">" + roleText + "</div>"
    "<p style=\"color:#888;font-size:13px\">※<span style=\"color:#e6b34d\">色付き部分</span>は"
    "表情・タイマー機能を動かすために本体が自動追記する固定ルールです。"
    "ロール変更時に入力する必要はありません (キャラ設定の文章だけでOK)</p>"
    "<p style=\"line-height:2\"><a style=\"color:#7fb0ff;font-size:17px\" href=\"/role\">ロールを変更する</a>"
    "　<a style=\"color:#7fb0ff;font-size:17px\" href=\"/\">メニューへ戻る</a></p>"
    "</body></html>";
  return html;
}

void handle_role_set() {
  if (server.method() != HTTP_POST) {
    return;
  }
  String role = server.arg("plain");
  applyRole(role != "" ? role : String(kDefaultRole));
  chatHistory.clear();
  init_chat_doc(InitBuffer.c_str());
  save_json();
  server.send(200, "text/html", buildRoleViewHtml("保存しました (現在のロール)"));
}

void handle_role_get() {
  server.send(200, "text/html", buildRoleViewHtml("現在のロール"));
}

bool routes_registered = false;
void registerRoutes() {
  if (routes_registered) return;
  routes_registered = true;
  server.on("/", handleRoot);
  server.on("/wifi", handle_config);
  server.on("/wifi_set", HTTP_POST, handle_config_set);
  server.on("/clear_keys", HTTP_POST, handle_clear_keys);
  server.on("/speech", handle_speech);
  server.on("/chat", handle_chat);
  server.on("/face", handle_face);
  server.on("/setting", handle_setting);
  server.on("/role", handle_role);
  server.on("/role_set", HTTP_POST, handle_role_set);
  server.on("/role_get", handle_role_get);
  server.onNotFound(handleNotFound);
}

// ====================================================================
//  Audio (TTS 再生まわり)
// ====================================================================
AudioOutputM5Speaker out(&M5.Speaker, m5spk_virtual_channel);
AudioGeneratorMP3 *mp3;
AudioFileSourceBuffer *buff = nullptr;
int preallocateBufferSize = 30 * 1024;
uint8_t *preallocateBuffer;
AudioFileSourceHTTPSStream *file = nullptr;
AudioFileSourceSPIFFS *sfile = nullptr;  // SPIFFSキャッシュ再生用 (起床ボイス等)

void playMP3(AudioFileSourceBuffer *buff) {
  mp3->begin(buff, &out);
}

void StatusCallback(void *cbData, int code, const char *string) {
  const char *ptr = reinterpret_cast<const char *>(cbData);
  char s1[64];
  strncpy_P(s1, string, sizeof(s1));
  s1[sizeof(s1) - 1] = 0;
  Serial.printf("STATUS(%s) '%d' = '%s'\n", ptr, code, s1);
  Serial.flush();
}

void stopPlayback() {
  if (mp3->isRunning()) mp3->stop();
  if (buff != nullptr) { delete buff; buff = nullptr; }
  if (file != nullptr) { delete file; file = nullptr; }
  if (sfile != nullptr) { delete sfile; sfile = nullptr; }
}

// TTS を再生し終わるまでブロックする ("なあに？" 等の短い相槌用)
void speakBlocking(const String& text) {
  M5.Mic.end();
  M5.Speaker.begin();
  Voicevox_tts((char*)text.c_str(), (char*)TTS_PARMS.c_str());
  // 首振り・リップシンクは servo/lipSync タスク側が動かすのでここでは再生のみ
  uint32_t start = millis();
  while (mp3->isRunning() && millis() - start < 30000) {
    if (!mp3->loop()) break;
    delay(1);
  }
  stopPlayback();
  delay(200);
  M5.Speaker.end();
  M5.Mic.begin();
}

// ====================================================================
//  おやすみ / 起床 (3分会話が無いと眠り、声で起きる)
// ====================================================================
constexpr uint32_t kSleepAfterMs = 3UL * 60UL * 1000UL;
static const char kWakeUpSpeech[]  = "じいいいいええええ！寝てしまった！";
static const char kWakeVoicePath[] = "/wake.mp3";  // SPIFFSキャッシュ (ネット不要で即発声)
uint32_t last_activity_ms = 0;  // 最後に「会話」があった時刻 (認識成功/発話/ボタン)
bool sleeping = false;

// SPIFFS にキャッシュした MP3 を再生し終わるまでブロック。無ければ false
bool speakFromSPIFFS(const char* path) {
  if (!SPIFFS.begin(true) || !SPIFFS.exists(path)) return false;
  M5.Mic.end();
  M5.Speaker.begin();
  sfile = new AudioFileSourceSPIFFS(path);
  buff = new AudioFileSourceBuffer(sfile, preallocateBuffer, preallocateBufferSize);
  mp3->begin(buff, &out);
  uint32_t start = millis();
  while (mp3->isRunning() && millis() - start < 15000) {
    if (!mp3->loop()) break;
    delay(1);
  }
  stopPlayback();
  delay(200);
  M5.Speaker.end();
  M5.Mic.begin();
  return true;
}

// 起床ボイスを SPIFFS にキャッシュする。取得済みで話者も同じなら何もしない。
// 眠りに入るタイミング (どうせ暇) に呼ぶので、失敗しても起床時に通常TTSで喋れる
void prepareWakeVoice() {
  String cachedSpk = nvs_get_string("chibi", "wake_spk");
  if (cachedSpk == TTS_SPEAKER_NO && SPIFFS.begin(true) && SPIFFS.exists(kWakeVoicePath)) return;
  printf("[SLEEP] 起床ボイスをキャッシュ中 (話者%s)...\n", TTS_SPEAKER_NO.c_str());
  if (Voicevox_tts_download(kWakeUpSpeech, TTS_PARMS.c_str(), kWakeVoicePath)) {
    nvs_set_string("chibi", "wake_spk", TTS_SPEAKER_NO);
    printf("[SLEEP] 起床ボイス保存OK\n");
  } else {
    printf("[SLEEP] 起床ボイス取得失敗 (起床時は通常TTSで喋る)\n");
  }
}

// ====================================================================
//  Avatar tasks (lipSync / servo)
// ====================================================================
void lipSync(void *args) {
  float gazeX, gazeY;
  int level = 0;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;) {
    level = abs(*out.getBuffer());
    if (level < 100) level = 0;
    if (level > 15000) level = 15000;
    float open = (float)level / 15000.0;
    avatar->setMouthOpenRatio(open);
    avatar->getGaze(&gazeY, &gazeX);
    avatar->setRotation(gazeX * 5);
    delay(50);
  }
}

void servo(void *args) {
  (void)args;
  for (;;) {
    bool talking = (mp3 != nullptr) && mp3->isRunning();
    float level01 = 0.0f;
    if (talking) {
      int lv = abs(*out.getBuffer());
      if (lv < 100) lv = 0;
      if (lv > 15000) lv = 15000;
      level01 = (float)lv / 15000.0f;
    }
    head::update(talking, level01);
    delay(50);
  }
}

// ====================================================================
//  Wi-Fi setup / AP config portal
// ====================================================================
void startConfigPortal() {
  portal_mode = true;
  server.stop();

  // アバターの描画タスクは10msごとに顔を描き直すため、止めずに案内を描くと
  // すぐ顔で上書きされてしまう (長押しが効いていないように見える原因だった)。
  // NOTE: suspend() は使わないこと。Face::draw() は startWrite()〜endWrite() で
  // 画面バスのトランザクションを張るため、その途中で凍結されるとバスを掴んだまま
  // になり、この後の描画が壊れる。stop() はフラグを下ろすだけで、描画タスクは
  // 現在のフレームを描き切ってから自分で終了する (ポータルからは戻らないので
  // 再開は不要)
  if (avatar_ready) {
    avatar.stop();
    delay(200);  // 現在のフレームを描き終えてタスクが終了するのを待つ
    avatar_ready = false;
  }
  head::focus(true);  // 設定中はサーボを正面で止めておく

  // 設定モードに入ったことを音でも知らせる (画面を見ていなくても分かるように)
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.tone(880, 120);
  delay(160);
  M5.Speaker.tone(1320, 220);
  delay(300);
  M5.Speaker.end();

  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ChibiStackChan-Setup", "stackchan");
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[CFG] AP mode: SSID=ChibiStackChan-Setup pass=stackchan  http://%s\n",
                ip.toString().c_str());

  // 128x128 の小画面向け案内表示。一目で「設定モードだ」と分かるように
  // 上部にタイトルバーを敷く
  const uint16_t kBarColor = M5.Lcd.color565(220, 110, 0);
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.fillRect(0, 0, 128, 20, kBarColor);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, kBarColor);
  M5.Lcd.setCursor(3, 2);
  M5.Lcd.print("SETUP MODE");

  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Lcd.setCursor(2, 26);
  M5.Lcd.print("1) Connect WiFi");
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(8, 38);
  M5.Lcd.print("ChibiStackChan");
  M5.Lcd.setCursor(8, 47);
  M5.Lcd.print("-Setup");
  M5.Lcd.setCursor(8, 56);
  M5.Lcd.print("pass: stackchan");

  M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Lcd.setCursor(2, 72);
  M5.Lcd.print("2) Open browser");
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(8, 84);
  M5.Lcd.printf("http://%s", ip.toString().c_str());

  M5.Lcd.drawFastHLine(0, 100, 128, M5.Lcd.color565(70, 70, 70));
  M5.Lcd.setTextColor(M5.Lcd.color565(150, 150, 150), TFT_BLACK);
  M5.Lcd.setCursor(2, 106);
  M5.Lcd.print("Save -> auto reboot");
  M5.Lcd.setCursor(2, 116);
  M5.Lcd.print("Quit = unplug USB");

  registerRoutes();
  server.begin();
  for (;;) {
    server.handleClient();
    delay(5);
  }
}

void Wifi_setup() {
  if (CFG_WIFI_SSID == "") {
    Serial.println("[WiFi] no SSID stored -> config portal");
    startConfigPortal();  // 戻らない
  }
  Serial.printf("[WiFi] connecting to '%s'\n", CFG_WIFI_SSID.c_str());
  WiFi.begin(CFG_WIFI_SSID.c_str(), CFG_WIFI_PASS.c_str());

  // 接続中の案内。渡した相手の家では前の持ち主のSSIDが見つからず15秒待つことに
  // なるため、「画面を押せば設定に入れる」ことを画面上でも知らせる
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(2, 4);
  M5.Lcd.print("Connecting WiFi");
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(2, 16);
  M5.Lcd.printf("%.20s", CFG_WIFI_SSID.c_str());
  M5.Lcd.drawFastHLine(0, 60, 128, M5.Lcd.color565(70, 70, 70));
  M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Lcd.setCursor(2, 68);
  M5.Lcd.print("Press screen 1s");
  M5.Lcd.setCursor(2, 78);
  M5.Lcd.print("-> WiFi SETUP");
  M5.Lcd.setTextColor(M5.Lcd.color565(150, 150, 150), TFT_BLACK);
  M5.Lcd.setCursor(2, 100);
  M5.Lcd.print("or wait 15s for");
  M5.Lcd.setCursor(2, 110);
  M5.Lcd.print("auto setup mode");
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(2, 34);  // 以降の "." はここから並ぶ

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    M5.Lcd.print(".");
    Serial.print(".");
    // 接続作業中も画面(Aボタン)長押しで設定APへ入れる
    for (int i = 0; i < 10; i++) {
      M5.update();
      if (M5.BtnA.pressedFor(700)) {
        Serial.println("[WiFi] BtnA held -> config portal");
        startConfigPortal();
      }
      delay(50);
    }
    if (millis() - start > 15000) break;  // 15s timeout
  }
  M5.Lcd.println("");
  Serial.println("");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] connect failed -> config portal");
    M5.Lcd.println("WiFi NG");
    startConfigPortal();  // 戻らない (保存で再起動)
  }
}

// ====================================================================
//  音声認識 (Whisper) と呼びかけ判定
// ====================================================================
// ==== 呼びかけ語頭欠け対策: トリガ前 0.5 秒のプリロールバッファ ====
// VAD監視中の20msチャンクを常にリングバッファへ保存し、録音の先頭に前置する。
constexpr int  kVadChunkSamples = 320;  // 20ms @ 16kHz (vad_triggered と共通)
constexpr int  kPreRollChunks   = 50;   // 50 x 20ms = 1.0s (トリガ確認が遅れても語頭を保護)
static int16_t vad_history[kPreRollChunks * kVadChunkSamples];
static int     vad_hist_pos = 0;    // 次に書き込むチャンク番号
static int     vad_hist_count = 0;  // 有効チャンク数

// 履歴を古い順に out へ並べ、サンプル数を返す
static size_t vadHistoryCopy(int16_t* out) {
  int start = (vad_hist_pos - vad_hist_count + kPreRollChunks) % kPreRollChunks;
  for (int i = 0; i < vad_hist_count; i++) {
    int idx = (start + i) % kPreRollChunks;
    memcpy(&out[i * kVadChunkSamples], &vad_history[idx * kVadChunkSamples],
           kVadChunkSamples * sizeof(int16_t));
  }
  return (size_t)vad_hist_count * kVadChunkSamples;
}

// 背景色で状態を見せる: 緑=録音中(聞いている) / 紺=認識・考え中 / 黒=待機
enum FaceState { FACE_IDLE, FACE_LISTENING, FACE_THINKING };
void showFaceState(FaceState s) {
  ColorPalette cp;  // デフォルトは黒背景
  if (s == FACE_LISTENING)      cp.set(COLOR_BACKGROUND, TFT_DARKGREEN);
  else if (s == FACE_THINKING)  cp.set(COLOR_BACKGROUND, M5.Lcd.color565(0, 0, 96));
  avatar.setColorPalette(cp);
}

// 直近の録音に含まれた「大きい音」の100ms窓の数 (喋りらしさ判定用)。
// 人の発話ならおおむね4〜35窓 (0.4〜3.5秒) に収まる。
// 物音は短すぎ、掃除機・音楽などの連続音は長すぎるので除外できる。
int g_voiced_100ms = 0;
// 直近の音声認識が通信/APIエラーだったか (false で空文字なら無音・物音)
bool g_stt_net_error = false;

String SpeechToText(bool use_preroll = false) {
  printf("Record start!%s\n", use_preroll ? " (プリロール付き)" : "");
  showFaceState(FACE_LISTENING);
  AudioWhisper* audio = new AudioWhisper();
  // 録音終了判定のしきい値は起動トリガ(vad_threshold)より低くする。
  // 「起こす」には大きな声が必要だが、「まだ喋っているか」の判定に同じ値を
  // 使うと、声が小さくなる文末を無音と誤認して途中で録音が切れるため
  const int stop_peak = max(1000, vad_threshold / 3);
  size_t pre_n = 0;
  if (use_preroll && vad_hist_count > 0) {
    static int16_t preroll_copy[kPreRollChunks * kVadChunkSamples];
    pre_n = vadHistoryCopy(preroll_copy);
    audio->Record(preroll_copy, pre_n, stop_peak);
  } else {
    audio->Record(nullptr, 0, stop_peak);
  }
  showFaceState(FACE_THINKING);
  if (audio->GetBuffer() == nullptr || audio->GetSize() <= 44) {
    printf("[REC] 録音バッファ確保失敗\n");
    g_voiced_100ms = 0;
    delete audio;
    return "";
  }
  // デバッグ: 録音データの振幅を区間別に確認 (無音WAVをWhisperに送っていないか)
  {
    const int16_t* wav = (const int16_t*)(audio->GetBuffer() + 44);
    const size_t total = (audio->GetSize() - 44) / 2;
    int prePeak = 0, livePeak = 0;
    for (size_t i = 0; i < total; i++) {
      int v = abs((int)wav[i]);
      if (i < pre_n) { if (v > prePeak) prePeak = v; }
      else           { if (v > livePeak) livePeak = v; }
    }
    printf("[REC] total=%u pre=%u prePeak=%d livePeak=%d\n",
           (unsigned)total, (unsigned)pre_n, prePeak, livePeak);
    // 100msごとのピーク一覧 (声がどの時間帯にあるか) + 大きい音の窓数を数える
    printf("[WAVPROF]");
    int voiced = 0;
    for (size_t b = 0; b < total; b += 1600) {
      const size_t e = (b + 1600 < total) ? b + 1600 : total;
      int peak = 0;
      for (size_t i = b; i < e; i++) {
        int v = abs((int)wav[i]);
        if (v > peak) peak = v;
      }
      if (peak >= vad_threshold) voiced++;
      printf(" %d", peak);
    }
    printf("\n");
    g_voiced_100ms = voiced;
  }
  printf("Record end. transcribing...\n");
  avatar.setExpression(Expression::Doubt);  // 認識中
  Whisper* stt = new Whisper(root_ca_openai, OPENAI_API_KEY.c_str());
  String ret = stt->Transcribe(audio);
  g_stt_net_error = stt->hadNetworkError();
  delete stt;
  delete audio;
  avatar.setExpression(Expression::Neutral);
  return ret;
}

// Whisper が無音/雑音時に返しがちな定型句 (YouTube学習由来) を弾く
bool isWhisperHallucination(const String& textIn) {
  String s = textIn;
  s.trim();
  if (s.length() == 0) return true;
  static const char* kPhrases[] = {
    "ご視聴",
    "ご清聴",
    "視聴ありがと",
    "視聴してくださ",
    "ご覧いただき",
    "字幕",
    "チャンネル登録",
    "高評価",
    "最後までご覧",
    "見てくれてありがと",
    "Thank you for watching",
    "Thanks for watching",
    "Please subscribe",
  };
  for (const char* p : kPhrases) {
    if (s.indexOf(p) >= 0) return true;
  }
  return false;
}

// 長音・波線・空白を取り除いた正規化文字列を作る。
// map に「正規化後のバイト位置 -> 元のバイト位置」の対応を入れる (不要なら nullptr)。
// Whisper の認識ゆれ (「スタックちゃーん」等) を吸収するため。
static String normalizeForWake(const String& in, std::vector<int>* map) {
  String out;
  int i = 0;
  const int n = in.length();
  while (i < n) {
    uint8_t c = in[i];
    int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    if (i + len > n) len = n - i;
    String ch = in.substring(i, i + len);
    if (ch != "ー" && ch != "〜" && ch != "～" && ch != " " && ch != "　") {
      if (map) {
        for (int k = 0; k < len; k++) map->push_back(i + k);
      }
      out += ch;
    }
    i += len;
  }
  return out;
}

// UTF-8 文字列を1文字ずつに分解し、各文字の開始バイト位置も返す
static void splitUtf8(const String& s, std::vector<String>* chars, std::vector<int>* offs) {
  int i = 0;
  const int n = s.length();
  while (i < n) {
    uint8_t c = s[i];
    int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    if (i + len > n) len = n - i;
    chars->push_back(s.substring(i, i + len));
    offs->push_back(i);
    i += len;
  }
}

// パターン P がテキスト T のどこかの部分文字列に編集距離 maxDist 以内で一致するか。
// 一致したら文字単位の範囲 [startCp, endCp) を返す。(部分文字列マッチ用DP)
static bool fuzzyFind(const std::vector<String>& T, const std::vector<String>& P,
                      int maxDist, int* startCp, int* endCp) {
  const int n = T.size(), m = P.size();
  if (m == 0 || n == 0) return false;
  std::vector<std::vector<int>> D(m + 1, std::vector<int>(n + 1));
  std::vector<std::vector<int>> S(m + 1, std::vector<int>(n + 1));  // 一致開始位置
  for (int j = 0; j <= n; j++) { D[0][j] = 0; S[0][j] = j; }
  for (int i = 1; i <= m; i++) { D[i][0] = i; S[i][0] = 0; }
  for (int i = 1; i <= m; i++) {
    for (int j = 1; j <= n; j++) {
      const int sub = D[i-1][j-1] + (P[i-1] == T[j-1] ? 0 : 1);  // 置換/一致
      const int del = D[i-1][j] + 1;                              // 聞き落とし
      const int ins = D[i][j-1] + 1;                              // 余分な文字
      int best = sub, src = 1;
      if (del < best) { best = del; src = 2; }
      if (ins < best) { best = ins; src = 3; }
      D[i][j] = best;
      S[i][j] = (src == 1) ? S[i-1][j-1] : (src == 2) ? S[i-1][j] : S[i][j-1];
    }
  }
  int bestJ = -1, bestD = maxDist + 1;
  for (int j = 1; j <= n; j++) {
    if (D[m][j] < bestD) { bestD = D[m][j]; bestJ = j; }
  }
  if (bestJ < 0) return false;
  *startCp = S[m][bestJ];
  *endCp = bestJ;
  return true;
}

// 認識テキストから呼びかけワードを探す。見つかったら開始位置を返し、
// matchLen にワード長を入れる。カンマ/読点区切りで複数登録可。
// 長音「ー」や空白の有無は無視。5文字以上のワードは1文字までの
// 聞き違い (「スタッフちゃん」等) も許容する。
int findWakePhrase(const String& text, int* matchLen) {
  std::vector<int> map;
  String normText = normalizeForWake(text, &map);
  std::vector<String> tcp;
  std::vector<int> toffs;
  splitUtf8(normText, &tcp, &toffs);

  String phrases = WAKE_PHRASE;
  phrases.replace("、", ",");
  int from = 0;
  while (from < (int)phrases.length()) {
    int comma = phrases.indexOf(',', from);
    if (comma < 0) comma = phrases.length();
    String p = phrases.substring(from, comma);
    p.trim();
    p = normalizeForWake(p, nullptr);
    if (p.length() > 0) {
      std::vector<String> pcp;
      std::vector<int> poffs;
      splitUtf8(p, &pcp, &poffs);
      const int maxDist = (pcp.size() >= 5) ? 1 : 0;  // 短いワードは完全一致のみ
      int scp = 0, ecp = 0;
      if (fuzzyFind(tcp, pcp, maxDist, &scp, &ecp)) {
        const int nStart = toffs[scp];
        const int nEnd = (ecp < (int)toffs.size()) ? toffs[ecp] : (int)normText.length();
        const int start = map[nStart];
        const int end = (nEnd < (int)map.size()) ? map[nEnd] : (int)text.length();
        *matchLen = end - start;
        return start;
      }
    }
    from = comma + 1;
  }
  return -1;
}

// 呼びかけワード直後の句読点・空白を除去
String stripLeadingPunct(String s) {
  while (s.length() > 0) {
    if (s.startsWith(" ") || s.startsWith("　")) { s.remove(0, s.startsWith(" ") ? 1 : 3); continue; }
    bool removed = false;
    const char* punct[] = {"、", "。", "！", "？", "，", "!", "?", ","};
    for (const char* p : punct) {
      if (s.startsWith(p)) {
        s.remove(0, strlen(p));
        removed = true;
        break;
      }
    }
    if (!removed) break;
  }
  s.trim();
  return s;
}

void sw_tone() {
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.tone(1000, 100);
  delay(300);
  M5.Speaker.end();
  M5.Mic.begin();
}

// 録音 → Whisper → (必要なら呼びかけ判定) → Gemini
// require_wake=true のときは呼びかけワードを含まない発話を無視する。
void listen_and_chat(bool require_wake) {
  head::focus(true);  // 即座に正面で静止し聞き取りに集中 (キョロキョロ中断・サーボ音防止)
  avatar.setExpression(Expression::Happy);  // 聞いてます
  // 呼びかけ起動時はトリガ前0.5秒を録音に前置して語頭欠けを防ぐ
  String ret = SpeechToText(require_wake);

  if (ret == "" || isWhisperHallucination(ret)) {
    if (ret != "") printf("音声認識: 幻聴とみなし無視 [%s]\n", ret.c_str());
    else printf("音声認識: 空文字 (無音/認識失敗)\n");
    showFaceState(FACE_IDLE);
    // Whisperが正常に処理して空だった = 物音や無音なので黙って待機に戻る。
    // 謝るのは通信/APIエラーで本当に聞き取れなかったときと、
    // ボタン起動 (意図的な操作) のときだけ。
    // ※音として認識できた物音 (「【電子レンジの音】」等) は文字として返って
    //   くるためここには来ず、なんでも返事モードならAIが反応する (仕様)
    printf("[REC] voiced=%d x100ms, 通信エラー=%s -> %s\n", g_voiced_100ms,
           g_stt_net_error ? "あり" : "なし",
           (!require_wake || g_stt_net_error) ? "返事する" : "無視");
    if (!require_wake || (wake_mode == 2 && g_stt_net_error)) {
      g_reply_expression = Expression::Sad;
      speech_text = "ごめん、うまく聞き取れなかったよ";
    }
    return;
  }
  printf("音声認識結果: %s\n", ret.c_str());

  if (require_wake) {
    int matchLen = 0;
    int idx = findWakePhrase(ret, &matchLen);
    if (idx < 0) {
      if (wake_mode != 2) {
        printf("呼びかけワードなし -> 無視 (有効ワード: [%s])\n", WAKE_PHRASE.c_str());
        showFaceState(FACE_IDLE);
        return;
      }
      // なんでも返事モード: ワードが無くても認識テキスト全体を質問として扱う
      printf("呼びかけワードなし -> なんでも返事モードで続行\n");
      idx = -1;
    }
    if (idx >= 0) {  // 呼びかけワードが見つかった場合はワード部分を除去
      printf("呼びかけ検出: [%s]\n", ret.substring(idx, idx + matchLen).c_str());
      String rest = stripLeadingPunct(ret.substring(idx + matchLen));
      if (rest.length() < 6) {  // 呼びかけのみ (日本語2文字未満) -> 聞き返す
        g_reply_expression = Expression::Happy;
        avatar.setExpression(Expression::Happy);
        showFaceState(FACE_IDLE);
        speakBlocking("なあに？");
        ret = SpeechToText();
        if (ret == "" || isWhisperHallucination(ret)) {
          // 呼びかけ済み = こちらに話しかけているのは確実なので返事する
          showFaceState(FACE_IDLE);
          g_reply_expression = Expression::Sad;
          speech_text = "ごめん、うまく聞き取れなかったよ";
          return;
        }
        printf("音声認識結果(2): %s\n", ret.c_str());
      } else {
        ret = rest;
      }
    }
  }

  if (!mp3->isRunning() && speech_text == "" && speech_text_buffer == "") {
    exec_chat(ret);
  }
}

// アイドル時の音量トリガ (声がしたら true)
bool vad_triggered() {
  // 直近25チャンク(0.5秒)のうち「大きい音」が8チャンク(0.16秒)以上あればトリガ。
  // 連続カウント方式だと「ス・タッ・ク」のような細切れの単語が弾かれるため窓方式。
  // 一瞬で減衰するノック音等は窓内で2〜4チャンクにしかならず弾かれる。
  constexpr int kWinChunks = 25;
  constexpr int kLoudNeeded = 8;
  static bool recent[kWinChunks] = {};
  static int  rpos = 0;
  static int  loud_cnt = 0;
  static int warmup = 40;   // マイク起動直後に捨てるチャンク数 (40 x 20ms = 0.8s)
  static int16_t buf[320];  // 20ms @ 16kHz
  // 注意: isEnabled() はピン設定の有無を返すだけで動作状態を反映しない。
  // 録音/再生後のマイク再始動を検知するには isRunning() を使う。
  if (!M5.Mic.isRunning()) {
    M5.Mic.begin();
    vad_hist_pos = 0;
    vad_hist_count = 0;  // 再生直後などの古い履歴を捨てる
    memset(recent, 0, sizeof(recent));
    loud_cnt = 0;
    warmup = 40;  // ES8311再起動直後はフルスケールのノイズが出るため0.8秒捨てる
    return false;
  }
  if (!M5.Mic.record(buf, 320, 16000)) return false;
  if (warmup > 0) {
    warmup--;
    return false;  // ウォームアップ中は履歴にもVAD評価にも使わない
  }
  // プリロール履歴へ保存 (トリガ時に録音の先頭へ前置される)
  memcpy(&vad_history[vad_hist_pos * kVadChunkSamples], buf, sizeof(buf));
  vad_hist_pos = (vad_hist_pos + 1) % kPreRollChunks;
  if (vad_hist_count < kPreRollChunks) vad_hist_count++;
  int peak = 0;
  for (int i = 0; i < 320; i++) {
    int v = abs(buf[i]);
    if (v > peak) peak = v;
  }
  // しきい値調整用: 直近1秒の最大ピークを毎秒ログに出す
  static int log_peak = 0;
  static uint32_t log_at = 0;
  if (peak > log_peak) log_peak = peak;
  if (millis() - log_at >= 1000) {
    printf("[VAD] peak=%d (threshold=%d)\n", log_peak, vad_threshold);
    log_peak = 0;
    log_at = millis();
  }
  // サーボ動作音「ジジッ」での誤トリガ防止: 首振り(キョロキョロ)の最中と
  // その後0.4秒は「大きい音」として数えない (音声自体は履歴に残るので、
  // 首振り中に話しかけられても動作が収まり次第プリロール付きで拾える)
  const bool servo_noise = head::movedRecently(400);
  const bool is_loud = !servo_noise && (peak >= vad_threshold);
  loud_cnt += (is_loud ? 1 : 0) - (recent[rpos] ? 1 : 0);
  recent[rpos] = is_loud;
  rpos = (rpos + 1) % kWinChunks;
  if (loud_cnt >= kLoudNeeded) {
    memset(recent, 0, sizeof(recent));
    loud_cnt = 0;
    return true;
  }
  return false;
}

// ====================================================================
//  setup / loop
// ====================================================================
void setup() {
  auto cfg = M5.config();
  // Atomic Echo Base (ES8311 codec: スピーカー+マイク) を使う
  // ※ M5Unified 0.2.x 以降が必要
  cfg.external_speaker.atomic_echo = true;
  M5.begin(cfg);

  // TTS ストリーミングバッファは PSRAM (8MB) に確保
  preallocateBuffer = (uint8_t *)heap_caps_malloc(preallocateBufferSize,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!preallocateBuffer) preallocateBuffer = (uint8_t *)malloc(preallocateBufferSize);
  if (!preallocateBuffer) {
    M5.Display.printf("FATAL: no memory for %d bytes\n", preallocateBufferSize);
    for (;;) { delay(1000); }
  }

  {
    auto micConfig = M5.Mic.config();
    micConfig.stereo = false;
    micConfig.sample_rate = 16000;
    uint8_t micgain = nvs_get_u8_or("setting", "micgain", 16);  // 既定16 (音割れしにくい実機調整値)
    if (micgain > 0) micConfig.magnification = micgain;
    M5.Mic.config(micConfig);
    printf("[MIC] magnification=%d\n", micConfig.magnification);
  }
  M5.Mic.begin();

  head::setup();

  // 音量・話者番号を NVS から (初回は既定値を書き込み)
  {
    uint32_t nvs_handle;
    size_t volume = 100;    // 実機調整済みの既定値
    uint8_t speaker_no = 3;
    if (ESP_OK == nvs_open("setting", NVS_READONLY, &nvs_handle)) {
      nvs_get_u32(nvs_handle, "volume", &volume);
      nvs_get_u8(nvs_handle, "speaker", &speaker_no);
      nvs_close(nvs_handle);
    } else if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
      nvs_set_u32(nvs_handle, "volume", volume);
      nvs_set_u8(nvs_handle, "speaker", speaker_no);
      nvs_set_u8(nvs_handle, "micgain", 16);
      nvs_close(nvs_handle);
    }
    if (volume > 255) volume = 255;
    if (speaker_no > 60) speaker_no = 3;
    M5.Speaker.setVolume(volume);
    M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
    TTS_SPEAKER_NO = String(speaker_no);
    TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
  }

  M5.Lcd.setTextSize(1);
  Serial.println("Connecting to WiFi");
  WiFi.disconnect();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  loadConfigFromNvs();

  // 起動時に画面(Aボタン)を押したままなら設定APへ
  M5.update();
  if (M5.BtnA.isPressed()) {
    Serial.println("[CFG] BtnA held at boot -> config portal");
    startConfigPortal();  // 戻らない
  }

  Wifi_setup();  // 画面表示は Wifi_setup() 側で行う

  wc::beginClock();  // NTP (JST)

  if (MDNS.begin(MDNS_HOST)) {
    Serial.println("MDNS responder started");
  }
  Serial.printf("Go to http://%s/ or http://%s.local/\n",
                WiFi.localIP().toString().c_str(), MDNS_HOST);
  // IPアドレスを大きく数秒表示 (128x128でも読めるように2行に分割)
  {
    String ip = WiFi.localIP().toString();
    int dot2 = ip.indexOf('.', ip.indexOf('.') + 1);  // 2個目のドット
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(0, 16);
    M5.Lcd.println("IP:");
    M5.Lcd.println(ip.substring(0, dot2 + 1));
    M5.Lcd.println(ip.substring(dot2 + 1));
    M5.Lcd.setTextSize(1);
    M5.Lcd.printf("\n %s.local", MDNS_HOST);
    delay(3500);  // 読む時間。「アイピー教えて」でも読み上げ可
    M5.Lcd.setTextSize(1);
  }

  registerRoutes();

  // ロール(システムプロンプト) を SPIFFS から復元、なければ既定
  applyRole(String(kDefaultRole));
  if (SPIFFS.begin(true)) {
    File f = SPIFFS.open("/data.json", "r");
    if (f) {
      DynamicJsonDocument saved(1024 * 10);
      if (deserializeJson(saved, f) == DeserializationError::Ok &&
          saved["messages"].is<JsonArray>() && saved["messages"].size() > 0) {
        InitBuffer = "";
        serializeJson(saved, InitBuffer);
        Serial.println("role loaded from SPIFFS");
      }
      f.close();
    }
  }

  server.begin();
  Serial.println("HTTP server started");

  audioLogger = &Serial;
  mp3 = new AudioGeneratorMP3();

  avatar.setScale(0.4);         // 320x240 -> 128x128 に合わせて縮小
  avatar.setPosition(-56, -96); // 縮小した顔を画面中央へ
  avatar.init();
  avatar_ready = true;
  avatar.addTask(lipSync, "lipSync");
  avatar.addTask(servo, "servo");

  delay(1000);

  last_activity_ms = millis();  // おやすみタイマーの起点

  // 起動あいさつ (スピーカー/鍵の動作確認を兼ねる)
  g_reply_expression = Expression::Happy;
  String first_wake = WAKE_PHRASE;
  first_wake.replace("、", ",");  // 読点区切りにも対応して最初の1語だけ読む
  if (first_wake.indexOf(',') > 0) first_wake = first_wake.substring(0, first_wake.indexOf(','));
  first_wake.trim();
  speech_text = "こんにちは、ちびスタックちゃんだよ。「" + first_wake + "」って呼んでね。";
}

void loop() {
  M5.update();

  // 画面(Aボタン)長押し3秒 -> 設定APモード
  if (M5.BtnA.pressedFor(3000)) {
    startConfigPortal();  // 戻らない
  }

  // 画面クリック -> 呼びかけ無しですぐ聞く (眠っていたら起こす)
  if (M5.BtnA.wasClicked() && !mp3->isRunning() &&
      speech_text == "" && speech_text_buffer == "") {
    if (sleeping) {
      sleeping = false;
      head::focus(false);
      avatar.setExpression(Expression::Neutral);
    }
    last_activity_ms = millis();
    sw_tone();
    listen_and_chat(false);
    head::focus(false);
  }

  // タイマー満了 → 会話や再生と重ならないタイミングでアナウンス
  if (timer_end_ms != 0 && (int32_t)(millis() - timer_end_ms) >= 0 &&
      speech_text == "" && speech_text_buffer == "" && !mp3->isRunning()) {
    timer_end_ms = 0;
    g_reply_expression = Expression::Happy;
    speech_text = "ピピピッ、ピピピッ！" + formatDuration(timer_total_sec) + "たったよ！";
    printf("[TIMER] fired\n");
  }

  // 返答があれば読み上げ開始
  if (speech_text != "") {
    if (sleeping) {  // タイマー満了など、喋る = 目が覚める
      sleeping = false;
      head::focus(false);
    }
    last_activity_ms = millis();
    showFaceState(FACE_IDLE);
    avatar.setExpression(g_reply_expression);
    speech_text_buffer = speech_text;
    speech_text = "";
    M5.Mic.end();
    M5.Speaker.begin();
    Voicevox_tts((char*)speech_text_buffer.c_str(), (char*)TTS_PARMS.c_str());
    if (!mp3->isRunning()) {
      // TTS開始失敗 (URL取得失敗/接続失敗)。ここで後始末しないと
      // speech_text_buffer が残り続け、以後ずっと busy 扱いになって
      // 呼びかけ・ボタン・タイマーが再起動まで一切効かなくなる
      printf("[TTS] 再生を開始できず。スキップして待機に戻ります\n");
      stopPlayback();
      speech_text_buffer = "";
      avatar.setExpression(Expression::Neutral);
      delay(200);
      M5.Speaker.end();
    }
  }

  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      stopPlayback();
      printf("mp3 stop\n");
      avatar.setExpression(Expression::Neutral);
      speech_text_buffer = "";
      delay(200);
      M5.Speaker.end();
      // マイク再開はここでは行わない: vad_triggered() が isRunning() を見て
      // 履歴リセット+ウォームアップ付きで再開する
    }
    delay(1);
  } else {
    server.handleClient();

    // 3分以上会話が無ければ眠る (眠い顔 + サーボ停止)。声やボタンで起きる
    if (!sleeping && speech_text == "" && speech_text_buffer == "" &&
        millis() - last_activity_ms > kSleepAfterMs) {
      sleeping = true;
      printf("[SLEEP] おやすみ...\n");
      avatar.setExpression(Expression::Sleepy);
      head::focus(true);   // キョロキョロ停止 + 脱力
      prepareWakeVoice();  // 暇なうちに起床ボイスをキャッシュしておく
    }

    // 呼びかけ待ち: 音量トリガ -> Whisper -> 呼びかけワード照合
    if (wake_enable && speech_text == "" && speech_text_buffer == "") {
      if (vad_triggered()) {
        printf("[VAD] triggered\n");
        if (sleeping) {
          // 眠りから起こされた: 聞き取りはせず、まず飛び起きる
          sleeping = false;
          head::focus(false);
          g_reply_expression = Expression::Happy;
          avatar.setExpression(Expression::Happy);
          printf("[SLEEP] 起きた！\n");
          if (!speakFromSPIFFS(kWakeVoicePath)) {
            speakBlocking(kWakeUpSpeech);  // キャッシュが無ければ通常TTS
          }
          avatar.setExpression(Expression::Neutral);
          last_activity_ms = millis();
        } else {
          listen_and_chat(true);
          head::focus(false);
        }
      }
    }
    delay(1);  // アイドル時に他タスクへ譲る (Task WDT対策)
  }
}
