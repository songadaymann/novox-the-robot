# Novox The Robot plugin (AU / VST3 / Standalone, plus AAX when NOVOX_AAX_SDK is set), built on JUCE 8.
# Configure from the repository root with -DNOVOX_BUILD_PLUGIN=ON.
# This directory is AGPLv3 (JUCE's open-source license); the engine it links is MIT.

include(FetchContent)

# The plugin cannot load a pronouncing dictionary at run time: the engine must have one compiled in.
if(NOT NOVOX_EMBED_DICT)
  message(FATAL_ERROR "The plugin needs the embedded CMU dictionary: configure with NOVOX_EMBED_DICT ON.")
endif()

# Development builds archive and install every bundle into ~/Library after each build. A release
# build (scripts/package-mac.sh) turns this off: it packages the bundles instead. On Windows it is
# off unless asked for: JUCE then copies the VST3 into Common Files\VST3, which needs an
# administrator.
set(novox_install_default ON)
if(WIN32)
  set(novox_install_default OFF)
endif()
option(NOVOX_INSTALL_AFTER_BUILD
  "Install plugin bundles after each build (macOS: archive and install into ~/Library; Windows: copy the VST3 into Common Files)"
  ${novox_install_default})

# macOS delivery goes through our dated archive and transactional installer.
# Windows follows NOVOX_INSTALL_AFTER_BUILD; other platforms retain JUCE's existing copy behavior.
set(novox_copy_plugin_after_build TRUE)
if(APPLE)
  set(novox_copy_plugin_after_build FALSE)
elseif(WIN32)
  set(novox_copy_plugin_after_build ${NOVOX_INSTALL_AFTER_BUILD})
