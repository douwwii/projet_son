#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <math.h>

/*
  Teensy 4.0 + Audio Shield (SGTL5000)
  Voice Gender Switcher :
  - MODE_BYPASS : micro direct
  - MODE_M2F    : hausse de pitch + formants plus hauts + coupe-bas + un peu d'air
  - MODE_F2M    : baisse de pitch + formants plus bas + coupe-bas plus faible

  Contrôles :
  - Pot A8 : pitch (speed du granular) selon le mode
  - Pot A4 : mix wet (dry/wet)
  - Bouton A0 (pullup) : toggle M2F <-> F2M
  - Auto-mode (YIN) : bascule M2F/F2M selon F0 lissée
  - Serial : commandes de réglage (p/o/g/f/w/d/k/j/a/z/u/y/+/-/b/m/r/s)
*/

//
// ============================
// 1) Chaîne audio (Audio Library)
// ============================
//
// Le signal suit ce chemin :
// mic -> preamp -> lowCut(HPF) -> granular(pitch shift) -> (formants + air) -> wetMix -> dry/wet -> out
//
// En parallèle, on analyse le signal (peak, rms, YIN, FFT) sans toucher au son.
//

AudioInputI2S        mic;        // Entrée micro via I2S (Audio Shield)
AudioAmplifier       preamp;     // Gain logiciel sur le signal micro
AudioFilterStateVariable lowCut; // Filtre SVF utilisé en HIGH-PASS (sortie 2)
AudioEffectGranular  granular;   // Pitch-shift par grains

AudioFilterStateVariable svf1;   // Formant 1 (bandpass sortie 1)
AudioFilterStateVariable svf2;   // Formant 2 (bandpass sortie 1)
AudioFilterStateVariable svf3;   // Formant 3 (bandpass sortie 1)
AudioFilterStateVariable airSVF; // Bande "air" (bandpass) autour de 6.5 kHz

AudioMixer4          formantMixer; // Mélange direct + 3 bandes formants
AudioMixer4          wetMix;       // Mélange wet principal + air
AudioMixer4          mixer;        // Mix final dry/wet

AudioOutputI2S       out;          // Sortie casque/line-out (I2S)

AudioAnalyzePeak     peakIn;       // Niveau crête (debug)
AudioAnalyzeRMS      rmsIn;        // Niveau RMS (debug)
AudioAnalyzeFFT1024  fftIn;        // Analyse spectrale (debug)
AudioAnalyzeNoteFrequency noteFreqIn; // Détection F0 (YIN) pour auto-switch

AudioControlSGTL5000 sgtl5000;     // Contrôle du codec (gain mic, volume casque, etc.)

//
// ============================
// 2) Connexions (patch cords)
// ============================
//
// Les AudioConnection décrivent le câblage des blocs DSP.
// Elles sont évaluées en temps réel par l'Audio Library.
//

AudioConnection c1(mic, 0, preamp, 0);

// Analyses sur le signal micro amplifié (ne modifie pas l'audio)
AudioConnection c3(preamp, 0, peakIn, 0);
AudioConnection c4(preamp, 0, rmsIn, 0);
AudioConnection c4b(preamp, 0, noteFreqIn, 0);
// Important : si tu veux réellement utiliser fftIn, il faut le connecter aussi :
AudioConnection c4c(preamp, 0, fftIn, 0);

// Coupe-bas (HPF) puis pitch-shift
AudioConnection cLC_in(preamp, 0, lowCut, 0);
AudioConnection cLC_toGran(lowCut, 2, granular, 0);

// Sortie granular : direct + alimente les filtres formants + air
AudioConnection c5 (granular, 0, formantMixer, 0);
AudioConnection c6 (granular, 0, svf1, 0);
AudioConnection c7 (granular, 0, svf2, 0);
AudioConnection c8 (granular, 0, svf3, 0);
AudioConnection cAirIn(granular, 0, airSVF, 0);

// Bandpass des formants -> mixer dédié aux formants
AudioConnection c9 (svf1, 1, formantMixer, 1);
AudioConnection c10(svf2, 1, formantMixer, 2);
AudioConnection c11(svf3, 1, formantMixer, 3);

// Wet principal + air
AudioConnection cWet0(formantMixer, 0, wetMix, 0);
AudioConnection cWetAir(airSVF, 1, wetMix, 1);

// Vers mix final
AudioConnection cWetToMain(wetMix, 0, mixer, 1);
AudioConnection cDry(preamp, 0, mixer, 0);

