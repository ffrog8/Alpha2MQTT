cmake_minimum_required(VERSION 3.16)
project(alpha2mqtt_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(host_tests
    tests/test_main.cpp
    tests/test_scheduler.cpp
    tests/test_crc.cpp
    tests/test_frames.cpp
    tests/test_config_codec.cpp
    tests/test_boot_modes.cpp
    tests/src/TimeProviderHost.cpp
    Alpha2MQTT/src/Scheduler.cpp
    Alpha2MQTT/src/BootModes.cpp
    Alpha2MQTT/src/ConfigCodec.cpp
    Alpha2MQTT/src/ModbusCodec.cpp
)

target_include_directories(host_tests PRIVATE
    Alpha2MQTT/include
    tests/include
    tests/third_party
)

if (MSVC)
    target_compile_options(host_tests PRIVATE /W4)
else()
    target_compile_options(host_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
