cmake_minimum_required(VERSION 3.20)
project(kugelaudio_consumer LANGUAGES CXX)

# Smoke test of the installed package: find_package + link + run.
find_package(kugelaudio 0.1 CONFIG REQUIRED)
add_executable(kugelaudio_consumer main.cpp)
target_link_libraries(kugelaudio_consumer PRIVATE kugelaudio::kugelaudio)
