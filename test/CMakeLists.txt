cmake_minimum_required(VERSION 3.22)
project(room_sensor_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Project-owned sources compiled with warnings-as-errors. Only the hand-written
# Application/ test sources are in this directory; any third-party/system
# headers are still -I's, not compiled here, so -Werror never sees vendor code.
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

add_definitions(-DVEML7700_UNIT_TEST)

find_package(Python3 COMPONENTS Interpreter QUIET)

add_executable(test_veml7700
    ${CMAKE_CURRENT_SOURCE_DIR}/test_veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_i2c_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_veml7700 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
)
add_test(NAME test_veml7700 COMMAND test_veml7700)

add_executable(test_arch
    ${CMAKE_CURRENT_SOURCE_DIR}/test_arch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_arch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
)
add_test(NAME test_arch COMMAND test_arch)

add_executable(test_hex64
    ${CMAKE_CURRENT_SOURCE_DIR}/test_hex64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Src/hex64.c
)
target_include_directories(test_hex64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
)
add_test(NAME test_hex64 COMMAND test_hex64)

add_executable(test_i2c_bus
    ${CMAKE_CURRENT_SOURCE_DIR}/test_i2c_bus.c
)
target_include_directories(test_i2c_bus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
)
add_test(NAME test_i2c_bus COMMAND test_i2c_bus)

add_executable(test_storage
    ${CMAKE_CURRENT_SOURCE_DIR}/test_storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_storage PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
)
add_test(NAME test_storage COMMAND test_storage)

add_executable(test_storage_states
    ${CMAKE_CURRENT_SOURCE_DIR}/test_storage_states.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
)
target_include_directories(test_storage_states PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
)
add_test(NAME test_storage_states COMMAND test_storage_states)

add_executable(test_provisioning
    ${CMAKE_CURRENT_SOURCE_DIR}/test_provisioning.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Src/provisioning.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_provisioning PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Inc
)
add_test(NAME test_provisioning COMMAND test_provisioning)

add_executable(test_boot_persistence
    ${CMAKE_CURRENT_SOURCE_DIR}/test_boot_persistence.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_boot_persistence PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
)
add_test(NAME test_boot_persistence COMMAND test_boot_persistence)

add_executable(test_config_redundancy
    ${CMAKE_CURRENT_SOURCE_DIR}/test_config_redundancy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_config_redundancy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
)
add_test(NAME test_config_redundancy COMMAND test_config_redundancy)

# App-level first-boot boot-orchestration regression. Links app.c (the real
# boot policy) plus the modules and host platform bindings it needs.
add_executable(test_boot_health
    ${CMAKE_CURRENT_SOURCE_DIR}/test_boot_health.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/self_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Src/provisioning.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_lifecycle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/boot_session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_manifest.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_capabilities.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/firmware_metadata.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_manifest_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Src/communication.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Src/communication_debug.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Src/hex64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_response.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Platform/Host/Src/platform_host_reset.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Platform/Host/Src/platform_host_watchdog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Platform/Host/Src/platform_host_hardware.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_i2c_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_boot_health PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Platform/Host/Inc
)
target_link_libraries(test_boot_health m)
add_test(NAME test_boot_health COMMAND test_boot_health)

add_executable(test_self_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test_self_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/self_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_i2c_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
)
target_include_directories(test_self_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Inc
)
add_test(NAME test_self_test COMMAND test_self_test)

add_executable(test_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/test_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Src/provisioning.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
)
target_include_directories(test_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Inc
)
add_test(NAME test_parser COMMAND test_parser)

add_executable(test_command
    ${CMAKE_CURRENT_SOURCE_DIR}/test_command.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Src/hex64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_response.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/self_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_manifest.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_capabilities.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/firmware_metadata.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_manifest_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/boot_session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_lifecycle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Src/provisioning.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_i2c_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Platform/Host/Src/platform_host_hardware.c
)
target_include_directories(test_command PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Inc
)
target_include_directories(test_command PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Inc)
target_include_directories(test_command PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Inc)
target_include_directories(test_command PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Platform/Host/Inc)
target_link_libraries(test_command m)
add_test(NAME test_command COMMAND test_command)

add_executable(test_telemetry
    ${CMAKE_CURRENT_SOURCE_DIR}/test_telemetry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Src/hex64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Src/communication.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_communication_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_telemetry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Inc
)
add_test(NAME test_telemetry COMMAND test_telemetry)

# Real-JSON wire-format validation: a C harness emits schema-v3 payloads (valid,
# invalid, mixed, max-CO2 and non-finite cases) and a Python script parses each
# with json.loads and asserts the decoded values. Proves every serialized
# payload is syntactically valid JSON (substring tests cannot catch malformed
# output).
add_executable(test_telemetry_json
    ${CMAKE_CURRENT_SOURCE_DIR}/test_telemetry_json.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Src/hex64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
)
target_include_directories(test_telemetry_json PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Inc
)
target_link_libraries(test_telemetry_json m)
add_test(NAME test_telemetry_json COMMAND test_telemetry_json)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_telemetry_json_parse
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_telemetry_json.py $<TARGET_FILE:test_telemetry_json>)
endif()

