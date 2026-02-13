#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>

/*
  ============================================================
  TEENSY 4.0 - Voice Gender Switcher (M<->F) via clavier Serial
  ============================================================

  Objectif :
  - Permettre de basculer instantanément entre :
      0 = BYPASS (micro direct)
      1 = Male -> Female
      2 = Female -> Male

  Principe audio (pipeline) :
  MIC -> preamp -> LowCut(HPF) -> Granular PitchShift -> (Formants + Air) -> WetMix -> Dry/Wet -> OUT

  Pourquoi ces blocs ?
  - LowCut : enlève le “chest” (grave poitrine), utile surtout pour M->F
  - Granular : change le pitch (hauteur) sans changer la durée
  - Formants : modifie le timbre (spectre) pour éviter le rendu “chipmunk”
  - Air : ajoute brillance haut-aigu (féminin) mais à doser (sifflantes)
*/


// ============================================================
// 1) OBJETS AUDIO (modules DSP de la Teensy Audio Library)
// ============================================================

// Entrée micro (audio shield SGTL5000, via I2S)
AudioInputI2S        mic;

// Gain logiciel (pratique pour ajuster sans toucher aux gains analogiques)
AudioAmplifier       preamp;

// Filtre state-variable (SVF) utilisé ici en HIGH-PASS pour couper les basses
// Sorties SVF : 0=lowpass, 1=bandpass, 2=highpass
AudioFilterStateVariable lowCut;

// Effet granular utilisé en mode pitch-shift
AudioEffectGranular  granular;

// 3 filtres SVF en bandpass pour créer 3 “bosses” = formants artificiels
AudioFilterStateVariable svf1;
AudioFilterStateVariable svf2;
AudioFilterStateVariable svf3;

// Un SVF bandpass dédié à “l’air” (boost vers 6-7 kHz)
AudioFilterStateVariable airSVF;

// Mixer des formants :
// ch0 = signal direct (pitch-shifté)
// ch1/ch2/ch3 = 3 bandes bandpass (formants)
AudioMixer4          formantMixer;

// Mixer du wet final :
// ch0 = sortie formantMixer (wet principal)
// ch1 = air bandpass ajouté
AudioMixer4          wetMix;

// Mixer final dry/wet :
// ch0 = dry (micro / preamp)
// ch1 = wet (transformé)
AudioMixer4          mixer;

// Sortie casque/line out via I2S
AudioOutputI2S       out;

// Mesures (debug) : pic et RMS pour vérifier niveau et clipping
AudioAnalyzePeak     peakIn;
AudioAnalyzeRMS      rmsIn;

// Contrôleur de la puce audio shield (gain micro, volume, etc.)
AudioControlSGTL5000 sgtl5000;


// ============================================================
// 2) PATCH CORDS (câblage audio entre blocs)
// ============================================================

// mic -> preamp
AudioConnection c1(mic, 0, preamp, 0);

// preamp -> analyzers (uniquement pour affichage, n’altère pas le son)
AudioConnection c3(preamp, 0, peakIn, 0);
AudioConnection c4(preamp, 0, rmsIn, 0);

// preamp -> lowCut (entrée du filtre)
AudioConnection cLC_in(preamp, 0, lowCut, 0);

// lowCut HIGH-PASS (sortie 2) -> granular
AudioConnection cLC_toGran(lowCut, 2, granular, 0);

// granular -> direct vers formantMixer + vers filtres formants + vers filtre air
AudioConnection c5 (granular, 0, formantMixer, 0); // direct
AudioConnection c6 (granular, 0, svf1, 0);
AudioConnection c7 (granular, 0, svf2, 0);
AudioConnection c8 (granular, 0, svf3, 0);
AudioConnection cAirIn(granular, 0, airSVF, 0);

// formants bandpass (sortie 1) -> formantMixer (ch1..3)
AudioConnection c9 (svf1, 1, formantMixer, 1);
AudioConnection c10(svf2, 1, formantMixer, 2);
AudioConnection c11(svf3, 1, formantMixer, 3);

// formantMixer -> wetMix ch0
AudioConnection cWet0(formantMixer, 0, wetMix, 0);

// air bandpass -> wetMix ch1
AudioConnection cWetAir(airSVF, 1, wetMix, 1);

// wetMix -> mixer final (wet sur ch1)
AudioConnection cWetToMain(wetMix, 0, mixer, 1);

// dry (preamp) -> mixer final (dry sur ch0)
AudioConnection cDry(preamp, 0, mixer, 0);

// mixer -> out (copie mono vers L/R)
AudioConnection cOutL(mixer, 0, out, 0);
AudioConnection cOutR(mixer, 0, out, 1);