// Sortie stéréo (copie mono sur L/R)
AudioConnection cOutL(mixer, 0, out, 0);
AudioConnection cOutR(mixer, 0, out, 1);

//
// ============================
// 3) Mémoire pour l'effet Granular
// ============================
//
// Le granular a besoin d'un buffer RAM pour stocker des fragments du signal.
// Plus grand = plus stable mais consomme de la RAM.
//

#define GRANULAR_MEMORY_SIZE 12800
int16_t granularMemory[GRANULAR_MEMORY_SIZE];

//
// ============================
// 4) Modes et paramètres globaux
// ============================

enum VoiceMode { MODE_BYPASS=0, MODE_M2F=1, MODE_F2M=2 };
VoiceMode mode = MODE_M2F;

float speed   = 1.24f;   // Ratio de pitch du granular
float grainMs = 60.0f;   // Taille des grains : petit = plus d'artefacts, grand = plus lisse

float wet     = 0.85f;   // Proportion wet (transformé) dans le mix final
bool  bypass  = false;   // Si true : sortie = dry uniquement
float outGain = 1.0f;    // Gain global appliqué au dry et au wet

float formantAmt = 0.55f; // Intensité des formants (0..1)
float lowCutHz   = 140.0f;// Fréquence du coupe-bas (HPF)
float airAmt     = 0.20f; // Intensité du boost haut-aigu "air"

//
// ============================
// 5) Entrées (pots et bouton)
// ============================

constexpr uint8_t PITCH_POT_PIN = A8; // contrôle speed
constexpr uint8_t WET_POT_PIN   = A3; // contrôle wet
constexpr uint8_t MODE_BTN_PIN  = A0; // bouton toggle M2F/F2M (pullup)

float potMinSpeed_M2F = 1.00f, potMaxSpeed_M2F = 1.90f;
float potMinSpeed_F2M = 0.55f, potMaxSpeed_F2M = 1.00f;

float potMinWet_M2F = 0.40f, potMaxWet_M2F = 1.00f;
float potMinWet_F2M = 0.40f, potMaxWet_F2M = 1.00f;

float potSmooth = 0.08f;
float wetPotSmooth = 0.08f;

static float potFiltered = 0.0f;
static float wetPotFiltered = 0.0f;

constexpr float SPEED_EPS = 0.005f;
constexpr float WET_EPS   = 0.01f;

constexpr uint32_t POT_PERIOD_MS = 15;
constexpr uint32_t WET_POT_PERIOD_MS = 15;

static elapsedMillis potTimer;
static elapsedMillis wetPotTimer;

// Débounce bouton
constexpr uint32_t MODE_BTN_DEBOUNCE_MS = 35;
static elapsedMillis modeBtnTimer;
static bool modeBtnPrev = true;

//
// ============================
// 6) Auto-détection F0 (YIN)
// ============================
//
// noteFreqIn calcule une estimation de F0 et une probabilité.
// On ne bascule de mode que si :
//  - le niveau est suffisant (peak/rms)
//  - la proba YIN est suffisante
//  - la F0 lissée dépasse un seuil avec votes (hystérésis temporelle)
//

bool autoMode = true;

constexpr uint32_t AUTO_PERIOD_MS = 65;
static elapsedMillis autoTimer;

static float autoF0Smooth = 0.0f;
static float autoLastF0   = -1.0f;
static float autoLastProb = 0.0f;
static float autoLastPeak = 0.0f;
static float autoLastRms  = 0.0f;

static uint8_t autoMaleVotes = 0;
static uint8_t autoFemaleVotes = 0;

constexpr float AUTO_F0_LOW  = 165.0f; // sous ce seuil -> plutôt voix grave
constexpr float AUTO_F0_HIGH = 165.0f; // au-dessus -> plutôt voix aiguë

constexpr float AUTO_MIN_PEAK = 0.008f;
constexpr float AUTO_MIN_RMS  = 0.003f;
constexpr float AUTO_YIN_THRESHOLD = 0.20f;
constexpr float AUTO_MIN_PROB = 0.75f;

constexpr uint8_t AUTO_CONFIRM_VOTES = 3;

//
// ============================
// 7) Utilitaires DSP (clamp + application des réglages)
// ============================

static float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

