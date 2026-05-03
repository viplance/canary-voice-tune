#pragma once

#include "DSP/PitchDetector.h"
#include "DSP/PitchShifter.h"
#include <JuceHeader.h>

class CanaryVoiceTuneAudioProcessor : public juce::AudioProcessor {
public:
  CanaryVoiceTuneAudioProcessor();
  ~CanaryVoiceTuneAudioProcessor() override;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;
  bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
  void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;

  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return JucePlugin_Name; }

  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  double getTailLengthSeconds() const override { return 0.0; }

  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int index) override {}
  const juce::String getProgramName(int index) override { return {}; }
  void changeProgramName(int index, const juce::String &newName) override {}

  void getStateInformation(juce::MemoryBlock &destData) override;
  void setStateInformation(const void *data, int sizeInBytes) override;

  juce::AudioProcessorValueTreeState apvts;
  std::atomic<float> currentDetectedPitch{0.0f};

  float getPopActivity() const { return pitchShifter.getPopActivity(); }

  void playPreviewTone(float freq);
  std::atomic<float> previewFrequencyHz{0.0f};
  std::atomic<int> previewSamplesRemaining{0};
  float previewPhase = 0.0f;

private:
  juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

  PitchDetector pitchDetector;
  PitchShifter pitchShifter;

  std::atomic<float> *attackParam = nullptr;
  std::atomic<float> *releaseParam = nullptr;
  std::atomic<float> *rangeParam = nullptr;
  std::atomic<float> *vibratoParam = nullptr;
  std::atomic<float> *sibilantsParam = nullptr;
  std::atomic<float> *breathParam = nullptr;
  std::atomic<float> *popParam = nullptr;
  std::atomic<float> *keyParams[88] = {nullptr};
  int lastBestMidi = -1;
  bool wasVoiced = false;
  float smoothedMidi = -1.0f;
  float smoothedTargetMidi = -1.0f;
  int voicedSampleCount = 0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
      CanaryVoiceTuneAudioProcessor)
};
