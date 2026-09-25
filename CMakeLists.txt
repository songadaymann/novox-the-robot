cmake_minimum_required(VERSION 3.20)
project(NewVirtualSinger VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(NVS_SOURCES
  engine/src/lexicon.cpp
  engine/src/phonemes.cpp
  engine/src/planner.cpp
  engine/src/score.cpp
  engine/src/synth.cpp
  engine/src/voice.cpp
  engine/src/nvs.cpp
)

# Static library: linked into the CLI and (later) the plugin.
add_library(nvs_static STATIC ${NVS_SOURCES})
target_include_directories(nvs_static PUBLIC engine/include PRIVATE engine/src)
set_target_properties(nvs_static PROPERTIES OUTPUT_NAME nvs POSITION_INDEPENDENT_CODE ON)
target_compile_options(nvs_static PRIVATE -Wall -Wextra)

# Shared library: loaded by the Python bindings.
add_library(nvs_shared SHARED ${NVS_SOURCES})
target_include_directories(nvs_shared PUBLIC engine/include PRIVATE engine/src)
set_target_properties(nvs_shared PROPERTIES OUTPUT_NAME nvs)

add_executable(nvs engine/cli/main.cpp)
target_link_libraries(nvs PRIVATE nvs_static)
