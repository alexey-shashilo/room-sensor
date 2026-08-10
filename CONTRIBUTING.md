# Contributing to Room Sensor

## Before committing

1. Run host tests:

```bash
cmake -S . -B build-host -DBUILD_HOST_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host --parallel
ctest --test-dir build-host --output-on-failure
```

2. Run sanitizers (if host GCC supports):

```bash
cmake -S . -B build-asan -DBUILD_HOST_TESTS=ON \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

3. Build firmware with 0 errors/0 warnings:

```bash
cmake -S . -B build-fw -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build-fw --target room_sensor --parallel
```

4. Verify no dynamic memory in Application/User:

```bash
! grep -rn 'malloc\|calloc\|realloc\|free' Application/User/ --include='*.c' --include='*.h'
```

5. Verify no HAL_Delay in portable modules

## Architecture rules

- Portable modules must not #include "stm32g4xx_hal.h"
- Telemetry must not access Storage
- Drivers must not access Communication
- No dynamic memory allocation
- No RTOS
- No blocking HAL_Delay in runtime loops

## Schema versioning

When changing wire format:
- `TELEMETRY_SCHEMA_VERSION` for telemetry JSON changes
- `COMMAND_SCHEMA_VERSION` for command protocol changes
- `IDENTITY_SCHEMA_VERSION` for identity persistence changes
- `CONFIG_SCHEMA_VERSION` for configuration persistence changes
- `STORAGE_RECORD_FORMAT_VERSION` for storage record changes

## Test policy

- Every new feature needs host-side tests
- Protocol changes need golden JSON fixture updates
- No regression — all existing tests must pass