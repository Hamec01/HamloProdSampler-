cmake_minimum_required(VERSION 3.22)

project(HPSAMPLER VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(JUCE_TARGET_ARCHITECTURE "x86_64")

include(CTest)

set(HPSAMPLER_JUCE_DIR "C:/DRUMENGINE/JUCE" CACHE PATH "Path to local JUCE 8 checkout")
if (NOT EXISTS "${HPSAMPLER_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${HPSAMPLER_JUCE_DIR}. Set -DHPSAMPLER_JUCE_DIR=<path>.")
endif()

add_subdirectory("${HPSAMPLER_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# ----------------------------------------------------------------------------
# 1. HPBridgeCore — shared protocol, schema validator, file & HamloLink reader
# ----------------------------------------------------------------------------
add_library(HPBridgeCore STATIC
    HPBridgeCore/src/BridgeValidator.cpp
    HPBridgeCore/src/FileSnapshotTransport.cpp
    HPBridgeCore/src/HamloLinkReader.cpp
)
target_include_directories(HPBridgeCore PUBLIC HPBridgeCore/include)
target_link_libraries(HPBridgeCore PUBLIC juce::juce_core)

# ----------------------------------------------------------------------------
# 2. HPSAMPLER_Core — headless model, classical DSP/MIR, ChopEngine, FX, Audio
# ----------------------------------------------------------------------------
add_library(HPSAMPLER_Core STATIC
    src/model/SamplerModel.cpp
    src/analysis/ClassicalAnalyzer.cpp
    src/fx/FxLite.cpp
    src/audio/VoiceEngine.cpp
    src/persistence/StateSerializer.cpp
)
target_include_directories(HPSAMPLER_Core PUBLIC src)
target_link_libraries(HPSAMPLER_Core
    PUBLIC
        HPBridgeCore
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_graphics
)

# ----------------------------------------------------------------------------
# 3. HPSAMPLER — VST3 + Standalone plugin
# ----------------------------------------------------------------------------
juce_add_plugin(HPSAMPLER
    COMPANY_NAME "HamloProd"
    BUNDLE_ID "com.hamloprod.hpsampler"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE HMLP
    PLUGIN_CODE HPS1
    FORMATS Standalone VST3
    PRODUCT_NAME "HPSAMPLER"
)

if (TARGET HPSAMPLER_VST3)
    target_compile_definitions(HPSAMPLER_VST3 PRIVATE JUCE_VST3_CAN_REPLACE_VST2=0)
endif()

target_sources(HPSAMPLER
    PRIVATE
        src/app/PluginProcessor.cpp
        src/app/PluginEditor.cpp
)

target_link_libraries(HPSAMPLER
    PRIVATE
        HPSAMPLER_Core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# 4. HPSAMPLER_Tests — automated unit & integration test suite
# ----------------------------------------------------------------------------
add_executable(HPSAMPLER_Tests
    tests/HPSamplerTests.cpp
)
target_link_libraries(HPSAMPLER_Tests PRIVATE HPSAMPLER_Core)
target_compile_definitions(HPSAMPLER_Tests PRIVATE JUCE_STANDALONE_APPLICATION=1)
add_test(NAME HPSAMPLER_Tests COMMAND HPSAMPLER_Tests)
