#pragma once

void Voicevox_tts(char *text,char *tts_parms);

// テキストを VoiceVox で合成し、MP3 を SPIFFS の path に保存する (事前キャッシュ用)。
// ネットワークを使うのは保存時のみで、再生はオフラインで即時にできる。
bool Voicevox_tts_download(const char* text, const char* tts_parms, const char* path);
