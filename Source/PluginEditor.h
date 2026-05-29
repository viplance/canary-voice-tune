#pragma once

#include "PluginProcessor.h"
#include "UI/PianoKeyboard.h"
#include "UI/RotaryKnob.h"
#include "UI/TuningModeSelector.h"
#include <JuceHeader.h>

class CanaryVoiceTuneAudioProcessorEditor
    : public juce::AudioProcessorEditor,
      public juce::Timer,
      public juce::AudioProcessorValueTreeState::Listener {
public:
  CanaryVoiceTuneAudioProcessorEditor(
      CanaryVoiceTuneAudioProcessor &);
  ~CanaryVoiceTuneAudioProcessorEditor() override;

  void paint(juce::Graphics &) override;
  void resized() override;
  void timerCallback() override;
  void parameterChanged(const juce::String &parameterID, float newValue) override;

private:
  void updateKeyboardFromAudioRing();
  void updatePopLamp();
  void updateBreathLamp();


  CanaryVoiceTuneAudioProcessor &audioProcessor;

  RotaryKnob attackKnob;
  RotaryKnob releaseKnob;
  RotaryKnob vibratoKnob;
  RotaryKnob exciterKnob;
  RotaryKnob sibilantsKnob;
  RotaryKnob breathKnob;
  RotaryKnob popKnob;

  TuningModeSelector tuningModeSelector;

  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      attackAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      releaseAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      vibratoAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      exciterAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      sibilantsAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      breathAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      popAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
      tuningModeAttachment;

  PianoKeyboard pianoKeyboard;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
      CanaryVoiceTuneAudioProcessorEditor)
};
