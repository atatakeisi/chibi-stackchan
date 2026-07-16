#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "WebVoiceVoxRootCA.h"
#include <AudioGeneratorMP3.h>
#include <AudioFileSourceBuffer.h>
#include "AudioFileSourceHTTPSStream.h"
//#include "AudioOutputM5Speaker.h"

//-----------------------------------------------------
extern String VOICEVOX_API_KEY;
extern AudioGeneratorMP3 *mp3;
extern AudioFileSourceBuffer *buff;
extern AudioFileSourceHTTPSStream *file;
extern int preallocateBufferSize;
extern uint8_t *preallocateBuffer;
//extern AudioOutputM5Speaker out;
void StatusCallback(void *cbData, int code, const char *string);
void playMP3(AudioFileSourceBuffer *buff);
//-----------------------------------------------------

String https_get(const char* url, const char* root_ca) {
  String payload = "";
  WiFiClientSecure *client = new WiFiClientSecure;
  if(client) {
    client -> setCACert(root_ca);
    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before WiFiClientSecure *client is 
      HTTPClient https;
  
      printf("[HTTPS] begin...\n");
      if (https.begin(*client, url)) {  // HTTPS
//      if (https.begin(*client, "https://api.tts.quest/v1/voicevox/?text=こんにちは世界！)) {  // HTTPS
//      if (https.begin(*client, "https://jigsaw.w3.org/HTTP/connection.html")) {  // HTTPS&speaker=1"
        printf("[HTTPS] GET...\n");
        // start connection and send HTTP header
        int httpCode = https.GET();
  
        // httpCode will be negative on error
        if (httpCode > 0) {
          // HTTP header has been send and Server response header has been handled
          printf("[HTTPS] GET... code: %d\n", httpCode);
  
          // file found at server
          if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            payload = https.getString();
          }
        } else {
          printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
        }  
        https.end();
      } else {
        printf("[HTTPS] Unable to connect\n");
      }
      // End extra scoping block
    }  
    delete client;
  } else {
    printf("Unable to create client\n");
  }
  return payload;
}

/* //https_post_json(const char* url, const char* json_string, root_ca_openai);
String https_post_json(const char* url, const char* json_string, const char* root_ca) {
  String payload = "";
  WiFiClientSecure *client = new WiFiClientSecure;
  if(client) {
    client -> setCACert(root_ca);
    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before WiFiClientSecure *client is 
      HTTPClient https;
      https.setTimeout( 25000 ); 
  
      printf("[HTTPS] begin...\n");
      if (https.begin(*client, url)) {  // HTTPS
        Serial.print("[HTTPS] POST...\n");
        // start connection and send HTTP header
        //https.setTimeout( 10000 ); 
        https.addHeader("Content-Type", "application/json");
        https.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
        //int httpCode = https.GET();
        int httpCode = https.POST((uint8_t *)json_string, strlen(json_string));
  
        // httpCode will be negative on error
        if (httpCode > 0) {
          // HTTP header has been send and Server response header has been handled
          Serial.printf("[HTTPS] POST... code: %d\n", httpCode);
  
          // file found at server
          if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            payload = https.getString();
          }
        } else {
          Serial.printf("[HTTPS] POST... failed, error: %s\n", https.errorToString(httpCode).c_str());
        }  
        https.end();
      } else {
        printf("[HTTPS] Unable to connect\n");
      }
      // End extra scoping block
    }  
    delete client;
  } else {
    printf("Unable to create client\n");
  }
  return payload;
}
 */
bool voicevox_tts_json_status(const char* url, const char* json_key, const char* root_ca) {
  bool json_data = false;
  DynamicJsonDocument doc(1000);
  String payload = https_get(url, root_ca);
  if(payload != ""){
    printf("%s\n", payload.c_str());
 //    StaticJsonDocument<1000> doc;
//    JsonObject object = doc.as();
    DeserializationError error = deserializeJson(doc, payload.c_str());
    if (error) {
      printf("deserializeJson() failed: ");
      printf("%s\n", error.f_str());
      return json_data;
    }
    json_data = doc[json_key];
//    Serial.println(json_data);
  }
  return json_data;
}

bool voicevox_wait_audio_ready(const char* status_url, const char* root_ca, uint32_t timeout_ms) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    String payload = https_get(status_url, root_ca);
    if (payload != "") {
      DynamicJsonDocument doc(1000);
      DeserializationError error = deserializeJson(doc, payload.c_str());
      if (!error) {
        bool isAudioError = doc["isAudioError"] | false;
        bool isAudioReady = doc["isAudioReady"] | false;
        if (isAudioError) {
          printf("voicevox status: isAudioError=true\n");
          return false;
        }
        if (isAudioReady) {
          printf("voicevox status: isAudioReady=true\n");
          return true;
        }
      }
    }
    // https_get は毎回TLSハンドシェイク(CPU重)なのでポーリング間隔は控えめに
    delay(300);
  }
  printf("voicevox status: wait timeout (\u9577\u3059\u304e\u308b\u6587\u306f\u5408\u6210\u306b\u6642\u9593\u304c\u304b\u304b\u308b)\n");
  return false;
}