add_executable(test_longrun
    ${CMAKE_CURRENT_SOURCE_DIR}/test_longrun.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Src/hex64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Src/communication.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_communication_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_longrun PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Inc
)
target_link_libraries(test_longrun m)
add_test(NAME test_longrun COMMAND test_longrun)

# SCD41 driver, runtime, RoomState, telemetry, display and self-test regression.
add_executable(test_scd41
    ${CMAKE_CURRENT_SOURCE_DIR}/test_scd41.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Src/hex64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/self_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_response.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Src/command.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_manifest.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_capabilities.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/firmware_metadata.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_manifest_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/boot_session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_lifecycle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Src/provisioning.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Src/communication.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Src/communication_debug.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Platform/Host/Src/platform_host_hardware.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_communication_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_i2c_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
)
target_include_directories(test_scd41 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Command/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Provisioning/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Platform/Host/Inc
)
target_link_libraries(test_scd41 m)
add_test(NAME test_scd41 COMMAND test_scd41)

# SHT45 driver, runtime, RoomState, telemetry, self-test and capabilities.
add_executable(test_sht45
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sht45.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/device_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Src/device_capabilities.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Src/telemetry_serializer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Src/hex64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_communication_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_i2c_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_unique_id.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Src/communication.c
)
target_include_directories(test_sht45 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Storage/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Device/Inc
)
target_link_libraries(test_sht45 m)
add_test(NAME test_sht45 COMMAND test_sht45)

# BMP390 driver compensation regression. Compiles the PRODUCTION bmp390.c with
# BMP390_UNIT_TEST so its static ParseCalib/Raw24/Compensate are exposed via
# bmp390_test.h. The test asserts against FROZEN Bosch BMP3_SensorAPI v2.0.6
# golden values (produced offline by a standalone oracle that links the official
# Bosch bmp3.c). No Bosch code is linked here and none is called by the test.
add_executable(test_bmp390
    ${CMAKE_CURRENT_SOURCE_DIR}/test_bmp390.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390.c
)
target_compile_definitions(test_bmp390 PRIVATE BMP390_UNIT_TEST=1)
target_include_directories(test_bmp390 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
)
target_link_libraries(test_bmp390 m)
add_test(NAME test_bmp390 COMMAND test_bmp390)

# BMP390 driver + runtime + RoomState integration. Links the production driver,
# runtime and RoomState with BMP390_UNIT_TEST. Uses a self-contained address-aware
# fake (inside the test) so discovery/ERR/recovery/validity are exercised without
# perturbing the shared fake_i2c_bus.
add_executable(test_bmp390_integration
    ${CMAKE_CURRENT_SOURCE_DIR}/test_bmp390_integration.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Src/room_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
)
target_compile_definitions(test_bmp390_integration PRIVATE BMP390_UNIT_TEST=1)
target_include_directories(test_bmp390_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
)
target_link_libraries(test_bmp390_integration m)
add_test(NAME test_bmp390_integration COMMAND test_bmp390_integration)

# Recovery policy / NOT_FOUND backoff / shared-bus health / long-run fault
# injection (Phases 2-10). Links the runtimes + fakes; the policy/monitor are
# header-inline.
add_executable(test_recovery
    ${CMAKE_CURRENT_SOURCE_DIR}/test_recovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Src/sht45_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Src/scd41_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Src/bmp390_runtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Src/veml7700.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_i2c_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_platform_time.c
)
target_include_directories(test_recovery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/sht45/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/scd41/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/bmp390/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/veml7700/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Drivers/display/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/App/Inc
)
target_link_libraries(test_recovery m)
add_test(NAME test_recovery COMMAND test_recovery)