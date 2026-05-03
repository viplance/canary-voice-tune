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
  attackParam = apvts.getRawParameterValue("ATTACK");
  releaseParam = apvts.getRawParameterValue("RELEASE");
  rangeParam = apvts.getRawParameterValue("RANGE");
  vibratoParam = apvts.getRawParameterValue("VIBRATO");
  sibilantsParam = apvts.getRawParameterValue("SIBILANTS");
  breathParam = apvts.getRawParameterValue("BREATH");
  for (int i = 0; i < 88; ++i) {
    keyParams[i] = apvts.getRawParameterValue("KEY_" + juce::String(i));
  }
}

CanaryVoiceTuneAudioProcessor::~CanaryVoiceTuneAudioProcessor() {}

juce::AudioProcessorValueTreeState::ParameterLayout
CanaryVoiceTuneAudioProcessor::createParameterLayout() {
  std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"ATTACK", 1}, "Attack", 0.1f, 100.0f, 80.0f));
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"RELEASE", 1}, "Release", 10.0f, 1000.0f, 170.0f));
  // Range: 0% = no correction (bypass), 100% = full snap to nearest note
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"RANGE", 1}, "Range", 0.0f, 100.0f, 50.0f));
  // Remove Vibrato: 0% = keep natural pitch wobble, 100% = perfectly flat note
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"VIBRATO", 1}, "Remove Vibrato", 0.0f, 100.0f, 50.0f));
  // Sibilants: high-shelf gain around 7 kHz for "s/sh/t" presence.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"SIBILANTS", 1}, "Sibilants", -12.0f, 12.0f, 0.0f));
  // Breath: peak/bell gain around 3 kHz for breathiness/air.
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"BREATH", 1}, "Breath", -12.0f, 12.0f, 0.0f));

  // Keys 0-87 for A0 to C8. 1 means enabled, 0 means disabled.
  for (int i = 0; i < 88; ++i) {
    juce::String idText = "KEY_" + juce::String(i);
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{idText, 1}, idText, true));
  }

  return {params.begin(), params.end()};
}

