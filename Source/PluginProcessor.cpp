#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DSP/NoteSelector.h"

CanaryVoiceTuneAudioProcessor::CanaryVoiceTuneAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(
          BusesProperties()
#if !JucePlugin_IsMidiEffect
#if !JucePlugin_IsSynth
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
              ),
      apvts(*this, nullptr, "Parameters", createParameterLayout())
#endif
{
  attackParam    = apvts.getRawParameterValue("ATTACK");
  releaseParam   = apvts.getRawParameterValue("RELEASE");
  vibratoParam   = apvts.getRawParameterValue("VIBRATO");
  exciterParam   = apvts.getRawParameterValue("EXCITER");
  sibilantsParam = apvts.getRawParameterValue("SIBILANTS");
  breathParam    = apvts.getRawParameterValue("BREATH");
  popParam       = apvts.getRawParameterValue("POP");
  tuningModeParam = apvts.getRawParameterValue("TUNING_MODE");
  for (int i = 0; i < 88; ++i)
    keyParams[i] = apvts.getRawParameterValue("KEY_" + juce::String(i));
}

CanaryVoiceTuneAudioProcessor::~CanaryVoiceTuneAudioProcessor() {}

int CanaryVoiceTuneAudioProcessor::getNumPrograms() {
  return 26;
}

int CanaryVoiceTuneAudioProcessor::getCurrentProgram() {
  return currentProgram;
}

const juce::String CanaryVoiceTuneAudioProcessor::getProgramName(int index) {
  if (index == 0) return "Default (Chromatic)";
  if (index == 1) return "Disable all";

  int rootIdx = ((index - 2) / 2) % 12;
  bool isMinor = ((index - 2) % 2 == 1);

  static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
  juce::String name = noteNames[rootIdx];
  name += isMinor ? " Minor" : " Major";
  return name;
}

void CanaryVoiceTuneAudioProcessor::changeProgramName(int index, const juce::String &newName) {
  juce::ignoreUnused(index, newName);
}

bool CanaryVoiceTuneAudioProcessor::isNoteEnabledForPreset(int presetIdx, int midiNote) const {
  if (presetIdx <= 0) return true;  // Chromatic
  if (presetIdx == 1) return false; // Disable all

  int rootIdx = ((presetIdx - 2) / 2) % 12;
  bool isMinor = ((presetIdx - 2) % 2 == 1);
  
  int pitchClass = midiNote % 12;
  int interval = (pitchClass - rootIdx + 12) % 12;
  
  if (isMinor) {
    // Natural Minor: 0, 2, 3, 5, 7, 8, 10
    return (interval == 0 || interval == 2 || interval == 3 || interval == 5 || interval == 7 || interval == 8 || interval == 10);
  } else {
    // Major: 0, 2, 4, 5, 7, 9, 11
    return (interval == 0 || interval == 2 || interval == 4 || interval == 5 || interval == 7 || interval == 9 || interval == 11);
  }
}

