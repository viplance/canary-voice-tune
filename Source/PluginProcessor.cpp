#include "PluginProcessor.h"
#include "PluginEditor.h"

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
  rangeParam     = apvts.getRawParameterValue("RANGE");
  vibratoParam   = apvts.getRawParameterValue("VIBRATO");
  sibilantsParam = apvts.getRawParameterValue("SIBILANTS");
  breathParam    = apvts.getRawParameterValue("BREATH");
  popParam       = apvts.getRawParameterValue("POP");
  for (int i = 0; i < 88; ++i)
    keyParams[i] = apvts.getRawParameterValue("KEY_" + juce::String(i));
}

CanaryVoiceTuneAudioProcessor::~CanaryVoiceTuneAudioProcessor() {}

juce::AudioProcessorValueTreeState::ParameterLayout
CanaryVoiceTuneAudioProcessor::createParameterLayout() {
  std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"ATTACK", 1}, "Attack", 0.1f, 150.0f, 100.0f));
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"RELEASE", 1}, "Release", 10.0f, 500.0f, 250.0f));
  // Range: 0% = no correction (bypass), 100% = full snap to nearest note
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"RANGE", 1}, "Range", 0.0f, 100.0f, 0.0f));
  // Vibrato: maximum pitch deviation (in semitones) allowed around the
  // smoothed centre. 0 = perfectly flat, 1 = up to ±1 semitone of wobble.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"VIBRATO", 2}, "Vibrato", 0.0f, 1.0f, 1.0f));
  // Sibilants: high-shelf gain around 7 kHz for "s/sh/t" presence.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"SIBILANTS", 1}, "Sibilants", -12.0f, 12.0f, 0.0f));
  // Breath Gate: detector threshold; 0 dB disables it.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"BREATH", 1}, "Breath", -48.0f, 0.0f, 0.0f));



  // Pop Filter: detector threshold; 0 dB disables it.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"POP", 1}, "Pop Filter", -24.0f, 0.0f, 0.0f));

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
  // Pick whichever channel has more energy in this block. A naive (L+R)/2
  // would silently cancel anti-phase stereo material (Haas spreads, stereo
  // wideners) and the detector would then report unvoiced even though the
  // singer is clearly audible.
  int n = buffer.getNumSamples();
  monoMix.resize((size_t)n);
  if (buffer.getNumChannels() == 1) {
    std::copy_n(buffer.getReadPointer(0), n, monoMix.data());
    return;
  }
  const float* l = buffer.getReadPointer(0);
  const float* r = buffer.getReadPointer(1);
  double eL = 0.0, eR = 0.0;
  for (int i = 0; i < n; ++i) { eL += l[i] * l[i]; eR += r[i] * r[i]; }
  std::copy_n((eL >= eR) ? l : r, n, monoMix.data());
}

void CanaryVoiceTuneAudioProcessor::resetVoicingState() {
  lockedMidi          = -1;
  lockEngageSamples   = 0;
  lockReleaseSamples  = 0;
  smoothedMidi        = -1.0f;
  smoothedTargetMidi  = -1.0f;
  voicedSampleCount   = 0;
}

