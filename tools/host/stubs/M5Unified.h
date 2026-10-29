#pragma once
// Minimal M5Unified surface for the desktop harness: only what Mantis Seadiver touches.
#include "Arduino.h"
#include <vector>
struct Vec3 { float x, y, z; };
struct ImuData { Vec3 accel; Vec3 gyro; };
namespace lgfx {                                   // just enough of LovyanGFX for the device-path syntax check
struct swap565_t { uint8_t h, l; };
struct Bus_SPI { struct config_t { uint32_t freq_write = 40000000; } c; const config_t& config() const { return c; } void config(const config_t& v) { c = v; } bool init() { return true; } };
}
struct HostPanel { void* getBus() { return nullptr; } };
struct HostDisplay { void setRotation(int) {} void setBrightness(int) {} void startWrite() {} void endWrite() {} void waitDMA() {}
  HostPanel* getPanel() { return nullptr; }
  template <class T> void pushImageDMA(int, int, int, int, const T*) {} };
class M5Canvas;
extern M5Canvas* g_hostLastCanvas;   // last canvas whose buffer was handed out
class M5Canvas {
 public:
  M5Canvas(HostDisplay*) {}
  void setColorDepth(int d) { depth_ = d; }
  void setPsram(bool) {}
  void* createSprite(int w, int h) { w_ = w; h_ = h; buf_.assign((size_t)w * h, 0); return buf_.data(); }
  void* getBuffer() { g_hostLastCanvas = this; return buf_.empty() ? nullptr : buf_.data(); }
  void pushSprite(HostDisplay*, int, int) { pushes++; }
  int width() const { return w_; }
  int height() const { return h_; }
  const uint16_t* pixels() const { return buf_.data(); }
  long pushes = 0;
 private:
  int w_ = 0, h_ = 0, depth_ = 16;
  std::vector<uint16_t> buf_;
};
namespace m5 {                                    // (signatures as in M5Unified's IMU_Class.hpp / MPU6886_Class)
enum imu_t { imu_none, imu_unknown, imu_sh200q, imu_mpu6050, imu_mpu6886, imu_mpu9250, imu_bmi270 };
struct IMU_Base { virtual ~IMU_Base() {} };
struct MPU6886_Class : IMU_Base { enum Fodr { ODR_1kHz = 0, ODR_500Hz = 1 }; void enableFIFO(Fodr) {} };
}
struct HostImu {
  ImuData data{{0, 0, 1}, {0, 0, 0}};
  float tempC = 30.f;                              // tests can warm the chip up
  bool isEnabled() { return true; }
  bool loadOffsetFromNVS() { return true; }
  bool update() { return true; }
  ImuData getImuData() { return data; }
  bool getTemp(float* t) { *t = tempC; return true; }
  m5::imu_t getType() const { return m5::imu_mpu6886; }
  m5::IMU_Base* getImuInstancePtr(int) const { return nullptr; }
};
struct HostPower { int last = 0; long changes = 0; void setVibration(int v) { if (v != last) changes++; last = v; } };
struct HostSpkCfg { uint8_t task_priority = 2; uint8_t task_pinned_core = 255; size_t dma_buf_len = 256; size_t dma_buf_count = 8; };
struct HostSpeaker {
  HostSpkCfg cfg; HostSpkCfg config() const { return cfg; } void config(const HostSpkCfg& c) { cfg = c; }
  bool en = false; long blocks = 0;
  bool isEnabled() { return en; } bool begin() { en = true; return true; } void end() { en = false; }
  void setVolume(int) {} size_t isPlaying(int) { return 0; }
  bool playRaw(const int16_t*, size_t, uint32_t, bool, uint32_t, int, bool) { blocks++; return true; }
};
struct TouchDetail {
  int x = 0, y = 0; bool pressed = false, was = false, rel = false;
  bool isPressed() const { return pressed; } bool wasPressed() const { return was; } bool wasReleased() const { return rel; }
};
struct HostTouch {
  bool down = false, prev = false; int x = 160, y = 120;
  TouchDetail getDetail() { TouchDetail d; d.x = x; d.y = y; d.pressed = down; d.was = down && !prev; d.rel = !down && prev; return d; }
};
struct HostRtcDT { struct { int year = 2026, month = 10, date = 19; } date; struct { int hours = 18, minutes = 40; } time; };
struct HostRtc { HostRtcDT getDateTime() { return HostRtcDT(); } };
// Buttons: tests set the edge flags for one frame (the harness clears them after each loop()).
struct HostBtn {
  bool pressed = false, clicked = false, hold = false, down = false;
  bool wasPressed() { return pressed; } bool wasClicked() { return clicked; } bool wasHold() { return hold; }
  bool isPressed() { return down; } void setHoldThresh(int) {}
  void clear() { pressed = clicked = hold = false; }
};
struct HostCfg { bool output_power = false, internal_imu = false, internal_mic = false, internal_spk = false; };
struct M5Class {
  HostDisplay Display; HostImu Imu; HostPower Power; HostSpeaker Speaker; HostTouch Touch; HostBtn BtnA, BtnB, BtnC; HostRtc Rtc;
  HostCfg config() { return {}; }
  void begin(HostCfg) {}
  void update() { Touch.prev = touchLatch; touchLatch = Touch.down; }
  bool touchLatch = false;
};
extern M5Class M5;
