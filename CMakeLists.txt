cmake_minimum_required(VERSION 3.16)
# Mac: one binary for Apple Silicon and Intel. Must be set before project().
# (Ignored on other systems.)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
project(JoseVirtualSoundcard CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)
find_package(Threads REQUIRED)

# Network audio engine: PTP clock, Dante device (Inferno), test tools.
# Shared by the app, the command-line tool and the driver later.
add_library(jvs_engine STATIC
  engine/src/clock.cpp
  engine/src/net.cpp
  engine/src/ptp.cpp
  engine/src/media.cpp
  engine/src/wav.cpp
  engine/src/player.cpp
  engine/src/session.cpp
  engine/src/clock_server.cpp
  engine/src/dante_device.cpp)
target_include_directories(jvs_engine PUBLIC engine/include dante)

# ---- Inferno (Dante protocol) through the Rust bridge in dante/ ----
file(GLOB_RECURSE JVS_RUST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/dante/src/*.rs
  ${CMAKE_SOURCE_DIR}/third_party/inferno/*.rs
  ${CMAKE_SOURCE_DIR}/third_party/inferno/*/Cargo.toml)
set(JVS_RUST_LIB ${CMAKE_BINARY_DIR}/libjvs_dante.a)
if(APPLE)
  add_custom_command(OUTPUT ${JVS_RUST_LIB}
    COMMAND ${CMAKE_COMMAND} -E env MACOSX_DEPLOYMENT_TARGET=12.0
            cargo build --release -p jvs_dante --target aarch64-apple-darwin
    COMMAND ${CMAKE_COMMAND} -E env MACOSX_DEPLOYMENT_TARGET=12.0
            cargo build --release -p jvs_dante --target x86_64-apple-darwin
    COMMAND lipo -create
            ${CMAKE_SOURCE_DIR}/target/aarch64-apple-darwin/release/libjvs_dante.a
            ${CMAKE_SOURCE_DIR}/target/x86_64-apple-darwin/release/libjvs_dante.a
            -output ${JVS_RUST_LIB}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ${JVS_RUST_SOURCES} ${CMAKE_SOURCE_DIR}/dante/Cargo.toml ${CMAKE_SOURCE_DIR}/Cargo.lock
    COMMENT "Building Inferno bridge (Rust) for arm64 and x86_64")
else()
  add_custom_command(OUTPUT ${JVS_RUST_LIB}
    COMMAND cargo build --release -p jvs_dante
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/target/release/libjvs_dante.a ${JVS_RUST_LIB}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ${JVS_RUST_SOURCES} ${CMAKE_SOURCE_DIR}/dante/Cargo.toml ${CMAKE_SOURCE_DIR}/Cargo.lock
    COMMENT "Building Inferno bridge (Rust)")
endif()
add_custom_target(jvs_dante_rust DEPENDS ${JVS_RUST_LIB})
add_dependencies(jvs_engine jvs_dante_rust)
target_link_libraries(jvs_engine PUBLIC ${JVS_RUST_LIB})
if(APPLE)
  target_link_libraries(jvs_engine PUBLIC "-framework SystemConfiguration" "-framework Security" iconv)
else()
  target_link_libraries(jvs_engine PUBLIC dl m util rt)
endif()
target_link_libraries(jvs_engine PUBLIC Threads::Threads)
if(APPLE)
  target_link_libraries(jvs_engine PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
endif()

add_executable(jvs-poc
  tools/jvs-poc/main.cpp
  tools/jvs-poc/test_master.cpp)
target_link_libraries(jvs-poc PRIVATE jvs_engine)

enable_testing()
add_executable(jvs_tests tests/test_main.cpp)
target_link_libraries(jvs_tests PRIVATE jvs_engine)
add_test(NAME unit COMMAND jvs_tests)

# Two Inferno devices talking Dante to each other on one machine.
# Needs the network (and root on Linux), so it is run by CI, not ctest.
add_executable(jvs_dante_test tests/dante_loopback.cpp tools/jvs-poc/test_master.cpp)
target_include_directories(jvs_dante_test PRIVATE tools/jvs-poc)
target_link_libraries(jvs_dante_test PRIVATE jvs_engine)

# The C interface the Mac app uses, against a second device and the fake clock.
add_executable(jvs_capi_test tests/capi_test.cpp tools/jvs-poc/test_master.cpp)
target_include_directories(jvs_capi_test PRIVATE tools/jvs-poc)
target_link_libraries(jvs_capi_test PRIVATE jvs_engine)

# ---- Core Audio driver (macOS) ----
if(APPLE)
  set(JVS_VERSION "dev" CACHE STRING "Version shown by the driver")
  add_library(jvs_driver MODULE driver/Driver.cpp)
  target_compile_definitions(jvs_driver PRIVATE JVS_VERSION="${JVS_VERSION}")
  target_link_libraries(jvs_driver PRIVATE jvs_engine "-framework CoreAudio")
  # Plain binary; scripts/build-driver.sh puts it into the .driver bundle.
  set_target_properties(jvs_driver PROPERTIES OUTPUT_NAME JoseVirtualSoundcard PREFIX "" SUFFIX "")

  # Test tool: finds the driver's device, configures it, plays and records.
  add_executable(jvs_driver_test tests/driver_test.cpp tools/jvs-poc/test_master.cpp)
  target_include_directories(jvs_driver_test PRIVATE tools/jvs-poc)
  target_link_libraries(jvs_driver_test PRIVATE jvs_engine "-framework CoreAudio")
endif()
