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

  // -------- UI-facing reads --------
  // Drain the audio-thread note ring and return the most-recent note seen
  // since the last call (-1 = no voiced note now, -2 = ring was empty so the
  // UI should keep its current state). Safe to call from any thread but
  // intended for the message thread only.
  int popLatestNoteEvent();

  float getPopActivity() const { return pitchShifter.getPopActivity(); }

  // -------- preview tone --------
  void playPreviewTone(float freq);

private:
  juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

  // ---- processBlock helpers ----
  void buildMonoMix(const juce::AudioBuffer<float>& buffer);
  void resetVoicingState();
  // Computes the corrected target ratio for the current block. Returns the
  // midi note that the singer is being tuned to (used for keyboard display)
  // and writes the ratio into outRatio.
  int chooseTargetNoteAndRatio(float detectedHz,
                               const bool* activeKeys,
                               float blockSize,
                               float sr,
                               float attackMs,
                               float correctionStrength,
                               float vibratoAmount,
                               float& outRatio);
  // `extraDelaySamples` is added on top of the shifter latency when the
  // event should become visible later than the audio (e.g. a note-off has
  // to wait for the shifter's release fade to complete).
  void pushNoteEvent(int noteIndex /* -1 if unvoiced, else 0..87 */,
                     int extraDelaySamples = 0);
  void renderPreviewTone(juce::AudioBuffer<float>& buffer);

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

  std::vector<float> monoMix; // scratch buffer for pitch detector input

  // Per-voiced-segment tracking (audio thread only)
  int lockedMidi = -1;        // captured note for the current voiced segment
  int lockEngageSamples = 0;  // samples since lockedMidi was set (fade-in)
  int lockReleaseSamples = 0; // consecutive samples far from lockedMidi
  bool wasVoiced = false;
  float smoothedMidi = -1.0f;
  float smoothedTargetMidi = -1.0f;
  int voicedSampleCount = 0;

  // Lock-free SPSC ring of (visibleAtClock, note-index) events. The audio
  // thread pushes one entry whenever the displayed note index changes,
  // stamped with the audio-clock sample at which the corresponding audio
  // will actually be heard. That's `pushClock + currentLatencySamples` for
  // a note-on, plus `releaseMs` for a note-off so the highlight stays lit
  // for the duration of the shifter's release tail (otherwise the key goes
  // dark while the tuner is still audibly fading out).
  static constexpr int kNoteHistorySize = 64;
  std::atomic<int>      noteHistoryNote[kNoteHistorySize] = {};
  std::atomic<int64_t>  noteHistoryVisibleAt[kNoteHistorySize] = {};
  std::atomic<int> noteHistoryWriteIdx { 0 };
  std::atomic<int> noteHistoryReadIdx { 0 };
  int lastPushedNote = -1; // audio thread only

  // Sample-accurate audio-thread clock. Incremented by buffer.numSamples per
  // block. UI thread reads it to compute "how old is this note event".
  std::atomic<int64_t> audioSampleClock { 0 };
  std::atomic<int>     currentLatencySamples { 0 };

  // Preview tone state
  std::atomic<float> previewFrequencyHz { 0.0f };
  std::atomic<int>   previewSamplesRemaining { 0 };
  float previewPhase = 0.0f;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
      CanaryVoiceTuneAudioProcessor)
};
