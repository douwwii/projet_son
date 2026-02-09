#pragma once
#include <Arduino.h>
#include <AudioStream.h>

class MyDsp : public AudioStream {
public:
  MyDsp();
  virtual void update(void);

private:
  audio_block_t* inputQueueArray[1]; // 1 entrée audio (mono)
};