/*
  applyDryWet :
  - Le mixer final a 2 entrées utilisées :
    ch0 = dry (micro)
    ch1 = wet (transformé)
  - On applique (1-wet) sur dry et wet sur wet, puis outGain sur les deux.
  - En bypass, on force dry=1 et wet=0.
*/
static void applyDryWet(bool bypassMode, float wetAmt, float globalGain) {
  wetAmt     = clampf(wetAmt, 0.0f, 1.0f);
  globalGain = clampf(globalGain, 0.0f, 2.0f);

  float gDry = 0.0f, gWet = 0.0f;
  if (bypassMode) {
    gDry = 1.0f;
    gWet = 0.0f;
  } else {
    gDry = (1.0f - wetAmt);
    gWet = wetAmt;
  }

  mixer.gain(0, gDry * globalGain);
  mixer.gain(1, gWet * globalGain);
  mixer.gain(2, 0.0f);
  mixer.gain(3, 0.0f);
}

/*
  applyLowCut :
  - lowCut est un SVF, on récupère la sortie 2 (= highpass).
  - fréquence = lowCutHz, resonance faible pour éviter une bosse.
  - Effet recherché : réduire l'énergie des graves (voix "poitrine").
*/
static void applyLowCut(float hz) {
  hz = clampf(hz, 80.0f, 250.0f);
  lowCut.frequency(hz);
  lowCut.resonance(0.7f);
}

/*
  applyAir :
  - airSVF est un bandpass centré ~6.5 kHz.
  - wetMix ajoute cette bande en petite quantité (0.25*amt) pour éviter les sifflantes.
*/
static void applyAir(float amt) {
  amt = clampf(amt, 0.0f, 1.0f);

  airSVF.frequency(6500.0f);
  airSVF.resonance(0.9f);

  wetMix.gain(0, 1.0f);         // wet principal
  wetMix.gain(1, 0.25f * amt);  // air ajouté
  wetMix.gain(2, 0.0f);
  wetMix.gain(3, 0.0f);
}

/*
  applyFormant :
  - Les formants sont simulés par 3 bandpass (svf1/2/3).
  - On règle leurs fréquences centrales (f1,f2,f3) et leur Q (resonance).
  - On mélange :
      formantMixer.ch0 = signal direct (granular)
      formantMixer.ch1..3 = bandes formants
  - MODE_M2F : on monte les formants et on augmente la part des bandes.
  - MODE_F2M : on baisse les formants modérément pour ne pas rendre la voix boueuse.
*/
static void applyFormant(VoiceMode m, float amt) {
  amt = clampf(amt, 0.0f, 1.0f);

  float f1, f2, f3;
  float q1, q2, q3;

  if (m == MODE_M2F) {
    f1 = 900.0f  + 250.0f * amt;
    f2 = 1800.0f + 450.0f * amt;
    f3 = 3000.0f + 500.0f * amt;
    q1 = 2.0f + 0.8f * amt;
    q2 = 2.4f + 1.0f * amt;
    q3 = 2.6f + 1.0f * amt;
  } else if (m == MODE_F2M) {
    f1 = 850.0f  - 250.0f * amt;
    f2 = 1700.0f - 450.0f * amt;
    f3 = 2800.0f - 600.0f * amt;
    q1 = 1.8f + 0.5f * amt;
    q2 = 2.0f + 0.7f * amt;
    q3 = 2.2f + 0.7f * amt;
  } else {
    f1 = 900.0f; f2 = 1800.0f; f3 = 3000.0f;
    q1 = 2.0f;   q2 = 2.4f;    q3 = 2.6f;
  }

  // Bornes de sécurité : évite des réglages incohérents
  f1 = clampf(f1, 300.0f, 2000.0f);
  f2 = clampf(f2, 600.0f, 3500.0f);
  f3 = clampf(f3, 1200.0f, 5000.0f);

  svf1.frequency(f1); svf2.frequency(f2); svf3.frequency(f3);
  svf1.resonance(q1); svf2.resonance(q2); svf3.resonance(q3);

  if (m == MODE_M2F) {
    float direct = 1.0f - 0.40f * amt;
    formantMixer.gain(0, direct);
    formantMixer.gain(1, 0.35f * amt);
    formantMixer.gain(2, 0.50f * amt);
    formantMixer.gain(3, 0.45f * amt);
  } else if (m == MODE_F2M) {
    float direct = 1.0f - 0.20f * amt;
    formantMixer.gain(0, direct);
    formantMixer.gain(1, 0.18f * amt);
    formantMixer.gain(2, 0.22f * amt);
    formantMixer.gain(3, 0.18f * amt);
  } else {
    formantMixer.gain(0, 1.0f);
    formantMixer.gain(1, 0.0f);
    formantMixer.gain(2, 0.0f);
    formantMixer.gain(3, 0.0f);
  }
}

