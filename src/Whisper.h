#ifndef _Whisper_H
#define _Whisper_H
#include <WiFiClientSecure.h>
#include "AudioWhisper.h"

class Whisper {
  WiFiClientSecure client;
  String key;
  bool net_error = false;
public:
  Whisper(const char* root_ca, const char* api_key);
  ~Whisper();
  String Transcribe(AudioWhisper* audio);
  // 直前の Transcribe が通信/APIエラーだったか (true) 。
  // false で空文字なら「正常に処理されたが無音・物音だった」の意味
  bool hadNetworkError() const { return net_error; }
};

#endif // _Whisper_H

