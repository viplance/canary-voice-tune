#include "PianoKeyboard.h"
#include <cmath>

PianoKeyboard::PianoKeyboard(juce::AudioProcessorValueTreeState& state) : apvts(state)
{
}

PianoKeyboard::~PianoKeyboard()
{
}

int PianoKeyboard::getNoteFromHz(float hz)
{
    if (hz <= 0.0f) return -1;
    // A4 = 440Hz = MIDI note 69
    int midiNote = static_cast<int>(std::round(69 + 12 * std::log2(hz / 440.0f)));
    if (midiNote < 21 || midiNote > 108) return -1;
    return midiNote - 21; // 0 to 87
}

bool PianoKeyboard::isBlackKey(int note)
{
    // A0 is note=0. MIDI 21. C=0, C#=1... in pitch class.
    int pitchClass = (note + 21) % 12;
    return pitchClass == 1 || pitchClass == 3 || pitchClass == 6 || pitchClass == 8 || pitchClass == 10;
}

int PianoKeyboard::getWhiteKeyIndex(int note)
{
    int count = 0;
    for (int i = 0; i < note; ++i)
    {
        if (!isBlackKey(i)) count++;
    }
    return count;
}

bool PianoKeyboard::updateDetectedPitch(float newPitchHz)
{
    int newNote = getNoteFromHz(newPitchHz);
    if (newNote != currentlyDetectedNote)
    {
        currentlyDetectedNote = newNote;
        return true;
    }
    return false;
}

juce::Rectangle<int> PianoKeyboard::getKeyBounds(int note)
{
    int numWhiteKeys = 52;
    float whiteKeyWidth = getWidth() / (float)numWhiteKeys;
    
    if (!isBlackKey(note))
    {
        int whiteIdx = getWhiteKeyIndex(note);
        return juce::Rectangle<int>(std::round(whiteIdx * whiteKeyWidth), 0, std::round(whiteKeyWidth), getHeight());
    }
    else
    {
        int whiteIdxBefore = getWhiteKeyIndex(note);
        float blackKeyWidth = whiteKeyWidth * 0.6f;
        float blackKeyHeight = getHeight() * 0.6f;
        float xPos = whiteIdxBefore * whiteKeyWidth - (blackKeyWidth * 0.5f);
        return juce::Rectangle<int>(std::round(xPos), 0, std::round(blackKeyWidth), std::round(blackKeyHeight));
    }
}

void PianoKeyboard::paint (juce::Graphics& g)
{
    g.fillAll(juce::Colour::fromRGB(255, 239, 0));

    // Draw white keys first
    for (int i = 0; i < 88; ++i)
    {
        if (isBlackKey(i)) continue;

        auto bounds = getKeyBounds(i);
        auto param = apvts.getParameter("KEY_" + juce::String(i));
        bool enabled = param ? param->getValue() > 0.5f : true;

        juce::Colour keyColor = enabled ? juce::Colours::white : juce::Colours::darkgrey;
        if (i == currentlyDetectedNote && enabled) keyColor = juce::Colour::fromRGB(0, 200, 255); // Highlight

        g.setColour(keyColor);
        g.fillRect(bounds);
        g.setColour(juce::Colours::black);
        g.drawRect(bounds, 1);
    }

    // Draw black keys
    for (int i = 0; i < 88; ++i)
    {
        if (!isBlackKey(i)) continue;

        auto bounds = getKeyBounds(i);
        auto param = apvts.getParameter("KEY_" + juce::String(i));
        bool enabled = param ? param->getValue() > 0.5f : true;

        juce::Colour keyColor = enabled ? juce::Colours::black : juce::Colours::darkgrey.darker();
        if (i == currentlyDetectedNote && enabled) keyColor = juce::Colour::fromRGB(0, 200, 255); // Highlight

        g.setColour(keyColor);
        g.fillRect(bounds);
        g.setColour(juce::Colours::white.withAlpha(0.2f));
        g.drawRect(bounds, 1);
    }
}

void PianoKeyboard::resized()
{
}

void PianoKeyboard::mouseDown (const juce::MouseEvent& e)
{
    // Find which key was clicked (check black keys first, then white keys)
    int clickedNote = -1;
    
    for (int i = 0; i < 88; ++i)
    {
        if (isBlackKey(i) && getKeyBounds(i).contains(e.getPosition()))
        {
            clickedNote = i;
            break;
        }
    }
    
    if (clickedNote == -1)
    {
        for (int i = 0; i < 88; ++i)
        {
            if (!isBlackKey(i) && getKeyBounds(i).contains(e.getPosition()))
            {
                clickedNote = i;
                break;
            }
        }
    }

    if (clickedNote != -1)
    {
        auto paramid = "KEY_" + juce::String(clickedNote);
        auto param = apvts.getParameter(paramid);
        if (param)
        {
            param->setValueNotifyingHost(param->getValue() > 0.5f ? 0.0f : 1.0f);
            repaint();
        }

        if (onKeyClicked)
        {
            float freq = 440.0f * std::pow(2.0f, (clickedNote + 21 - 69) / 12.0f);
            onKeyClicked(freq);
        }
    }
}