/*
  reconfigureGranular :
  - beginPitchShift(grainMs) initialise l'algorithme de pitch-shift (taille des grains)
  - setSpeed(speed) définit le ratio de lecture/recopie des grains :
      speed > 1 => pitch monte
      speed < 1 => pitch descend
*/
static void reconfigureGranular() {
  grainMs = clampf(grainMs, 20.0f, 200.0f);
  speed   = clampf(speed,   0.50f, 2.50f);

  granular.beginPitchShift(grainMs);
  granular.setSpeed(speed);
}

/*
  applyPreset :
  - Charge un ensemble cohérent de paramètres selon le mode.
  - Puis applique immédiatement toutes les configurations DSP.
  - En BYPASS, on ne modifie pas les blocs, on force juste le mix final en dry-only.
*/
static void applyPreset(VoiceMode m) {
  mode = m;

  if (mode == MODE_BYPASS) {
    bypass = true;
    applyDryWet(bypass, wet, outGain);
    return;
  }

  bypass = false;

  if (mode == MODE_M2F) {
    speed      = 1.22f;
    grainMs    = 70.0f;
    wet        = 0.90f;
    formantAmt = 0.50f;
    lowCutHz   = 120.0f;
    airAmt     = 0.20f;
  } else { // MODE_F2M
    speed      = 0.82f;
    grainMs    = 70.0f;
    wet        = 0.85f;
    formantAmt = 0.55f;
    lowCutHz   = 90.0f;
    airAmt     = 0.00f;
  }

  reconfigureGranular();
  applyLowCut(lowCutHz);
  applyFormant(mode, formantAmt);
  applyAir(airAmt);
  applyDryWet(bypass, wet, outGain);
}

/*
  applyModeSwitch :
  - Change de mode sans écraser wet/speed venant des potentiomètres.
  - On réapplique simplement la topologie DSP avec les valeurs actuelles.
*/
static void applyModeSwitch(VoiceMode m) {
  mode = m;
  bypass = false;
  reconfigureGranular();
  applyLowCut(lowCutHz);
  applyFormant(mode, formantAmt);
  applyAir(airAmt);
  applyDryWet(bypass, wet, outGain);
}

//
// ============================
// 8) Lecture des potentiomètres (lissage + anti-jitter)
// ============================
//
// Le lissage est un filtre IIR : y += alpha*(x - y).
// Ça évite que la conversion ADC fasse vibrer les paramètres audio.
//

static void updatePitchFromPot() {
  if (potTimer < POT_PERIOD_MS) return;
  potTimer = 0;

  int raw = analogRead(PITCH_POT_PIN);       // 0..4095
  potFiltered += potSmooth * ((float)raw - potFiltered);

  float x = potFiltered / 4095.0f;           // 0..1

  float minS, maxS;
  if (mode == MODE_M2F) { minS = potMinSpeed_M2F; maxS = potMaxSpeed_M2F; }
  else if (mode == MODE_F2M) { minS = potMinSpeed_F2M; maxS = potMaxSpeed_F2M; }
  else { minS = 1.0f; maxS = 1.0f; }

  float newSpeed = minS + x * (maxS - minS);
  newSpeed = clampf(newSpeed, 0.50f, 2.50f);

  // On n'update le granular que si le changement est significatif (sinon micro-jitter audible)
  if (fabsf(newSpeed - speed) > SPEED_EPS) {
    speed = newSpeed;
    granular.setSpeed(speed);
  }
}

static void updateWetFromPot() {
  if (wetPotTimer < WET_POT_PERIOD_MS) return;
  wetPotTimer = 0;

  int raw = analogRead(WET_POT_PIN);         // 0..4095
  wetPotFiltered += wetPotSmooth * ((float)raw - wetPotFiltered);

  float x = wetPotFiltered / 4095.0f;

  float minW, maxW;
  if (mode == MODE_M2F) { minW = potMinWet_M2F; maxW = potMaxWet_M2F; }
  else if (mode == MODE_F2M) { minW = potMinWet_F2M; maxW = potMaxWet_F2M; }
  else { minW = potMinWet_M2F; maxW = potMaxWet_M2F; }

  float newWet = minW + x * (maxW - minW);
  newWet = clampf(newWet, 0.0f, 1.0f);

  if (fabsf(newWet - wet) > WET_EPS) {
    wet = newWet;
    applyDryWet(bypass, wet, outGain);
  }
}

