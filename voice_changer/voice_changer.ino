#include <Audio.h>
#include <Wire.h>
#include <SPI.h>

// ============================================================
// TEENSY 4.0 - Male -> Female (Pitch + "Formant EQ")
// Basé sur AudioEffectGranular (pitch shift stable) + formant shaper
// ============================================================

// ------------------ Audio objects ------------------
AudioInputI2S        mic;
AudioAmplifier       preamp;        // gain logiciel (laisser à 1.0 au début)
AudioEffectGranular  granular;      // pitch shift officiel

// Formant shaper (3 résonances + mix)
AudioFilterStateVariable svf1;
AudioFilterStateVariable svf2;
AudioFilterStateVariable svf3;
AudioMixer4          formantMixer;  // direct + 3 bandes

// Dry/Wet mix
AudioMixer4          mixer;
AudioOutputI2S       out;

AudioAnalyzePeak     peakIn;
AudioAnalyzeRMS      rmsIn;

AudioControlSGTL5000 sgtl5000;

// ------------------ Patch cords ------------------
// mic -> preamp
AudioConnection c1(mic, 0, preamp, 0);

// preamp -> granular
AudioConnection c2(preamp, 0, granular, 0);

// preamp -> analyzers (diagnostic clip)
AudioConnection c3(preamp, 0, peakIn, 0);
AudioConnection c4(preamp, 0, rmsIn, 0);

// Granular -> formant filters + direct
AudioConnection c5(granular, 0, formantMixer, 0);  // direct
AudioConnection c6(granular, 0, svf1, 0);
AudioConnection c7(granular, 0, svf2, 0);
AudioConnection c8(granular, 0, svf3, 0);

// bandpass outputs -> formantMixer
AudioConnection c9 (svf1, 1, formantMixer, 1);
AudioConnection c10(svf2, 1, formantMixer, 2);
AudioConnection c11(svf3, 1, formantMixer, 3);

// Dry/Wet mix: dry from preamp, wet from formantMixer
AudioConnection c12(preamp,      0, mixer, 0);   // dry
AudioConnection c13(formantMixer,0, mixer, 1);   // wet

// out
AudioConnection c14(mixer, 0, out, 0);
AudioConnection c15(mixer, 0, out, 1);

// ------------------ Granular memory ------------------
#define GRANULAR_MEMORY_SIZE 12800
int16_t granularMemory[GRANULAR_MEMORY_SIZE];

// ------------------ Parameters ------------------
float speed      = 1.30f;   // pitch up
float wet        = 0.85f;   // 0..1
bool  bypass     = false;
float grainMs    = 60.0f;   // 40..90ms souvent bien

float formantAmt = 0.55f;   // 0..1
float outGain    = 1.0f;    // gain global appliqué au mix (0.5..1.2 typique)

// ------------------ Utils ------------------
static float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

static void applyDryWet(bool bypassMode, float wetAmt, float globalGain) {
  wetAmt = clampf(wetAmt, 0.0f, 1.0f);
  globalGain = clampf(globalGain, 0.0f, 2.0f);

  float gDry, gWet;
  if (bypassMode) {
    gDry = 1.0f;
    gWet = 0.0f;
  } else {
    gDry = (1.0f - wetAmt);
    gWet = wetAmt;
  }

  // Apply global gain safely
  mixer.gain(0, gDry * globalGain);
  mixer.gain(1, gWet * globalGain);
  mixer.gain(2, 0.0f);
  mixer.gain(3, 0.0f);
}

static void applyFormant(float amt) {
  amt = clampf(amt, 0.0f, 1.0f);

  // Centres de "formants" (approx) : ajustables
  svf1.frequency(900.0f);
  svf2.frequency(1800.0f);
  svf3.frequency(3000.0f);

  // Q (résonance) : trop haut => nasal/robot
  svf1.resonance(2.2f);
  svf2.resonance(2.8f);
  svf3.resonance(3.0f);

  float direct = 1.0f - 0.35f * amt;
  float g1 = 0.35f * amt;
  float g2 = 0.45f * amt;
  float g3 = 0.40f * amt;

  formantMixer.gain(0, direct);
  formantMixer.gain(1, g1);
  formantMixer.gain(2, g2);
  formantMixer.gain(3, g3);
}

static void reconfigureGranular() {
  granular.beginPitchShift(grainMs);
  granular.setSpeed(speed);
}

