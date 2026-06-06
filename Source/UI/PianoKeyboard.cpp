#include "PianoKeyboard.h"
#include <cmath>

namespace {
// Build a path for a key rectangle with only its BOTTOM corners rounded, like a
// real piano key. The top stays square so adjacent keys/labels still line up.
juce::Path makeBottomRoundedKey(juce::Rectangle<float> r, float radius)
{
    radius = juce::jmin(radius, r.getWidth() * 0.5f, r.getHeight() * 0.5f);
    juce::Path p;
    if (radius < 0.5f) { p.addRectangle(r); return p; }
    p.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight(),
                          radius, radius,
                          /*topLeft*/    false, /*topRight*/    false,
                          /*bottomLeft*/ true,  /*bottomRight*/ true);
    return p;
}
} // namespace

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

    // Reserve a strip at the top for octave-number labels; keys start below it.
    int top = kLabelStripH;
    int keysHeight = getHeight() - top;

    if (!isBlackKey(note))
    {
        int whiteIdx = getWhiteKeyIndex(note);
        return juce::Rectangle<int>(std::round(whiteIdx * whiteKeyWidth), top, std::round(whiteKeyWidth), keysHeight);
    }
    else
    {
        int whiteIdxBefore = getWhiteKeyIndex(note);
        float blackKeyWidth = whiteKeyWidth * 0.6f;
        float blackKeyHeight = keysHeight * 0.6f;
        float xPos = whiteIdxBefore * whiteKeyWidth - (blackKeyWidth * 0.5f);
        return juce::Rectangle<int>(std::round(xPos), top, std::round(blackKeyWidth), std::round(blackKeyHeight));
    }
}

juce::String PianoKeyboard::getNoteName(int note)
{
    // Letter only (no octave number); the octave number lives in the top strip.
    // note 0 = A0 (MIDI 21).
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#",
                                     "G", "G#", "A", "A#", "B" };
    int midi = note + 21;
    return juce::String(names[midi % 12]);
}

void PianoKeyboard::paint (juce::Graphics& g)
{
    // Draw the keyboard background as transparent/soft beige
    g.fillAll(juce::Colours::transparentBlack);

    // Octave-number strip across the top of the keyboard.
    drawOctaveStrip(g);

    // Draw white keys first
    for (int i = 0; i < 88; ++i)
    {
        if (isBlackKey(i)) continue;

        auto bounds = getKeyBounds(i);
        auto param = apvts.getParameter("KEY_" + juce::String(i));
        bool enabled = param ? param->getValue() > 0.5f : true;

        auto keyPath = makeBottomRoundedKey(bounds.toFloat(), kWhiteKeyCorner);

        juce::ColourGradient keyGrad (
            juce::Colours::white, 0.0f, (float)bounds.getY(),
            juce::Colour::fromRGB(246, 240, 226), 0.0f, (float)bounds.getBottom(),
            false);

        juce::Colour keyColor = enabled ? juce::Colour::fromRGB(250, 246, 234) : juce::Colour::fromRGB(189, 182, 168);

        g.setColour(keyColor);
        if (enabled) {
            g.setGradientFill(keyGrad);
        }
        g.fillPath(keyPath);

        // Highlight currently detected note with Canary Gold glow
        if (i == currentlyDetectedNote && enabled) {
            juce::ColourGradient highlightGrad (
                juce::Colour::fromRGB(255, 210, 40), 0.0f, (float)bounds.getY(),
                juce::Colour::fromRGB(235, 155, 10), 0.0f, (float)bounds.getBottom(),
                false);
            g.setGradientFill(highlightGrad);
            g.fillPath(keyPath);
        }

        // Clearly mark a DISABLED (out-of-scale) white key: dim it further and
        // draw a small muted dot near the top so it reads as "off" even when
        // the tint difference is subtle on bright displays.
        if (!enabled) {
            g.setColour(juce::Colours::black.withAlpha(0.10f));
            g.fillPath(keyPath);
            float d = juce::jmin(6.0f, bounds.getWidth() * 0.28f);
            g.setColour(juce::Colour::fromRGB(150, 142, 128));
            g.fillEllipse(bounds.getCentreX() - d * 0.5f, bounds.getY() + 6.0f, d, d);
        }

        // Draw soft, premium warm-grey white-key border
        g.setColour(juce::Colour::fromRGB(205, 195, 178));
        g.strokePath(keyPath, juce::PathStrokeType(1.0f));

        // Note name near the bottom of each white key.
        drawKeyLabel(g, bounds, getNoteName(i), enabled, false);
    }

    // Draw black keys
    for (int i = 0; i < 88; ++i)
    {
        if (!isBlackKey(i)) continue;

        auto bounds = getKeyBounds(i);
        auto param = apvts.getParameter("KEY_" + juce::String(i));
        bool enabled = param ? param->getValue() > 0.5f : true;

        auto keyPath = makeBottomRoundedKey(bounds.toFloat(), kBlackKeyCorner);

        juce::Colour keyColor = enabled ? juce::Colour::fromRGB(45, 41, 37) : juce::Colour::fromRGB(120, 112, 103);

        g.setColour(keyColor);
        g.fillPath(keyPath);

        // Highlight currently detected note with Canary Gold glow
        if (i == currentlyDetectedNote && enabled) {
            juce::ColourGradient highlightGrad (
                juce::Colour::fromRGB(255, 210, 40), 0.0f, (float)bounds.getY(),
                juce::Colour::fromRGB(235, 155, 10), 0.0f, (float)bounds.getBottom(),
                false);
            g.setGradientFill(highlightGrad);
            g.fillPath(keyPath);
        }

        // Elegant top-edge highlight on black keys (square top edge)
        if (enabled && i != currentlyDetectedNote) {
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.drawRect(bounds.withHeight(2), 1);
        }

        // Clearly mark a DISABLED (out-of-scale) black key: hatch overlay + dot.
        if (!enabled) {
            float d = juce::jmin(5.0f, bounds.getWidth() * 0.45f);
            g.setColour(juce::Colour::fromRGB(60, 55, 50));
            g.fillEllipse(bounds.getCentreX() - d * 0.5f, bounds.getY() + 4.0f, d, d);
        }

        g.setColour(juce::Colour::fromRGB(25, 22, 20));
        g.strokePath(keyPath, juce::PathStrokeType(1.0f));

        // Black keys are intentionally left unlabelled.
    }
}

