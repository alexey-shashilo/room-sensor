# Platform Abstraction Layer (PAL)

## Layering

```
+------------------------------------------------------+
|                 Application                          |
|     RoomState, Telemetry, Runtime, Drivers,           |
|     Command, Config, Identity, Storage, Scheduler,    |
|     Diagnostics                                        |
+------------------------------------------------------+
|                 Platform API (headers only)          |
|  platform.h (umbrella), i2c_bus.h, platform_time.h,   |
|  platform_delay.h, platform_watchdog.h,               |
|  platform_reset.h, platform_unique_id.h,              |
|  platform_flash.h, platform_gpio.h, platform_init.h   |
+------------------------------------------------------+
|         STM32G474 Implementation (Src/*_stm32)       |
|  HAL, CubeMX, GPIO, RCC, NVIC, Startup                |
+------------------------------------------------------+
```

## Allowed dependencies

| Layer | May depend on |
|-------|---------------|
| Application | Platform API headers only |
| Platform API | `<stdint.h>`, `platform_types.h` |
| STM32 implementation | Platform API + `stm32g4xx_hal.h` |

Application must NEVER include `stm32g4xx_hal.h`, `stm32g4xx.h`, or any HAL type.

## Porting checklist to STM32H7

To add a new MCU (e.g. STM32H7), implement only:

1. New CubeMX project (`*.ioc`)
2. Linker script (`STM32H750xx_FLASH.ld`)
3. Startup file (`startup_stm32h750xx.s`)
4. Platform source files in `Platform/Src/`:
   - `platform_time_stm32h7.c`
   - `platform_flash_stm32h7.c`
   - `platform_watchdog_stm32h7.c`
   - `platform_reset_stm32h7.c`
   - `platform_uid_stm32h7.c`
   - `platform_gpio_stm32h7.c`
   - `i2c_bus_stm32h7.c`
   - `platform_init_stm32h7.c`
5. Adjust `platform_types.h` if the new MCU has different capabilities

**No Application module requires modification.**

## Current HAL dependency boundary

HAL is included ONLY in:
- `Application/User/Platform/Src/*.c` (STM32 implementations)
- `Application/User/Platform/Inc/i2c_bus_stm32.h`
- `Core/*` (CubeMX generated)

## Porting procedure

1. Create new CubeMX project for target MCU
2. Configure clocks, I2C, UART, GPIO, watchdog
3. Copy `Application/`, `Platform/Inc/` unchanged
4. Implement `Platform/Src/*_stm32h7.c` against the new HAL
5. Update `cmake/gcc-arm-none-eabi.cmake` for new CPU flags
6. Assemble build; fix Platform adapter compile errors only

## Estimated effort for STM32H7

Given the current clean separation (HAL touches only Platform/Src), adding STM32H7
requires ~8 new Platform adapter files (each 40-80 lines) + CubeMX + linker +
startup. Estimated: 1-2 focused developer-days, no Application changes.

## Host platform

Host tests use the same Platform API with fakes in `test/fakes/`. No HAL needed.