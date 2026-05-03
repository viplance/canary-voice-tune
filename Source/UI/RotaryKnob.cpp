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

    // Track
    g.setColour (juce::Colour::fromRGB(40, 42, 50));
    g.fillEllipse (rx, ry, rw, rw);

    g.setColour (juce::Colour::fromRGB(25, 27, 32));
    g.drawEllipse (rx, ry, rw, rw, 2.0f);

    // Fill
    juce::Path p;
    auto pointerLength = radius * 0.33f;
    auto pointerThickness = 3.0f;
    p.addRectangle (-pointerThickness * 0.5f, -radius, pointerThickness, pointerLength);
    p.applyTransform (juce::AffineTransform::rotation (angle).translated (centreX, centreY));

    // Glow / highlight
    g.setColour (juce::Colour::fromRGB(0, 200, 255));
    g.fillPath (p);

    // Outline arc
    juce::Path arc;
    arc.addCentredArc(centreX, centreY, radius - 2.0f, radius - 2.0f, 0.0f, rotaryStartAngle, angle, true);
    g.strokePath(arc, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

RotaryKnob::RotaryKnob()
{
    setLookAndFeel (&lf);
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 20);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxTextColourId, juce::Colours::black);
}

RotaryKnob::~RotaryKnob()
{
    setLookAndFeel (nullptr);
}
