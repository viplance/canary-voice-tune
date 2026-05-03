# CanaryVoiceTune

A high-performance VST3 audio plugin for MacOS M-Series (Apple Silicon) to perform transparent voice pitch correction.

## Features

- **Real-Time Pitch Detection**: Continuously infers human voice pitch and highlights the respective key on the UI.
- **Interactive Piano Keyboard GUI**: Define tuning scales easily by toggling keys on/off with a single click.
- **Modern Parameter UI**: Fine-tune pitch snapping with Attack, Release, and Accuracy controllers.
- **Apple Silicon Native**: Completely built for `arm64` macs, using CMake and JUCE 8.

## Building for MacOS (Apple Silicon)

Requirements:

- macOS 11+
- CMake 3.20+
- Xcode Command Line Tools
- Homebrew (for easy dependency installation)

### 1. Install Build Prerequisites via Homebrew

If you don't have CMake, you can install it using Homebrew:

```bash
brew install cmake
```

### 2. Build the Plugin

You can build the plugin easily by using the provided `npm` script if you use Node:

```bash
npm run build
```

Alternatively, you can compile manually:

```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

Once built, the `.vst3` file will be generated in `build/CanaryVoiceTune_artefacts/Release/VST3/CanaryVoiceTune.vst3`. Copy this bundle to `/Library/Audio/Plug-Ins/VST3/` or `~/Library/Audio/Plug-Ins/VST3/` to use it in Logic Pro, Ableton Live, Reaper, or other DAWs.

## Architecture Description

**GUI / UI Components**

- Handled primarily by `PluginEditor` and `AudioProcessorValueTreeState` (APVTS).
- Features a vector-painted custom `RotaryKnob` Component with a glowing `ModernRotaryLookAndFeel`.
- The `PianoKeyboard` Component manages scale states through an APVTS connection, reading incoming detected pitches to display live visual cues using 30 FPS timers.

**DSP Engine**

- `PitchDetector` captures audio samples, processing them locally (via zero-crossing/Yin mechanisms) and sets an atomic variable that the UI can read without locking the audio thread.
- `PitchShifter` performs the real-time DSP payload, smoothing corrections utilizing settings defined by the user (Attack, Release, Accuracy).