//String tts_status_url;
String voicevox_tts_url(const char* url, const char* root_ca) {
  String tts_url = "";
  WiFiClientSecure *client = new WiFiClientSecure;
  if(client) {
    client -> setCACert(root_ca);
    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before WiFiClientSecure *client is 
      HTTPClient https;
  
      printf("[HTTPS] begin...\n");
      if (https.begin(*client, url)) {  // HTTPS
        printf("[HTTPS] GET...\n");
        // start connection and send HTTP header
        int httpCode = https.GET();
  
        // httpCode will be negative on error
        if (httpCode > 0) {
          // HTTP header has been send and Server response header has been handled
          printf("[HTTPS] GET... code: %d\n", httpCode);
  
          // file found at server
          if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            String payload = https.getString();
            printf("%s\n", payload.c_str());
            //StaticJsonDocument<1000> doc;
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, payload.c_str());
            if (error) {
              printf("deserializeJson() failed: ");
              printf("%s\n", error.f_str());
              return tts_url;
            }
  String json_string;
  serializeJsonPretty(doc, json_string);
  printf("====================\n");
  printf("%s\n", json_string.c_str());
  printf("====================\n");

            if(!doc["success"]) return tts_url;
            const char* status_url = doc["audioStatusUrl"];
            if (status_url && strlen(status_url) > 0) {
              // 長い文は合成に時間がかかる (12秒では足りないことがあった)
              voicevox_wait_audio_ready(status_url, root_ca, 20000);
            }

            const char* mp3url = doc["mp3DownloadUrl"];
            if (!mp3url || strlen(mp3url) == 0) {
              mp3url = doc["mp3StreamingUrl"];
            }
            printf("mp3url: %s\n", mp3url ? mp3url : "(\u306a\u3057)");
            printf("isApiKeyValid:");
            if(doc["isApiKeyValid"]) printf("OK\n");
            else printf("NG (\u30dd\u30a4\u30f3\u30c8\u5207\u308c/\u30ad\u30fc\u4e0d\u6b63\u306e\u53ef\u80fd\u6027)\n");
            if (mp3url) {
              tts_url = String(mp3url);
            }

            // const char* status_url = doc["audioStatusUrl"];
            // Serial.println(status_url);
            // //tts_status_url = String(status_url);
            // if(voicevox_tts_json_status(status_url, "isAudioError", root_ca))  return tts_url;
            // while(!voicevox_tts_json_status(status_url, "isAudioReady", root_ca)) delay(1);
          }
        } else {
          printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
        }  
        https.end();
      } else {
        printf("[HTTPS] Unable to connect\n");
      }
      // End extra scoping block
    }  
    delete client;
  } else {
    printf("Unable to create client\n");
  }
  return tts_url;
}

static String URLEncode(const char* msg) {
  const char *hex = "0123456789ABCDEF";
  String encodedMsg = "";

  while (*msg != '\0') {
    if ( ('a' <= *msg && *msg <= 'z')
         || ('A' <= *msg && *msg <= 'Z')
         || ('0' <= *msg && *msg <= '9')
         || *msg  == '-' || *msg == '_' || *msg == '.' || *msg == '~' ) {
      encodedMsg += *msg;
    } else {
      encodedMsg += '%';
      encodedMsg += hex[*msg >> 4];
      encodedMsg += hex[*msg & 0xf];
    }
    msg++;
  }
  return encodedMsg;
}

// char *text0 = "みなさん、こんにちは！私の名前はスタックチャンです。よろしくね！";
// char *tts_parms0 = "&speaker=1";
// char *tts_parms01 = "&speaker=19";
// char *tts_parms02 = "&speaker=28";
// char *tts_parms03 = "&speaker=42";
// char *tts_parms04 = "&speaker=12";
// char *tts_parms05 = "&speaker=45";
// char *tts_parms06 = "&speaker=3";

void Voicevox_tts(char *text,char *tts_parms){
//  String tts_url = String("https://api.tts.quest/v1/voicevox/?text=") +  URLEncode(text) + String(tts_parms);
//  String tts_url = String("https://api.tts.quest/v3/voicevox/synthesis?key=y958S773N4I7356&text=") +  URLEncode(text) + String(tts_parms);
  String tts_url = String("https://api.tts.quest/v3/voicevox/synthesis?key=")+ VOICEVOX_API_KEY +  String("&text=") +  URLEncode(text) + String(tts_parms);
  String URL = voicevox_tts_url(tts_url.c_str(), root_ca);
  Serial.println(tts_url);

  if(URL == "") return;
//  while(!voicevox_tts_json_status(tts_status_url.c_str(), "isAudioReady", root_ca)) delay(1);
//delay(2500);
  file = new AudioFileSourceHTTPSStream(URL.c_str(), root_ca);
//  file->RegisterStatusCB(StatusCallback, (void*)"file");
  buff = new AudioFileSourceBuffer(file, preallocateBuffer, preallocateBufferSize);
//  mp3->begin(buff, &out);
  playMP3(buff);
}

bool Voicevox_tts_download(const char* text, const char* tts_parms, const char* path) {
  String tts_url = String("https://api.tts.quest/v3/voicevox/synthesis?key=") + VOICEVOX_API_KEY +
                   String("&text=") + URLEncode(text) + String(tts_parms);
  String URL = voicevox_tts_url(tts_url.c_str(), root_ca);
  if (URL == "") return false;
  if (!SPIFFS.begin(true)) return false;

  bool ok = false;
  WiFiClientSecure *client = new WiFiClientSecure;
  if (client) {
    client->setCACert(root_ca);
    {
      HTTPClient https;
      https.setTimeout(15000);
      if (https.begin(*client, URL)) {
        int code = https.GET();
        if (code == HTTP_CODE_OK) {
          File f = SPIFFS.open(path, "w");
          if (f) {
            int written = https.writeToStream(&f);
            f.close();
            ok = (written > 0);
            if (!ok) SPIFFS.remove(path);  // 中途半端なファイルを残さない
            Serial.printf("[TTS] cache %s: %d bytes\n", path, written);
          }
        } else {
          Serial.printf("[TTS] cache download failed: HTTP %d\n", code);
        }
        https.end();
      }
    }
    delete client;
  }
  return ok;
}