// ============================================================
// 3) MEMOIRE POUR LE GRANULAR (obligatoire)
// ============================================================

// Taille du buffer RAM pour le granular pitch shift.
// Plus grand = plus stable (souvent), mais consomme RAM.
#define GRANULAR_MEMORY_SIZE 12800
int16_t granularMemory[GRANULAR_MEMORY_SIZE];


// ============================================================
// 4) MODE DE TRANSFORMATION (presets)
// ============================================================

// 0 = bypass, 1 = male->female, 2 = female->male
enum VoiceMode { MODE_BYPASS=0, MODE_M2F=1, MODE_F2M=2 };
VoiceMode mode = MODE_M2F;   // mode actif au démarrage


// ============================================================
// 5) PARAMETRES REGLABLES (en live via Serial)
// ============================================================

// --- Pitch shifter ---
float speed   = 1.24f;   // ratio de pitch : 1.0 = neutre, >1 = plus aigu, <1 = plus grave
float grainMs = 60.0f;   // taille des grains (ms) : petit = plus nerveux/artefacts, grand = plus lisse/latence

// --- Mix ---
float wet     = 0.85f;   // quantité de signal transformé (0=dry only, 1=wet only)
bool  bypass  = false;   // si true : dry=1 wet=0 (micro direct)
float outGain = 1.0f;    // gain global appliqué aux deux voies dry et wet

// --- Timbre ---
float formantAmt = 0.55f; // intensité de l’effet “formants” (0..1)
float lowCutHz   = 140.0f;// fréquence de coupure du high-pass (enlève grave “poitrine”)
float airAmt     = 0.20f; // quantité d’“air” (boost haut-aigu), à doser pour éviter sifflantes


// ============================================================
// 6) OUTILS (sécurité / configuration)
// ============================================================

// clamp : borne une valeur (évite réglages dangereux/absurdes)
static float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