void CanaryVoiceTuneAudioProcessor::setCurrentProgram(int index) {
  if (index < 0 || index >= getNumPrograms()) return;
  currentProgram = index;
  
  auto setParam = [this](const juce::String& paramId, float plainValue) {
      if (auto* param = apvts.getParameter(paramId)) {
          float normalized = param->convertTo0to1(plainValue);
          param->setValueNotifyingHost(normalized);
      }
  };

  if (index == 0) {
      // Default (Chromatic)
      setParam("ATTACK", 100.0f);
      setParam("RELEASE", 250.0f);
      setParam("VIBRATO", 1.0f);
      setParam("EXCITER", 0.0f);
      setParam("SIBILANTS", 0.0f);
      setParam("BREATH", 0.0f);
      setParam("POP", 0.0f);
      for (int i = 0; i < 88; ++i)
          setParam("KEY_" + juce::String(i), 1.0f);
  } else if (index == 1) {
      // Disable all — bypass tuning entirely (no active target notes)
      setParam("ATTACK", 100.0f);
      setParam("RELEASE", 250.0f);
      setParam("VIBRATO", 1.0f);
      setParam("EXCITER", 0.0f);
      setParam("SIBILANTS", 0.0f);
      setParam("BREATH", 0.0f);
      setParam("POP", 0.0f);
      for (int i = 0; i < 88; ++i)
          setParam("KEY_" + juce::String(i), 0.0f);
  } else {
      // Harmonic Presets (C Major, C Minor, etc.)
      setParam("ATTACK", 20.0f);
      setParam("RELEASE", 150.0f);
      setParam("VIBRATO", 0.8f);
      setParam("EXCITER", 0.75f);
      setParam("SIBILANTS", 0.0f);
      setParam("BREATH", -35.0f);
      setParam("POP", -12.0f);
      for (int i = 0; i < 88; ++i) {
          bool enabled = isNoteEnabledForPreset(index, i + 21);
          setParam("KEY_" + juce::String(i), enabled ? 1.0f : 0.0f);
      }
  }
}

juce::AudioProcessorValueTreeState::ParameterLayout
CanaryVoiceTuneAudioProcessor::createParameterLayout() {
  std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"ATTACK", 1}, "Attack", 0.1f, 150.0f, 100.0f));
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"RELEASE", 1}, "Release", 10.0f, 500.0f, 250.0f));
  // (Range control removed — note matching is always maximal.)
  // Vibrato: maximum pitch deviation (in semitones) allowed around the
  // smoothed centre. 0 = perfectly flat, 1 = up to ±1 semitone of wobble.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"VIBRATO", 2}, "Vibrato", 0.0f, 1.0f, 1.0f));
  // Sibilants: high-shelf gain around 7 kHz for "s/sh/t" presence.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"SIBILANTS", 1}, "Sibilants", -12.0f, 12.0f, 0.0f));
  // Exciter: harmonic enhancer level (0.0 dB to 12.0 dB)
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"EXCITER", 1}, "Exciter", 0.0f, 6.0f, 0.0f));
  // Breath Gate: detector threshold; 0 dB disables it.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"BREATH", 1}, "Breath", -48.0f, 0.0f, 0.0f));



  // Pop Filter: detector threshold; 0 dB disables it.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"POP", 1}, "Pop Filter", -36.0f, 0.0f, 0.0f));

  // Tuning Mode: Choice between Modern (Transparent) and Classic (Hard-Tune / Low-Latency)
  params.push_back(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"TUNING_MODE", 1}, "Tuning Mode",
      juce::StringArray{"Modern (Transparent)", "Classic (Low-Latency)"}, 0));

  // Keys 0-87 for A0 to C8. true = note is in the active scale.
  for (int i = 0; i < 88; ++i) {
    juce::String idText = "KEY_" + juce::String(i);
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{idText, 1}, idText, true));
  }

  return { params.begin(), params.end() };
}

void CanaryVoiceTuneAudioProcessor::prepareToPlay(double sampleRate,
                                                  int samplesPerBlock) {
  pitchDetector.prepare(sampleRate, samplesPerBlock);
  pitchShifter.prepare(sampleRate, samplesPerBlock);
  int lat = pitchShifter.getLatencySamples();
  setLatencySamples(lat);
  currentLatencySamples.store(lat, std::memory_order_release);
  audioSampleClock.store(0, std::memory_order_release);
}

void CanaryVoiceTuneAudioProcessor::releaseResources() {}

bool CanaryVoiceTuneAudioProcessor::isBusesLayoutSupported(
    const BusesLayout &layouts) const {
#if JucePlugin_IsMidiEffect
  juce::ignoreUnused(layouts);
  return true;
#else
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;

#if !JucePlugin_IsSynth
  if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
    return false;
#endif

  return true;
#endif
}

// =============================================================================
// processBlock helpers
// =============================================================================