//
// ============================
// 9) Auto-mode : estimation F0 + votes + hystérésis
// ============================

static float estimateF0(float *probOut) {
  if (!noteFreqIn.available()) return -1.0f;

  float prob = noteFreqIn.probability();
  float f0 = noteFreqIn.read();

  if (probOut) *probOut = prob;

  // Filtrage des valeurs absurdes (bruit, erreurs YIN)
  if (f0 < 80.0f || f0 > 320.0f) return -1.0f;
  if (prob < AUTO_MIN_PROB) return -1.0f;

  return f0;
}

static void updateAutoMode() {
  if (!autoMode) return;
  if (autoTimer < AUTO_PERIOD_MS) return;
  autoTimer = 0;

  // On récupère le niveau pour éviter de voter sur du silence/bruit faible
  if (peakIn.available()) autoLastPeak = peakIn.read();
  if (rmsIn.available())  autoLastRms  = rmsIn.read();

  if (autoLastPeak < AUTO_MIN_PEAK && autoLastRms < AUTO_MIN_RMS) {
    autoMaleVotes = 0;
    autoFemaleVotes = 0;
    return;
  }

  float prob = 0.0f;
  float f0 = estimateF0(&prob);
  if (f0 <= 0.0f) return;

  autoLastF0 = f0;
  autoLastProb = prob;

  // Lissage de F0 : évite que le seuil saute à chaque syllabe
  if (autoF0Smooth <= 0.0f) autoF0Smooth = f0;
  autoF0Smooth += 0.20f * (f0 - autoF0Smooth);

  // Votes : plus robustes qu'un simple seuil instantané
  if (autoF0Smooth > AUTO_F0_HIGH) {
    if (autoFemaleVotes < 255) autoFemaleVotes++;
    if (autoMaleVotes > 0) autoMaleVotes--;
  } else if (autoF0Smooth < AUTO_F0_LOW) {
    if (autoMaleVotes < 255) autoMaleVotes++;
    if (autoFemaleVotes > 0) autoFemaleVotes--;
  } else {
    if (autoMaleVotes > 0) autoMaleVotes--;
    if (autoFemaleVotes > 0) autoFemaleVotes--;
  }

  // Bascule uniquement après plusieurs votes consécutifs
  if (mode == MODE_M2F && autoFemaleVotes >= AUTO_CONFIRM_VOTES) {
    applyModeSwitch(MODE_F2M);
    autoFemaleVotes = 0;
    autoMaleVotes = 0;
  } else if (mode == MODE_F2M && autoMaleVotes >= AUTO_CONFIRM_VOTES) {
    applyModeSwitch(MODE_M2F);
    autoMaleVotes = 0;
    autoFemaleVotes = 0;
  }
}

//
// ============================
// 10) Serial : menu + status
// ============================

static void printMenu() {
  Serial.println("\n=== COMMANDES ===");
  Serial.println("Modes: 0=BYPASS  1=M->F  2=F->M");
  Serial.println("Pitch: p/o (speed +/- 0.02)");
  Serial.println("Grain: g/f (grainMs +/- 5ms)");
  Serial.println("Mix  : w/d (wet +/- 0.05)");
  Serial.println("Formants: k/j (formantAmt +/- 0.05)");
  Serial.println("Air  : a/z (airAmt +/- 0.05)");
  Serial.println("LowCut: u/y (lowCutHz +/- 10Hz)");
  Serial.println("Gain global: + / - (outGain +/- 0.05)");
  Serial.println("Other: b=bypass  m=auto  r=reset preset  s=status");
}

