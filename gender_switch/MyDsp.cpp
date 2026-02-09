#include "MyDsp.h"
#include "HPF.h"

MyDsp::MyDsp()
: AudioStream(1, inputQueueArray) // 1 entrée
{
  hpf_.set(60.0f, 44100.0f); // 60 Hz : bon départ pour voix
  hpf_.reset();
}

void MyDsp::update(void) {
  audio_block_t* inBlock = receiveReadOnly(0);
  if (!inBlock) return;

  audio_block_t* outBlock = allocate();
  if (!outBlock) { release(inBlock); return; }

  // AUDIO_BLOCK_SAMPLES = 128, samples int16_t
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    float x = (float)inBlock->data[i];

    float y = hpf_.process(x);

    // clamp int16
    if (y > 32767.0f) y = 32767.0f;
    if (y < -32768.0f) y = -32768.0f;

    outBlock->data[i] = (int16_t)y;
  }

  transmit(outBlock, 0);
  release(outBlock);
  release(inBlock);
}
