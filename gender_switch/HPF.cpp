#include "HPF.h"
#include <math.h>

void HPF1::set(float cutoffHz, float fs) {
  if (cutoffHz < 1.0f) cutoffHz = 1.0f;
  if (fs < 1000.0f) fs = 44100.0f;

  // Filtre RC discret : a = RC / (RC + dt)
  // RC = 1/(2*pi*fc), dt = 1/fs
  float RC = 1.0f / (2.0f * (float)M_PI * cutoffHz);
  float dt = 1.0f / fs;
  a_ = RC / (RC + dt);
}
