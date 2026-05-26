#include "RotaryKnob.h"

ModernRotaryLookAndFeel::ModernRotaryLookAndFeel()
{
}

void ModernRotaryLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                                float sliderPos, const float rotaryStartAngle, const float rotaryEndAngle,
                                                juce::Slider& slider)
{
    auto radius = (float) juce::jmin (width / 2, height / 2) - 4.0f;
    auto centreX = (float) x + (float) width  * 0.5f;
    auto centreY = (float) y + (float) height * 0.5f;
    auto rx = centreX - radius;
    auto ry = centreY - radius;
    auto rw = radius * 2.0f;
    auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Premium Neo-Skeuomorphic cream/beige knob face
    juce::ColourGradient knobFace (
        juce::Colour::fromRGB(255, 255, 255), centreX - radius * 0.4f, centreY - radius * 0.4f,
        juce::Colour::fromRGB(222, 215, 198), centreX + radius * 0.7f, centreY + radius * 0.7f,
        true);
    g.setGradientFill(knobFace);
    g.fillEllipse(rx, ry, rw, rw);

    // Bevel highlights & shadow borders for visual depth
    g.setColour(juce::Colours::white.withAlpha(0.85f));
    g.drawEllipse(rx + 0.5f, ry + 0.5f, rw - 1.0f, rw - 1.0f, 1.0f);
    g.setColour(juce::Colour::fromRGB(185, 177, 160).withAlpha(0.6f));
    g.drawEllipse(rx, ry, rw, rw, 1.5f);

    // Dynamic golden pointer dot (elegant and clean)
    float dotRadius = 2.5f;
    float dotDist = radius - 7.0f;
    float dotX = centreX + std::cos(angle - juce::MathConstants<float>::halfPi) * dotDist;
    float dotY = centreY + std::sin(angle - juce::MathConstants<float>::halfPi) * dotDist;
    g.setColour(juce::Colour::fromRGB(205, 150, 20)); // Canary-gold
    g.fillEllipse(dotX - dotRadius, dotY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);

    // Outer progress arc (shows current value level in soft gold)
    juce::Path arc;
    arc.addCentredArc(centreX, centreY, radius - 3.5f, radius - 3.5f, 0.0f, rotaryStartAngle, angle, true);
    g.setColour(juce::Colour::fromRGB(220, 165, 30));
    g.strokePath(arc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

RotaryKnob::RotaryKnob()
{
    setLookAndFeel (&lf);
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 20);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxTextColourId, juce::Colour::fromRGB(95, 80, 65)); // Warm gold-bronze
}

RotaryKnob::~RotaryKnob()
{
    setLookAndFeel (nullptr);
}

void RotaryKnob::setLampIntensity(float intensity)
{
    if (intensity < 0.0f) {
        // Negative => disable lamp entirely.
        if (lampEnabled || lampIntensity != 0.0f) {
            lampEnabled = false;
            lampIntensity = 0.0f;
            repaint();
        }
        return;
    }
    if (! lampEnabled) lampEnabled = true;
    if (intensity > 1.0f) intensity = 1.0f;
    if (std::abs(intensity - lampIntensity) > 0.005f) {
        lampIntensity = intensity;
        repaint();
    }
}

void RotaryKnob::paint(juce::Graphics& g)
{
    juce::Slider::paint(g);
    if (! lampEnabled) return;

    // Place a small disk roughly at the visual centre of the knob face.
    auto bounds = getLocalBounds();
    auto textHeight = getTextBoxHeight();
    auto knobArea = bounds.withTrimmedBottom(textHeight);
    float cx = (float) knobArea.getCentreX();
    float cy = (float) knobArea.getCentreY();
    float r  = 4.5f;

    // Smooth grey -> red interpolation.
    juce::Colour idle  (90, 90, 95);
    juce::Colour hot   (255, 50, 40);
    juce::Colour body  = idle.interpolatedWith(hot, lampIntensity);

    // Soft outer glow when lit
    if (lampIntensity > 0.05f) {
        juce::Colour glow = hot.withAlpha(0.35f * lampIntensity);
        g.setColour(glow);
        g.fillEllipse(cx - r * 2.4f, cy - r * 2.4f, r * 4.8f, r * 4.8f);
    }

    g.setColour(body);
    g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
    // Subtle dark outline so the lamp reads on either knob colour.
    g.setColour(juce::Colour::fromRGB(20, 22, 26));
    g.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f, 1.0f);
}
