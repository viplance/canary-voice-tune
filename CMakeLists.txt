cmake_minimum_required(VERSION 3.20)

file(READ "package.json" PACKAGE_JSON)
string(JSON PKG_VERSION GET "${PACKAGE_JSON}" "version")

project(CanaryVoiceTune VERSION ${PKG_VERSION})

# Use locally pre-downloaded JUCE from vendor/JUCE
# The build script downloads it via curl before running cmake
add_subdirectory(vendor/JUCE)

# Pre-create artifact directories that juceaide needs at configure time
# (workaround for JUCE develop branch bug where it doesn't mkdir before writing Info.plist)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/CanaryVoiceTune_artefacts/JuceLibraryCode/CanaryVoiceTune_VST3")

juce_add_plugin(CanaryVoiceTune
    COMPANY_NAME "SharkoAudio"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Shrk
    PLUGIN_CODE Flvt
    FORMATS VST3
    PRODUCT_NAME "CanaryVoiceTune")

target_sources(CanaryVoiceTune PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/UI/PianoKeyboard.cpp
    Source/UI/PianoKeyboard.h
    Source/UI/RotaryKnob.cpp
    Source/UI/RotaryKnob.h
    Source/DSP/PitchDetector.cpp
    Source/DSP/PitchDetector.h
    Source/DSP/PitchShifter.cpp
    Source/DSP/PitchShifter.h
)

# Generate JuceHeader.h — this creates the auto-generated header and adds
# the output dir to the target's include search paths automatically.
juce_generate_juce_header(CanaryVoiceTune)

target_compile_definitions(CanaryVoiceTune PUBLIC
    # JUCE_WEB_BROWSER and JUCE_USE_CURL would be on by default, but you might not need them.
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(CanaryVoiceTune PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_plugin_client
    juce::juce_gui_extra
    juce::juce_gui_basics
    juce::juce_graphics
    juce::juce_events
    juce::juce_core
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
)

set_target_properties(CanaryVoiceTune PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

if (APPLE)
    set_target_properties(CanaryVoiceTune PROPERTIES
        OSX_ARCHITECTURES "arm64"
    )
endif()
