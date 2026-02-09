#include <Audio.h>
#include "MyDsp.h"

// ===== I/O Audio Shield =====
AudioInputI2S        audioIn;
MyDsp                dsp;
AudioOutputI2S       audioOut;
AudioControlSGTL5000 sgtl5000;

// ===== Analyse niveau micro =====
AudioAnalyzePeak peakIn;

// ===== Câblage =====
AudioConnection c1(audioIn, 0, dsp, 0);
AudioConnection c2(dsp, 0, audioOut, 0);
AudioConnection c3(dsp, 0, audioOut, 1);

// Micro -> Peak analyzer (en parallèle)
AudioConnection c4(audioIn, 0, peakIn, 0);

elapsedMillis tPrint;

void setup() {
  Serial.begin(115200);
  delay(500);

  AudioMemory(20);

  sgtl5000.enable();
  sgtl5000.inputSelect(AUDIO_INPUT_MIC);
  sgtl5000.micGain(20);   // ajuste si besoin (10-30 typique)
  sgtl5000.volume(0.5);   // volume sortie

  Serial.println("Speak into the mic. Peak level should move.");
}

void loop() {
  // Toutes les 100 ms, on affiche le niveau si dispo
  if (tPrint > 100) {
    tPrint = 0;

    if (peakIn.available()) {
      float p = peakIn.read(); // 0.0 .. 1.0
      Serial.print("Mic peak: ");
      Serial.println(p, 3);
    } else {
      Serial.println("Mic peak: (no data yet)");
    }
  }
}
