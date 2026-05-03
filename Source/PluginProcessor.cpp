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
  accuracyParam = apvts.getRawParameterValue("ACCURACY");
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
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"ACCURACY", 1}, "Accuracy", 0.0f, 100.0f, 0.0f));

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
  float accuracy = (accuracyParam ? accuracyParam->load() : 100.0f) / 100.0f;

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

    if (isVoiced && anyKeyActive) {
      float floatMidi = 69.0f + 12.0f * std::log2(detectedHz / 440.0f);
      int nearestMidi = (int)std::round(floatMidi);

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

      float targetMidi = floatMidi + (bestMidi - floatMidi) * accuracy;
      float targetHz = 440.0f * std::pow(2.0f, (targetMidi - 69.0f) / 12.0f);
      targetRatio = targetHz / detectedHz;

      // Limit ratio to prevent crazy shifts
      if (targetRatio > 2.0f)
        targetRatio = 2.0f;
      if (targetRatio < 0.5f)
        targetRatio = 0.5f;

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
