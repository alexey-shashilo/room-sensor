cmake_minimum_required(VERSION 3.22)
project(room_sensor_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_definitions(-DVEML7700_UNIT_TEST)

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
)
target_include_directories(test_command PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Telemetry/Inc)
target_include_directories(test_command PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Application/User/Communication/Inc)
target_include_directories(test_command PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Platform/Host/Inc)
target_link_libraries(test_command m)
add_test(NAME test_command COMMAND test_command)

add_executable(test_telemetry
    ${CMAKE_CURRENT_SOURCE_DIR}/test_telemetry.c
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

add_executable(test_longrun
    ${CMAKE_CURRENT_SOURCE_DIR}/test_longrun.c
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

enable_testing()