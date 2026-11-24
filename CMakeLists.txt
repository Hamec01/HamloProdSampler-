cmake_minimum_required(VERSION 3.22)

project(HPSAMPLER VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(JUCE_TARGET_ARCHITECTURE "x86_64")

include(CTest)

set(HPSAMPLER_JUCE_DIR "C:/DRUMENGINE/JUCE" CACHE PATH "Path to local JUCE 8 checkout")
if (NOT EXISTS "${HPSAMPLER_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${HPSAMPLER_JUCE_DIR}. Set -DHPSAMPLER_JUCE_DIR=<path>.")
endif()

add_subdirectory("${HPSAMPLER_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

if (MSVC)
    add_compile_options(/bigobj /utf-8)
endif()

# ----------------------------------------------------------------------------
# HPBridgeCore — shared protocol (hp.sample-context.v1 / hp.drum-guide.v1)
# ----------------------------------------------------------------------------
add_library(HPBridgeCore STATIC
    HPBridgeCore/src/BridgeValidator.cpp
    HPBridgeCore/src/FileSnapshotTransport.cpp
    HPBridgeCore/src/HamloLinkReader.cpp
    HPBridgeCore/src/BridgeSession.cpp
)
target_include_directories(HPBridgeCore PUBLIC HPBridgeCore/include)
target_link_libraries(HPBridgeCore PUBLIC juce::juce_core juce::juce_events)

# ----------------------------------------------------------------------------
# hpdg_analysis — classical tempo/key analysis ported from HPDG (no neural nets)
# ----------------------------------------------------------------------------
add_library(HPDGAnalysis STATIC
    third_party/hpdg_analysis/Analysis/DrumBreakTranscriber.cpp
    third_party/hpdg_analysis/Analysis/SampleLabelReader.cpp
    third_party/hpdg_analysis/Analysis/SampleLineTranscriber.cpp
    third_party/hpdg_analysis/Analysis/SampleHarmonyAnalyzer.cpp
)
target_include_directories(HPDGAnalysis PUBLIC third_party/hpdg_analysis)
target_link_libraries(HPDGAnalysis PUBLIC juce::juce_core juce::juce_dsp juce::juce_audio_formats)
target_compile_definitions(HPDGAnalysis PUBLIC JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

# ----------------------------------------------------------------------------
# HPSAMPLER_Core — model, analysis, chop engine, renderer, audio engine
# ----------------------------------------------------------------------------
add_library(HPSAMPLER_Core STATIC
    src/core/Project.cpp
    src/core/Serialization.cpp
    src/analysis/SourceAnalysis.cpp
    src/analysis/ChopEngine.cpp
    src/render/PadRenderer.cpp
    src/audio/Engine.cpp
    src/bridge/BridgeAdapter.cpp
)
target_include_directories(HPSAMPLER_Core PUBLIC src third_party/signalsmith-stretch/include third_party/signalsmith-linear/include)
target_link_libraries(HPSAMPLER_Core
    PUBLIC
        HPBridgeCore
        HPDGAnalysis
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
)
target_compile_definitions(HPSAMPLER_Core PUBLIC JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

# ----------------------------------------------------------------------------
# HPSAMPLER — VST3 + Standalone
# ----------------------------------------------------------------------------
juce_add_plugin(HPSAMPLER
    COMPANY_NAME "HamloProd"
    BUNDLE_ID "com.hamloprod.hpsampler"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE HMLP
    PLUGIN_CODE HPS1
    FORMATS Standalone VST3
    PRODUCT_NAME "HPSAMPLER"
)
# VST3 class IDs: set on the SHARED code (PUBLIC) so the factory and the moduleinfo.json helper agree. A mismatch
# (flag only on HPSAMPLER_VST3) made FL Studio scan IDs the factory does not have -> "failed to load".
# Keeps the ABCDEF01... IDs the first builds had (FL projects made with them stay loadable).
target_compile_definitions(HPSAMPLER PUBLIC JUCE_VST3_CAN_REPLACE_VST2=1 JUCE_FORCE_USE_LEGACY_PARAM_IDS=1)

target_sources(HPSAMPLER PRIVATE
    src/app/PluginProcessor.cpp
    src/app/PluginEditor.cpp
    src/ui/LookAndFeel.cpp
    src/ui/SourceView.cpp
    src/ui/PatternView.cpp
    src/ui/PadGrid.cpp
    src/ui/ControlStrip.cpp
    src/ui/PadInspector.cpp
)
juce_add_binary_data(HPSAMPLER_Assets SOURCES assets/hamloprod_logo_white.png)

target_link_libraries(HPSAMPLER
    PRIVATE
        HPSAMPLER_Assets
        HPSAMPLER_Core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
)

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
add_executable(HPSAMPLER_Tests tests/Tests.cpp)
target_link_libraries(HPSAMPLER_Tests PRIVATE HPSAMPLER_Core)
target_compile_definitions(HPSAMPLER_Tests PRIVATE JUCE_STANDALONE_APPLICATION=1)
add_test(NAME HPSAMPLER_Tests COMMAND HPSAMPLER_Tests)

# Visual review tool: renders the real editor to PNG (not shipped)
add_executable(HPSAMPLER_Shot WIN32 tools/Shot.cpp)
target_link_libraries(HPSAMPLER_Shot PRIVATE HPSAMPLER HPSAMPLER_Core juce::juce_audio_utils)
target_compile_definitions(HPSAMPLER_Shot PRIVATE JUCE_STANDALONE_APPLICATION=1)
target_include_directories(HPSAMPLER_Shot PRIVATE src)

# Loads the built VST3 like a host (diagnostics, not shipped)
add_executable(HPSAMPLER_HostCheck tools/HostCheck.cpp)
target_link_libraries(HPSAMPLER_HostCheck PRIVATE juce::juce_audio_processors juce::juce_audio_utils)
target_compile_definitions(HPSAMPLER_HostCheck PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_PLUGINHOST_VST3=1 JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
add_executable(HPSAMPLER_RawVst3Check tools/RawVst3Check.cpp
    ${HPSAMPLER_JUCE_DIR}/modules/juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/funknown.cpp)
target_include_directories(HPSAMPLER_RawVst3Check PRIVATE ${HPSAMPLER_JUCE_DIR}/modules/juce_audio_processors_headless/format_types/VST3_SDK)
