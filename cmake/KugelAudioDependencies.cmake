# Resolves the SDK's dependencies: an installed package first (vcpkg, Homebrew,
# the system), then a pinned FetchContent download when KUGELAUDIO_FETCH_DEPS
# is ON. Pins were checked against the upstream releases on 2026-10-18.
#
#   libcurl        system / vcpkg `curl`        (never downloaded)
#   IXWebSocket    12.0.1  vcpkg `ixwebsocket`  (12.1.0 was four days old)
#   nlohmann_json  3.12.0  vcpkg `nlohmann-json`
#   GoogleTest     1.18.0  vcpkg `gtest`         (tests only)

include(FetchContent)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT KUGELAUDIO_FETCH_DEPS)
    message(FATAL_ERROR "nlohmann_json not found; install it or set KUGELAUDIO_FETCH_DEPS=ON")
  endif()
  message(STATUS "kugelaudio: fetching nlohmann_json 3.12.0")
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  set(JSON_Install OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(ixwebsocket CONFIG QUIET)
if(NOT ixwebsocket_FOUND)
  if(NOT KUGELAUDIO_FETCH_DEPS)
    message(FATAL_ERROR "ixwebsocket not found; install it or set KUGELAUDIO_FETCH_DEPS=ON")
  endif()
  message(STATUS "kugelaudio: fetching IXWebSocket 12.0.1")
  FetchContent_Declare(ixwebsocket
    URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v12.0.1.tar.gz
    URL_HASH SHA256=d23bdc91dbfe2b9ae13c322d539392d7a6b8b506560f41c90e227fa0f86a2405
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  set(USE_TLS ON CACHE BOOL "" FORCE)
  # zlib only serves per-message deflate, which the SDK turns off.
  set(USE_ZLIB OFF CACHE BOOL "" FORCE)
  # Installed next to kugelaudio so find_dependency(ixwebsocket) succeeds for
  # consumers of the installed package.
  set(IXWEBSOCKET_INSTALL ${KUGELAUDIO_INSTALL} CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(ixwebsocket)
  if(NOT TARGET ixwebsocket::ixwebsocket)
    add_library(ixwebsocket::ixwebsocket ALIAS ixwebsocket)
  endif()
endif()

if(KUGELAUDIO_BUILD_TESTS)
  find_package(GTest 1.14 CONFIG QUIET)
  if(NOT GTest_FOUND)
    if(NOT KUGELAUDIO_FETCH_DEPS)
      message(FATAL_ERROR "GoogleTest not found; install it or set KUGELAUDIO_FETCH_DEPS=ON")
    endif()
    message(STATUS "kugelaudio: fetching GoogleTest 1.18.0")
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
      URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
      DOWNLOAD_EXTRACT_TIMESTAMP ON)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()
