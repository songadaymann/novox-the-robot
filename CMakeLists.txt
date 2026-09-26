cmake_minimum_required(VERSION 3.20)
project(NewVirtualSinger VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(NVS_SOURCES
  engine/src/import.cpp
  engine/src/lexicon.cpp
  engine/src/phonemes.cpp
  engine/src/planner.cpp
  engine/src/score.cpp
  engine/src/synth.cpp
  engine/src/voice.cpp
  engine/src/nvs.cpp
)

# Embed the CMU dictionary so the engine needs no external files.
option(NVS_EMBED_DICT "Compile the CMU pronouncing dictionary into the engine" ON)
if(NVS_EMBED_DICT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    set(NVS_DICT_CPP ${CMAKE_BINARY_DIR}/generated/cmudict_embedded.cpp)
    add_custom_command(
      OUTPUT ${NVS_DICT_CPP}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
      COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/embed_dict.py
              ${CMAKE_SOURCE_DIR}/data/cmudict.dict ${NVS_DICT_CPP}
      DEPENDS ${CMAKE_SOURCE_DIR}/data/cmudict.dict ${CMAKE_SOURCE_DIR}/tools/embed_dict.py
      COMMENT "Embedding CMU dictionary")
    list(APPEND NVS_SOURCES ${NVS_DICT_CPP})
    add_compile_definitions(NVS_EMBEDDED_DICT=1)
  else()
    message(WARNING "Python 3 not found: building without the embedded dictionary")
  endif()
endif()

# Compile the engine once; static and shared libraries both use these objects.
add_library(nvs_objects OBJECT ${NVS_SOURCES})
target_include_directories(nvs_objects PUBLIC engine/include PRIVATE engine/src)
set_target_properties(nvs_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(nvs_objects PRIVATE /W3 /utf-8 /bigobj)
else()
  target_compile_options(nvs_objects PRIVATE -Wall -Wextra)
endif()

# Static library: linked into the CLI, tests and (later) the plugin.
add_library(nvs_static STATIC $<TARGET_OBJECTS:nvs_objects>)
target_include_directories(nvs_static PUBLIC engine/include)
set_target_properties(nvs_static PROPERTIES OUTPUT_NAME nvs)

# Shared library: loaded by the Python bindings.
add_library(nvs_shared SHARED $<TARGET_OBJECTS:nvs_objects>)
target_include_directories(nvs_shared PUBLIC engine/include)
set_target_properties(nvs_shared PROPERTIES OUTPUT_NAME nvs)

option(NVS_BUILD_CLI "Build the nvs command-line tool" ON)
option(NVS_BUILD_TESTS "Build the engine tests" ON)

if(NVS_BUILD_CLI)
  add_executable(nvs engine/cli/main.cpp)
  target_link_libraries(nvs PRIVATE nvs_static)
  install(TARGETS nvs RUNTIME DESTINATION bin COMPONENT cli)
endif()

# Python wheels (scikit-build-core) ship the shared library inside the package.
if(SKBUILD)
  install(TARGETS nvs_shared LIBRARY DESTINATION nvs COMPONENT python RUNTIME DESTINATION nvs COMPONENT python)
endif()

# Tests: `ctest --test-dir build` (or run build/nvs_tests).
if(NVS_BUILD_TESTS)
  enable_testing()
  add_executable(nvs_tests tests/engine_tests.cpp)
  target_link_libraries(nvs_tests PRIVATE nvs_static)
  target_include_directories(nvs_tests PRIVATE engine/src)
  target_compile_definitions(nvs_tests PRIVATE NVS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME engine COMMAND nvs_tests)
endif()

# DAW plugin + standalone app (JUCE, AGPLv3). Off by default: it downloads JUCE.
option(NVS_BUILD_PLUGIN "Build the AU/VST3/Standalone plugin (fetches JUCE)" OFF)
if(NVS_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()

# WebAssembly module for the JavaScript package (emcmake cmake -B build-wasm ...).
if(EMSCRIPTEN)
  set(NVS_WASM_EXPORTS "_nvs_create,_nvs_destroy,_nvs_set_sample_rate,_nvs_load_dictionary,_nvs_add_word,_nvs_render,_nvs_plan,_nvs_import,_nvs_phonemize,_nvs_voice_names,_nvs_stream_create,_nvs_stream_destroy,_nvs_stream_length,_nvs_stream_position,_nvs_stream_lead,_nvs_stream_seek,_nvs_stream_process,_nvs_last_error,_nvs_free,_malloc,_free")
  add_executable(nvs_wasm $<TARGET_OBJECTS:nvs_objects>)
  set_target_properties(nvs_wasm PROPERTIES OUTPUT_NAME nvs_wasm SUFFIX ".js")
  target_link_options(nvs_wasm PRIVATE
    -O3
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createNvsModule
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_FUNCTIONS=[${NVS_WASM_EXPORTS}]
    -sEXPORTED_RUNTIME_METHODS=[UTF8ToString,stringToNewUTF8,HEAPF32,HEAPU8,HEAPU32]
    -sDISABLE_EXCEPTION_CATCHING=0)
  target_compile_options(nvs_objects PRIVATE -fexceptions)
  target_link_options(nvs_wasm PRIVATE -fexceptions)
endif()
