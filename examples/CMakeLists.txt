foreach(example IN ITEMS
    generate_wav
    streaming_turn
    stream_input_llm
    multi_context
    list_voices
    enhance_file)
  add_executable(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE kugelaudio::kugelaudio)
endforeach()