void CanaryVoiceTuneAudioProcessor::buildMonoMix(
    const juce::AudioBuffer<float>& buffer) {
  int n = buffer.getNumSamples();
  monoMix.resize((size_t)n);
  if (buffer.getNumChannels() == 1) {
    std::copy_n(buffer.getReadPointer(0), n, monoMix.data());
    return;
  }
  const float* l = buffer.getReadPointer(0);
  const float* r = buffer.getReadPointer(1);
  // Update slow EMA of per-channel energy (time constant ~200 ms at 44100/256).
  // Using a slow average instead of a per-block winner prevents the detector
  // from seeing a different-phase channel every block when stereo material has
  // similar energy on L and R (Haas, wideners) — that was causing the
  // slowAnchor to see phase-shifted pitch estimates and fire the octave guard.
  float blockEL = 0.0f, blockER = 0.0f;
  for (int i = 0; i < n; ++i) { blockEL += l[i]*l[i]; blockER += r[i]*r[i]; }
  const float emaAlpha = 0.05f;
  channelEnergyL = channelEnergyL * (1.0f - emaAlpha) + blockEL * emaAlpha;
  channelEnergyR = channelEnergyR * (1.0f - emaAlpha) + blockER * emaAlpha;
  std::copy_n((channelEnergyL >= channelEnergyR) ? l : r, n, monoMix.data());
}

void CanaryVoiceTuneAudioProcessor::resetVoicingState() {
  releaseMidi            = -1;
  attackSamples          = 0;
  noteHeldSamples        = 0;
  smoothedMidi           = -1.0f;
  smoothedTargetMidi     = -1.0f;
  voicedSampleCount      = 0;
  candidateMidi          = -1;
  candidateStableSamples = 0;
}

void CanaryVoiceTuneAudioProcessor::resetNoteLockState() {
  // Partial reset used on consonants and voiced onsets: clears the note-lock
  // state machine. smoothedMidi is reset to the current detected pitch so that
  // the note selector sees the new note immediately instead of lagging behind
  // the previous note's smoothed value across a legato/portamento transition.
  releaseMidi            = -1;
  attackSamples          = 0;
  noteHeldSamples        = 0;
  smoothedMidi           = -1.0f;   // will be seeded from detectedHz on next voiced block
  smoothedTargetMidi     = -1.0f;
  voicedSampleCount      = 0;
  candidateMidi          = -1;
  candidateStableSamples = 0;
}

