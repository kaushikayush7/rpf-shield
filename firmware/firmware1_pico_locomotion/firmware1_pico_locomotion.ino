// FIRMWARE 1 (v2) Raspberry Pi Pico locomotion engine
// Board: Raspberry Pi Pico (Earle Philhower core). Library: Adafruit PWM Servo Driver.
// Channel map: 0/1 Front-Left hip/knee, 2/3 Front-Right, 4/5 Back-Left, 6/7 Back-Right

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

// 50 Hz frame = 20 ms = 4096 ticks -> 1 tick = 4.88 us. 0.5 ms = 102, 2.5 ms = 512.
// Calibrate per servo.
#define SERVOMIN 102
#define SERVOMAX 512

// Right-side servos are mounted mirrored -> invert. TRIM per-joint mechanical zero offset.
const int8_t DIR[8]  = {1, 1, -1, -1, 1, 1, -1, -1};
const int8_t TRIM[8] = {0, 0,  0,  0, 0, 0,  0,  0};

const uint32_t CMD_TIMEOUT_MS = 600; // link-loss failsafe: no command -> stand
char cmd = 'S';
uint32_t lastCmdMs = 0, lastLoopMs = 0;
float phase = 0;

int toPulse(float a) { return (int)(SERVOMIN + (a / 180.0f) * (SERVOMAX - SERVOMIN)); }

// 'logical' angle: 90 neutral, same meaning on every leg (mirroring handled by DIR)
void setJoint(uint8_t ch, float logical) {
  float a = 90 + DIR[ch] * (logical - 90) + TRIM[ch];
  a = constrain(a, 10, 170);
  pwm.setPWM(ch, 0, toPulse(a));
}

// mechanical dead-bands
void setLeg(uint8_t hipCh, float hip, float knee) { setJoint(hipCh, hip); setJoint(hipCh + 1, knee); }
void neutralStance() { for (uint8_t i = 0; i < 8; i++) setJoint(i, 90); }
void squatStance() {
  // sniffing squat
  for (uint8_t leg = 0; leg < 4; leg++) setLeg(leg * 2, 45, 140);
}

// Diagonal trot. scale>0 that side steps forward, scale<0 backward, scale = stride length.
// Pair A = FL(0) + BR(6); Pair B = FR(2) + BL(4).
void trot(float scaleL, float scaleR, float dphase) {
  const float HIP = 30, KNEE = 28;
  phase += dphase; if (phase >= 2 * PI) phase -= 2 * PI;
  float pA = phase, pB = phase + PI;
  
  auto hip  = [&](float p, float s) { return 90 + sin(p) * HIP * s; };
  auto knee = [&](float p, float s) {
    float sg = (s >= 0) ? 1.0f : -1.0f;
    return 90 + max(0.0f, cos(p) * sg) * KNEE;
  };

  setLeg(0, hip(pA, scaleL), knee(pA, scaleL)); // FL
  setLeg(6, hip(pA, scaleR), knee(pA, scaleR)); // BR
  setLeg(2, hip(pB, scaleR), knee(pB, scaleR)); // FR
  setLeg(4, hip(pB, scaleL), knee(pB, scaleL)); // BL
}

void setup() {
  Serial1.begin(115200); // GP1=RX0 must match Main ESP32 Serial2
  Wire.setSDA(4); Wire.setSCL(5); Wire.begin();
  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(50);
  neutralStance();
  lastCmdMs = millis();
}

void loop() {
  uint32_t now = millis();
  while (Serial1.available()) { cmd = Serial1.read(); lastCmdMs = now; }
  
  if (now - lastCmdMs > CMD_TIMEOUT_MS) cmd = 'S';
  
  if (now - lastLoopMs >= 20) { // 50 Hz
    lastLoopMs = now;
    switch (cmd) {
      case 'F': trot(1.0, 1.0, 0.20); break;   // forward
      case 'L': trot(-0.8, 0.8, 0.20); break;  // true turn-in-place
      case 'R': trot(0.8, -0.8, 0.20); break;
      case 'C': squatStance(); break;
      default: neutralStance(); break;
    }
  }
}
