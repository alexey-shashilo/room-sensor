cmake_minimum_required(VERSION 3.22)
project(veml7700_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_definitions(-DVEML7700_UNIT_TEST)

add_executable(test_veml7700
    ${CMAKE_CURRENT_SOURCE_DIR}/test_veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_i2c_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
)

target_include_directories(test_veml7700 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
)

enable_testing()
add_test(NAME test_veml7700 COMMAND test_veml7700)