int CanaryVoiceTuneAudioProcessor::chooseTargetNoteAndRatio(
    float detectedHz, const bool* activeKeys, float blockSize, float sr,
    float attackMs, float releaseMs, float vibratoAmount,
    float& outRatio) {
  juce::ignoreUnused(releaseMs);
  voicedSampleCount += (int)blockSize;

  float floatMidi = 69.0f + 12.0f * std::log2(detectedHz / 440.0f);

  // ---- Vibrato shaping (adaptive lowpass on the pitch in semitone domain)
  // The smoothed centre tracks the slow pitch trend; the raw deviation around
  // that centre is then clamped to ±vibratoAmount semitones. Wide-amplitude
  // jumps (real note changes) bypass the smoothing so the tracker doesn't lag
  // a ratio behind on melodic transitions.
  float blockDt = blockSize / sr;
  // 120 ms is fast enough to follow legato semitone steps between adjacent
  // notes while still averaging out typical 5 Hz vocal vibrato (period ~200 ms).
  float vibTimeMs = 120.0f;
  if (smoothedMidi < 0.0f) {
    smoothedMidi = floatMidi;
  } else {
    // The jump bypass should engage only for real note changes — i.e. a
    // pitch excursion clearly larger than the allowed vibrato. Otherwise
    // a wide vibrato would repeatedly trigger the bypass and the smoother
    // would chase the wobble instead of averaging it out.
    float delta = std::abs(floatMidi - smoothedMidi);
    float jumpLo = vibratoAmount + 0.5f;
    float jumpHi = vibratoAmount + 1.2f;
    float jumpFactor;
    if (delta < jumpLo)      jumpFactor = 0.0f;
    else if (delta > jumpHi) jumpFactor = 1.0f;
    else                     jumpFactor = (delta - jumpLo) / (jumpHi - jumpLo);

    float effectiveTimeMs = vibTimeMs * (1.0f - jumpFactor) + 1.0f * jumpFactor;
    float adaptiveAlpha = 1.0f - std::exp(-blockDt / (effectiveTimeMs / 1000.0f));
    smoothedMidi += adaptiveAlpha * (floatMidi - smoothedMidi);
  }
  float deviation     = floatMidi - smoothedMidi;
  float clampedDev    = juce::jlimit(-vibratoAmount, vibratoAmount, deviation);
  float effectiveMidi = smoothedMidi + clampedDev;

  // ---- Pick the active key closest to the smoothed pitch ------------------
  // Hysteresis around the currently-locked note: the user's Range knob
  // controls how aggressively we want to stick to the chosen key. With
  // strong correction we don't want the lock flipping between two adjacent
  // active keys when `smoothedMidi` sits right on the boundary; with no
  // correction the hysteresis is minimal so the displayed note still
  // follows the singer closely.
  float lockPitch    = (smoothedMidi > 0.0f) ? smoothedMidi : effectiveMidi;
  // Note-selection hysteresis is a small FIXED stick-band, independent of the
  // Range knob. It used to scale with correctionStrength (0.1 + 0.5*strength),
  // which at Range=100 produced 0.6 semitones — larger than the 0.5-semitone
  // half-distance between two adjacent (e.g. chromatic) notes. That made the
  // incumbent stick past its own boundary and the displayed note jump/flicker.
  // Range now controls only the correction strength (the pull-to-note amount),
  // not how the note is chosen. The fixed value matches the old Range=0 case
  // (0.1 + 0.5*0 = 0.1), so behaviour at Range 0 is unchanged.
  const float hysteresisSt = 0.1f;
  // Pick the enabled key closest in pitch to the detected note. The selection
  // is symmetric — it never prefers the higher or lower neighbour — so when
  // several disabled keys separate two enabled ones, the switch happens at the
  // true midpoint between them (e.g. with only C and E enabled, C#→C, D#→E).
  // The incumbent gets a symmetric stick-band of `hysteresisSt` semitones at
  // that midpoint to suppress flicker from pitch jitter.
  int bestMidi = NoteSelector::chooseActiveNote(activeKeys, lockPitch,
                                                releaseMidi, hysteresisSt);
  if (bestMidi < 0) bestMidi = (int)std::round(lockPitch);

  // ---- Note capture & Release inertia ------------------------------------
  // First capture: latch the first stable bestMidi after a 50 ms grace.
  // Note switch: allowed only after the current note has been held for at
  //   least releaseMs.  Within that window the current note is "sticky" —
  //   the singer must hold the new note for attackMs before we actually
  //   switch.  This decouples Attack (how fast we glide onto a new note)
  //   from Release (how long the current note holds before we even start
  //   considering a switch).
  const int graceSamples        = (int)(sr * 0.050f);
  const int attackStableSamples = (int)(sr * juce::jmax(attackMs, 1.0f) / 1000.0f);
  // Decoupled responsive note-switching refractory period (fixed 30 ms) to allow
  // rapid and precise melodic transitions, independent of vocal effects releaseMs.
  const int releaseHoldSamples  = (int)(sr * 0.030f);

  if (voicedSampleCount >= graceSamples && releaseMidi < 0) {
    releaseMidi            = bestMidi;
    attackSamples          = 0;
    noteHeldSamples        = 0;
    candidateMidi          = -1;
    candidateStableSamples = 0;
  }
  if (releaseMidi >= 0) {
    attackSamples   += (int)blockSize;
    noteHeldSamples += (int)blockSize;

    if (bestMidi != releaseMidi) {
      // Only propose a switch once the current note has been held for at
      // least releaseMs — before that we ignore the incoming bestMidi.
      if (noteHeldSamples >= releaseHoldSamples) {
        if (bestMidi == candidateMidi) {
          candidateStableSamples += (int)blockSize;
        } else {
          candidateMidi          = bestMidi;
          candidateStableSamples = (int)blockSize;
        }
        if (candidateStableSamples >= attackStableSamples) {
          releaseMidi            = candidateMidi;
          attackSamples          = 0;
          noteHeldSamples        = 0;
          candidateMidi          = -1;
          candidateStableSamples = 0;
        }
      }
    } else {
      // Singer is back on the held note — drop any pending candidate.
      candidateMidi          = -1;
      candidateStableSamples = 0;
    }
  }

  // ---- Attack fade-in -----------------------------------------------------
  // While attacking, blend toward dry so the Attack curve is audible.
  float engageFade = juce::jlimit(0.0f, 1.0f,
                                  1000.0f * (float)attackSamples / sr / attackMs);
  float lockBypass = 1.0f - engageFade;
  // Incumbent stickiness is now handled symmetrically and spacing-aware inside
  // NoteSelector::chooseActiveNote (above), so the previous fixed "snap to the
  // lock within ±1 semitone" override is removed: it ignored where the nearest
  // *enabled* neighbour actually sits and could keep the lock even when a
  // closer enabled key existed, biasing the choice. `bestMidi` already is the
  // closest enabled key with hysteresis applied.

  // ---- Compute the ratio --------------------------------------------------
  // Target the note that survived the capture/stability state machine. Using
  // raw bestMidi here bypasses the candidate hold above and makes the tuner
  // chase one-block selector jitter ("quack") even though releaseMidi is still
  // latched. The display follows the same held note so audio and UI agree.
  int selectedMidi = (releaseMidi >= 0) ? releaseMidi : bestMidi;
  float rawTargetMidi  = selectedMidi + clampedDev;
  rawTargetMidi        = rawTargetMidi * (1.0f - lockBypass)
                       + effectiveMidi * lockBypass;

  // Short portamento smooths the ±1 semitone step that occurs when bestMidi
  // crosses a bucket boundary.  15 ms is fast enough to settle within a 40 ms
  // analysis window (e.g. a 120 ms note with 80 ms onset-skip) while still
  // being inaudible as a click on note transitions.
  float blockDtMs       = 1000.0f * blockSize / sr;
  float portamentoTimeMs = 15.0f;
  float targetAlpha = 1.0f - std::exp(-blockDtMs / portamentoTimeMs);
  if (smoothedTargetMidi < 0.0f) smoothedTargetMidi = rawTargetMidi;
  else                            smoothedTargetMidi += targetAlpha
                                      * (rawTargetMidi - smoothedTargetMidi);

  float targetHz = 440.0f * std::pow(2.0f, (smoothedTargetMidi - 69.0f) / 12.0f);
  // The cancellation denominator must reflect the live (per-block) pitch,
  // not a multi-block-smoothed estimate, otherwise the shifter applies a
  // stale ratio and the singer's wobble survives to the output. Caller hands
  // us the raw per-block estimate as `detectedHz`.
  outRatio = juce::jlimit(0.5f, 2.0f, targetHz / detectedHz);
  return selectedMidi;
}