int CanaryVoiceTuneAudioProcessor::chooseTargetNoteAndRatio(
    float detectedHz, const bool* activeKeys, float blockSize, float sr,
    float attackMs, float correctionStrength, float vibratoAmount,
    float& outRatio) {
  voicedSampleCount += (int)blockSize;

  float floatMidi = 69.0f + 12.0f * std::log2(detectedHz / 440.0f);

  // ---- Vibrato shaping (adaptive lowpass on the pitch in semitone domain)
  // The smoothed centre tracks the slow pitch trend; the raw deviation around
  // that centre is then clamped to ±vibratoAmount semitones. Wide-amplitude
  // jumps (real note changes) bypass the smoothing so the tracker doesn't lag
  // a ratio behind on melodic transitions.
  float blockDt = blockSize / sr;
  // Always smooth aggressively enough to recover the centre pitch; the
  // wobble around it is reintroduced via the clamp below.
  float vibTimeMs = 201.0f;
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
  float hysteresisSt = 0.1f + 0.5f * correctionStrength;
  int   bestMidi     = -1;
  float minDist      = 1e9f;
  for (int testMidi = 21; testMidi <= 108; ++testMidi) {
    if (! activeKeys[testMidi - 21]) continue;
    float d = std::abs(lockPitch - (float)testMidi);
    // Give the incumbent locked note a "discount" so a challenger must be
    // genuinely closer, not merely tied, to take its place.
    if (testMidi == lockedMidi) d -= hysteresisSt;
    if (d < minDist) { minDist = d; bestMidi = testMidi; }
  }
  if (bestMidi < 0) bestMidi = (int)std::round(lockPitch);

  // ---- Note lock ----------------------------------------------------------
  // First lock: capture the first stable bestMidi after a 50 ms grace.
  // Re-lock: instead of "any 120 ms drift > 1.5 st" (which was both too
  // strict for legato moves and too lenient for sub-semitone wobble), we
  // require the candidate to be a *single*, *unchanged* bestMidi held for
  // `attackMs` worth of samples. That ties stabilisation latency to the
  // same control the user uses for snap responsiveness.
  const int lockGraceSamples   = (int)(sr * 0.050f);
  const int reLockStableSamples = (int)(sr * juce::jmax(attackMs, 1.0f) / 1000.0f);
  if (voicedSampleCount >= lockGraceSamples && lockedMidi < 0) {
    lockedMidi = bestMidi;
    lockEngageSamples = 0;
    lockReleaseSamples = 0;
    candidateMidi = -1;
    candidateStableSamples = 0;
  }
  if (lockedMidi >= 0) {
    lockEngageSamples += (int)blockSize;

    if (bestMidi != lockedMidi) {
      if (bestMidi == candidateMidi) {
        candidateStableSamples += (int)blockSize;
      } else {
        candidateMidi = bestMidi;
        candidateStableSamples = (int)blockSize;
      }
      if (candidateStableSamples >= reLockStableSamples) {
        lockedMidi = candidateMidi;
        lockEngageSamples = 0;
        lockReleaseSamples = 0;
        candidateMidi = -1;
        candidateStableSamples = 0;
      }
    } else {
      // Singer is back on the locked note — drop the pending candidate.
      candidateMidi = -1;
      candidateStableSamples = 0;
    }
  }

  // ---- Lock-engage fade ---------------------------------------------------
  // While engaging, blend toward dry so the attack curve is audible. Once
  // engaged we always target an active key — if the singer drifts far from
  // the locked note we follow the closest active key (`bestMidi`) instead
  // of leaking the raw pitch, otherwise inactive notes would sound.
  float engageFade = juce::jlimit(0.0f, 1.0f,
                                  1000.0f * (float)lockEngageSamples / sr / attackMs);
  float lockBypass = 1.0f - engageFade;
  if (lockedMidi >= 0) {
    float drift = std::abs(effectiveMidi - (float)lockedMidi);
    // Within ~1 semitone of the lock, snap to the lock. Beyond that, follow
    // the nearest active key so a sustained off-key pitch is still corrected
    // (the lock itself re-arms after 120 ms of sustained drift, above).
    if (drift <= 1.0f) bestMidi = lockedMidi;
  }

  // ---- Compute the ratio --------------------------------------------------
  // Target is always an active key (with `correctionStrength` controlling how
  // far we pull the raw pitch toward it). `lockBypass` only fades in the dry
  // signal during the lock-engage window.
  float diff           = smoothedMidi - bestMidi;
  float remainingDiff  = diff * (1.0f - correctionStrength);
  float rawTargetMidi  = bestMidi + remainingDiff + clampedDev;
  rawTargetMidi        = rawTargetMidi * (1.0f - lockBypass)
                       + effectiveMidi * lockBypass;

  // Short portamento smooths the ±1 semitone step that occurs when bestMidi
  // crosses a bucket boundary.
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
  return bestMidi;
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
  float correctionStrength = (rangeParam   ? rangeParam->load()    : 0.0f) / 100.0f;
  float vibratoAmount      = vibratoParam ? vibratoParam->load() : 1.0f;
  float sibilantsDb        = sibilantsParam? sibilantsParam->load(): 0.0f;
  float breathDb           = breathParam   ? breathParam->load()   : 0.0f;
  float popMaxDb           = popParam      ? popParam->load()      : 0.0f;

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
  bool  isVoiced    = (detectedHz > 0.0f) && !isConsonant;


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
    resetVoicingState();
    releaseHoldSamplesRemaining = 0;
    releaseHoldMidi = -1;
    consonantFastRelease = true;
    effectiveReleaseMs = 10.0f;
  } else if (isVoiced && !wasVoiced) {
    resetVoicingState();
    releaseHoldSamplesRemaining = 0;
    releaseHoldMidi = -1;
  } else if (!isVoiced) {
    if (wasVoiced && lockedMidi >= 0) {
      releaseHoldMidi = lockedMidi;
      releaseHoldSamplesRemaining =
          (int)(releaseMs * 0.001f * (float)getSampleRate());
    }
    if (releaseHoldSamplesRemaining > 0) {
      releaseHoldSamplesRemaining -= buffer.getNumSamples();
    } else {
      resetVoicingState();
      releaseHoldMidi = -1;
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
        attackMs, correctionStrength, vibratoAmount, targetRatio);
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
  std::unique_ptr<juce::XmlElement> xml(state.createXml());
  copyXmlToBinary(*xml, destData);
}

void CanaryVoiceTuneAudioProcessor::setStateInformation(const void *data,
                                                        int sizeInBytes) {
  std::unique_ptr<juce::XmlElement> xmlState(
      getXmlFromBinary(data, sizeInBytes));
  if (xmlState && xmlState->hasTagName(apvts.state.getType()))
    apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new CanaryVoiceTuneAudioProcessor();
}
