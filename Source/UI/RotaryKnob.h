#pragma once

#include <JuceHeader.h>

class ModernRotaryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ModernRotaryLookAndFeel();
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, const float rotaryStartAngle, const float rotaryEndAngle,
                           juce::Slider& slider) override;
    juce::Font getLabelFont (juce::Label& label) override;
};

class RotaryKnob : public juce::Slider
{
public:
    RotaryKnob();
    ~RotaryKnob() override;

    // Optional indicator lamp drawn at the knob centre. Intensity is in [0,1]:
    //   0 = idle (dim grey diode)
    //   1 = fully lit (bright red)
    // Pass any value in between for smooth fade. Setting <0 disables the lamp
    // (the default — most knobs don't have one).
    void setLampIntensity(float intensity);
    bool hasLamp() const { return lampEnabled; }
    float getLampIntensity() const { return lampIntensity; }

    void paint(juce::Graphics& g) override;

private:
    ModernRotaryLookAndFeel lf;
    bool  lampEnabled  = false;
    float lampIntensity = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RotaryKnob)
};
