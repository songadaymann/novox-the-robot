cmake_minimum_required(VERSION 3.20)
project(NovoxTheRobot VERSION 1.0.0 LANGUAGES C CXX)
# What people see (the installer, the download page, the editor): the version plus a stage.
set(NOVOX_VERSION_LABEL "1.0 beta" CACHE STRING "Release name shown to people")
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

# Windows (MSVC): the static C runtime, so the CLI, the Python DLL, the app and the plugin need no
# Visual C++ Redistributable. A target takes its runtime when it is created, so this comes before
# every target here and in JUCE and whisper.cpp (whose older cmake_minimum_required would otherwise
# turn CMP0091 off there). Sources are UTF-8; the generator expression keeps /utf-8 from rc.exe.
if(MSVC)
  set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
  if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(NOVOX_SOURCES
  engine/src/articulation.cpp
  engine/src/import.cpp
  engine/src/lexicon.cpp
  engine/src/phonemes.cpp
  engine/src/planner.cpp
  engine/src/score.cpp
  engine/src/singing_pitch.cpp
  engine/src/singing_segment.cpp
  engine/src/spectrum.cpp
  engine/src/syllables.cpp
  engine/src/synth.cpp
  engine/src/transcribe.cpp
  engine/src/voice.cpp
  engine/src/novox.cpp
)

# Embed the CMU dictionary so the engine needs no external files.
option(NOVOX_EMBED_DICT "Compile the CMU pronouncing dictionary into the engine" ON)
if(NOVOX_EMBED_DICT)
  # The plugin (NOVOX_BUILD_PLUGIN, below) has no other way to get a dictionary: it needs Python.
  if(NOVOX_BUILD_PLUGIN)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
  else()
    find_package(Python3 COMPONENTS Interpreter)
  endif()
  if(Python3_FOUND)
    set(NOVOX_DICT_CPP ${CMAKE_BINARY_DIR}/generated/cmudict_embedded.cpp)
    add_custom_command(
      OUTPUT ${NOVOX_DICT_CPP}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
      COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/embed_dict.py
              ${CMAKE_SOURCE_DIR}/data/cmudict.dict ${NOVOX_DICT_CPP}
      DEPENDS ${CMAKE_SOURCE_DIR}/data/cmudict.dict ${CMAKE_SOURCE_DIR}/tools/embed_dict.py
      COMMENT "Embedding CMU dictionary")
    list(APPEND NOVOX_SOURCES ${NOVOX_DICT_CPP})
    add_compile_definitions(NOVOX_EMBEDDED_DICT=1)
  else()
    message(WARNING "Python 3 not found: building without the embedded dictionary")
  endif()
endif()

# Compile the engine once; static and shared libraries both use these objects.
# Windows is the exception for engine/src/novox.cpp, which defines the C API: only the DLL may
# export it (NOVOX_BUILDING_DLL, see engine/include/novox.h), or every program linking the static
# library would export it too and write an import library of its own. Each library compiles that
# one file itself there.
set(NOVOX_OBJECT_SOURCES ${NOVOX_SOURCES})
if(WIN32)
  list(REMOVE_ITEM NOVOX_OBJECT_SOURCES engine/src/novox.cpp)