void CanaryVoiceTuneAudioProcessor::prepareToPlay(double sampleRate,
                                                       int samplesPerBlock) {
  pitchDetector.prepare(sampleRate, samplesPerBlock);
  pitchShifter.prepare(sampleRate, samplesPerBlock);
  setLatencySamples(pitchShifter.getLatencySamples());
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

void CanaryVoiceTuneAudioProcessor::processBlock(
    juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) {
  juce::ScopedNoDenormals noDenormals;
  auto totalNumInputChannels = getTotalNumInputChannels();
  auto totalNumOutputChannels = getTotalNumOutputChannels();

  for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());

  // Basic processing:
  // Wait for pitch detector
  float attackMs = attackParam ? attackParam->load() : 10.0f;
  float releaseMs = releaseParam ? releaseParam->load() : 100.0f;
  float rangePercent = rangeParam ? rangeParam->load() : 100.0f;
  // 0% = no correction (keep original deviation from note),
  // 100% = full snap to nearest active note.
  float correctionStrength = rangePercent / 100.0f;
  float vibratoRemoval = vibratoParam ? (vibratoParam->load() / 100.0f) : 0.0f;

  bool activeKeys[88];
  bool anyKeyActive = false;
  for (int j = 0; j < 88; ++j) {
    activeKeys[j] = keyParams[j] ? (keyParams[j]->load() > 0.5f) : true;
    if (activeKeys[j])
      anyKeyActive = true;
  }

  if (totalNumInputChannels > 0) {
    auto *channelData = buffer.getReadPointer(0);
    float detectedHz =
        pitchDetector.process(channelData, buffer.getNumSamples());

    bool isVoiced = (detectedHz > 0.0f);
    float actualOutputHz = detectedHz;
    float targetRatio = 1.0f;

    // On voicing onset, drop hysteresis memory and reset the smoothed pitch
    // tracker so we don't "magnet" to the previous phrase's note (chirp/quack
    // at the start of the word) and so vibrato smoothing starts from the
    // current pitch rather than ramping in from the old value.
    if (isVoiced && !wasVoiced) {
      lastBestMidi = -1;
      smoothedMidi = -1.0f;
      smoothedTargetMidi = -1.0f;
      voicedSampleCount = 0;
    }
    wasVoiced = isVoiced;

    const double sr = getSampleRate();
    const int onsetGraceSamples = (int)(sr * 0.050); // 50ms

    if (isVoiced) {
      voicedSampleCount += buffer.getNumSamples();
    }

    if (isVoiced && anyKeyActive) {
      float floatMidi = 69.0f + 12.0f * std::log2(detectedHz / 440.0f);

      // Smooth the pitch in the log (semitone) domain to remove vibrato.
      // The lowpass time constant scales with the Remove Vibrato amount.
      // We also apply an adaptive "snap" so that real note changes (octave
      // leaps, melodic transitions) don't lag behind. The snap itself is
      // disabled at 100% Remove Vibrato — at full strength the user wants a
      // perfectly flat tone with no sudden ratio jumps, even if it costs a
      // bit of latency on note changes. Without disabling the snap, slow
      // pitch drift on a held note eventually trips the threshold and causes
      // an audible "tuning jump".
      float blockDt = (float)buffer.getNumSamples() / (float)sr;
      float vibTimeMs = 1.0f + vibratoRemoval * 200.0f; // 1ms .. 201ms
      if (smoothedMidi < 0.0f) {
        smoothedMidi = floatMidi;
      } else {
        float delta = std::abs(floatMidi - smoothedMidi);
        // 0..0.5 semitone: full smoothing (vibrato range).
        // 0.5..1.2 semitone: smoothing relaxes linearly.
        // > 1.2 semitone: snap (real note change).
        float jumpFactor;
        if (delta < 0.5f)      jumpFactor = 0.0f;
        else if (delta > 1.2f) jumpFactor = 1.0f;
        else                   jumpFactor = (delta - 0.5f) / 0.7f;

        // Do NOT suppress jumpFactor by vibratoRemoval. We always want real
        // note changes to snap through quickly — otherwise the smoother trails
        // behind and the shifter chases the old note, causing a sweep/quack.

        float effectiveTimeMs = vibTimeMs * (1.0f - jumpFactor) + 1.0f * jumpFactor;
        float adaptiveAlpha = 1.0f - std::exp(-blockDt / (effectiveTimeMs / 1000.0f));
        smoothedMidi += adaptiveAlpha * (floatMidi - smoothedMidi);
      }
      float effectiveMidi = smoothedMidi * vibratoRemoval
                          + floatMidi * (1.0f - vibratoRemoval);

      int nearestMidi = (int)std::round(effectiveMidi);

      int bestMidi = nearestMidi;
      int minDistance = 100;

      for (int dx = -88; dx <= 88; ++dx) {
        int testMidi = nearestMidi + dx;
        if (testMidi < 21 || testMidi > 108)
          continue;
        int keyIndex = testMidi - 21;
        if (activeKeys[keyIndex]) {
          if (std::abs(dx) < minDistance) {
            minDistance = std::abs(dx);
            bestMidi = testMidi;
          }
        }
      }

      // Hysteresis: stick to the current note unless the new note is clearly
      // closer. This prevents rapid toggling when pitch sits near a semitone
      // boundary, which is heard as a "quack" / "duck" effect.
      if (lastBestMidi >= 21 && lastBestMidi <= 108 && activeKeys[lastBestMidi - 21]) {
          float distToLast = std::abs(effectiveMidi - (float)lastBestMidi);
          float distToNew  = std::abs(effectiveMidi - (float)bestMidi);
          // Stay on current note if we're within 0.7 semitones of it AND the
          // new candidate isn't at least 0.35 semitones closer.
          if (distToLast < 0.7f || (distToNew > distToLast - 0.35f && bestMidi != lastBestMidi)) {
              bestMidi = lastBestMidi;
          }
      }
      lastBestMidi = bestMidi;

      float diff = effectiveMidi - bestMidi;
      // Keep (1 - strength) of the original deviation from the note.
      float remainingDiff = diff * (1.0f - correctionStrength);
      float rawTargetMidi = bestMidi + remainingDiff;

      // Smooth the *target* note in the log domain. When the input glides
      // across a bucket boundary, bestMidi jumps by a whole semitone — without
      // smoothing this becomes an instant ±1 semitone step in targetRatio,
      // which the shifter ramps over Attack ms and is heard as a "duck" /
      // chirp on note transitions. A short fixed portamento smooths bucket
      // crossings without adding noticeable lag.
      float blockDtMs = 1000.0f * (float)buffer.getNumSamples() / (float)sr;
      float portamentoTimeMs = 15.0f;
      float targetAlpha = 1.0f - std::exp(-blockDtMs / portamentoTimeMs);
      if (smoothedTargetMidi < 0.0f) {
        smoothedTargetMidi = rawTargetMidi;
      } else {
        smoothedTargetMidi += targetAlpha * (rawTargetMidi - smoothedTargetMidi);
      }
      float targetHz = 440.0f * std::pow(2.0f, (smoothedTargetMidi - 69.0f) / 12.0f);
      targetRatio = targetHz / detectedHz;

      // Limit ratio to prevent crazy shifts
      if (targetRatio > 2.0f)
        targetRatio = 2.0f;
      if (targetRatio < 0.5f)
        targetRatio = 0.5f;

      // Onset grace: hold ratio at 1.0 for the first 30ms of voicing so the
      // YIN detector has time to lock and we don't shift on the first jittery
      // pitch estimate (which is the main source of "quack" at word starts).
      if (voicedSampleCount < onsetGraceSamples) {
        targetRatio = 1.0f;
      }

      actualOutputHz = 440.0f * std::pow(2.0f, (bestMidi - 69.0f) / 12.0f);
    }

    if (isVoiced)
      currentDetectedPitch.store(actualOutputHz);
    else
      currentDetectedPitch.store(0.0f);

    pitchShifter.setTargetShift(targetRatio, attackMs, releaseMs, isVoiced, detectedHz);
    float sibilantsDb = sibilantsParam ? sibilantsParam->load() : 0.0f;
    float breathDb    = breathParam    ? breathParam->load()    : 0.0f;
    pitchShifter.setToneShaping(sibilantsDb, breathDb);
    pitchShifter.process(buffer);
  }

  // Preview tone generation
  int samplesRem = previewSamplesRemaining.load();
  if (samplesRem > 0) {
    float freq = previewFrequencyHz.load();
    float phaseDelta = (freq * 2.0f * juce::MathConstants<float>::pi) / static_cast<float>(getSampleRate());
    int totalSamplesForTone = static_cast<int>(getSampleRate() * 0.5);

    for (int i = 0; i < buffer.getNumSamples(); ++i) {
      if (samplesRem > 0) {
        float env = 1.0f;
        // Fade out
        if (samplesRem < 1000) {
          env = static_cast<float>(samplesRem) / 1000.0f;
        }
        // Fade in
        int samplesPlayed = totalSamplesForTone - samplesRem;
        if (samplesPlayed < 1000) {
          env *= static_cast<float>(samplesPlayed) / 1000.0f;
        }

        float sample = std::sin(previewPhase) * 0.2f * env;
        previewPhase += phaseDelta;
        if (previewPhase >= 2.0f * juce::MathConstants<float>::pi) {
          previewPhase -= 2.0f * juce::MathConstants<float>::pi;
        }

        for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
          buffer.addSample(ch, i, sample);
        }
        samplesRem--;
      } else {
        break;
      }
    }
    previewSamplesRemaining.store(samplesRem);
  }
}

void CanaryVoiceTuneAudioProcessor::playPreviewTone(float freq) {
    previewFrequencyHz.store(freq);
    previewSamplesRemaining.store(static_cast<int>(getSampleRate() * 0.5));
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
  if (xmlState.get() != nullptr)
    if (xmlState->hasTagName(apvts.state.getType()))
      apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new CanaryVoiceTuneAudioProcessor();
}