void CanaryVoiceTuneAudioProcessor::pushNoteEvent(int noteIndex,
                                                  int extraDelaySamples) {
  if (noteIndex == lastPushedNote) return;
  lastPushedNote = noteIndex;
  int w = noteHistoryWriteIdx.load(std::memory_order_relaxed);
  noteHistoryNote[w].store(noteIndex, std::memory_order_relaxed);
  // Compute the audio-clock at which this event becomes audible. For a
  // note-on (or note-change) that's pushClock + shifter latency. For a
  // note-off, the caller adds `releaseMs` worth of samples so the highlight
  // stays lit while the shifter completes its release fade.
  int64_t pushClock = audioSampleClock.load(std::memory_order_relaxed);
  int64_t lat       = (int64_t)currentLatencySamples.load(std::memory_order_relaxed);
  int64_t visibleAt = pushClock + lat + (int64_t)extraDelaySamples;
  noteHistoryVisibleAt[w].store(visibleAt, std::memory_order_release);
  noteHistoryWriteIdx.store((w + 1) % kNoteHistorySize,
                            std::memory_order_release);
}

int CanaryVoiceTuneAudioProcessor::popLatestNoteEvent() {
  int w = noteHistoryWriteIdx.load(std::memory_order_acquire);
  int r = noteHistoryReadIdx.load(std::memory_order_relaxed);
  if (r == w) return -2; // ring empty
  // Each event carries the audio-clock sample at which it becomes audible.
  // Drain all entries that are due, returning the most recent of them.
  int64_t now = audioSampleClock.load(std::memory_order_acquire);
  int latest = -2;
  int newR = r;
  while (newR != w) {
    int64_t visibleAt = noteHistoryVisibleAt[newR].load(std::memory_order_acquire);
    if (now < visibleAt) break;   // not yet audible; keep for later
    latest = noteHistoryNote[newR].load(std::memory_order_relaxed);
    newR = (newR + 1) % kNoteHistorySize;
  }
  noteHistoryReadIdx.store(newR, std::memory_order_release);
  return latest;
}

