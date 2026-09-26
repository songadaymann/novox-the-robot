# Novox The Robot plugin (AU / VST3 / Standalone), built on JUCE 8.
# Configure from the repository root with -DNOVOX_BUILD_PLUGIN=ON.
# This directory is AGPLv3 (JUCE's open-source license); the engine it links is MIT.

include(FetchContent)
FetchContent_Declare(juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.15
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

juce_add_plugin(NovoxTheRobot
  COMPANY_NAME "Song A Day"
  BUNDLE_ID com.songaday.novox
  PLUGIN_MANUFACTURER_CODE Sngd
  PLUGIN_CODE Nvox
  FORMATS AU VST3 Standalone
  PRODUCT_NAME "Novox The Robot"
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  NEEDS_WEBVIEW2 TRUE
  AU_MAIN_TYPE kAudioUnitType_MusicDevice
  VST3_CATEGORIES Instrument Synth
  COPY_PLUGIN_AFTER_BUILD TRUE)

# The editor UI (HTML/JS) is compiled into the plugin. JUCE's JavaScript bridge
# is copied in from the JUCE sources as juce.js.
configure_file(${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/index.js
               ${CMAKE_CURRENT_BINARY_DIR}/ui/juce.js COPYONLY)
configure_file(${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/check_native_interop.js
               ${CMAKE_CURRENT_BINARY_DIR}/ui/check_native_interop.js COPYONLY)
juce_add_binary_data(NovoxUiData SOURCES
  ui/index.html
  ui/app.js
  ui/backend.js
  ui/style.css
  ${CMAKE_CURRENT_BINARY_DIR}/ui/juce.js
  ${CMAKE_CURRENT_BINARY_DIR}/ui/check_native_interop.js)

target_sources(NovoxTheRobot PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/Song.cpp)

target_compile_definitions(NovoxTheRobot PUBLIC
  JUCE_WEB_BROWSER=1
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(NovoxTheRobot
  PRIVATE
    NovoxUiData
    novox_static
    juce::juce_audio_utils
    juce::juce_gui_extra
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Headless tests: host the processor with a fake playhead.
juce_add_console_app(NovoxPluginTests PRODUCT_NAME "NovoxPluginTests")
target_sources(NovoxPluginTests PRIVATE
  Tests/PluginTests.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/Song.cpp)
target_compile_definitions(NovoxPluginTests PRIVATE
  JucePlugin_Name="Novox The Robot"
  JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(NovoxPluginTests PRIVATE
  NovoxUiData novox_static juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)
