#ifndef _AUDIOWHISPER_H
#define _AUDIOWHISPER_H

#include "AudioWhisper.h"

class AudioWhisper {
  byte* record_buffer;
  size_t wav_size;
 public:
  AudioWhisper();
  ~AudioWhisper();

  const byte* GetBuffer() const { return record_buffer; }
  size_t GetSize() const;

  // preroll: 録音の先頭に前置する音声 (トリガ前の語頭欠け対策)。不要なら省略。
  // silenceStopPeak: >0 なら発話後にこの音量未満が続いた時点で録音を打ち切る
  //                  (短い呼びかけの後に長い無音を送って幻聴になるのを防ぐ)。
  void Record(const int16_t* preroll = nullptr, size_t prerollSamples = 0,
              int silenceStopPeak = 0);
};

#endif // _AUDIOWHISPER_H
