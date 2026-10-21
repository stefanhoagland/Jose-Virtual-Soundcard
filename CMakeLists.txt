cmake_minimum_required(VERSION 3.16)
project(JoseVirtualSoundcard CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE)
  # One binary for Apple Silicon and Intel Macs.
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

add_compile_options(-Wall -Wextra)
find_package(Threads REQUIRED)

# Network audio engine, shared by the test tool now and the driver later.
add_library(jvs_engine STATIC
  engine/src/clock.cpp
  engine/src/net.cpp
  engine/src/ptp.cpp
  engine/src/sdp.cpp
  engine/src/sap.cpp
  engine/src/rtp.cpp
  engine/src/wav.cpp)
target_include_directories(jvs_engine PUBLIC engine/include)
target_link_libraries(jvs_engine PUBLIC Threads::Threads)

add_executable(jvs-poc
  tools/jvs-poc/main.cpp
  tools/jvs-poc/player.cpp
  tools/jvs-poc/test_master.cpp)
target_link_libraries(jvs-poc PRIVATE jvs_engine)
if(APPLE)
  target_link_libraries(jvs-poc PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()

enable_testing()
add_executable(jvs_tests tests/test_main.cpp)
target_link_libraries(jvs_tests PRIVATE jvs_engine)
add_test(NAME unit COMMAND jvs_tests)
