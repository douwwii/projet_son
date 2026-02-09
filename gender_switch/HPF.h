#pragma once
#include <Arduino.h>

class HPF1 {
public:
  HPF1() = default;

  // cutoffHz: fréquence de coupure (ex: 80 à 150 Hz)
  // fs: fréquence d'échantillonnage (Teensy audio = 44100.0)
  void set(float cutoffHz, float fs);

  // Traite un échantillon (float)
  inline float process(float x) {
    // y[n] = a * ( y[n-1] + x[n] - x[n-1] )
    float y = a_ * (y1_ + x - x1_);
    x1_ = x;
    y1_ = y;
    return y;
  }

  void reset() { x1_ = 0.0f; y1_ = 0.0f; }

private:
  float a_  = 1.0f;
  float x1_ = 0.0f;
  float y1_ = 0.0f;
};