void CanaryVoiceTuneAudioProcessor::renderPreviewTone(
    juce::AudioBuffer<float>& buffer) {
  int samplesRem = previewSamplesRemaining.load();
  if (samplesRem <= 0) return;

  float freq        = previewFrequencyHz.load();
  float sr          = (float)getSampleRate();
  float phaseDelta  = freq * juce::MathConstants<float>::twoPi / sr;
  int   total       = (int)(sr * 0.5f);
  int   numSamples  = buffer.getNumSamples();
  int   numChannels = buffer.getNumChannels();

  for (int i = 0; i < numSamples && samplesRem > 0; ++i) {
    // Linear fade-in / fade-out at the ends to avoid clicks (1000 samples
    // each = ~22 ms at 44.1 kHz).
    float env = 1.0f;
    if (samplesRem < 1000)              env  =        (float)samplesRem  / 1000.0f;
    int played = total - samplesRem;
    if (played < 1000)                  env *=        (float)played      / 1000.0f;

    float sample = std::sin(previewPhase) * 0.2f * env;
    previewPhase += phaseDelta;
    if (previewPhase >= juce::MathConstants<float>::twoPi)
      previewPhase -= juce::MathConstants<float>::twoPi;

    for (int ch = 0; ch < numChannels; ++ch)
      buffer.addSample(ch, i, sample);
    --samplesRem;
  }
  previewSamplesRemaining.store(samplesRem);
}

// =============================================================================
// processBlock — flow only; details live in the helpers above
// =============================================================================

