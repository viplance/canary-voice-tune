#include "PluginEditor.h"
#include "PluginProcessor.h"

CanaryVoiceTuneAudioProcessorEditor::CanaryVoiceTuneAudioProcessorEditor(
    CanaryVoiceTuneAudioProcessor &p)
    : AudioProcessorEditor(&p), audioProcessor(p), pianoKeyboard(p.apvts) {
  attackAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "ATTACK", attackKnob);
  releaseAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "RELEASE", releaseKnob);
  rangeAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "RANGE", rangeKnob);
  vibratoAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "VIBRATO", vibratoKnob);
  sibilantsAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "SIBILANTS", sibilantsKnob);
  breathAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "BREATH", breathKnob);

  attackKnob.setTextValueSuffix(" ms");
  releaseKnob.setTextValueSuffix(" ms");
  rangeKnob.setTextValueSuffix(" %");
  vibratoKnob.setTextValueSuffix(" %");
  sibilantsKnob.setTextValueSuffix(" dB");
  breathKnob.setTextValueSuffix(" dB");

  addAndMakeVisible(attackKnob);
  addAndMakeVisible(releaseKnob);
  addAndMakeVisible(rangeKnob);
  addAndMakeVisible(vibratoKnob);
  addAndMakeVisible(sibilantsKnob);
  addAndMakeVisible(breathKnob);
  addAndMakeVisible(pianoKeyboard);

  setSize(1280, 400);
  startTimerHz(30); // 30 FPS for visual updates

  pianoKeyboard.onKeyClicked = [this](float freq) {
    audioProcessor.playPreviewTone(freq);
  };
}

CanaryVoiceTuneAudioProcessorEditor::~CanaryVoiceTuneAudioProcessorEditor() {}

void CanaryVoiceTuneAudioProcessorEditor::paint(juce::Graphics &g) {
  g.fillAll(juce::Colour::fromRGB(255, 239, 0)); // Yellow background

  g.setColour(juce::Colours::black);
  g.setFont(20.0f);
  g.drawFittedText(juce::String("CanaryVoiceTune v") + PLUGIN_VERSION,
                   getLocalBounds().removeFromTop(40),
                   juce::Justification::centred, 1);

  // Labels sit right above each knob
  g.setFont(13.0f);
  g.setColour(juce::Colours::black);

  auto getLabelBounds = [](juce::Component &comp) {
    return comp.getBounds().withY(comp.getY() - 22).withHeight(22).expanded(20, 0);
  };

  g.drawText("Attack", getLabelBounds(attackKnob),
             juce::Justification::centredBottom, false);
  g.drawText("Release", getLabelBounds(releaseKnob),
             juce::Justification::centredBottom, false);
  g.drawText("Range", getLabelBounds(rangeKnob),
             juce::Justification::centredBottom, false);
  g.drawText("Remove Vibrato", getLabelBounds(vibratoKnob),
             juce::Justification::centredBottom, false);
  g.drawText("Sibilants", getLabelBounds(sibilantsKnob),
             juce::Justification::centredBottom, false);
  g.drawText("Breath", getLabelBounds(breathKnob),
             juce::Justification::centredBottom, false);
}

void CanaryVoiceTuneAudioProcessorEditor::resized() {
  auto area = getLocalBounds();
  area.removeFromTop(40); // Title header

  // -- Knob strip just below title: label row (22px) + knob row (88px) = 110px
  // --
  auto knobStrip = area.removeFromTop(110);

  // Piano keyboard fills all remaining space at the bottom
  pianoKeyboard.setBounds(area.reduced(12));

  // Labels are drawn in paint() at knob.getBounds().translated(0, -22)
  auto labelRow = knobStrip.removeFromTop(22); // reserved for painted labels
  (void)labelRow;
  int knobWidth = knobStrip.getWidth() / 6;

  attackKnob.setBounds(
      knobStrip.removeFromLeft(knobWidth).withSizeKeepingCentre(80, 80));
  releaseKnob.setBounds(
      knobStrip.removeFromLeft(knobWidth).withSizeKeepingCentre(80, 80));
  rangeKnob.setBounds(
      knobStrip.removeFromLeft(knobWidth).withSizeKeepingCentre(80, 80));
  vibratoKnob.setBounds(
      knobStrip.removeFromLeft(knobWidth).withSizeKeepingCentre(80, 80));
  sibilantsKnob.setBounds(
      knobStrip.removeFromLeft(knobWidth).withSizeKeepingCentre(80, 80));
  breathKnob.setBounds(
      knobStrip.removeFromLeft(knobWidth).withSizeKeepingCentre(80, 80));
}

void CanaryVoiceTuneAudioProcessorEditor::timerCallback() {
  float pitch = audioProcessor.currentDetectedPitch.load();
  if (pianoKeyboard.updateDetectedPitch(pitch)) {
    pianoKeyboard.repaint();
  }
}
