#include "HeadServo.h"

namespace head {

namespace {

constexpr int      kLedcCh   = 4;      // M5.Speaker/Avatar と被らないチャネル
constexpr int      kLedcBits = 16;
constexpr int      kLedcFreq = 50;     // 50Hz (20ms)
constexpr uint32_t kPulseMinUs = 500;  // 0deg
constexpr uint32_t kPulseMaxUs = 2400; // 180deg

bool     attached      = false;
float    currentDeg    = kCenterDeg;
float    phase         = 0.0f;
uint32_t lastMs        = 0;
uint32_t idleSinceMs   = 0;
// idle glance state
uint32_t nextGlanceMs  = 0;
uint32_t glanceEndMs   = 0;
int      glanceTarget  = kCenterDeg;

void writeDeg(float deg) {
  if (deg < kMinDeg) deg = kMinDeg;
  if (deg > kMaxDeg) deg = kMaxDeg;
  currentDeg = deg;
  uint32_t us = kPulseMinUs + (uint32_t)((kPulseMaxUs - kPulseMinUs) * deg / 180.0f);
  uint32_t duty = (uint32_t)((uint64_t)us * ((1 << kLedcBits) - 1) / 20000);
  ledcWrite(kLedcCh, duty);
}

void attach() {
  if (!attached) {
    ledcAttachPin(kServoPin, kLedcCh);
    attached = true;
  }
}

void detach() {
  if (attached) {
    ledcWrite(kLedcCh, 0);  // パルス停止 = SG90 は脱力状態(保持トルク無し)
    ledcDetachPin(kServoPin);
    pinMode(kServoPin, OUTPUT);
    digitalWrite(kServoPin, LOW);
    attached = false;
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
    glanceEndMs = 0;
    nextGlanceMs = now + pickGlanceDelay();
    return;
  }

  // ---- idle ----
  if (glanceEndMs != 0) {           // キョロッと横を向いている最中
    if (now >= glanceEndMs) {
      glanceEndMs = 0;
      writeDeg(kCenterDeg);
      idleSinceMs = now;
      nextGlanceMs = now + pickGlanceDelay();
    }
    return;
  }

  if (now >= nextGlanceMs) {        // ときどきキョロッ
    attach();
    glanceTarget = kCenterDeg + (random(2) ? kGlanceDeg : -kGlanceDeg) - (int)random(6);
    writeDeg(glanceTarget);
    glanceEndMs = now + 700 + random(600);
    return;
  }

  if (fabsf(currentDeg - kCenterDeg) > 0.5f) {  // センターへ復帰
    writeDeg(kCenterDeg);
    idleSinceMs = now;
    return;
  }

  if (attached && now - idleSinceMs > kIdleDetachMs) {
    detach();                       // 省電力 & ジッタ音防止
  }
}

}  // namespace head
