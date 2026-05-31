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
  vibratoAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "VIBRATO", vibratoKnob);
  exciterAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "EXCITER", exciterKnob);
  sibilantsAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "SIBILANTS", sibilantsKnob);
  breathAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "BREATH", breathKnob);
  popAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "POP", popKnob);
  tuningModeAttachment =
      std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
          audioProcessor.apvts, "TUNING_MODE", tuningModeSelector);

  attackKnob.setTextValueSuffix(" ms");
  releaseKnob.setTextValueSuffix(" ms");
  vibratoKnob.setTextValueSuffix(" st");
  exciterKnob.setTextValueSuffix(" dB");
  sibilantsKnob.setTextValueSuffix(" dB");
  breathKnob.setTextValueSuffix(" dB");
  popKnob.setTextValueSuffix(" dB");
  popKnob.setLampIntensity(0.0f);
  breathKnob.setLampIntensity(0.0f);

  tuningModeSelector.onValueChange = [this]() {
      repaint();
  };

  addAndMakeVisible(tuningModeSelector);
  addAndMakeVisible(attackKnob);
  addAndMakeVisible(releaseKnob);
  addAndMakeVisible(vibratoKnob);
  addAndMakeVisible(exciterKnob);
  addAndMakeVisible(sibilantsKnob);
  addAndMakeVisible(breathKnob);
  addAndMakeVisible(popKnob);
  addAndMakeVisible(pianoKeyboard);

  setSize(1280, 390);
  startTimerHz(60);

  pianoKeyboard.onKeyClicked = [this](float freq) {
    audioProcessor.playPreviewTone(freq);
  };

  for (int i = 0; i < 88; ++i)
    audioProcessor.apvts.addParameterListener("KEY_" + juce::String(i), this);
}

CanaryVoiceTuneAudioProcessorEditor::~CanaryVoiceTuneAudioProcessorEditor() {
  for (int i = 0; i < 88; ++i)
    audioProcessor.apvts.removeParameterListener("KEY_" + juce::String(i), this);
}

void CanaryVoiceTuneAudioProcessorEditor::parameterChanged(const juce::String &parameterID, float newValue) {
  juce::ignoreUnused(newValue);
  if (parameterID.startsWith("KEY_")) {
    juce::MessageManager::callAsync([this]() {
      pianoKeyboard.repaint();
    });
  }
}