endif()
FetchContent_Declare(juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.15
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

# Windows: JUCE links the WebView2 loader statically from Microsoft's Microsoft.Web.WebView2 NuGet
# package. Its FindWebView2.cmake looks for a *Microsoft.Web.WebView2* folder inside
# JUCE_WEBVIEW2_PACKAGE_LOCATION, so the version JUCE 8.0.15 suggests is fetched into the build tree
# (no NuGet setup needed), pinned by the package's SHA-512 from nuget.org's catalog. Pass
# -DJUCE_WEBVIEW2_PACKAGE_LOCATION=<folder> to use a package you already have instead. Must come
# before the JUCE targets: they look the package up when they are created.
if(WIN32 AND NOT JUCE_WEBVIEW2_PACKAGE_LOCATION)
  set(novox_webview2_version 1.0.3485.44)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  FetchContent_Declare(webview2
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${novox_webview2_version}/microsoft.web.webview2.${novox_webview2_version}.nupkg
    URL_HASH SHA512=d01a59afdbda1da11d640db98934ffed12c0f813551b75f97e052673d12e3238f88f29f4c24ac56a9313f5cd3f022a1e8d40e103a3798aa4d42d1322ddb342de
    DOWNLOAD_NAME microsoft.web.webview2.${novox_webview2_version}.zip  # a .nupkg is a zip
    SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/nuget/Microsoft.Web.WebView2.${novox_webview2_version}")
  FetchContent_MakeAvailable(webview2)  # no CMakeLists.txt inside: this only downloads and unpacks
  get_filename_component(JUCE_WEBVIEW2_PACKAGE_LOCATION "${webview2_SOURCE_DIR}" DIRECTORY)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_CURL OFF CACHE BOOL "" FORCE)
set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
if(WIN32)
  # Windows builds run on other people's PCs: ggml's AVX2 baseline instead of the build machine's
  # own CPU (a CI runner may have AVX-512), and ggml's own thread pool instead of OpenMP, which
  # would add vcomp140.dll. ggml turns the baseline's instruction sets off by default when
  # SOURCE_DATE_EPOCH is set or a cache predates GGML_NATIVE OFF, so they are set too (with MSVC,
  # AVX2 brings FMA and F16C). The Mac settings stay as they are (scripts/package-mac.sh).
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
  foreach(instructions GGML_SSE42 GGML_AVX GGML_AVX2 GGML_BMI2)
    set(${instructions} ON CACHE BOOL "" FORCE)
  endforeach()
endif()
# The patch stops whisper.cpp aborting (and taking the DAW with it) when a DTW timestamp window
# is under 160 ms long; upstream still asserts there through v1.9.5. See the patch's header.
FetchContent_Declare(whisper
  GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
  GIT_TAG v1.8.3
  GIT_SHALLOW TRUE
  PATCH_COMMAND "${CMAKE_COMMAND}" "-DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/whisper-dtw-short-window.patch"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/patches/git-apply-once.cmake")
FetchContent_MakeAvailable(whisper)

# Optional AAX (Pro Tools) build, off unless NOVOX_AAX_SDK is set as a cache variable or as an
# environment variable of the same name:
#   -DNOVOX_AAX_SDK=/absolute/path/to/aax-sdk   an AAX SDK downloaded from developer.avid.com
#   -DNOVOX_AAX_SDK=bundled                     the AAX SDK 2.9 that ships inside JUCE 8.0.15
# Pro Tools only loads AAX binaries signed with PACE's wraptool. Steps and licensing: docs/HOSTS.md.
# Unset (the default), nothing below changes the build.
set(NOVOX_AAX_SDK "" CACHE STRING
  "AAX SDK folder, or 'bundled' for the copy inside JUCE, to also build an AAX plugin (empty: no AAX)")
set(novox_aax_sdk "${NOVOX_AAX_SDK}")
set(novox_aax_sdk_from "-DNOVOX_AAX_SDK")
if(novox_aax_sdk STREQUAL "" AND NOT "$ENV{NOVOX_AAX_SDK}" STREQUAL "")
  set(novox_aax_sdk "$ENV{NOVOX_AAX_SDK}")
  set(novox_aax_sdk_from "the NOVOX_AAX_SDK environment variable")
endif()
set(novox_formats AU VST3 Standalone)
set(novox_aax_args)
if(NOT novox_aax_sdk STREQUAL "")
  if(novox_aax_sdk STREQUAL "bundled")
    set(novox_aax_sdk "${juce_SOURCE_DIR}/modules/juce_audio_plugin_client/AAX/SDK")
  endif()
  string(REGEX REPLACE "^~" "$ENV{HOME}" novox_aax_sdk "${novox_aax_sdk}")
  get_filename_component(novox_aax_sdk "${novox_aax_sdk}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
  if(NOT EXISTS "${novox_aax_sdk}/Interfaces/AAX.h"
     OR NOT EXISTS "${novox_aax_sdk}/Interfaces/AAX_Version.h"
     OR NOT IS_DIRECTORY "${novox_aax_sdk}/Interfaces/ACF"
     OR NOT EXISTS "${novox_aax_sdk}/Libs/AAXLibrary/source/AAX_CHostServices.cpp")
    message(FATAL_ERROR
      "NOVOX_AAX_SDK (from ${novox_aax_sdk_from}) is '${novox_aax_sdk}', which is not an AAX SDK "
      "folder: expected Interfaces/AAX.h, Interfaces/AAX_Version.h, Interfaces/ACF/ and "
      "Libs/AAXLibrary/source/ inside it. Point it at the unzipped SDK from developer.avid.com, "
      "use 'bundled' for the copy inside JUCE, or set it empty to build without AAX.")
  endif()
  file(STRINGS "${novox_aax_sdk}/Interfaces/AAX_Version.h" novox_aax_version
       REGEX "^#define AAX_SDK_VERSION[ \t]")
  string(REGEX MATCH "0x[0-9A-Fa-f]+" novox_aax_version "${novox_aax_version}")
  if(novox_aax_version STREQUAL "")
    message(FATAL_ERROR "Could not read AAX_SDK_VERSION from ${novox_aax_sdk}/Interfaces/AAX_Version.h")
  endif()
  math(EXPR novox_aax_version_int "${novox_aax_version}")
  if(novox_aax_version_int LESS 518) # 0x0206: JUCE 8.0.15 needs AAX SDK 2.6.1 or newer
    message(FATAL_ERROR "The AAX SDK at ${novox_aax_sdk} is version ${novox_aax_version}; JUCE needs 2.6.1 or newer.")
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    juce_set_aax_sdk_path("${novox_aax_sdk}")
    list(APPEND novox_formats AAX)
    # Instrument category; one stereo or mono instance per track (no multi-mono copies of a synth).
    # Never change AAX_IDENTIFIER once Pro Tools sessions exist: sessions find the plugin by it.
    set(novox_aax_args
      AAX_IDENTIFIER com.songaday.novoxtherobot
      AAX_CATEGORY AAX_ePlugInCategory_SWGenerators
      DISABLE_AAX_MULTI_MONO TRUE)
    message(STATUS "Novox: building AAX with AAX SDK ${novox_aax_version} at ${novox_aax_sdk}")
  else()
    message(WARNING "NOVOX_AAX_SDK is set but AAX plugins build only on macOS and Windows; skipping AAX.")
  endif()
endif()

juce_add_plugin(NovoxTheRobot
  COMPANY_NAME "Song A Day"
  BUNDLE_ID com.songaday.novox
  PLUGIN_MANUFACTURER_CODE Sngd
  PLUGIN_CODE Nvox
  FORMATS ${novox_formats}
  ${novox_aax_args}
  ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/../assets/icons/novox-robot-white.png"
  PRODUCT_NAME "Novox The Robot"
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT "Record your singing and convert it into notes and lyrics."
  NEEDS_WEBVIEW2 TRUE
  AU_MAIN_TYPE kAudioUnitType_MusicDevice
  VST3_CATEGORIES Instrument Synth
  COPY_PLUGIN_AFTER_BUILD ${novox_copy_plugin_after_build})

target_compile_definitions(NovoxTheRobot PRIVATE NOVOX_VERSION_LABEL="${NOVOX_VERSION_LABEL}")

# The editor UI (HTML/JS) is compiled into the plugin. JUCE's JavaScript bridge
# is copied in from the JUCE sources as juce.js.
configure_file(${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/index.js
               ${CMAKE_CURRENT_BINARY_DIR}/ui/juce.js COPYONLY)
configure_file(${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/check_native_interop.js
               ${CMAKE_CURRENT_BINARY_DIR}/ui/check_native_interop.js COPYONLY)
juce_add_binary_data(NovoxUiData SOURCES
  ui/index.html
  ui/app.js
  ui/audio-import.js
  ui/lyric-text.js
  ui/note-lyrics.js
  ui/melody-clipboard.js
  ui/pitch-correction.js
  ui/note-shaping.js
  ui/song-merge.js
  ui/platform.js
  ui/backend.js
  ui/style.css
  ${CMAKE_CURRENT_BINARY_DIR}/ui/juce.js
  ${CMAKE_CURRENT_BINARY_DIR}/ui/check_native_interop.js)

target_sources(NovoxTheRobot PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/MicrophoneCapture.cpp
  Source/AudioImport.cpp
  Source/HostMmc.cpp
  Source/Song.cpp)

target_compile_definitions(NovoxTheRobot PUBLIC
  JUCE_USE_MP3AUDIOFORMAT=1
  JUCE_WEB_BROWSER=1
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(NovoxTheRobot
  PRIVATE
    NovoxUiData
    novox_static
    whisper
    juce::juce_cryptography
    juce::juce_audio_utils
    juce::juce_gui_extra
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Headless tests: host the processor with a fake playhead. They build the editor too, so on
# Windows they link WebView2 like the plugin.
juce_add_console_app(NovoxPluginTests PRODUCT_NAME "NovoxPluginTests" NEEDS_WEBVIEW2 TRUE)
target_sources(NovoxPluginTests PRIVATE
  Tests/PluginTests.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/MicrophoneCapture.cpp
  Source/AudioImport.cpp
  Source/HostMmc.cpp
  Source/Song.cpp)
target_compile_definitions(NovoxPluginTests PRIVATE
  NOVOX_VERSION_LABEL="${NOVOX_VERSION_LABEL}"
  JucePlugin_Name="Novox The Robot"
  JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
target_compile_definitions(NovoxPluginTests PRIVATE JUCE_USE_MP3AUDIOFORMAT=1)
target_include_directories(NovoxTheRobot PRIVATE ${CMAKE_SOURCE_DIR}/engine/src)
target_include_directories(NovoxPluginTests PRIVATE ${CMAKE_SOURCE_DIR}/engine/src)
target_link_libraries(NovoxPluginTests PRIVATE
  NovoxUiData novox_static whisper juce::juce_cryptography juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)

# Windows: compile JUCE's WebView2 backend into every target that builds the editor
# (PluginEditor.cpp). NEEDS_WEBVIEW2 only links the loader; without this the editor's resource
# provider does not exist there. PUBLIC on the plugin, so its VST3 and app wrappers agree.
if(WIN32)
  target_compile_definitions(NovoxTheRobot PUBLIC JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
  target_compile_definitions(NovoxPluginTests PRIVATE JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
endif()

if(APPLE)
  target_compile_definitions(NovoxPluginTests PRIVATE NOVOX_DROP_TESTS=1)

# Two real embedded editors exercise the native bridge and macOS clipboard.
juce_add_console_app(NovoxEditorInteractionTests PRODUCT_NAME "NovoxEditorInteractionTests")
target_sources(NovoxEditorInteractionTests PRIVATE Tests/EditorInteractionTests.cpp)
set_source_files_properties(Tests/EditorInteractionTests.cpp PROPERTIES LANGUAGE OBJCXX)
target_compile_definitions(NovoxEditorInteractionTests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
target_include_directories(NovoxEditorInteractionTests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(NovoxEditorInteractionTests PRIVATE NovoxTheRobot
  juce::juce_audio_utils juce::juce_gui_extra)

# Preserve the original AU identity used by existing Logic projects. Both versions
# run the same current engine/editor; do not strand saved Nvsg instances.
juce_add_plugin(NewVirtualSingerCompat
  COMPANY_NAME "Song A Day"
  BUNDLE_ID com.songaday.newvirtualsinger
  PLUGIN_MANUFACTURER_CODE Sngd
  PLUGIN_CODE Nvsg
  FORMATS AU
  ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/../assets/icons/novox-robot-white.png"
  PRODUCT_NAME "New Virtual Singer"
  PLUGIN_NAME "Novox The Robot"
  DESCRIPTION "Novox The Robot"
  IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  AU_MAIN_TYPE kAudioUnitType_MusicDevice
  COPY_PLUGIN_AFTER_BUILD ${novox_copy_plugin_after_build})
target_sources(NewVirtualSingerCompat PRIVATE
  Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/MicrophoneCapture.cpp Source/AudioImport.cpp Source/HostMmc.cpp Source/Song.cpp)
target_compile_definitions(NewVirtualSingerCompat PRIVATE NOVOX_VERSION_LABEL="${NOVOX_VERSION_LABEL}")
target_compile_definitions(NewVirtualSingerCompat PUBLIC
  JUCE_USE_MP3AUDIOFORMAT=1 JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_include_directories(NewVirtualSingerCompat PRIVATE ${CMAKE_SOURCE_DIR}/engine/src)
target_link_libraries(NewVirtualSingerCompat PRIVATE
  NovoxUiData novox_static whisper juce::juce_cryptography juce::juce_audio_utils juce::juce_gui_extra
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# Keep both AU identities current when building the normal delivery target (development only:
# the compatibility AU keeps Jonathan's older Logic projects opening and is not released).
if(NOVOX_INSTALL_AFTER_BUILD)
  add_dependencies(NovoxTheRobot_All NewVirtualSingerCompat_AU)
endif()

add_executable(NovoxAuHostSmoke Tests/AuHostSmoke.cpp)
target_compile_features(NovoxAuHostSmoke PRIVATE cxx_std_17)
target_link_libraries(NovoxAuHostSmoke PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()

if(APPLE)
  foreach(target NovoxTheRobot NewVirtualSingerCompat NovoxPluginTests)
    target_compile_definitions(${target} PRIVATE NOVOX_DROP_HANDLER=${target}DropHandler)
    target_sources(${target} PRIVATE Source/MacShortcuts.mm Source/MacRegionDrop.mm)
  endforeach()
elseif(WIN32)
  # Space for the host's transport (forwardNovoxHostSpace), as MacShortcuts.mm on a Mac.
  foreach(target NovoxTheRobot NovoxPluginTests)
    target_sources(${target} PRIVATE Source/WinShortcuts.cpp)
  endforeach()
endif()

# Native Mac title bar and system Options menu belong to standalone only.
target_compile_definitions(NovoxTheRobot_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
target_sources(NovoxTheRobot_Standalone PRIVATE Source/StandaloneApp.cpp)

if(APPLE AND NOVOX_INSTALL_AFTER_BUILD)
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
  set(NOVOX_PLUGIN_ARCHIVE_DIR "${CMAKE_SOURCE_DIR}/out/plugin-versions" CACHE PATH
      "Dated plugin binaries and matching source snapshots; retained across builds")
  set(novox_archive_script "${CMAKE_SOURCE_DIR}/scripts/plugin_archive.py")

  # Capture dirty and untracked project sources before any shared engine, editor,
  # or plugin code compiles. The installer rejects source changes during a build.
  add_custom_target(NovoxArchivePrepare
    COMMAND "${Python3_EXECUTABLE}" "${novox_archive_script}" prepare
      --source-root "${CMAKE_SOURCE_DIR}"
      --archive-root "${NOVOX_PLUGIN_ARCHIVE_DIR}"
      --build-dir "${CMAKE_BINARY_DIR}"
    COMMENT "Saving dated Novox source snapshot"
    VERBATIM)
  foreach(target novox_objects NovoxUiData NovoxTheRobot NewVirtualSingerCompat)
    add_dependencies(${target} NovoxArchivePrepare)
  endforeach()

  # Match JUCE's active format list, including optional AAX. Each format keeps
  # its previous installed bundle before replacement; a failed backup stops the
  # install. Both AU identities remain usable by existing Logic sessions.
  foreach(shared_target NovoxTheRobot NewVirtualSingerCompat)
    get_target_property(format_targets ${shared_target} JUCE_ACTIVE_PLUGIN_TARGETS)
    foreach(target IN LISTS format_targets)
      get_target_property(kind ${target} JUCE_TARGET_KIND_STRING)
      if(kind STREQUAL "App")
        continue()
      endif()
      get_target_property(bundle ${target} JUCE_PLUGIN_ARTEFACT_FILE)
      get_target_property(destination ${target} JUCE_PLUGIN_COPY_DIR)
      if(bundle AND destination)
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND "${CMAKE_COMMAND}" "-Dsrc=${bundle}"
            -P "${juce_SOURCE_DIR}/extras/Build/CMake/checkBundleSigning.cmake"
          COMMAND "${Python3_EXECUTABLE}" "${novox_archive_script}" install
            --source-root "${CMAKE_SOURCE_DIR}"
            --archive-root "${NOVOX_PLUGIN_ARCHIVE_DIR}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --bundle "${bundle}"
            --destination "$<GENEX_EVAL:${destination}>"
          COMMENT "Archiving and installing ${target}"
          VERBATIM)
      endif()
    endforeach()
  endforeach()
endif()
