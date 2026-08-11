# Embedded Stack Budget

Device: STM32G474 (Cortex-M4, FPU), 128 KB RAM, 512 KB Flash.

## Objective

Define and document a justified main-stack reservation so that the stack gate
in CI reflects an explicit budget rather than an unstated default. This document
complements `scripts/check_stack_usage.py`.

## Important: what static usage is NOT

GCC's `-fstack-usage` reports the **static (frame-only)** usage of each function.
That is NOT the worst-case total stack:

- It does not sum a parent and its callees (call-chain depth).
- It does not include interrupt (ISR) frames that preempt the main thread.
- It only reflects the current build type (Debug `-O0` and Release `-Os`
  produce different frame sizes).

Therefore:

> **max total stack != largest `*.su` static entry.**

The budget below is derived by combining measured top frames with their call
chain, an ISR allowance, and an explicit engineering margin.

## Largest measured project-owned static frames (Debug / -O0)

From `scripts/check_stack_usage.py` over a Debug firmware build:

| Function | static bytes |
|----------|-------------|
| `Command_Run` | 1152 |
| `HandleGetManifest` | 1152 |
| `WriteProgramRecord` | 632 |
| `Storage_RecoverCorruptRecord` | 632 |
| `Storage_Read` / `Storage_GetHealth` / `SelectWriteSlot` | 608 |
| `Storage_GetPageInfo` | 600 |
| `Provisioning_Init` | 360 |

## Relevant call chains

The command/response path is the deepest, because the response buffer and the
manifest serializer live on the same stack:

- `Command_Run` (1152) →
  `CommandDispatcher_Dispatch` (small) →
  `HandleGetManifest` (1152) →
  `DeviceManifest_Serialize` (176)

Worst-case combined frame for that chain: ≈ 1152 + 1152 + 176 ≈ **2.5 KB**.

`Config_SelfCheck` / `DeviceIdentity_SelfCheck` (320 each) are reached from
`SelfTest_Run`, which also holds a small report — a shallow, small chain.

## Chosen budget

`_Min_Stack_Size = 0x2000` (8 KB).

Composition:

- Largest measured command/response call chain: **≈ 2.5 KB**
- ISR allowance (watchdog / system timer / pending exception frames): **≈ 1 KB**
- Typical sensing/telemetry loop depth: **< 1 KB**
- Engineering margin (future growth, `-O0` vs `-Os` differences): **remainder**

8 KB is well within the 128 KB device.

## Linker reservation and RAM

- RAM: 128 KB
- `.data` + `.bss`: ≈ 6.2 KB (from `arm-none-eabi-size`/link map)
- Reserved stack: 8 KB (`_Min_Stack_Size`)
- Reserved heap: 0.5 KB (`_Min_Heap_Size`)
- Remaining margin: ≈ 113 KB (no issue)

The linker asserts heap+stack fit in RAM; increasing the reservation to 8 KB is
trivially safe on this part.

## CI stack gate

`scripts/check_stack_usage.py` enforces a per-function **static** threshold of
2048 B (calibrated to the measured 1152 B command/response frames with margin).
It is fail-closed: if no project-owned `*.su` frames are produced, CI fails
(rather than silently passing a missing measurement).

Strictly, the gate bounds individual frame sizes; the call-chain budget above is
the governing constraint for the actual (8 KB) reservation.