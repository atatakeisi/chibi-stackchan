#include "HeadServo.h"

namespace head {

namespace {

constexpr int      kLedcCh   = 4;      // M5.Speaker/Avatar と被らないチャネル
constexpr int      kLedcBits = 14;     // ESP32-S3 の LEDC は最大14ビット (16だと初期化失敗)
constexpr int      kLedcFreq = 50;     // 50Hz (20ms)
constexpr uint32_t kPulseMinUs = 500;  // 0deg
constexpr uint32_t kPulseMaxUs = 2400; // 180deg

bool     attached      = false;
float    currentDeg    = kCenterDeg;
float    phase         = 0.0f;
uint32_t lastMs        = 0;
uint32_t idleSinceMs   = 0;
// idle glance state (ゆっくり横を向く → 少し止まる → ゆっくり戻る)
enum class GlancePhase : uint8_t { Wait, MoveOut, Hold, MoveBack };
GlancePhase   gphase   = GlancePhase::Wait;
uint32_t nextGlanceMs  = 0;
uint32_t glanceEndMs   = 0;
int      glanceTarget  = kCenterDeg;
float    glanceDps     = kGlanceDpsMin;  // このグランスの移動速度 (毎回ランダム)
// 聞き取り集中 (focus): アイドル動作を止めて正面で静止する
volatile bool focused  = false;
uint32_t focusAtMs     = 0;
// 最後にサーボが動いた時刻 (動作音による音量トリガ誤反応の防止用)
volatile uint32_t lastMoveMs = 0;

void writeDeg(float deg) {
  if (deg < kMinDeg) deg = kMinDeg;
  if (deg > kMaxDeg) deg = kMaxDeg;
  if (fabsf(deg - currentDeg) > 0.2f) lastMoveMs = millis();
  currentDeg = deg;
  uint32_t us = kPulseMinUs + (uint32_t)((kPulseMaxUs - kPulseMinUs) * deg / 180.0f);
  uint32_t duty = (uint32_t)((uint64_t)us * ((1 << kLedcBits) - 1) / 20000);
  ledcWrite(kLedcCh, duty);
}

void attach() {
  if (!attached) {
    ledcAttachPin(kServoPin, kLedcCh);
    attached = true;
    lastMoveMs = millis();  // 繋いだ瞬間に位置合わせで動くことがある
  }
}

void detach() {
  if (attached) {
    ledcWrite(kLedcCh, 0);  // パルス停止 = SG90 は脱力状態(保持トルク無し)
    ledcDetachPin(kServoPin);
    pinMode(kServoPin, OUTPUT);
    digitalWrite(kServoPin, LOW);
    attached = false;
    lastMoveMs = millis();  // 脱力時にカクッと音が出ることがある
  }
}

uint32_t pickGlanceDelay() {
  return kGlanceMinMs + (uint32_t)random(kGlanceMaxMs - kGlanceMinMs);
}

}  // namespace

void setup() {
  ledcSetup(kLedcCh, kLedcFreq, kLedcBits);
  attach();
  writeDeg(kCenterDeg);
  lastMs = millis();
  idleSinceMs = lastMs;
  nextGlanceMs = lastMs + pickGlanceDelay();
}

void center() {
  attach();
  writeDeg(kCenterDeg);
  phase = 0.0f;
  idleSinceMs = millis();
  gphase = GlancePhase::Wait;
}

void focus(bool on) {
  focused = on;
  if (on) {
    // 一気に正面へ戻すと「ジッ」という動作音が録音の先頭に乗るため、
    // ここでは態勢だけ整え、update() の focused 処理でゆっくり正面へ戻す
    attach();
    phase = 0.0f;
    gphase = GlancePhase::Wait;
    focusAtMs = millis();
  } else {
    idleSinceMs = millis();
    nextGlanceMs = millis() + pickGlanceDelay();  // 解除直後にキョロッとしない
  }
}

bool movedRecently(uint32_t ms) {
  return millis() - lastMoveMs < ms;
}

// currentDeg から target へ speed(deg/s) で1ステップ近づく。到達したら true
static bool stepToward(float target, float speed, float dt) {
  const float step = speed * dt;
  const float diff = target - currentDeg;
  if (fabsf(diff) <= step) {
    writeDeg(target);
    return true;
  }
  writeDeg(currentDeg + (diff > 0 ? step : -step));
  return false;
}

void update(bool talking, float level01) {
  uint32_t now = millis();
  float dt = (now - lastMs) / 1000.0f;
  if (dt > 0.5f) dt = 0.5f;
  lastMs = now;

  if (talking) {
    // 声が大きいほど少しだけ大きく・速く首を振る
    attach();
    phase += 2.0f * PI * kSwingHz * (0.8f + 0.6f * level01) * dt;
    float amp = kSwingDeg * (0.55f + 0.45f * level01);
    writeDeg(kCenterDeg + amp * sinf(phase));
    idleSinceMs = now;
    gphase = GlancePhase::Wait;
    nextGlanceMs = now + pickGlanceDelay();
    return;
  }

  if (focused) {
    // 聞き取り集中中: 正面で静止。落ち着いたら脱力してサーボ音を消す
    if (fabsf(currentDeg - kCenterDeg) > 0.5f) {  // 「なあに？」の首振り後など
      attach();
      stepToward(kCenterDeg, 60.0f, dt);
      focusAtMs = now;
    } else if (attached && now - focusAtMs > 400) {
      detach();
    }
    return;
  }

  // ---- idle: ときどきゆっくりキョロッ (速さ・角度・間隔は毎回ランダム) ----
  switch (gphase) {
    case GlancePhase::Wait:
      if (now >= nextGlanceMs) {
        attach();
        const int amp = kGlanceDegMin + (int)random(kGlanceDeg - kGlanceDegMin + 1);
        glanceTarget = kCenterDeg + (random(2) ? amp : -amp);
        glanceDps = kGlanceDpsMin +
                    (kGlanceDpsMax - kGlanceDpsMin) * (random(1000) / 1000.0f);
        gphase = GlancePhase::MoveOut;
        break;
      }
      if (fabsf(currentDeg - kCenterDeg) > 0.5f) {  // 会話後などのセンター復帰はゆっくり
        attach();
        stepToward(kCenterDeg, kCenterReturnDps, dt);
        idleSinceMs = now;
        break;
      }
      if (attached && now - idleSinceMs > kIdleDetachMs) {
        detach();                   // 省電力 & ジッタ音防止
      }
      break;

    case GlancePhase::MoveOut:
      if (stepToward(glanceTarget, glanceDps, dt)) {
        glanceEndMs = now + 600 + random(1400);  // 横を向いたまま少し止まる
        gphase = GlancePhase::Hold;
      }
      break;

    case GlancePhase::Hold:
      if (now >= glanceEndMs) gphase = GlancePhase::MoveBack;
      break;

    case GlancePhase::MoveBack:
      if (stepToward(kCenterDeg, glanceDps, dt)) {
        idleSinceMs = now;
        nextGlanceMs = now + pickGlanceDelay();
        gphase = GlancePhase::Wait;
      }
      break;
  }
}

}  // namespace head