static void printMenu() {
  Serial.println("\n=== COMMANDES ===");
  Serial.println("Pitch:");
  Serial.println("  p / o : speed +/- 0.02");
  Serial.println("Grain:");
  Serial.println("  g / f : grainMs +/- 5ms");
  Serial.println("Mix:");
  Serial.println("  w / d : wet +/- 0.05");
  Serial.println("Formants:");
  Serial.println("  k / j : formant +/- 0.05");
  Serial.println("Gain global:");
  Serial.println("  + / - : outGain +/- 0.05");
  Serial.println("Other:");
  Serial.println("  b : bypass on/off");
  Serial.println("  s : status");
  Serial.println("  r : reset");
  Serial.println("Conseil: si nasal/robot -> baisse formant (j) ou baisse resonance.");
}

static void printStatus() {
  Serial.println("\n--- STATUS ---");
  Serial.print("speed="); Serial.print(speed, 2);
  Serial.print("  grain="); Serial.print(grainMs, 1); Serial.print("ms");
  Serial.print("  wet="); Serial.print(wet, 2);
  Serial.print("  formant="); Serial.print(formantAmt, 2);
  Serial.print("  outGain="); Serial.print(outGain, 2);
  Serial.print("  bypass="); Serial.println(bypass ? "ON" : "OFF");

  if (peakIn.available()) {
    float p = peakIn.read();
    Serial.print("PEAK(in)="); Serial.print(p, 3);
    if (p > 0.90f) Serial.print("  !!! CLIP RISK -> baisse micGain/volume");
    Serial.println();
  }
  if (rmsIn.available()) {
    Serial.print("RMS(in)="); Serial.println(rmsIn.read(), 4);
  }

  Serial.print("CPU="); Serial.print(AudioProcessorUsage(), 1);
  Serial.print("%  CPUmax="); Serial.print(AudioProcessorUsageMax(), 1);
  Serial.print("%  Mem="); Serial.print(AudioMemoryUsage());
  Serial.print("/"); Serial.println(AudioMemoryUsageMax());
}

void setup() {
  Serial.begin(115200);
  delay(500);

  AudioMemory(60);

  sgtl5000.enable();
  sgtl5000.inputSelect(AUDIO_INPUT_MIC);

  // IMPORTANT: éviter le clip
  sgtl5000.micGain(8);      // adapte 6..15
  sgtl5000.volume(0.75);    // évite 0.8 au début (sortie peut saturer)

  preamp.gain(1.0f);

  granular.begin(granularMemory, GRANULAR_MEMORY_SIZE);
  reconfigureGranular();

  applyFormant(formantAmt);
  applyDryWet(bypass, wet, outGain);

  Serial.println("\n=== Male -> Female : Pitch + Formant EQ ===");
  printMenu();
  printStatus();
}

void loop() {
  static elapsedMillis t;
  if (t > 2000) { t = 0; if (peakIn.available() || rmsIn.available()) printStatus(); }

  if (!Serial.available()) return;
  char c = Serial.read();

  switch (c) {
    case 'p':
      speed = clampf(speed + 0.02f, 0.50f, 2.50f);
      granular.setSpeed(speed);
      break;
    case 'o':
      speed = clampf(speed - 0.02f, 0.50f, 2.50f);
      granular.setSpeed(speed);
      break;

    case 'g':
      grainMs = clampf(grainMs + 5.0f, 20.0f, 200.0f);
      reconfigureGranular();
      break;
    case 'f':
      grainMs = clampf(grainMs - 5.0f, 20.0f, 200.0f);
      reconfigureGranular();
      break;

    case 'w':
      wet = clampf(wet + 0.05f, 0.0f, 1.0f);
      applyDryWet(bypass, wet, outGain);
      break;
    case 'd':
      wet = clampf(wet - 0.05f, 0.0f, 1.0f);
      applyDryWet(bypass, wet, outGain);
      break;

    case 'k':
      formantAmt = clampf(formantAmt + 0.05f, 0.0f, 1.0f);
      applyFormant(formantAmt);
      break;
    case 'j':
      formantAmt = clampf(formantAmt - 0.05f, 0.0f, 1.0f);
      applyFormant(formantAmt);
      break;

    case '+':
      outGain = clampf(outGain + 0.05f, 0.0f, 1.5f);
      applyDryWet(bypass, wet, outGain);
      break;
    case '-':
      outGain = clampf(outGain - 0.05f, 0.0f, 1.5f);
      applyDryWet(bypass, wet, outGain);
      break;

    case 'b':
      bypass = !bypass;
      applyDryWet(bypass, wet, outGain);
      break;

    case 'r':
      speed = 1.35f;
      wet = 0.85f;
      grainMs = 60.0f;
      formantAmt = 0.55f;
      outGain = 1.0f;
      bypass = false;
      reconfigureGranular();
      applyFormant(formantAmt);
      applyDryWet(bypass, wet, outGain);
      break;

    case 's':
      printStatus();
      return;

    default:
      printMenu();
      return;
  }

  printStatus();
}
