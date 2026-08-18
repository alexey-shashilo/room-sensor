# room_sensor_core.cmake — defines portable core sources, includes, and a helper function

set(CORE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/self_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Src/display_pages.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/display/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/scd41/Src/scd41.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/scd41/Src/scd41_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/sht45/Src/sht45.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/sht45/Src/sht45_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/bmp390/Src/bmp390.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/bmp390/Src/bmp390_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/sgp41/Src/sgp41.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/sgp41/Src/sgp41_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/gas_index/Src/gas_index.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Device/Src/device_lifecycle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Provisioning/Src/provisioning.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Common/Src/hex64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Network/Src/network_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Mqtt/Src/mqtt_codec.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Mqtt/Src/mqtt_client.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Mqtt/Src/mqtt_utf8.c
)

set(CORE_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/display/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/scd41/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/sht45/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/bmp390/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/sgp41/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Drivers/gas_index/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Telemetry/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Communication/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Command/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Device/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Provisioning/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Network/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/User/Mqtt/Inc
)

function(core_add_library TARGET)
    add_library(${TARGET} STATIC ${CORE_SRC})
    target_include_directories(${TARGET} PUBLIC ${CORE_INC})
    # Project-owned portable core compiled with warnings as errors.
    target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_definitions(${TARGET} PUBLIC
        ROOM_SENSOR_VERSION="0.2.0-dev"
        ROOM_SENSOR_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    )
endfunction()