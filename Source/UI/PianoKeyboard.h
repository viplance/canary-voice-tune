#pragma once

#include <JuceHeader.h>

class PianoKeyboard : public juce::Component
{
public:
    PianoKeyboard(juce::AudioProcessorValueTreeState& apvts);
    ~PianoKeyboard() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    
    // Returns true if the key highlight changed and requires a repaint
    bool updateDetectedPitch(float newPitchHz);

    std::function<void(float)> onKeyClicked;

private:
    juce::AudioProcessorValueTreeState& apvts;
    
    int getNoteFromHz(float hz);
    bool isBlackKey(int note);
    int getWhiteKeyIndex(int note);
    juce::Rectangle<int> getKeyBounds(int note);
    juce::String getNoteName(int note);
    void drawKeyLabel(juce::Graphics& g, juce::Rectangle<int> bounds,
                      const juce::String& name, bool enabled, bool blackKey);
    void drawOctaveStrip(juce::Graphics& g);

    // Height (px) of the octave-number strip above the keys.
    static constexpr int kLabelStripH = 18;

    // Bottom-corner rounding (px) of the keys, like a real keyboard. Top edges
    // stay square so adjacent keys still butt together cleanly.
    static constexpr float kWhiteKeyCorner = 3.0f;
    static constexpr float kBlackKeyCorner = 2.0f;

    int currentlyDetectedNote = -1; // 0 to 87

    // Sub-components could be used, but since we only need one octave, custom painting is simpler.
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoKeyboard)
};