void CanaryVoiceTuneAudioProcessor::processBlock(
    juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) {
  juce::ignoreUnused(midiMessages);
  juce::ScopedNoDenormals noDenormals;

  auto totalNumInputChannels  = getTotalNumInputChannels();
  auto totalNumOutputChannels = getTotalNumOutputChannels();
  for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());

  if (totalNumInputChannels <= 0) return;

  // ---- Snapshot user controls --------------------------------------------
  float attackMs           = attackParam   ? attackParam->load()   : 10.0f;
  float releaseMs          = releaseParam  ? releaseParam->load()  : 100.0f;
  float vibratoAmount      = vibratoParam ? vibratoParam->load() : 1.0f;
  float exciterDb          = exciterParam  ? exciterParam->load()  : 0.0f;
  float sibilantsDb        = sibilantsParam? sibilantsParam->load(): 0.0f;
  float breathDb           = breathParam   ? breathParam->load()   : 0.0f;
  float popMaxDb           = popParam      ? popParam->load()      : 0.0f;
  int   tuningMode         = tuningModeParam ? (int)std::round(tuningModeParam->load()) : 0;

  pitchShifter.setTuningMode(tuningMode);
  int newLatency = pitchShifter.getLatencySamples();
  if (newLatency != currentLatencySamples.load())
  {
      setLatencySamples(newLatency);
      currentLatencySamples.store(newLatency);
  }

  bool activeKeys[88];
  bool anyKeyActive = false;
  for (int j = 0; j < 88; ++j) {
    activeKeys[j] = keyParams[j] ? (keyParams[j]->load() > 0.5f) : true;
    if (activeKeys[j]) anyKeyActive = true;
  }

  // ---- Pitch detection ---------------------------------------------------
  // `process()` updates state and returns the smoothed pitch (good for UI
  // and lock decisions). `getInstantPitch()` is the unsmoothed per-block
  // estimate — we feed THAT to the tuner so the cancellation ratio reflects
  // the live wobble. Using the smoothed pitch here makes the ratio lag the
  // input by 3-4 blocks and the wobble survives at the output.
  buildMonoMix(buffer);
  float smoothedHz  = pitchDetector.process(monoMix.data(), buffer.getNumSamples());
  float instantHz   = pitchDetector.getInstantPitch();
  float detectedHz  = (instantHz > 0.0f) ? instantHz : smoothedHz;
  bool  isConsonant = pitchDetector.isConsonant();
  bool  isBreath    = pitchDetector.isBreath();

  float blockSumSq = 0.0f;
  for (int i = 0; i < (int)monoMix.size(); ++i) blockSumSq += monoMix[i] * monoMix[i];
  float blockRms = (monoMix.size() > 0) ? std::sqrt(blockSumSq / (float)monoMix.size()) : 0.0f;

  // Octave-jump guard in the selector layer: if detectedHz jumps more than
  // one octave from the previous valid pitch, the detector made an error —
  // clamp to the previous value so the shifter doesn't apply a wild ratio.
  // Legitimate large melodic intervals (up to a 7th ≈ 10 st) are preserved;
  // only octave-class errors (≥ 11 st from prior) are suppressed.
  if (detectedHz > 0.0f && smoothedHz > 0.0f) {
    float prevHz = smoothedHz;
    if (prevHz > 0.0f) {
      float semitones = 12.0f * std::abs(std::log2(detectedHz / prevHz));
      if (semitones >= 11.0f)
        detectedHz = prevHz;   // ignore the jump — hold previous estimate
    }
  }

  bool  isVoiced    = (detectedHz > 0.0f) && !isConsonant && (blockRms > 0.01f);


  // ---- Maintain voicing state at segment boundaries ----------------------
  // Onset: fresh start, clear everything.
  // Consonant (sibilant, plosive, fricative): wipe all lock state instantly
  // and tell the shifter to drop to dry over ~10 ms — we don't want the
  // tuner to either prolong the previous vowel into the consonant or to
  // tune the consonant itself, and we want the next vowel to start fresh.
  // Offset (silence after vowel): keep the lock alive for `releaseMs` so
  // (a) the shifter's release fade still has a coherent target to bend
  // toward, and (b) the keyboard highlight stays on the just-sung note for
  // the whole tail. Only when the release window expires (or the singer
  // resumes) do we actually clear the voicing state.
  bool  consonantFastRelease = false;
  float effectiveReleaseMs   = releaseMs;
  if (isConsonant) {
    resetNoteLockState();
    releaseTailSamplesRemaining = 0;
    releaseTailMidi = -1;
    consonantFastRelease = true;
    effectiveReleaseMs = 10.0f;
  } else if (isVoiced && !wasVoiced) {
    resetNoteLockState();
    releaseTailSamplesRemaining = 0;
    releaseTailMidi = -1;
  } else if (!isVoiced) {
    if (wasVoiced && releaseMidi >= 0) {
      releaseTailMidi = releaseMidi;
      releaseTailSamplesRemaining =
          (int)(releaseMs * 0.001f * (float)getSampleRate());
    }
    if (releaseTailSamplesRemaining > 0) {
      releaseTailSamplesRemaining -= buffer.getNumSamples();
    } else {
      resetVoicingState();
      releaseTailMidi = -1;
    }
  }
  wasVoiced = isVoiced;

  // ---- Compute target ratio + the note we're tuning to -------------------
  float targetRatio = 1.0f;
  int displayedMidi = -1;
  if (isVoiced && anyKeyActive) {
    displayedMidi = chooseTargetNoteAndRatio(
        detectedHz, activeKeys,
        (float)buffer.getNumSamples(), (float)getSampleRate(),
        attackMs, releaseMs, vibratoAmount, targetRatio);
  }

  // ---- Notify UI of any note-change event --------------------------------
  // Note-off events are delayed by the matching release tail so the
  // keyboard highlight tracks the shifter's fade-out. Vowel-to-silence uses
  // the user's Release; consonant uses the fast 10 ms reset.
  int displayedNoteIdx = (isVoiced && displayedMidi >= 21 && displayedMidi <= 108)
                            ? (displayedMidi - 21) : -1;
  int extraDelay = (displayedNoteIdx < 0)
                       ? (int)(effectiveReleaseMs * 0.001f * (float)getSampleRate())
                       : 0;
  pushNoteEvent(displayedNoteIdx, extraDelay);

  // ---- Hand off to the shifter -------------------------------------------
  // On a consonant we pass `effectiveReleaseMs=10` so the shifter ramps
  // its ratio back to 1.0 quickly. Suppress that path with juce::ignoreUnused
  // of consonantFastRelease — the flag is implicit in effectiveReleaseMs.
  juce::ignoreUnused(consonantFastRelease);
  pitchShifter.setTargetShift(targetRatio, attackMs, effectiveReleaseMs,
                              isVoiced, detectedHz, vibratoAmount);
  pitchShifter.setExciter(exciterDb, isConsonant);
  pitchShifter.setToneShaping(sibilantsDb, 0.0f);
  pitchShifter.setBreathGate(breathDb, isBreath);
  pitchShifter.setPopFilter(popMaxDb);
  pitchShifter.process(buffer);


  // ---- Preview tone (when the user clicks a key on the keyboard) ---------
  renderPreviewTone(buffer);

  // Advance the audio clock AFTER the block is processed. `pushNoteEvent`
  // above stamps the event with the start-of-block clock, so the UI knows
  // exactly which sample produced this note.
  audioSampleClock.fetch_add((int64_t)buffer.getNumSamples(),
                             std::memory_order_release);
}

