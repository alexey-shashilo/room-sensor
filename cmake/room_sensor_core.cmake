# room_sensor_core.cmake
# Defines the portable room_sensor_core static library.
# This target must compile with a host compiler — no ARM toolchain required.
# No STM32 HAL dependencies allowed.

set(CORE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/self_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/display/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Telemetry/Src/telemetry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Telemetry/Src/telemetry_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Communication/Src/communication.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Communication/Src/communication_debug.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Command/Src/command.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Command/Src/command_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Command/Src/command_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Command/Src/command_response.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Device/Src/device_manifest.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Device/Src/device_capabilities.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Device/Src/firmware_metadata.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Device/Src/boot_session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Device/Src/device_manifest_serializer.c
)

set(CORE_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/display/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Telemetry/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Communication/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Command/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Device/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Common/Inc
)

add_library(room_sensor_core STATIC ${CORE_SRC})
target_include_directories(room_sensor_core PUBLIC ${CORE_INC})
target_compile_options(room_sensor_core PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(room_sensor_core PUBLIC
    ROOM_SENSOR_VERSION="0.2.0-dev"
    ROOM_SENSOR_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)