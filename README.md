# RPF-SHIELD: Quadruped Autonomous Recon & Threat Interception System

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: ESP32 / RP2040](https://img.shields.io/badge/Hardware-ESP32%20%7C%20RP2040-blue.svg)](#)
[![AI: DeepFace / OpenCV](https://img.shields.io/badge/AI-DeepFace%20%7C%20OpenCV-green.svg)](#)

RPF-SHIELD is an autonomous quadruped (4-legged) robotic platform engineered for specialized security, threat detection, and tactical reconnaissance. The system combines multi-sensor electronic nose sniffing, chemical swab colorimetry, ultrasonic obstacle navigation, and AI-driven facial expression/stress tracking.

---

## 🏗 System Architecture

```text
                  +--------------------------+
                  |   AI Intercept Subsystem |
                  |  (Laptop / Edge PC GUI)  |
                  +------------+-------------+
                               | WiFi (HTTP API)
                               v
+------------------+     +------------------+     +------------------+
| ESP32-CAM Node   |     | Main ESP32 Hub   |     | Raspberry Pi Pico|
| (Stream/Upload)  |     | (Sensors/Master) |     | (PCA9685 Servos) |
+--------+---------+     +--------+---------+     +--------+---------+
         |                        |                        |
         | HTTP Stream            | UART (115200)          | I2C (0x40)
         +----------------------->|----------------------->| PCA9685 PWM
                                  |                        | 8x Servos
```

### Module Responsibilities:
1. **Pico Locomotion Engine (`firmware1_pico_locomotion.ino`)**: Handles gait kinematics (trot, squat, turn, neutral) driving 8 servos via PCA9685. Features link-loss safety timeout.
2. **ESP32-CAM Node (`firmware2_esp32_cam.ino`)**: Serves high-speed MJPEG video stream on port 81 and non-blocking cloud evidence uploads (ImgBB) on port 80.
3. **Main ESP32 Hub (`firmware3_main_esp32.ino`)**: Central controller housing the **Command Centre web dashboard**, MQ-series gas sensors (E-Nose), TCS34725 RGB colorimeter, HC-SR04 ultrasonic sonar, battery telemetry, and subsumption motion arbitration.
4. **AI Subsystem (`face_recon_intercept.py`)**: Runs facial detection, expression/stress estimation using DeepFace, and remote autonomous tracking commands under a dual-key arming policy.

---

## 🛠 Hardware Requirements & BOM

| Component | Quantity | Purpose |
| :--- | :---: | :--- |
| **Raspberry Pi Pico** | 1 | Locomotion Controller |
| **ESP32 Dev Module** | 1 | Main Controller & Web Dashboard |
| **AI Thinker ESP32-CAM** | 1 | Video Stream & Evidence Upload |
| **PCA9685 16-Channel PWM** | 1 | Servo Driver Board |
| **MG996R / DS3218 Servos** | 8 | Quadruped Leg Joint Motors |
| **MQ-2, MQ-3, MQ-135** | 1 ea | E-Nose Gas Sensor Array |
| **TCS34725 Colorimeter** | 1 | Chemical Swab Test Reader |
| **HC-SR04 Ultrasonic Sensor**| 1 | Distance / Obstacle Avoidance |
| **Active Buzzer & 5V Fan** | 1 ea | Alert Audio & Sensor Airflow |
| **3S LiPo Battery & Buck Reg**| 1 | System Power Supply |

---

## 🚀 Directory Structure

```text
rpf-shield/
├── firmware/
│   ├── firmware1_pico_locomotion/
│   │   └── firmware1_pico_locomotion.ino
│   ├── firmware2_esp32_cam/
│   │   └── firmware2_esp32_cam.ino
│   └── firmware3_main_esp32/
│       └── firmware3_main_esp32.ino
└── ai_subsystem/
    ├── face_recon_intercept.py
    └── requirements.txt
```

---

## 💻 Setup & Flashing Instructions

### 1. Pico Locomotion Engine
* Open Arduino IDE or VS Code with Earle Philhower RP2040 core installed.
* Install `Adafruit PWM Servo Driver Library`.
* Connect Pico via USB and flash `firmware1_pico_locomotion.ino`.
* **Wiring**: GP1 (UART RX) -> Main ESP32 TX2 (GPIO17). GP4/GP5 (I2C) -> PCA9685 SDA/SCL.

### 2. ESP32-CAM Surveillance Node
* Open `firmware2_esp32_cam.ino`.
* Update `WIFI_SSID`, `WIFI_PASS`, and `IMGBB_KEY`.
* Select Board: **AI Thinker ESP32-CAM** (Enable PSRAM).
* Flash firmware and assign static IP / DHCP reservation in your router.

### 3. Main ESP32 Hub & Command Centre
* Open `firmware3_main_esp32.ino`.
* Set `WIFI_SSID`, `WIFI_PASS`, and `CAM_IP` (IP of your ESP32-CAM).
* Install `Adafruit TCS34725` library.
* Select Board: **ESP32 Dev Module** and flash.
* Access the Command Centre dashboard by opening `http://<MAIN_ESP32_IP>/` in any browser.

### 4. AI Subsystem (Laptop / PC)
```bash
cd ai_subsystem
pip install -r requirements.txt
python face_recon_intercept.py
```
* Press `a` in the OpenCV window to toggle between **ADVISORY** and **ARMED** mode.

---

## 🔒 Security & Safe Operation

* **Dual-Key Safety Rule**: Autonomous navigation requires explicit enabling on both the web dashboard (`AI intercept ON`) AND pressing `a` in the AI desktop terminal.
* **Non-Blocking Architecture**: High-priority safety behaviors (threat detection halt & sonar obstacle avoidance) override AI navigation commands automatically.

---

## 📄 License

This project is licensed under the MIT License.
