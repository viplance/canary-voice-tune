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
  for (int i = 0; i < 88; ++i) {
    keyParams[i] = apvts.getRawParameterValue("KEY_" + juce::String(i));
  }
}

CanaryVoiceTuneAudioProcessor::~CanaryVoiceTuneAudioProcessor() {}

juce::AudioProcessorValueTreeState::ParameterLayout
CanaryVoiceTuneAudioProcessor::createParameterLayout() {
  std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"ATTACK", 1}, "Attack", 0.1f, 100.0f, 50.0f));
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"RELEASE", 1}, "Release", 10.0f, 1000.0f, 150.0f));
  // Range: 0% = no correction (bypass), 100% = full snap to nearest note
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"RANGE", 1}, "Range", 0.0f, 100.0f, 100.0f));
  // Remove Vibrato: 0% = keep natural pitch wobble, 100% = perfectly flat note
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"VIBRATO", 1}, "Remove Vibrato", 0.0f, 100.0f, 0.0f));

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
    const int onsetGraceSamples = (int)(sr * 0.030); // 30ms

    if (isVoiced) {
      voicedSampleCount += buffer.getNumSamples();
    }

    if (isVoiced && anyKeyActive) {
      float floatMidi = 69.0f + 12.0f * std::log2(detectedHz / 440.0f);

      // Smooth the pitch in the log (semitone) domain to remove vibrato.
      // Vibrato is small (~±0.5 semitone) and fast (~5-7Hz). Real note changes
      // are large (>=1 semitone) and should pass through without lag — so the
      // lowpass is adaptive: strong inside the vibrato deadband, then it
      // relaxes and finally snaps for big jumps (note transitions, octave
      // leaps). Without this, a long time constant makes the tuner chase the
      // previous note for hundreds of ms after a real pitch change.
      float blockDt = (float)buffer.getNumSamples() / (float)sr;
      float vibTimeMs = 1.0f + vibratoRemoval * 250.0f; // 1ms .. 251ms
      if (smoothedMidi < 0.0f) {
        smoothedMidi = floatMidi;
      } else {
        float delta = std::abs(floatMidi - smoothedMidi);
        // 0..0.6 semitone: full smoothing (vibrato).
        // 0.6..1.5 semitone: smoothing relaxes linearly.
        // >1.5 semitone: snap to new pitch (real note change).
        float jumpFactor;
        if (delta < 0.6f)      jumpFactor = 0.0f;
        else if (delta > 1.5f) jumpFactor = 1.0f;
        else                   jumpFactor = (delta - 0.6f) / 0.9f;

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

      // Hysteresis to prevent note jumping (duck effect)
      if (lastBestMidi >= 21 && lastBestMidi <= 108 && activeKeys[lastBestMidi - 21]) {
          float distToLast = std::abs(effectiveMidi - lastBestMidi);
          float distToNew = std::abs(effectiveMidi - bestMidi);
          // Only switch if the new note is significantly closer (e.g., 0.8 semitones better)
          // This creates a strong "magnet" effect to the current note to prevent wobble
          if (distToLast < 1.0f && distToNew > distToLast - 0.4f) {
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
      // chirp on note transitions. A short ~12ms portamento makes the
      // transition sound natural without lagging.
      float blockDtMs = 1000.0f * (float)buffer.getNumSamples() / (float)sr;
      float portamentoTimeMs = 12.0f;
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
    pitchShifter.process(buffer);
  }
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
