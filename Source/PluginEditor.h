#pragma once

#include "PluginProcessor.h"
#include "UI/PianoKeyboard.h"
#include "UI/RotaryKnob.h"
#include <JuceHeader.h>

class CanaryVoiceTuneAudioProcessorEditor
    : public juce::AudioProcessorEditor,
      public juce::Timer {
public:
  CanaryVoiceTuneAudioProcessorEditor(
      CanaryVoiceTuneAudioProcessor &);
  ~CanaryVoiceTuneAudioProcessorEditor() override;

  void paint(juce::Graphics &) override;
  void resized() override;
  void timerCallback() override;

private:
  CanaryVoiceTuneAudioProcessor &audioProcessor;

  RotaryKnob attackKnob;
  RotaryKnob releaseKnob;
  RotaryKnob rangeKnob;
  RotaryKnob vibratoKnob;

  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      attackAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      releaseAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      rangeAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      vibratoAttachment;

  PianoKeyboard pianoKeyboard;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
      CanaryVoiceTuneAudioProcessorEditor)
};
