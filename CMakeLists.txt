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
  add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/pm-${name}-lib" EXCLUDE_FROM_ALL)
endfunction()

# Logger, PicoSHA2, and p-ranav/glob must exist before commons. Its installer
# and glob units include them whenever this package is not the top-level project.
if(NOT TARGET logger AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/cmake/logging.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/cmake/logging.cmake")
endif()
if(TARGET logger)
  target_compile_features(logger PUBLIC cxx_std_20)
endif()

include(FetchContent)
if(NOT picosha2_SOURCE_DIR)
  FetchContent_Declare(picosha2
    GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
  FetchContent_GetProperties(picosha2)
  if(NOT picosha2_POPULATED)
    FetchContent_Populate(picosha2)
  endif()
endif()
if(NOT asio_SOURCE_DIR)
  set(_pm_asio_cached "${PM_XBLOX_HOST_ROOT}/build/release-debug/_deps/asio-src")
  if(EXISTS "${_pm_asio_cached}/asio/include/asio.hpp")
    set(asio_SOURCE_DIR "${_pm_asio_cached}")
  else()
    FetchContent_Declare(asio
      GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
      GIT_TAG        asio-1-28-0
      GIT_SHALLOW    TRUE)
    FetchContent_GetProperties(asio)
    if(NOT asio_POPULATED)
      FetchContent_Populate(asio)
    endif()
  endif()
endif()
if(NOT TARGET pranav_glob)
  FetchContent_Declare(pranav_glob
    GIT_REPOSITORY https://github.com/p-ranav/glob.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
  FetchContent_GetProperties(pranav_glob)
  if(NOT pranav_glob_POPULATED)
    FetchContent_Populate(pranav_glob)
  endif()
  add_library(pranav_glob STATIC ${pranav_glob_SOURCE_DIR}/source/glob.cpp)
  target_include_directories(pranav_glob PUBLIC ${pranav_glob_SOURCE_DIR}/include)
  target_compile_features(pranav_glob PUBLIC cxx_std_17)
  if(MSVC)
    target_compile_options(pranav_glob PRIVATE /permissive-)
  endif()
endif()

# commons before search, audio, and video. Those packages link pm::commons
# when the target is already in the build. audio before video: video's
# public headers include pm/audio.
set(PM_COMMONS_LEAN ON)
pm_xblox_add_sibling(commons)
set(_pm_boost_headers "${PM_XBLOX_HOST_ROOT}/build/release-debug/_deps/boost_headers-src")
set(_pm_boost_url_lib "${PM_XBLOX_HOST_ROOT}/build/release-debug/_deps/boost-build/libs/url/libboost_url-vc145-mt-x64-1_88.lib")
if(TARGET pm-commons-lib AND EXISTS "${_pm_boost_headers}/boost/url.hpp" AND EXISTS "${_pm_boost_url_lib}")
  target_include_directories(pm-commons-lib PRIVATE "${_pm_boost_headers}")
  target_compile_definitions(pm-commons-lib PRIVATE BOOST_ALL_NO_LIB)
  target_link_libraries(pm-commons-lib PUBLIC "${_pm_boost_url_lib}")
endif()
pm_xblox_add_sibling(search)
pm_xblox_add_sibling(audio)
pm_xblox_add_sibling(video)

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

list(APPEND CMAKE_MODULE_PATH "${PM_XBLOX_HOST_ROOT}/cmake")
set(VIPS_ROOT "${PM_XBLOX_HOST_ROOT}/dist/vendor/vips-dev-8.18")
find_package(Vips QUIET)

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
if(TARGET pm-image-lib)
  target_link_libraries(pm-xblox-lib PUBLIC pm-image-lib)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../image.cpp/src/images/image_quality.hpp")
  target_include_directories(pm-xblox-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../image.cpp/src")
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
    "${PM_XBLOX_HOST_ROOT}/packages/llm.cpp/src"
    "${PM_XBLOX_HOST_ROOT}/src/core")
endif()
if(WIN32 AND EXISTS "${PM_XBLOX_HOST_ROOT}/packages/win32/src/inspect/app_inspect.hpp")
  target_include_directories(pm-xblox-lib PRIVATE
    "${PM_XBLOX_HOST_ROOT}/packages/win32/src")
endif()
if(EXISTS "${PM_XBLOX_HOST_ROOT}/build/release-debug/include/pm_branding_config.hpp")
  target_include_directories(pm-xblox-lib PRIVATE
    "${PM_XBLOX_HOST_ROOT}/build/release-debug/include")
endif()
if(picosha2_SOURCE_DIR)
  target_include_directories(pm-xblox-lib PRIVATE "${picosha2_SOURCE_DIR}")
endif()
if(asio_SOURCE_DIR)
  target_include_directories(pm-xblox-lib PRIVATE "${asio_SOURCE_DIR}/asio/include")
endif()
set(_pm_curl_inc "${PM_XBLOX_HOST_ROOT}/build/release-debug/_deps/curl-src/include")
set(_pm_curl_lib "${PM_XBLOX_HOST_ROOT}/build/release-debug/_deps/curl-build/lib/libcurl.lib")
if(EXISTS "${_pm_curl_inc}/curl/curl.h")
  target_include_directories(pm-xblox-lib PRIVATE "${_pm_curl_inc}")
endif()
if(EXISTS "${PM_XBLOX_HOST_ROOT}/packages/commons.cpp/src/commands/command_variables.cpp")
  include("${PM_XBLOX_HOST_ROOT}/packages/llm.cpp/cmake/host-sources.cmake")
  pm_llm_sources(run _pm_llm_run)
  target_sources(pm-xblox-lib PRIVATE
    "${PM_XBLOX_HOST_ROOT}/packages/commons.cpp/src/commands/command_variables.cpp"
    "${PM_XBLOX_HOST_ROOT}/src/core/command_variables_host.cpp"
    "${PM_XBLOX_HOST_ROOT}/packages/commons.cpp/src/process/app_exe_directory.cpp"
    "${PM_XBLOX_HOST_ROOT}/src/commands/command_registry.cpp"
    "${PM_XBLOX_HOST_ROOT}/src/commands/options_registry.cpp"
    "${PM_XBLOX_HOST_ROOT}/src/android/xblox_transfer_local.cpp"
    "${PM_XBLOX_HOST_ROOT}/packages/commons.cpp/src/net/http_client.cpp"
    ${_pm_llm_run}
    "${CMAKE_CURRENT_SOURCE_DIR}/cli/standalone_host.cpp")
endif()
if(EXISTS "${_pm_curl_lib}")
  target_link_libraries(pm-xblox-lib PUBLIC "${_pm_curl_lib}" ws2_32 wldap32 crypt32 normaliz bcrypt)
endif()
if(TARGET Vips::vips)
  target_link_libraries(pm-xblox-lib PUBLIC Vips::vips)
  set(_pm_vips_libdir "${PM_XBLOX_HOST_ROOT}/dist/vendor/vips-dev-8.18/lib")
  if(EXISTS "${_pm_vips_libdir}/libglib-2.0.lib")
    target_link_libraries(pm-xblox-lib PUBLIC
      "${_pm_vips_libdir}/libglib-2.0.lib"
      "${_pm_vips_libdir}/libgobject-2.0.lib")
  endif()
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
target_compile_definitions(pm-xblox-lib PRIVATE
  ${_pm_xblox_feature_defs}
  POLYMECH_STATIC_BUILD=1
  ASIO_STANDALONE
  CURL_STATICLIB)

if(MSVC)
  target_compile_definitions(pm-xblox-lib PRIVATE
    _CRT_SECURE_NO_WARNINGS NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(pm-xblox-lib PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-xblox-lib PRIVATE -Wall -Wextra)
endif()

add_executable(pm-xblox cli/main.cpp cli/xblox_cli.cpp)
target_link_libraries(pm-xblox PRIVATE pm-xblox-lib CLI11::CLI11)
target_compile_features(pm-xblox PRIVATE cxx_std_20)
target_include_directories(pm-xblox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cli")
if(EXISTS "${PM_XBLOX_HOST_ROOT}/src/constants.hpp")
  target_include_directories(pm-xblox PRIVATE
    "${PM_XBLOX_HOST_ROOT}/src"
    "${PM_XBLOX_HOST_ROOT}/src/core")
endif()
if(EXISTS "${PM_XBLOX_HOST_ROOT}/build/release-debug/include/pm_branding_config.hpp")
  target_include_directories(pm-xblox PRIVATE
    "${PM_XBLOX_HOST_ROOT}/build/release-debug/include")
endif()
target_compile_definitions(pm-xblox PRIVATE
  ${_pm_xblox_feature_defs}
  POLYMECH_STATIC_BUILD=1
  ASIO_STANDALONE)
if(MSVC)
  target_compile_definitions(pm-xblox PRIVATE
    _CRT_SECURE_NO_WARNINGS NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(pm-xblox PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-xblox PRIVATE -Wall -Wextra)
endif()