void CanaryVoiceTuneAudioProcessorEditor::paint(juce::Graphics &g) {
  float w = (float) getWidth();
  float h = (float) getHeight();

  // 1. Soft milk-cream to pastel-yellow background gradient (warm, visually light and pleasing)
  juce::ColourGradient bgGrad (
      juce::Colour::fromRGB(255, 254, 245), 0.0f, 0.0f,
      juce::Colour::fromRGB(250, 245, 232), w, h,
      false);
  g.setGradientFill(bgGrad);
  g.fillAll();

  // 2. Soft, diffuse glowing light from the inside (radial glow centered at bottom-middle)
  juce::ColourGradient glowGrad (
      juce::Colour::fromRGB(255, 255, 255).withAlpha(0.65f), w * 0.5f, h * 0.8f,
      juce::Colour::fromRGB(255, 255, 255).withAlpha(0.0f), w * 0.5f, h * 0.8f + w * 0.4f,
      true);
  g.setGradientFill(glowGrad);
  g.fillAll();

  // 3. Flowing translucent feather-wave layers (canary plumage contours / silk ribbons)
  auto drawPlumageWave = [&](float yStart, float c1y, float c2y, float yEnd, float opacity) {
      juce::Path wave;
      wave.startNewSubPath(0.0f, yStart);
      wave.cubicTo(w * 0.25f, c1y, w * 0.65f, c2y, w, yEnd);
      wave.lineTo(w, h);
      wave.lineTo(0.0f, h);
      wave.closeSubPath();
      
      juce::ColourGradient waveGrad (
          juce::Colour::fromRGB(255, 255, 255).withAlpha(opacity), 0.0f, yStart,
          juce::Colour::fromRGB(245, 235, 215).withAlpha(0.02f), w, yEnd,
          false);
      g.setGradientFill(waveGrad);
      g.fillPath(wave);
  };
  
  // Render layered waves for three-dimensional plumage depth
  drawPlumageWave(h * 0.55f, h * 0.40f, h * 0.70f, h * 0.45f, 0.07f);
  drawPlumageWave(h * 0.65f, h * 0.55f, h * 0.80f, h * 0.50f, 0.05f);
  drawPlumageWave(h * 0.75f, h * 0.70f, h * 0.90f, h * 0.60f, 0.03f);

  // 4. Elegant golden feather silhouettes in the top-left corner
  auto drawFeather = [&](juce::Point<float> p0, juce::Point<float> p1, 
                         juce::Point<float> p2, juce::Point<float> p3, 
                         float alphaQuill, float alphaBarbs) {
      
      auto getBezierPoint = [](juce::Point<float> pt0, juce::Point<float> pt1, 
                               juce::Point<float> pt2, juce::Point<float> pt3, float t) {
          float u = 1.0f - t;
          return pt0 * (u * u * u) + pt1 * (3.0f * u * u * t) + pt2 * (3.0f * u * t * t) + pt3 * (t * t * t);
      };
      
      auto getBezierTangent = [](juce::Point<float> pt0, juce::Point<float> pt1, 
                                 juce::Point<float> pt2, juce::Point<float> pt3, float t) {
          float u = 1.0f - t;
          return (pt1 - pt0) * (3.0f * u * u) + (pt2 - pt1) * (6.0f * u * t) + (pt3 - pt2) * (3.0f * t * t);
      };
      
      juce::Path quill;
      quill.startNewSubPath(p0);
      quill.cubicTo(p1, p2, p3);
      g.setColour(juce::Colour::fromRGB(220, 175, 75).withAlpha(alphaQuill));
      g.strokePath(quill, juce::PathStrokeType(1.2f));
      
      juce::Path barbs;
      for (float t = 0.04f; t < 0.96f; t += 0.016f) {
          auto pt = getBezierPoint(p0, p1, p2, p3, t);
          auto tangent = getBezierTangent(p0, p1, p2, p3, t);
          float len = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y);
          if (len > 0.0f) tangent /= len;
          
          juce::Point<float> leftNormal (-tangent.y, tangent.x);
          juce::Point<float> rightNormal (tangent.y, -tangent.x);
          
          // Outer profile tapers at both ends
          float baseWidth = 60.0f * std::sin(t * juce::MathConstants<float>::pi);
          
          // Left barb (curves gently upwards toward the feather tip)
          juce::Point<float> leftEnd = pt + leftNormal * baseWidth + tangent * (baseWidth * 0.35f);
          juce::Point<float> leftControl = pt + leftNormal * (baseWidth * 0.5f) + tangent * (baseWidth * 0.08f);
          barbs.startNewSubPath(pt);
          barbs.quadraticTo(leftControl, leftEnd);
          
          // Right barb
          juce::Point<float> rightEnd = pt + rightNormal * baseWidth * 0.85f + tangent * (baseWidth * 0.3f);
          juce::Point<float> rightControl = pt + rightNormal * (baseWidth * 0.45f) + tangent * (baseWidth * 0.06f);
          barbs.startNewSubPath(pt);
          barbs.quadraticTo(rightControl, rightEnd);
      }
      g.setColour(juce::Colour::fromRGB(220, 175, 75).withAlpha(alphaBarbs));
      g.strokePath(barbs, juce::PathStrokeType(0.7f));
  };

  // Draw two overlapping procedural golden feathers curving from top-left
  drawFeather(juce::Point<float>(20.0f, -10.0f), 
              juce::Point<float>(90.0f, 25.0f), 
              juce::Point<float>(170.0f, 75.0f), 
              juce::Point<float>(220.0f, 150.0f), 
              0.15f, 0.07f);
              
  drawFeather(juce::Point<float>(-10.0f, 30.0f), 
              juce::Point<float>(50.0f, 60.0f), 
              juce::Point<float>(110.0f, 100.0f), 
              juce::Point<float>(145.0f, 160.0f), 
              0.10f, 0.04f);

  // 5. Soft Glassmorphism Control Board (Frosted glass panel behind knobs)
  juce::Rectangle<float> glassBoard (12.0f, 48.0f, w - 24.0f, 136.0f);
  
  // Soft drop shadow
  g.setColour(juce::Colour::fromRGB(115, 95, 70).withAlpha(0.06f));
  g.fillRoundedRectangle(glassBoard.translated(0.0f, 2.5f), 12.0f);
  
  // Translucent frosted glass fill
  g.setColour(juce::Colours::white.withAlpha(0.40f));
  g.fillRoundedRectangle(glassBoard, 12.0f);
  
  // Bright frosted bevel border
  g.setColour(juce::Colours::white.withAlpha(0.60f));
  g.drawRoundedRectangle(glassBoard, 12.0f, 1.2f);

  // 6. Header Title Render (Frosted golden text)
  g.setColour(juce::Colour::fromRGB(95, 80, 65)); // Warm golden bronze
  g.setFont(juce::Font(juce::FontOptions("Outfit", 21.0f, juce::Font::bold)));
  g.drawFittedText(juce::String("Canary Voice Tune v") + PLUGIN_VERSION,
                   juce::Rectangle<int>(0, 0, getWidth(), 44),
                   juce::Justification::centred, 1);



  // Labels sit right above each knob in soft golden-charcoal text
  g.setFont(juce::Font(juce::FontOptions("Outfit", 12.0f, juce::Font::plain)));
  g.setColour(juce::Colour::fromRGB(105, 90, 75)); // Warm gold-charcoal

  auto getLabelBounds = [](juce::Component &comp) {
    return comp.getBounds().withY(comp.getY() - 22).withHeight(22).expanded(20, 0);
  };

  bool isClassic = audioProcessor.apvts.getRawParameterValue("TUNING_MODE")->load() > 0.5f;
  if (! isClassic) {
      g.setColour(juce::Colour::fromRGB(180, 120, 10)); // Active golden-bronze
  } else {
      g.setColour(juce::Colour::fromRGB(145, 135, 120)); // Inactive grey-bronze
  }
  g.drawText("Modern", getLabelBounds(tuningModeSelector),
             juce::Justification::centredBottom, false);

  g.setColour(juce::Colour::fromRGB(105, 90, 75)); // Restore warm gold-charcoal for other labels
  g.drawText("Attack", getLabelBounds(attackKnob),
             juce::Justification::centredBottom, false);
  g.drawText("Release", getLabelBounds(releaseKnob),
             juce::Justification::centredBottom, false);
  g.drawText("Vibrato", getLabelBounds(vibratoKnob),
             juce::Justification::centredBottom, false);
  g.drawText("Exciter", getLabelBounds(exciterKnob),
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
  auto knobStrip = area.removeFromTop(150);

  // Piano keyboard fills all remaining space at the bottom
  pianoKeyboard.setBounds(area.reduced(12));

  // Labels are drawn in paint() at knob.getBounds().translated(0, -22)
  auto labelRow = knobStrip.removeFromTop(22); // reserved for painted labels
  (void)labelRow;
  int controlWidth = knobStrip.getWidth() / 8;

  tuningModeSelector.setBounds(
      knobStrip.removeFromLeft(controlWidth).withSizeKeepingCentre(80, 80));
  attackKnob.setBounds(
      knobStrip.removeFromLeft(controlWidth).withSizeKeepingCentre(80, 80));
  releaseKnob.setBounds(
      knobStrip.removeFromLeft(controlWidth).withSizeKeepingCentre(80, 80));
  vibratoKnob.setBounds(
      knobStrip.removeFromLeft(controlWidth).withSizeKeepingCentre(80, 80));
  exciterKnob.setBounds(
      knobStrip.removeFromLeft(controlWidth).withSizeKeepingCentre(80, 80));
  sibilantsKnob.setBounds(
      knobStrip.removeFromLeft(controlWidth).withSizeKeepingCentre(80, 80));
  breathKnob.setBounds(
      knobStrip.removeFromLeft(controlWidth).withSizeKeepingCentre(80, 80));
  popKnob.setBounds(
      knobStrip.removeFromLeft(controlWidth).withSizeKeepingCentre(80, 80));
}

