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
  popAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "POP", popKnob);

  attackKnob.setTextValueSuffix(" ms");
  releaseKnob.setTextValueSuffix(" ms");
  rangeKnob.setTextValueSuffix(" %");
  vibratoKnob.setTextValueSuffix(" %");
  sibilantsKnob.setTextValueSuffix(" dB");
  breathKnob.setTextValueSuffix(" dB");
  popKnob.setTextValueSuffix(" dB");
  // Initialise the activity lamp on the Pop Filter knob (idle = grey diode).
  popKnob.setLampIntensity(0.0f);

  addAndMakeVisible(attackKnob);
  addAndMakeVisible(releaseKnob);
  addAndMakeVisible(rangeKnob);
  addAndMakeVisible(vibratoKnob);
  addAndMakeVisible(sibilantsKnob);
  addAndMakeVisible(breathKnob);
  addAndMakeVisible(popKnob);
  addAndMakeVisible(pianoKeyboard);

  setSize(1280, 350);
  // Higher refresh so short notes (30–80 ms) aren't missed between UI ticks
  // — the audio thread can change `currentDetectedPitch` many times per
  // 33 ms frame and we only ever read the latest, so a coarse timer can
  // visually skip notes in fast melodic passages.
  startTimerHz(60);

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
  g.drawText("Pop Filter", getLabelBounds(popKnob),
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
  int knobWidth = knobStrip.getWidth() / 7;

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
  popKnob.setBounds(
      knobStrip.removeFromLeft(knobWidth).withSizeKeepingCentre(80, 80));
}

void CanaryVoiceTuneAudioProcessorEditor::timerCallback() {
  // Drain the audio thread's note-change ring. We highlight the LATEST note
  // (so the keyboard always reflects what's playing right now), but by
  // walking the ring we make sure that even if multiple distinct notes were
  // pushed between two ticks, the UI saw them all (matters in case anything
  // downstream cares — repaint is cheap and idempotent).
  int writeIdx = audioProcessor.noteHistoryWriteIdx.load(std::memory_order_acquire);
  int latestNote = -2; // sentinel
  while (audioProcessor.noteHistoryReadIdx != writeIdx) {
    latestNote = audioProcessor.noteHistory[audioProcessor.noteHistoryReadIdx]
                     .load(std::memory_order_acquire);
    audioProcessor.noteHistoryReadIdx =
        (audioProcessor.noteHistoryReadIdx + 1) %
        CanaryVoiceTuneAudioProcessor::kNoteHistorySize;
  }
  if (latestNote != -2) {
    // -1 means "no voiced note now" — pass 0 Hz so the keyboard clears.
    // NB: cast to float, otherwise the (latestNote + 21 - 69) / 12 division
    // is integer and rounds to whole octaves — every note within an octave
    // would otherwise be reported as the same A.
    float hz = (latestNote >= 0)
                   ? 440.0f * std::pow(2.0f, (float)((latestNote + 21) - 69) / 12.0f)
                   : 0.0f;
    if (pianoKeyboard.updateDetectedPitch(hz)) {
      pianoKeyboard.repaint();
    }
  }

  // Pop Filter lamp: smoothly interpolate UI intensity toward the actual
  // ducking activity. The DSP-side popActivity already follows an asymmetric
  // attack/release on gain reduction, so we just need a light extra smoothing
  // to make the lamp glow rather than flicker between frames.
  float target = audioProcessor.getPopActivity();
  // Fast rise (visible immediately when a pop hits), slower fall (gentle decay).
  float current = popKnob.getLampIntensity();
  float a = (target > current) ? 0.6f : 0.15f;
  popKnob.setLampIntensity(current + a * (target - current));
}