void CanaryVoiceTuneAudioProcessor::playPreviewTone(float freq) {
  previewFrequencyHz.store(freq);
  previewSamplesRemaining.store((int)(getSampleRate() * 0.5));
  previewPhase = 0.0f;
}

juce::AudioProcessorEditor *CanaryVoiceTuneAudioProcessor::createEditor() {
  return new CanaryVoiceTuneAudioProcessorEditor(*this);
}

void CanaryVoiceTuneAudioProcessor::getStateInformation(
    juce::MemoryBlock &destData) {
  auto state = apvts.copyState();
  state.setProperty("currentProgram", currentProgram, nullptr);
  std::unique_ptr<juce::XmlElement> xml(state.createXml());
  copyXmlToBinary(*xml, destData);
}

void CanaryVoiceTuneAudioProcessor::setStateInformation(const void *data,
                                                        int sizeInBytes) {
  std::unique_ptr<juce::XmlElement> xmlState(
      getXmlFromBinary(data, sizeInBytes));
  if (xmlState && xmlState->hasTagName(apvts.state.getType())) {
    auto tree = juce::ValueTree::fromXml(*xmlState);
    apvts.replaceState(tree);
    if (tree.hasProperty("currentProgram"))
      currentProgram = static_cast<int>(tree.getProperty("currentProgram"));
  }
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new CanaryVoiceTuneAudioProcessor();
}
