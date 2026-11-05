cmake_minimum_required(VERSION 3.20)

# Host add_subdirectory must not start a nested project. The host compiles
# the engine into pm-media (those TUs still include host headers) and links
# this INTERFACE target for the include root. A top-level configure builds
# the static library and the pm-xblox CLI.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pm-xblox
    VERSION 0.1.0
    DESCRIPTION "PolyMech XBlox flow runtime"
    LANGUAGES CXX)
endif()

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_library(pm-xblox-lib INTERFACE)
  target_include_directories(pm-xblox-lib INTERFACE
    "${PM_XBLOX_SRC}")
  return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/host-sources.cmake")
pm_xblox_prepare()

if(NOT TARGET CLI11::CLI11 OR NOT TARGET nlohmann_json::nlohmann_json)
  include(FetchContent)
  if(NOT TARGET CLI11::CLI11)
    set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(cli11
      GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
      GIT_TAG v2.4.2
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(cli11)
  endif()
  if(NOT TARGET nlohmann_json::nlohmann_json)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.11.3
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
  endif()
endif()

set(PM_XBLOX_HOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
  "Tanit tree that still provides host headers the engine includes")

function(pm_xblox_add_sibling name)
  if(TARGET "pm-${name}-lib")
    return()
  endif()
  set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/../${name}.cpp")
  if(NOT EXISTS "${_dir}/CMakeLists.txt")
    return()
  endif()
  if(name STREQUAL "audio" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/cmake/host-layout.cmake")
    set(PM_AUDIO_LINK_COMMONS ON CACHE BOOL "" FORCE)
  endif()
  if(name STREQUAL "video" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/cmake/host-layout.cmake")
    set(PM_VIDEO_LINK_COMMONS ON CACHE BOOL "" FORCE)
  endif()
  add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/pm-${name}-lib" EXCLUDE_FROM_ALL)
endfunction()

# commons before search. search links commons. audio and video opt into commons.
pm_xblox_add_sibling(commons)
pm_xblox_add_sibling(search)
pm_xblox_add_sibling(video)
pm_xblox_add_sibling(audio)

if(NOT TARGET logger AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../logger/CMakeLists.txt")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../logger"
                   "${CMAKE_BINARY_DIR}/pm-xblox-logger" EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET muparser AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../muparser/CMakeLists.txt")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPENMP OFF CACHE BOOL "" FORCE)
  set(ENABLE_SAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../muparser"
                   "${CMAKE_BINARY_DIR}/pm-xblox-muparser" EXCLUDE_FROM_ALL)
endif()

# Optional blocks stay out until the host installs them. constants.hpp turns
# many of these on when the macro is missing, which would compile calls the
# standalone image does not link.
set(FEATURE_XBLOX ON)
foreach(_feat IN ITEMS
    JQ MODBUS MODBUS_XBLOX MQTT MQTT_XBLOX SSH SSH_XBLOX
    SERVICE_XBLOX VECTOR_XBLOX VECTOR_LOCAL_XBLOX MCP_CLIENT
    ONNX LLAMA VLM STT WHISPER VIBEVOICE MOSS_TTS
    VIDEO VIDEO_DETECTION COMMAND_VIDEO COMMAND_LLM COMMAND_RESIZE
    COMMAND_AUDIO VOICE_COMMANDS COMMAND_IPC_NEXT COMMAND_XBLOX)
  set(FEATURE_${_feat} OFF)
endforeach()

pm_xblox_core_sources(PM_XBLOX_LIB_SOURCES)
list(APPEND PM_XBLOX_LIB_SOURCES
  "${PM_XBLOX_JSON_QUERY}"
  "${PM_XBLOX_SESSION}")

add_library(pm-xblox-lib STATIC ${PM_XBLOX_LIB_SOURCES})
target_include_directories(pm-xblox-lib
  PUBLIC
    "${PM_XBLOX_SRC}")
target_compile_features(pm-xblox-lib PUBLIC cxx_std_20)
target_link_libraries(pm-xblox-lib PUBLIC nlohmann_json::nlohmann_json)
if(TARGET pm-commons-lib)
  target_link_libraries(pm-xblox-lib PUBLIC pm-commons-lib)
endif()
if(TARGET pm-search-lib)
  target_link_libraries(pm-xblox-lib PUBLIC pm-search-lib)
endif()
if(TARGET pm-video-lib)
  target_link_libraries(pm-xblox-lib PUBLIC pm-video-lib)
endif()
if(TARGET pm-audio-lib)
  target_link_libraries(pm-xblox-lib PUBLIC pm-audio-lib)
endif()
if(TARGET logger)
  target_link_libraries(pm-xblox-lib PUBLIC logger)
endif()
if(TARGET muparser)
  target_link_libraries(pm-xblox-lib PUBLIC muparser)
endif()

if(EXISTS "${PM_XBLOX_HOST_ROOT}/src/constants.hpp")
  target_include_directories(pm-xblox-lib PRIVATE
    "${PM_XBLOX_HOST_ROOT}/src"
    "${PM_XBLOX_HOST_ROOT}/src/core")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../video.cpp/include")
  target_include_directories(pm-xblox-lib PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../video.cpp/include")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/src")
  target_include_directories(pm-xblox-lib PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/src")
endif()

set(_pm_xblox_feature_defs "FEATURE_XBLOX=1")
foreach(_feat IN ITEMS
    JQ MODBUS MODBUS_XBLOX MQTT MQTT_XBLOX SSH SSH_XBLOX
    SERVICE_XBLOX VECTOR_XBLOX VECTOR_LOCAL_XBLOX MCP_CLIENT
    ONNX LLAMA VLM STT WHISPER VIBEVOICE MOSS_TTS
    VIDEO VIDEO_DETECTION COMMAND_VIDEO COMMAND_LLM COMMAND_RESIZE
    COMMAND_AUDIO VOICE_COMMANDS COMMAND_IPC_NEXT COMMAND_XBLOX)
  list(APPEND _pm_xblox_feature_defs "FEATURE_${_feat}=0")
endforeach()
target_compile_definitions(pm-xblox-lib PRIVATE ${_pm_xblox_feature_defs})

if(MSVC)
  target_compile_definitions(pm-xblox-lib PRIVATE
    _CRT_SECURE_NO_WARNINGS NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(pm-xblox-lib PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-xblox-lib PRIVATE -Wall -Wextra)
endif()

add_executable(pm-xblox cli/main.cpp)
target_link_libraries(pm-xblox PRIVATE pm-xblox-lib CLI11::CLI11)
target_compile_features(pm-xblox PRIVATE cxx_std_20)
if(MSVC)
  target_compile_definitions(pm-xblox PRIVATE
    _CRT_SECURE_NO_WARNINGS NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(pm-xblox PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-xblox PRIVATE -Wall -Wextra)
endif()
