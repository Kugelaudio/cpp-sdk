include(GoogleTest)

# Unit and mock-server tests reach into src/ for the internal mapping and
# diagnostics seams, and use IXWebSocket's servers as local mocks.
function(kugelaudio_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE
    kugelaudio::kugelaudio
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    ixwebsocket::ixwebsocket
    CURL::libcurl
    Threads::Threads)
  target_include_directories(${name} PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(${name} PRIVATE
    KUGELAUDIO_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 120)
endfunction()

kugelaudio_add_test(kugelaudio_unit_tests
  test_config_errors.cpp
  test_mapping_audio.cpp
  test_diagnostics.cpp)

kugelaudio_add_test(kugelaudio_mock_tests
  test_rest_mock.cpp
  test_tts_stream_mock.cpp
  test_streaming_session_mock.cpp
  test_multi_context_mock.cpp
  test_enhance_mock.cpp)

kugelaudio_add_test(kugelaudio_live_tests test_live.cpp)