/*
  applyDryWet :
  - met à jour les gains du mixer final
  - ch0 = dry, ch1 = wet
  - applique aussi outGain
*/
static void applyDryWet(bool bypassMode, float wetAmt, float globalGain) {
  wetAmt     = clampf(wetAmt, 0.0f, 1.0f);
  globalGain = clampf(globalGain, 0.0f, 2.0f);

  float gDry, gWet;
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
  - configure le SVF lowCut en mode high-pass (sortie 2)
  - coupe les graves pour enlever “chest”
*/
static void applyLowCut(float hz) {
  hz = clampf(hz, 80.0f, 250.0f);
  lowCut.frequency(hz);
  lowCut.resonance(0.7f); // doux, évite une bosse agressive
}

/*
  applyAir :
  - configure airSVF en bandpass autour de 6.5 kHz
  - ajoute cette bande au wet via wetMix ch1
  - gain limité (0.25*amt) pour éviter sifflantes
*/
static void applyAir(float amt) {
  amt = clampf(amt, 0.0f, 1.0f);

  airSVF.frequency(6500.0f);
  airSVF.resonance(0.9f);

  wetMix.gain(0, 1.0f);         // wet principal
  wetMix.gain(1, 0.25f * amt);  // air ajouté (limité)
  wetMix.gain(2, 0.0f);
  wetMix.gain(3, 0.0f);
}

/*
  applyFormant :
  - met à jour les fréquences centrales des 3 formants (svf1/2/3)
  - règle leurs Q (résonance)
  - règle le mix entre direct et bandes
  - Diffère selon le mode :
      MODE_M2F : formants montent
      MODE_F2M : formants descendent (modéré)
*/
static void applyFormant(VoiceMode m, float amt) {
  amt = clampf(amt, 0.0f, 1.0f);

  float f1, f2, f3;
  float q1, q2, q3;

  if (m == MODE_M2F) {
    // M -> F : on monte les formants
    f1 = 900.0f  + 250.0f * amt;   // 900 -> 1250
    f2 = 1800.0f + 450.0f * amt;   // 1800 -> 2450
    f3 = 3000.0f + 500.0f * amt;   // 3000 -> 3700
    q1 = 2.0f + 0.8f * amt;
    q2 = 2.4f + 1.0f * amt;
    q3 = 2.6f + 1.0f * amt;
  } else if (m == MODE_F2M) {
    // F -> M : on descend les formants (attention : trop bas = voix “boueuse”)
    f1 = 850.0f  - 250.0f * amt;   // 850 -> 600
    f2 = 1700.0f - 450.0f * amt;   // 1700 -> 1250
    f3 = 2800.0f - 600.0f * amt;   // 2800 -> 2200
    q1 = 1.8f + 0.5f * amt;
    q2 = 2.0f + 0.7f * amt;
    q3 = 2.2f + 0.7f * amt;
  } else {
    // BYPASS : neutre
    f1 = 900.0f; f2 = 1800.0f; f3 = 3000.0f;
    q1 = 2.0f;   q2 = 2.4f;    q3 = 2.6f;
  }

  // Sécurité : on borne les fréquences
  f1 = clampf(f1, 300.0f, 2000.0f);
  f2 = clampf(f2, 600.0f, 3500.0f);
  f3 = clampf(f3, 1200.0f, 5000.0f);

  // Appliquer fréquences et résonances
  svf1.frequency(f1); svf2.frequency(f2); svf3.frequency(f3);
  svf1.resonance(q1); svf2.resonance(q2); svf3.resonance(q3);

  // Mix : direct vs bandes
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
  - configure le pitch shift avec grainMs
  - applique speed
  - à appeler quand on change grainMs ou en preset
*/
static void reconfigureGranular() {
  grainMs = clampf(grainMs, 20.0f, 200.0f);
  speed   = clampf(speed,   0.50f, 2.50f);

  granular.beginPitchShift(grainMs);
  granular.setSpeed(speed);
}

/*
  applyPreset :
  - applique un pack de paramètres cohérents selon mode (M2F / F2M / BYPASS)
  - puis reconfigure tous les blocs (granular, filtres, mixers)
*/
static void applyPreset(VoiceMode m) {
  mode = m;

  // Bypass = micro direct
  if (mode == MODE_BYPASS) {
    bypass = true;
    applyDryWet(bypass, wet, outGain);
    return;
  }

  bypass = false;

  // Paramètres de base pour chaque transformation
  if (mode == MODE_M2F) {
    speed      = 1.22f;  // pitch up
    grainMs    = 70.0f;
    wet        = 0.90f;
    formantAmt = 0.50f;  // formants up
    lowCutHz   = 120.0f; // coupe le grave “poitrine”
    airAmt     = 0.20f;  // un peu d’air
  } else { // MODE_F2M
    speed      = 0.82f;  // pitch down modéré (évite “monstre”)
    grainMs    = 70.0f;
    wet        = 0.85f;
    formantAmt = 0.55f;  // intensité de descente des formants
    lowCutHz   = 90.0f;  // garder le bas
    airAmt     = 0.00f;  // pas d’air
  }

  // Ré-appliquer tous les réglages DSP
  reconfigureGranular();
  applyLowCut(lowCutHz);
  applyFormant(mode, formantAmt);
  applyAir(airAmt);
  applyDryWet(bypass, wet, outGain);
}


// ============================================================
// 7) AFFICHAGE SERIAL (menu + status)
// ============================================================

static void printMenu() {
  Serial.println("\n=== COMMANDES ===");
  Serial.println("Modes:");
  Serial.println("  0 : BYPASS (micro direct)");
  Serial.println("  1 : Male -> Female");
  Serial.println("  2 : Female -> Male");
  Serial.println("Pitch:");
  Serial.println("  p / o : speed +/- 0.02");
  Serial.println("Grain:");
  Serial.println("  g / f : grainMs +/- 5ms");
  Serial.println("Mix:");
  Serial.println("  w / d : wet +/- 0.05");
  Serial.println("Formants:");
  Serial.println("  k / j : formantAmt +/- 0.05");
  Serial.println("Air:");
  Serial.println("  a / z : airAmt +/- 0.05");
  Serial.println("LowCut:");
  Serial.println("  u / y : lowCutHz +/- 10Hz");
  Serial.println("Gain global:");
  Serial.println("  + / - : outGain +/- 0.05");
  Serial.println("Other:");
  Serial.println("  b : bypass on/off");
  Serial.println("  s : status");
  Serial.println("  r : reset preset courant");
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

  // Affiche peak/RMS si disponibles (la lib met à jour périodiquement)
  if (peakIn.available()) {
    float p = peakIn.read();
    Serial.print("PEAK(in)="); Serial.print(p, 3);
    if (p > 0.90f) Serial.print("  !!! CLIP RISK");
    Serial.println();
  }
  if (rmsIn.available()) {
    Serial.print("RMS(in)="); Serial.println(rmsIn.read(), 4);
  }

  // CPU et mémoire audio (debug performance)
  Serial.print("CPU="); Serial.print(AudioProcessorUsage(), 1);
  Serial.print("%  CPUmax="); Serial.print(AudioProcessorUsageMax(), 1);
  Serial.print("%  Mem="); Serial.print(AudioMemoryUsage());
  Serial.print("/"); Serial.println(AudioMemoryUsageMax());
}


// ============================================================
// 8) SETUP / LOOP (comportement principal)
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  // Réserve des blocs audio (si crackles -> augmenter)
  AudioMemory(70);

  // Active l’audio shield et sélectionne l’entrée micro
  sgtl5000.enable();
  sgtl5000.inputSelect(AUDIO_INPUT_MIC);

  // Gains hardware : à ajuster selon micro + distance
  sgtl5000.micGain(8);      // 6..15 typique
  sgtl5000.volume(0.65);    // casque

  // Gain logiciel (laisser à 1 au début)
  preamp.gain(1.0f);

  // Donne la RAM au granular (obligatoire)
  granular.begin(granularMemory, GRANULAR_MEMORY_SIZE);

  // Init “safe” : configure tous les blocs une première fois
  applyLowCut(lowCutHz);
  applyAir(airAmt);
  applyFormant(mode, formantAmt);
  reconfigureGranular();
  applyDryWet(bypass, wet, outGain);

  Serial.println("\n=== Voice Gender Switcher (0/1/2) ===");
  printMenu();

  // Démarrage en Male->Female par défaut
  applyPreset(MODE_M2F);
  printStatus();
}

void loop() {
  // Affiche l’état toutes les ~2s (utile pour voir CPU/clip)
  static elapsedMillis t;
  if (t > 2000) {
    t = 0;
    if (peakIn.available() || rmsIn.available()) printStatus();
  }

  // Pas de caractère reçu -> rien à faire
  if (!Serial.available()) return;

  // Lecture d’un caractère (commande)
  char c = Serial.read();

  switch (c) {

    // ------------------ Modes ------------------
    case '0': applyPreset(MODE_BYPASS); break;
    case '1': applyPreset(MODE_M2F);    break;
    case '2': applyPreset(MODE_F2M);    break;

    // ------------------ Pitch (speed) ------------------
    case 'p': // pitch up
      speed = clampf(speed + 0.02f, 0.50f, 2.50f);
      granular.setSpeed(speed);
      break;
    case 'o': // pitch down
      speed = clampf(speed - 0.02f, 0.50f, 2.50f);
      granular.setSpeed(speed);
      break;

    // ------------------ Grain size ------------------
    // Changer grainMs nécessite de reconfigurer le pitch shift
    case 'g':
      grainMs = clampf(grainMs + 5.0f, 20.0f, 200.0f);
      reconfigureGranular();
      break;
    case 'f':
      grainMs = clampf(grainMs - 5.0f, 20.0f, 200.0f);
      reconfigureGranular();
      break;

    // ------------------ Dry/Wet ------------------
    case 'w':
      wet = clampf(wet + 0.05f, 0.0f, 1.0f);
      applyDryWet(bypass, wet, outGain);
      break;
    case 'd':
      wet = clampf(wet - 0.05f, 0.0f, 1.0f);
      applyDryWet(bypass, wet, outGain);
      break;

    // ------------------ Formants ------------------
    case 'k':
      formantAmt = clampf(formantAmt + 0.05f, 0.0f, 1.0f);
      applyFormant(mode, formantAmt);
      break;
    case 'j':
      formantAmt = clampf(formantAmt - 0.05f, 0.0f, 1.0f);
      applyFormant(mode, formantAmt);
      break;

    // ------------------ Air ------------------
    case 'a':
      airAmt = clampf(airAmt + 0.05f, 0.0f, 1.0f);
      applyAir(airAmt);
      break;
    case 'z':
      airAmt = clampf(airAmt - 0.05f, 0.0f, 1.0f);
      applyAir(airAmt);
      break;

    // ------------------ LowCut ------------------
    case 'u':
      lowCutHz = clampf(lowCutHz + 10.0f, 80.0f, 250.0f);
      applyLowCut(lowCutHz);
      break;
    case 'y':
      lowCutHz = clampf(lowCutHz - 10.0f, 80.0f, 250.0f);
      applyLowCut(lowCutHz);
      break;

    // ------------------ Gain global ------------------
    case '+':
      outGain = clampf(outGain + 0.05f, 0.0f, 1.5f);
      applyDryWet(bypass, wet, outGain);
      break;
    case '-':
      outGain = clampf(outGain - 0.05f, 0.0f, 1.5f);
      applyDryWet(bypass, wet, outGain);
      break;

    // ------------------ Bypass toggle ------------------
    case 'b':
      bypass = !bypass;
      applyDryWet(bypass, wet, outGain);
      break;

    // ------------------ Reset preset courant ------------------
    case 'r':
      applyPreset(mode);
      break;

    // ------------------ Status ------------------
    case 's':
      printStatus();
      return;

    // ------------------ Aide ------------------
    default:
      printMenu();
      return;
  }

  // Après chaque commande, on affiche l’état pour feedback immédiat
  printStatus();
}
