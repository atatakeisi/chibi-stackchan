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
constexpr uint32_t kGlanceMinMs   = 9000;  // アイドル時キョロッと動く間隔(最小)
constexpr uint32_t kGlanceMaxMs   = 20000; //   〃 (最大)
constexpr int      kGlanceDeg     = 18;    // キョロッの振れ幅(±)
// ---------------------------------------------------------------------------

void setup();

// 50ms 周期程度で呼ぶ。talking=TTS再生中, level01=声の大きさ 0.0-1.0
void update(bool talking, float level01);

// すぐ正面へ(録音開始時など)
void center();

}  // namespace head
