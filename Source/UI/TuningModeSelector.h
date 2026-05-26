#pragma once

#include <JuceHeader.h>

class TuningModeSelector : public juce::Slider
{
public:
    TuningModeSelector()
    {
        setSliderStyle(juce::Slider::LinearVertical);
        setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        setRange(0.0, 1.0, 1.0); // Range 0 (Modern) to 1 (Classic), step 1
    }

    ~TuningModeSelector() override = default;

    void mouseDown (const juce::MouseEvent& e) override
    {
        juce::ignoreUnused (e);
        double current = getValue();
        setValue (current > 0.5 ? 0.0 : 1.0, juce::sendNotificationSync);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        juce::ignoreUnused (e);
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        float w = (float)bounds.getWidth();
        float h = (float)bounds.getHeight();
        float cx = w * 0.5f;

        // Current slider value (0.0 = Modern, 1.0 = Classic)
        double v = getValue();

        // 1. Draw the recessed capsule-shaped slot track
        // Centered vertically at cy = 30.0f to match the rotary knobs exactly
        float trackW = 24.0f;
        float trackH = 44.0f; // Fits from y = 8.0f to y = 52.0f (center cy = 30.0f)
        float trackX = cx - trackW * 0.5f;
        float trackY = 8.0f;
        
        // Recessed slot background in disabled key grey
        g.setColour(juce::Colour::fromRGB(215, 208, 194));
        g.fillRoundedRectangle(trackX, trackY, trackW, trackH, trackW * 0.5f);
        
        // Slot inner border/recess shadow
        g.setColour(juce::Colour::fromRGB(175, 168, 154));
        g.drawRoundedRectangle(trackX, trackY, trackW, trackH, trackW * 0.5f, 1.2f);

        // 2. Interpolate thumb Y position (0.0 -> top, 1.0 -> bottom)
        float topY = trackY + trackW * 0.5f; // y = 8 + 12 = 20.0f
        float bottomY = trackY + trackH - trackW * 0.5f; // y = 8 + 44 - 12 = 40.0f
        float thumbY = topY + (float)v * (bottomY - topY);

        // 3. Draw the circular switch handle
        float tw = 24.0f;
        float th = 24.0f;
        float tx = cx - tw * 0.5f;
        float ty = thumbY - th * 0.5f;

        // Drop shadow under the circular handle
        g.setColour(juce::Colour::fromRGB(0, 0, 0).withAlpha(0.18f));
        g.fillEllipse(tx, ty + 1.5f, tw, th);

        // Circular skeuomorphic handle gradient fill (cream/beige)
        juce::ColourGradient handleGrad(
            juce::Colour::fromRGB(255, 255, 255), cx - 6.0f, thumbY - 6.0f,
            juce::Colour::fromRGB(222, 215, 198), cx + 9.0f, thumbY + 9.0f,
            true);
        g.setGradientFill(handleGrad);
        g.fillEllipse(tx, ty, tw, th);

        // Bevel highlights & outer borders
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawEllipse(tx + 0.5f, ty + 0.5f, tw - 1.0f, th - 1.0f, 1.0f);
        g.setColour(juce::Colour::fromRGB(185, 177, 160).withAlpha(0.6f));
        g.drawEllipse(tx, ty, tw, th, 1.2f);

        // Elegant golden pointer dot in the center
        float dotRadius = 2.5f;
        g.setColour(juce::Colour::fromRGB(205, 150, 20)); // Canary gold
        g.fillEllipse(cx - dotRadius, thumbY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);

        // 4. Draw label: "Classic" at the bottom to match the other knob values
        g.setFont(juce::Font("Outfit", 12.0f, juce::Font::plain));

        if (v >= 0.5) {
            g.setColour(juce::Colour::fromRGB(180, 120, 10)); // Active golden-bronze
        } else {
            g.setColour(juce::Colour::fromRGB(145, 135, 120)); // Inactive grey-bronze
        }
        g.drawText("Classic", 0, 60, (int)w, 20, juce::Justification::centred, false);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TuningModeSelector)
};
