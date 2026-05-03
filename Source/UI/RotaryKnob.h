#pragma once

#include <JuceHeader.h>

class ModernRotaryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ModernRotaryLookAndFeel();
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, const float rotaryStartAngle, const float rotaryEndAngle,
                           juce::Slider& slider) override;
};

class RotaryKnob : public juce::Slider
{
public:
    RotaryKnob();
    ~RotaryKnob() override;

private:
    ModernRotaryLookAndFeel lf;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RotaryKnob)
};
