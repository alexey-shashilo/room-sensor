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

## Build hygiene: canonical build directories

Agents and developers MUST use only the canonical build directories below and
MUST NOT create ad-hoc `build-*`/`cmake-build-*` directories for individual
checks.

| Purpose                | Directory           | Configured with                      |
|------------------------|---------------------|--------------------------------------|
| Host tests + CTest     | `build-host`        | `-DBUILD_HOST_TESTS=ON`              |
| Portable core (host)   | `build-core`        | `-DBUILD_HOST_PLATFORM=ON`           |
| ARM firmware (Debug)   | `build-fw-debug`    | `-DCMAKE_BUILD_TYPE=Debug` + ARM toolchain |
| ARM firmware (Release) | `build-fw-release`  | `-DCMAKE_BUILD_TYPE=Release` + ARM toolchain |
| Sanitizer host build   | `build-sanitizers`  | `-DBUILD_HOST_TESTS=ON` + `-fsanitize` |

Rules:

- Use ONLY the canonical directories. Do not invent `build-check`, `build-final`,
  `build-tmpXY`, etc.
- For a clean build, delete the relevant canonical directory and reconfigure
  there — do not create another uniquely named directory.
- Temporary build directories are allowed only when technically required and
  MUST be removed before the phase ends.
- Build directories and generated artifacts (`.o`, `.obj`, `.elf`, `.bin`,
  `.hex`, `.map`, `.su`, `CMakeCache.txt`, `build.ninja`, `Testing/`) must remain
  ignored by Git (see `.gitignore`).
- Never place manually-authored source or persistent data inside a build
  directory.
- Before finishing a phase, verify no unexpected build artifacts are tracked or
  left in the repository root.
- Never run a broad `git clean -fdx` while uncommitted work exists.

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