endif()
add_library(novox_objects OBJECT ${NOVOX_OBJECT_SOURCES})
target_include_directories(novox_objects PUBLIC engine/include PRIVATE engine/src)
set_target_properties(novox_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(novox_objects PRIVATE /W3 /bigobj)
else()
  target_compile_options(novox_objects PRIVATE -Wall -Wextra)
endif()

# Static library: linked into the CLI, tests and (later) the plugin.
add_library(novox_static STATIC $<TARGET_OBJECTS:novox_objects>)
target_include_directories(novox_static PUBLIC engine/include)
set_target_properties(novox_static PROPERTIES OUTPUT_NAME novox)

# Shared library: loaded by the Python bindings.
add_library(novox_shared SHARED $<TARGET_OBJECTS:novox_objects>)
target_include_directories(novox_shared PUBLIC engine/include)
set_target_properties(novox_shared PROPERTIES OUTPUT_NAME novox)

if(WIN32)
  foreach(library novox_static novox_shared)
    target_sources(${library} PRIVATE engine/src/novox.cpp)
    target_include_directories(${library} PRIVATE engine/src)
    if(MSVC)
      target_compile_options(${library} PRIVATE /W3 /bigobj)
    endif()
  endforeach()
  target_compile_definitions(novox_shared PRIVATE NOVOX_BUILDING_DLL)
  # The DLL stays novox.dll (bindings/python loads it by that name) and its import library is
  # novox.lib, so the static library gets its own name. novox.exe would share the DLL's .pdb.
  set_target_properties(novox_static PROPERTIES OUTPUT_NAME novox_static)
  set_target_properties(novox_shared PROPERTIES PDB_NAME novox_shared)
endif()

option(NOVOX_BUILD_CLI "Build the novox command-line tool" ON)
option(NOVOX_BUILD_TESTS "Build the engine tests" ON)

if(NOVOX_BUILD_CLI)
  add_executable(novox engine/cli/main.cpp)
  target_link_libraries(novox PRIVATE novox_static)
  install(TARGETS novox RUNTIME DESTINATION bin COMPONENT cli)
endif()

# Python wheels (scikit-build-core) ship the shared library inside the package.
if(SKBUILD)
  install(TARGETS novox_shared LIBRARY DESTINATION novox COMPONENT python RUNTIME DESTINATION novox COMPONENT python)
endif()

# Tests: `ctest --test-dir build` (or run build/novox_tests).
if(NOVOX_BUILD_TESTS)
  enable_testing()
  add_executable(novox_tests tests/engine_tests.cpp tests/singing_pitch_tests.cpp tests/singing_segment_tests.cpp
                             tests/syllables_tests.cpp)
  target_link_libraries(novox_tests PRIVATE novox_static)
  target_include_directories(novox_tests PRIVATE engine/src)
  target_compile_definitions(novox_tests PRIVATE NOVOX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(novox_tests PRIVATE /bigobj)  # the large test files, like the engine's
  endif()
  add_test(NAME engine COMMAND novox_tests)
endif()

# DAW plugin + standalone app (JUCE, AGPLv3). Off by default: it downloads JUCE.
option(NOVOX_BUILD_PLUGIN "Build the AU/VST3/Standalone plugin (fetches JUCE)" OFF)
if(NOVOX_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()

# WebAssembly module for the JavaScript package (emcmake cmake -B build-wasm ...).
if(EMSCRIPTEN)
  set(NOVOX_WASM_EXPORTS "_novox_create,_novox_destroy,_novox_set_sample_rate,_novox_load_dictionary,_novox_add_word,_novox_render,_novox_plan,_novox_lyric_tokens,_novox_import,_novox_phonemize,_novox_transcribe,_novox_voice_names,_novox_stream_create,_novox_stream_destroy,_novox_stream_length,_novox_stream_position,_novox_stream_lead,_novox_stream_seek,_novox_stream_process,_novox_last_error,_novox_free,_malloc,_free")
  add_executable(novox_wasm $<TARGET_OBJECTS:novox_objects>)
  set_target_properties(novox_wasm PROPERTIES OUTPUT_NAME novox_wasm SUFFIX ".js")
  target_link_options(novox_wasm PRIVATE
    -O3
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createNovoxModule
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_FUNCTIONS=[${NOVOX_WASM_EXPORTS}]
    -sEXPORTED_RUNTIME_METHODS=[UTF8ToString,stringToNewUTF8,HEAPF32,HEAPU8,HEAPU32]
    -sDISABLE_EXCEPTION_CATCHING=0)
  target_compile_options(novox_objects PRIVATE -fexceptions)
  target_link_options(novox_wasm PRIVATE -fexceptions)
endif()
