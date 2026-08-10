/* Host implementation of Platform API — no STM32 dependencies */

#include "platform_time.h"
#include "platform_delay.h"
#include "platform_flash.h"
#include "platform_watchdog.h"
#include "platform_reset.h"
#include "platform_unique_id.h"
#include "i2c_bus.h"
#include "platform_gpio.h"
#include "platform_init.h"
#include <stdint.h>
#include <stdbool.h>

/* === Time === */
void     HostTime_Set(uint32_t ms);
void     HostTime_Advance(uint32_t delta);
uint32_t HostTime_Get(void);

/* === Flash === */
void     HostFlash_Init(void);
void    *HostFlash_GetData(void);
void     HostFlash_SetWriteFail(bool fail);

/* === I2C === */
typedef struct
{
    uint8_t regs[256];
    bool present;
} HostI2cDevice;

void     HostI2c_RegisterDevice(uint16_t addr);
void     HostI2c_RemoveDevice(uint16_t addr);
void     HostI2c_SetAlsRead(uint16_t raw);
int      HostI2c_GetCallCount(void);
int      HostI2c_GetWriteCount(void);
void     HostI2c_ResetCounters(void);
void     HostPlatform_GetI2cBus(struct I2cBus *bus);

/* === UID === */
void     HostUid_Set(const uint8_t uid[12]);
void     HostUid_SetFail(bool fail);
int      HostUid_GetCallCount(void);

/* === GPIO === */
bool     HostLed_GetState(void);

/* === Reset === */
bool     HostReset_WasRequested(void);
bool     HostReset_Clear(void);

/* === Watchdog === */
int      HostWdg_GetRefreshCount(void);
uint32_t HostWdg_GetLastRefreshMs(void);