#include "WeatherClock.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// main.cpp のグローバル設定 (Web UI から変更・NVS保存)
extern String WEATHER_PLACE;
extern float  WEATHER_LAT;
extern float  WEATHER_LON;

namespace wc {

namespace {

const char* kWeekdayJa[7] = {"日", "月", "火", "水", "木", "金", "土"};

// WMO weather code -> 日本語
String codeToJa(int code) {
  if (code == 0) return "快晴";
  if (code <= 2) return "晴れ";
  if (code == 3) return "くもり";
  if (code == 45 || code == 48) return "霧";
  if (code >= 51 && code <= 57) return "霧雨";
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "雨";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "雪";
  if (code >= 95) return "雷雨";
  return "不明";
}

}  // namespace

void beginClock() {
  configTzTime("JST-9", "ntp.nict.jp", "ntp.jst.mfeed.ad.jp", "pool.ntp.org");
}

String nowJa() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return "";
  char buf[80];
  snprintf(buf, sizeof(buf), "%d年%d月%d日(%s曜日) %d時%d分",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           kWeekdayJa[t.tm_wday], t.tm_hour, t.tm_min);
  return String(buf);
}

bool wantsWeather(const String& text) {
  const char* kw[] = {"天気", "気温", "暑い", "寒い", "雨", "雪", "傘", "降水", "晴れ"};
  for (const char* k : kw) {
    if (text.indexOf(k) >= 0) return true;
  }
  return false;
}

bool fetchTodayWeather(String& outSummary) {
  if (WiFi.status() != WL_CONNECTED) return false;

  char url[320];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max&timezone=Asia%%2FTokyo&forecast_days=1",
           WEATHER_LAT, WEATHER_LON);

  WiFiClientSecure client;
  client.setInsecure();  // 天気情報のみのため証明書検証は省略
  HTTPClient https;
  https.setTimeout(10000);
  if (!https.begin(client, url)) return false;
  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[weather] HTTP %d\n", code);
    https.end();
    return false;
  }
  String payload = https.getString();
  https.end();

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return false;

  int   wcodeNow = doc["current"]["weather_code"] | -1;
  float tempNow  = doc["current"]["temperature_2m"] | -99.0f;
  int   wcodeDay = doc["daily"]["weather_code"][0] | wcodeNow;
  float tmax     = doc["daily"]["temperature_2m_max"][0] | -99.0f;
  float tmin     = doc["daily"]["temperature_2m_min"][0] | -99.0f;
  int   pop      = doc["daily"]["precipitation_probability_max"][0] | -1;

  String s = WEATHER_PLACE + "の今日の天気は" + codeToJa(wcodeDay);
  if (tmax > -90 && tmin > -90) {
    s += "、最高気温" + String((int)roundf(tmax)) + "度、最低気温" +
         String((int)roundf(tmin)) + "度";
  }
  if (pop >= 0) s += "、降水確率" + String(pop) + "パーセント";
  if (tempNow > -90) {
    s += "。現在は" + codeToJa(wcodeNow) + "で気温" +
         String((int)roundf(tempNow)) + "度";
  }
  s += "。";
  outSummary = s;
  Serial.println("[weather] " + s);
  return true;
}

}  // namespace wc
