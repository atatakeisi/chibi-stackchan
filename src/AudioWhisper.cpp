#include <M5Unified.h>
#include "AudioWhisper.h"

// 最大6秒 (無音検出で早期打ち切りされるので通常はもっと短い)
constexpr size_t record_number = 640;
constexpr size_t record_length = 150;
constexpr size_t record_size = record_number * record_length;
constexpr size_t record_samplerate = 16000;
constexpr int headerSize = 44;

AudioWhisper::AudioWhisper() {
  const auto size = record_size * sizeof(int16_t) + headerSize;
  record_buffer = static_cast<byte*>(::heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (record_buffer == nullptr) {
    wav_size = 0;
    return;
  }
  ::memset(record_buffer, 0, size);
  wav_size = size;
}

AudioWhisper::~AudioWhisper() {
  ::heap_caps_free(record_buffer);  // malloc確保なので delete ではなく free
}

size_t AudioWhisper::GetSize() const {
  return wav_size;
}

int16_t* MakeHeader(byte* header, size_t sampleCount) {
  const auto wavDataSize = sampleCount * 2;
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  unsigned int fileSizeMinus8 = wavDataSize + headerSize - 8;
  header[4] = (byte)(fileSizeMinus8 & 0xFF);
  header[5] = (byte)((fileSizeMinus8 >> 8) & 0xFF);
  header[6] = (byte)((fileSizeMinus8 >> 16) & 0xFF);
  header[7] = (byte)((fileSizeMinus8 >> 24) & 0xFF);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  header[16] = 0x10;  // linear PCM
  header[17] = 0x00;
  header[18] = 0x00;
  header[19] = 0x00;
  header[20] = 0x01;  // linear PCM
  header[21] = 0x00;
  header[22] = 0x01;  // monoral
  header[23] = 0x00;
  header[24] = 0x80;  // sampling rate 16000
  header[25] = 0x3E;
  header[26] = 0x00;
  header[27] = 0x00;
  header[28] = 0x00;  // Byte/sec = 16000x2x1 = 32000
  header[29] = 0x7D;
  header[30] = 0x00;
  header[31] = 0x00;
  header[32] = 0x02;  // 16bit monoral
  header[33] = 0x00;
  header[34] = 0x10;  // 16bit
  header[35] = 0x00;
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  header[40] = (byte)(wavDataSize & 0xFF);
  header[41] = (byte)((wavDataSize >> 8) & 0xFF);
  header[42] = (byte)((wavDataSize >> 16) & 0xFF);
  header[43] = (byte)((wavDataSize >> 24) & 0xFF);
  return (int16_t*)&header[headerSize];
}

void AudioWhisper::Record(const int16_t* preroll, size_t prerollSamples, int silenceStopPeak) {
  if (record_buffer == nullptr) return;  // PSRAM確保失敗時は何もしない
  M5.Mic.begin();
  auto *wavData = reinterpret_cast<int16_t*>(&record_buffer[headerSize]);
  size_t offset = 0;
  if (preroll && prerollSamples > 0) {
    if (prerollSamples > record_size) prerollSamples = record_size;
    ::memcpy(wavData, preroll, prerollSamples * sizeof(int16_t));
    offset = prerollSamples;
  }
  // 発話終了検出: 一度声が入った後、silenceStopPeak 未満が 1.5 秒続いたら打ち切る。
  // (短い呼びかけ+長い無音を Whisper に送ると幻聴になるため。
  //  1.5 秒は息継ぎや文中の間で切れない程度の余裕)
  const int stop_chunks = (int)(1.5f * record_samplerate / record_length);
  bool voice_seen = (prerollSamples > 0);  // プリロール有 = トリガ済みの声がある
  int silent_run = 0;
  while (offset + record_length <= record_size) {
    M5.Mic.record(&wavData[offset], record_length, record_samplerate);
    offset += record_length;
    // record() は非同期 (キュー2段) なので、書き込み完了済みの3チャンク前を評価する
    if (silenceStopPeak > 0 && offset >= prerollSamples + 3 * record_length) {
      const int16_t* done = &wavData[offset - 3 * record_length];
      int peak = 0;
      for (size_t i = 0; i < record_length; i++) {
        int v = abs((int)done[i]);
        if (v > peak) peak = v;
      }
      if (peak >= silenceStopPeak) {
        voice_seen = true;
        silent_run = 0;
      } else if (voice_seen && ++silent_run >= stop_chunks) {
        break;
      }
    }
  }
  while (M5.Mic.isRecording()) { M5.delay(1); }  // 投入済みチャンクの完了を待つ
  M5.Mic.end();
  MakeHeader(record_buffer, offset);
  wav_size = offset * sizeof(int16_t) + headerSize;
}