static void printStatus() {
  Serial.println("\n--- STATUS ---");
  Serial.print("mode="); Serial.print((int)mode);
  Serial.print("  speed="); Serial.print(speed, 2);
  Serial.print("  grain="); Serial.print(grainMs, 1); Serial.print("ms");
  Serial.print("  wet="); Serial.print(wet, 2);
  Serial.print("  formantAmt="); Serial.print(formantAmt, 2);
  Serial.print("  air="); Serial.print(airAmt, 2);
  Serial.print("  lowCut="); Serial.print(lowCutHz, 0); Serial.print("Hz");
  Serial.print("  outGain="); Serial.print(outGain, 2);
  Serial.print("  bypass="); Serial.println(bypass ? "ON" : "OFF");

  Serial.print("autoMode="); Serial.print(autoMode ? "ON" : "OFF");
  Serial.print("  F0="); Serial.print(autoLastF0, 1); Serial.print("Hz");
  Serial.print("  prob="); Serial.print(autoLastProb, 2);
  Serial.print("  F0smoothed="); Serial.println(autoF0Smooth, 1);

  if (peakIn.available()) {
    float p = peakIn.read();
    Serial.print("PEAK(in)="); Serial.print(p, 3);
    if (p > 0.90f) Serial.print("  !!! CLIP RISK");
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

//
// ============================
// 11) setup / loop
// ============================

void setup() {
  Serial.begin(115200);
  delay(300);

  // Allocation des "audio blocks" (augmente si craquements)
  AudioMemory(70);

  // Initialisation du codec SGTL5000
  sgtl5000.enable();
  sgtl5000.inputSelect(AUDIO_INPUT_MIC);

  // Gain analogique micro (préampli du codec)
  sgtl5000.micGain(8);

  // Volume casque du codec (à garder dans une plage safe)
  sgtl5000.volume(0.85f);

  // Gain logiciel sur le flux micro
  preamp.gain(1.0f);

  // Buffer RAM obligatoire pour le granular
  granular.begin(granularMemory, GRANULAR_MEMORY_SIZE);

  // Initialisation DSP
  applyLowCut(lowCutHz);
  applyAir(airAmt);
  applyFormant(mode, formantAmt);
  reconfigureGranular();
  applyDryWet(bypass, wet, outGain);

  // Fenêtrage FFT (analyse) + initialisation YIN
  fftIn.windowFunction(AudioWindowHanning1024);
  noteFreqIn.begin(AUTO_YIN_THRESHOLD);

  // IO
  pinMode(PITCH_POT_PIN, INPUT);
  pinMode(WET_POT_PIN, INPUT);
  pinMode(MODE_BTN_PIN, INPUT_PULLUP);

  analogReadResolution(12);
  analogReadAveraging(8);

  potFiltered = (float)analogRead(PITCH_POT_PIN);
  wetPotFiltered = (float)analogRead(WET_POT_PIN);

  Serial.println("\n=== Voice Gender Switcher ===");
  printMenu();

  // Démarrage en M->F
  applyPreset(MODE_M2F);
  printStatus();
}

void loop() {
  updatePitchFromPot();
  updateWetFromPot();
  updateAutoMode();

  // Gestion bouton mode (détection front + debounce)
  bool btnNow = digitalRead(MODE_BTN_PIN); // HIGH=repos, LOW=appuyé
  if (btnNow != modeBtnPrev) {
    if (modeBtnTimer > MODE_BTN_DEBOUNCE_MS) {
      modeBtnTimer = 0;
      modeBtnPrev = btnNow;
      if (btnNow == LOW) {
        if (mode == MODE_M2F) applyPreset(MODE_F2M);
        else applyPreset(MODE_M2F);
      }
    }
  }

  // Status périodique
  static elapsedMillis t;
  if (t > 2000) {
    t = 0;
    if (peakIn.available() || rmsIn.available()) printStatus();
  }

  // Commandes série
  if (!Serial.available()) return;
  char c = Serial.read();

  switch (c) {
    case '0': applyPreset(MODE_BYPASS); break;
    case '1': applyPreset(MODE_M2F);    break;
    case '2': applyPreset(MODE_F2M);    break;

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
      applyFormant(mode, formantAmt);
      break;
    case 'j':
      formantAmt = clampf(formantAmt - 0.05f, 0.0f, 1.0f);
      applyFormant(mode, formantAmt);
      break;

    case 'a':
      airAmt = clampf(airAmt + 0.05f, 0.0f, 1.0f);
      applyAir(airAmt);
      break;
    case 'z':
      airAmt = clampf(airAmt - 0.05f, 0.0f, 1.0f);
      applyAir(airAmt);
      break;

    case 'u':
      lowCutHz = clampf(lowCutHz + 10.0f, 80.0f, 250.0f);
      applyLowCut(lowCutHz);
      break;
    case 'y':
      lowCutHz = clampf(lowCutHz - 10.0f, 80.0f, 250.0f);
      applyLowCut(lowCutHz);
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

    case 'm':
      autoMode = !autoMode;
      autoMaleVotes = 0;
      autoFemaleVotes = 0;
      break;

    case 'r':
      applyPreset(mode);
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