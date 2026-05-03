#pragma once

#include "DSP/PitchDetector.h"
#include "DSP/PitchShifter.h"
#include <JuceHeader.h>
#include <vector>

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

  // Lock-free note-change history so the UI can see every distinct note that
  // played between two timer callbacks (a 60Hz timer can skip notes shorter
  // than ~16 ms otherwise). Audio thread pushes whenever the chosen midi
  // note changes; UI thread drains the ring on each tick.
  static constexpr int kNoteHistorySize = 32;
  std::atomic<int> noteHistory[kNoteHistorySize] = {};
  std::atomic<int> noteHistoryWriteIdx { 0 };
  int noteHistoryReadIdx = 0;          // UI-thread only
  std::atomic<int> lastPushedNote { -1 }; // audio-thread only writer

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
  std::vector<float> monoMix; // scratch for stereo->mono pitch detection
  int lastBestMidi = -1;
  int lockedMidi = -1;        // captured note for the current voiced segment
  int lockEngageSamples = 0;  // samples since lockedMidi was set (for fade-in)
  int lockReleaseSamples = 0; // samples spent far from lockedMidi (for relock)
  bool wasVoiced = false;
  float smoothedMidi = -1.0f;
  float smoothedTargetMidi = -1.0f;
  int voicedSampleCount = 0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
      CanaryVoiceTuneAudioProcessor)
};
