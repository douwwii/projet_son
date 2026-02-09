#include "MyDsp.h"

MyDsp::MyDsp()
: AudioStream(1, inputQueueArray) // 1 entrée
{}

void MyDsp::update(void) {
  // Récupère un bloc audio (128 samples typiquement) depuis l'entrée 0
  audio_block_t* inBlock = receiveReadOnly(0);
  if (!inBlock) return;

  // Renvoi direct (mono -> stéréo)
  transmit(inBlock, 0); // vers sortie 0 du MyDsp
  // (On duplique sur la sortie 1 côté routage, voir le .ino)

  release(inBlock);
}
