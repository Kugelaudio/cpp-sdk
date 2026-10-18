cmake_minimum_required(VERSION 3.20)

project(kugelaudio
  VERSION 0.1.1
  DESCRIPTION "Official C++ SDK for the KugelAudio text-to-speech API"
  HOMEPAGE_URL "https://github.com/Kugelaudio/cpp-sdk"
  LANGUAGES CXX)

set(KUGELAUDIO_IS_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(KUGELAUDIO_IS_TOP_LEVEL ON)
endif()

# Tests follow CMake's conventional BUILD_TESTING switch when this is the
# top-level project; KUGELAUDIO_BUILD_TESTS overrides it.
set(KUGELAUDIO_TESTS_DEFAULT OFF)
if(KUGELAUDIO_IS_TOP_LEVEL AND BUILD_TESTING)
  set(KUGELAUDIO_TESTS_DEFAULT ON)
endif()
option(KUGELAUDIO_BUILD_TESTS "Build the KugelAudio unit and mock-server tests"
  ${KUGELAUDIO_TESTS_DEFAULT})
option(KUGELAUDIO_BUILD_EXAMPLES "Build the KugelAudio examples" OFF)
option(KUGELAUDIO_INSTALL "Generate the install target" ${KUGELAUDIO_IS_TOP_LEVEL})
option(KUGELAUDIO_FETCH_DEPS
  "Download IXWebSocket, nlohmann_json and GoogleTest with FetchContent when find_package does not find them"
  ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(KugelAudioDependencies)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/include/kugelaudio/version.hpp"
  @ONLY)

add_library(kugelaudio
  src/audio.cpp
  src/base64.cpp
  src/client.cpp
  src/diagnostics.cpp
  src/enhance.cpp
  src/errors.cpp
  src/http_transport.cpp
  src/json_mapping.cpp
  src/multi_context_session.cpp
  src/resources.cpp
  src/streaming_session.cpp
  src/tts.cpp
  src/url.cpp
  src/ws_connection.cpp)
add_library(kugelaudio::kugelaudio ALIAS kugelaudio)

target_compile_features(kugelaudio PUBLIC cxx_std_17)
set_target_properties(kugelaudio PROPERTIES
  CXX_EXTENSIONS OFF
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  EXPORT_NAME kugelaudio)

include(GNUInstallDirs)
target_include_directories(kugelaudio
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

# nlohmann_json is header-only and used only inside src/, so it never becomes
# a link dependency of consumers.
target_link_libraries(kugelaudio
  PRIVATE
    "$<BUILD_INTERFACE:nlohmann_json::nlohmann_json>"
    CURL::libcurl
    ixwebsocket::ixwebsocket
    Threads::Threads)

if(MSVC)
  target_compile_options(kugelaudio PRIVATE /W4 /permissive-)
else()
  target_compile_options(kugelaudio PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(KUGELAUDIO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(KUGELAUDIO_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(KUGELAUDIO_INSTALL)
  include(CMakePackageConfigHelpers)
  set(KUGELAUDIO_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/kugelaudio")

  install(TARGETS kugelaudio
    EXPORT kugelaudioTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/kugelaudio DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/include/kugelaudio/version.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/kugelaudio")
  install(EXPORT kugelaudioTargets
    FILE kugelaudioTargets.cmake
    NAMESPACE kugelaudio::
    DESTINATION ${KUGELAUDIO_CMAKE_DIR})

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kugelaudioConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/kugelaudioConfig.cmake"
    INSTALL_DESTINATION ${KUGELAUDIO_CMAKE_DIR})
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/kugelaudioConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/kugelaudioConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/kugelaudioConfigVersion.cmake"
    DESTINATION ${KUGELAUDIO_CMAKE_DIR})
endif()
