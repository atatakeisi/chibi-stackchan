#include <ArduinoJson.h>
#include "Whisper.h"

namespace {
constexpr const char* API_HOST = "api.openai.com";
constexpr int API_PORT = 443;
constexpr const char* API_PATH = "/v1/audio/transcriptions";
}  // namespace

Whisper::Whisper(const char* root_ca, const char* api_key) : client(), key(api_key) {
  client.setCACert(root_ca);
  client.setTimeout(10000);
  if (!client.connect(API_HOST, API_PORT)) {
    // 長時間アイドル後などに一度目が失敗することがあるので1回だけ再試行
    printf("[whisper] connection failed! retrying...\n");
    client.stop();
    delay(200);
    if (!client.connect(API_HOST, API_PORT)) {
      printf("[whisper] connection failed!\n");
    }
  }
}

Whisper::~Whisper() {
  client.stop();
}

String Whisper::Transcribe(AudioWhisper* audio) {
  net_error = false;
  if (!client.connected()) {
    // 接続失敗のまま送信すると応答待ちタイムアウトまで無言で固まるので即諦める
    printf("[whisper] not connected. skip transcribe\n");
    net_error = true;
    return "";
  }
  char boundary[64] = "------------------------";
  for (auto i = 0; i < 2; ++i) {
    ltoa(random(0x7fffffff), boundary + strlen(boundary), 16);
  }
  const String header = "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
    "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"language\"\r\n\r\nja\r\n"
    "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"speak.wav\"\r\n"
    "Content-Type: application/octet-stream\r\n\r\n";
  const String footer = "\r\n--" + String(boundary) + "--\r\n";

  // header
  client.printf("POST %s HTTP/1.1\n", API_PATH);
  client.printf("Host: %s\n", API_HOST);
  client.println("Accept: */*");
  client.printf("Authorization: Bearer %s\n", key.c_str());
  client.printf("Content-Length: %d\n", header.length() + audio->GetSize() + footer.length());
  client.printf("Content-Type: multipart/form-data; boundary=%s\n", boundary);
  client.println();
  client.print(header.c_str());
  client.flush();

  auto ptr = audio->GetBuffer();
  auto remainings = audio->GetSize();
  while (remainings > 0) {
    auto sz = (remainings > 512) ? 512 : remainings;
    client.write(ptr, sz);
    client.flush();
    remainings -= sz;
    ptr += sz;
  }
  client.flush();

  // footer
  client.print(footer.c_str());
  client.flush();

  // wait response (delayを挟まないと待機中CPU100%で発熱する)
  const auto now = ::millis();
  while (client.available() == 0) {
    if (::millis() - now > 10000) {
      printf("[whisper] response timeout\n");
      net_error = true;
      return "";
    }
    if (!client.connected()) {
      printf("[whisper] connection closed before response\n");
      net_error = true;
      return "";
    }
    delay(5);
  }

  // 応答全体を受信する。available()==0 の瞬間があっても本文途中で打ち切らない
  // よう、JSON終端 (}) か切断か1.2秒無通信まで待つ。
  // (旧実装の readStringUntil は最終行で終端文字を1秒待つビジーループになっていた)
  String resp = "";
  uint32_t lastData = ::millis();
  while (::millis() - lastData < 1200) {
    bool got = false;
    while (client.available()) {
      resp += (char)client.read();
      got = true;
    }
    if (got) lastData = ::millis();
    if (resp.endsWith("}")) break;  // 本文(JSON)の終端まで受信済み
    if (!client.connected() && client.available() == 0) break;
    delay(5);
  }
  const int hdrEnd = resp.indexOf("\r\n\r\n");
  String body = (hdrEnd >= 0) ? resp.substring(hdrEnd + 4) : String("");
  if (body == "") {
    printf("[whisper] empty/invalid response\n");
    net_error = true;
    return "";
  }

  // NOTE: 旧実装は 200 バイト固定バッファで、長い認識結果だと NoMemory で
  // 解析に失敗し空文字(=無音扱い)になっていた。長文の発話も受けられる大きさに
  DynamicJsonDocument doc(4096);
  if (::deserializeJson(doc, body) != DeserializationError::Ok) {
    printf("[whisper] JSON parse failed: %s\n", body.substring(0, 120).c_str());
    net_error = true;
    return "";
  }
  if (!doc["text"].is<const char*>()) {
    // {"error":{...}} などのAPIエラー応答
    printf("[whisper] API error: %s\n", body.substring(0, 200).c_str());
    net_error = true;
    return "";
  }
  return doc["text"].as<String>();
}