void CanaryVoiceTuneAudioProcessorEditor::timerCallback() {
  updateKeyboardFromAudioRing();
  updatePopLamp();
  updateBreathLamp();
}


void CanaryVoiceTuneAudioProcessorEditor::updateKeyboardFromAudioRing() {
  // popLatestNoteEvent returns -2 when the ring is empty (no change since
  // last tick), -1 when the singer is silent, or 0..87 for a real note.
  int latest = audioProcessor.popLatestNoteEvent();
  if (latest == -2) return;

  // 0 Hz tells the keyboard to clear its highlight. Use float arithmetic for
  // the semitone -> Hz conversion: integer division would collapse all notes
  // within an octave to the same A.
  float hz = (latest >= 0)
                 ? 440.0f * std::pow(2.0f, (float)((latest + 21) - 69) / 12.0f)
                 : 0.0f;
  if (pianoKeyboard.updateDetectedPitch(hz))
    pianoKeyboard.repaint();
}

void CanaryVoiceTuneAudioProcessorEditor::updatePopLamp() {
  // Asymmetric smoothing: lamp lights up immediately when a pop is detected
  // (a=0.6 ≈ ~50 ms rise) but decays gently after (a=0.15 ≈ ~200 ms fall).
  float target  = audioProcessor.getPopActivity();
  float current = popKnob.getLampIntensity();
  float a = (target > current) ? 0.6f : 0.15f;
  popKnob.setLampIntensity(current + a * (target - current));
}

void CanaryVoiceTuneAudioProcessorEditor::updateBreathLamp() {
  // Asymmetric smoothing: lamp lights up immediately when a breath is gated
  // (a=0.6 ≈ ~50 ms rise) but decays gently after (a=0.15 ≈ ~200 ms fall).
  float target  = audioProcessor.getBreathActivity();
  float current = breathKnob.getLampIntensity();
  float a = (target > current) ? 0.6f : 0.15f;
  breathKnob.setLampIntensity(current + a * (target - current));
}
