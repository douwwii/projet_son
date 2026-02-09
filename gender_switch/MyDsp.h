#pragma once
#include <Arduino.h>
#include <AudioStream.h>
#include "HPF.h"

class MyDsp : public AudioStream {
public:
  MyDsp();
  virtual void update(void);
  HPF1 hpf_;


private:
  audio_block_t* inputQueueArray[1]; // 1 entrée audio (mono)
};
