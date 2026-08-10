cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(BUILD_HOST_TESTS "Build host-side unit tests" OFF)
option(BUILD_HOST_PLATFORM "Build with host platform instead of STM32" OFF)

# ============================================================
# Common definitions
# ============================================================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_PROJECT_NAME room_sensor)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
project(${CMAKE_PROJECT_NAME})

# ============================================================
# Git SHA injection (deterministic fallback)
# ============================================================
execute_process(
    COMMAND git rev-parse --short=8 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_SHA)
    set(GIT_SHA "unknown")
endif()
set(ROOM_SENSOR_GIT_SHA "${GIT_SHA}" CACHE STRING "Git commit SHA")

# ============================================================
# Portable Core Library
# ============================================================
include(cmake/room_sensor_core.cmake)
target_compile_definitions(room_sensor_core PUBLIC
    ROOM_SENSOR_GIT_SHA="${GIT_SHA}"
)

# ============================================================
# Platform selection
# ============================================================
if(BUILD_HOST_TESTS)
    add_subdirectory(test)
    return()
endif()

if(BUILD_HOST_PLATFORM)
    # Host platform build — compile core only with host platform
    # (used for local portability verification)
    enable_language(C)

    add_executable(room_sensor_host)
    set_target_properties(room_sensor_host PROPERTIES WIN32_EXECUTABLE FALSE)

    target_sources(room_sensor_host PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/host_main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/platform_host_time.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/platform_host_flash.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/platform_host_i2c.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/platform_host_uid.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/platform_host_watchdog.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/platform_host_reset.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/platform_host_gpio.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/platform_host_init.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Src/platform_host_hardware.c
    )

    target_include_directories(room_sensor_host PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Platform/Host/Inc
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Communication/Inc
    )

    target_link_libraries(room_sensor_host PRIVATE room_sensor_core m)

    add_custom_target(run_host
        COMMAND room_sensor_host
        DEPENDS room_sensor_host
    )

    message(STATUS "Host platform build — no ARM toolchain required")

else()
    # ============================================================
    # STM32G474 Firmware build
    # ============================================================
    enable_language(C ASM)

    add_executable(${CMAKE_PROJECT_NAME})

    add_subdirectory(cmake/stm32cubemx)

    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/i2c_bus_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/platform_time_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/platform_watchdog_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/platform_reset_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/platform_flash_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/platform_unique_id_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/platform_gpio_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/platform_init_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Src/platform_hardware_stm32.c
    )

    target_link_libraries(${CMAKE_PROJECT_NAME}
    room_sensor_core stm32cubemx)

    list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
)

string(TIMESTAMP BUILD_TIMESTAMP UTC)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        ROOM_SENSOR_BUILD_TIMESTAMP="${BUILD_TIMESTAMP}"
    )
endif()