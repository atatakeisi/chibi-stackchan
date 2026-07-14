// HeadServo - single SG90-type PWM servo for left/right head shake.
//
// ATOMS3R の Grove ポート (G1 / G2 / 5V / GND) に接続する想定。
// 首は連続回転しない。センター(90度)を中心に左右へ振るだけ。
//   - 会話(TTS再生)中: 声の大きさに合わせて左右にゆらゆら首振り
//   - アイドル時: センターへ戻り、しばらくしたら PWM 停止(省電力・ジッタ防止)。
//     ときどき小さくキョロッと横を向く"生きてる感"モーションあり。
#pragma once

#include <Arduino.h>

namespace head {

// ---- 調整用定数 -----------------------------------------------------------
constexpr int      kServoPin      = 2;     // Grove G2 (黄:信号)。G1に配線したら 1 に
constexpr int      kCenterDeg     = 90;    // 正面
constexpr int      kSwingDeg      = 28;    // 会話中の首振り振幅(±)
constexpr int      kMinDeg        = 40;    // 機構保護のための可動範囲
constexpr int      kMaxDeg        = 140;
constexpr float    kSwingHz       = 0.45f; // 会話中の首振りの速さ(往復/秒)
constexpr uint32_t kIdleDetachMs  = 1500;  // アイドルでセンター復帰後、PWMを止めるまで
constexpr uint32_t kGlanceMinMs   = 12000; // アイドル時にキョロッと動く間隔(最小)
constexpr uint32_t kGlanceMaxMs   = 30000; //   〃 (最大)
constexpr int      kGlanceDegMin  = 6;     // キョロッの振れ幅(最小±)
constexpr int      kGlanceDeg     = 18;    //   〃 (最大±)
constexpr float    kGlanceDpsMin  = 14.0f; // キョロッの首の速さ deg/s (最小)。遅いほどジジジ感
constexpr float    kGlanceDpsMax  = 38.0f; //   〃 (最大)
constexpr float    kCenterReturnDps = 25.0f; // 会話後などに正面へ戻る速さ deg/s (ぎゅっと戻さない)
// ---------------------------------------------------------------------------

void setup();

// 50ms 周期程度で呼ぶ。talking=TTS再生中, level01=声の大きさ 0.0-1.0
void update(bool talking, float level01);

// すぐ正面へ(録音開始時など)
void center();

// 聞き取り集中モード。on=true で正面へ静かに戻して静止し、少ししたら脱力する
// (アイドルのキョロキョロを止め、サーボの動作音がマイクに乗らないようにする)。
// 会話(TTS再生)の首振りには影響しない。
void focus(bool on);

// サーボが直近 ms ミリ秒以内に動いた(またはPWMを繋いだ/切った)なら true。
// 動作音「ジジッ」をマイクが拾って音量トリガが誤反応するのを防ぐために使う。
bool movedRecently(uint32_t ms);

}  // namespace head
