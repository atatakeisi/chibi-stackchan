// WeatherClock - 現在時刻(NTP) と 今日の天気(Open-Meteo, APIキー不要) の取得。
// 取得結果は Gemini への「参考情報」として質問に添付され、
// スタックちゃんが自然な言葉で時刻・天気を答えられるようになる。
#pragma once

#include <Arduino.h>

namespace wc {

// NTP 同期を開始する (Wi-Fi 接続後に一度呼ぶ。JST固定)
void beginClock();

// "2026年7月4日(土曜日) 15時23分" のような文字列。未同期なら空文字。
String nowJa();

// Open-Meteo から今日の天気を取得して日本語要約を返す。
// place/lat/lon は main.cpp のグローバル設定 (Web UIで変更可)。
// 失敗時は false。
bool fetchTodayWeather(String& outSummary);

// テキストが天気の話題を含むか (含むときだけ天気APIを呼ぶ)
bool wantsWeather(const String& text);

}  // namespace wc