void PianoKeyboard::drawOctaveStrip(juce::Graphics& g)
{
    auto strip = juce::Rectangle<int>(0, 0, getWidth(), kLabelStripH);

    // Soft strip background matching the warm-beige theme.
    g.setColour(juce::Colour::fromRGB(238, 231, 216));
    g.fillRect(strip);
    g.setColour(juce::Colour::fromRGB(205, 195, 178));
    g.drawLine(0.0f, (float)kLabelStripH, (float)getWidth(), (float)kLabelStripH, 1.0f);

    g.setFont(juce::Font(juce::FontOptions(juce::jlimit(11.0f, 15.0f, kLabelStripH * 0.78f),
                                           juce::Font::bold)));

    g.setColour(juce::Colour::fromRGB(120, 112, 98));

    // The keyboard starts at A0, so the first span (A0..B0, before the first C)
    // is the partial octave 0. Label it explicitly since the loop below only
    // starts a span at each C.
    {
        int firstC = getWidth();
        for (int j = 0; j < 88; ++j)
            if ((j + 21) % 12 == 0) { firstC = getKeyBounds(j).getX(); break; }
        // Align "0" to the very start, over the A0 key.
        auto a0 = getKeyBounds(0); // A0
        juce::Rectangle<int> labelArea(a0.getX() + 3, 0,
                                       juce::jmax(a0.getWidth(), firstC), kLabelStripH);
        g.drawText("0", labelArea, juce::Justification::centredLeft, false);
    }

    // For each octave, find its left edge (this C key) and right edge (the next
    // C key, or the keyboard end). Draw a divider at the left boundary and the
    // octave number centred in the middle of the span.
    for (int i = 0; i < 88; ++i)
    {
        if ((i + 21) % 12 != 0) continue; // start of an octave = C

        int left = getKeyBounds(i).getX();

        // Right edge = x of the next C, else the right end of the keyboard.
        int right = getWidth();
        for (int j = i + 1; j < 88; ++j)
            if ((j + 21) % 12 == 0) { right = getKeyBounds(j).getX(); break; }

        // Octave boundary divider.
        g.setColour(juce::Colour::fromRGB(205, 195, 178));
        g.drawLine((float)left, 0.0f, (float)left, (float)kLabelStripH, 1.0f);

        int octave = (i + 21) / 12 - 1;
        juce::Rectangle<int> labelArea(left, 0, right - left, kLabelStripH);
        g.setColour(juce::Colour::fromRGB(120, 112, 98));
        g.drawText(juce::String(octave), labelArea, juce::Justification::centred, false);
    }
}

void PianoKeyboard::drawKeyLabel(juce::Graphics& g, juce::Rectangle<int> bounds,
                                 const juce::String& name, bool enabled, bool blackKey)
{
    // Only label if the key is wide enough to read; otherwise it's just clutter.
    if (bounds.getWidth() < 11) return;

    float fontH = juce::jlimit(9.0f, 14.0f, bounds.getWidth() * 0.85f);
    g.setFont(juce::Font(juce::FontOptions(fontH, juce::Font::bold)));

    juce::Colour textColor = blackKey
        ? (enabled ? juce::Colours::white.withAlpha(0.78f) : juce::Colours::white.withAlpha(0.40f))
        : (enabled ? juce::Colour::fromRGB(120, 112, 98)   : juce::Colour::fromRGB(150, 143, 130));

    g.setColour(textColor);
    auto labelArea = bounds.withTrimmedBottom(3).removeFromBottom((int)fontH + 4);
    g.drawText(name, labelArea, juce::Justification::centredBottom, false);
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
