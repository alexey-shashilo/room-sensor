cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(BUILD_HOST_TESTS "Build host-side unit tests" OFF)

if(BUILD_HOST_TESTS)
    add_subdirectory(test)
    return()
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_PROJECT_NAME room_sensor)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

enable_language(C ASM)

add_executable(${CMAKE_PROJECT_NAME})

add_subdirectory(cmake/stm32cubemx)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/display/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/i2c_bus_stm32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/platform_time_stm32.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/display/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Common/Inc
)

list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
)