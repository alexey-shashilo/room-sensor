# Room Sensor

Firmware for NUCLEO-G474RE with VEML7700 ambient light sensor and SH1106/SSD1306 OLED display.

## Hardware

| Component | Connection |
|-----------|-----------|
| MCU       | STM32G474RET6 (NUCLEO-G474RE) |
| I2C1 SCL  | PB8 (Arduino D15) |
| I2C1 SDA  | PB9 (Arduino D14) |
| Light     | VEML7700 (0x10) |
| Display   | SH1106 / SSD1306 (0x3C) |

## Software Versions

- STM32CubeMX 6.18.1 (FW_G4 1.6.3)
- ARM GCC 14.3.rel1 (GNU Tools for STM32)
- CMake 4.3.1 + Ninja 1.13.2

## Build & Flash

```bash
# Configure
cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build/Debug --target room_sensor

# Flash (STM32CubeProgrammer)
STM32_Programmer_CLI -c port=SWD mode=UR \
  -w build/Debug/room_sensor.elf -rst
```

## Architecture

```
Application/User/
├── App/                   # Application logic, lifecycle, scheduling
├── Drivers/
│   ├── display/           # OLED driver (SH1106 + SSD1306)
│   └── veml7700/          # Ambient light sensor driver
├── Platform/              # I2C bus abstraction (portable)
└── Common/                # Shared types and status structs
```

## Pin Configuration (room_sensor.ioc)

| Signal | Pin  | Function |
|--------|------|----------|
| I2C1_SCL | PB8 | AF4 |
| I2C1_SDA | PB9 | AF4 |

## Features

- I2C bus abstraction — drivers do not depend on stm32g4xx_hal.h
- Cooperative scheduling (no blocking HAL_Delay in main loop)
- Device state machine with automatic retry/reconnect
- Runtime SH1106/SSD1306 selection via DisplayController enum
- VEML datasheet-compliant register encoding with read-back verification
- No exported globals — App_GetStatus() provides runtime status