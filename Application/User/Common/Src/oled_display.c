#include "oled_display.h"
#include "ssd1306.h"

#define OLED_I2C_TIMEOUT_MS 100U

static HAL_StatusTypeDef OLED_WriteCmd(I2C_HandleTypeDef *hi2c, uint8_t addr, uint8_t cmd)
{
    uint8_t tx[2] = {0x00U, cmd};
    return HAL_I2C_Master_Transmit(hi2c, addr, tx, 2U, OLED_I2C_TIMEOUT_MS);
}

bool OLED_Init(OLED_HandleTypeDef *dev, I2C_HandleTypeDef *hi2c)
{
    if ((dev == NULL) || (hi2c == NULL)) return false;

    dev->hi2c = hi2c;
    dev->initialized = 0U;
    dev->controller = OLED_CONTROLLER_UNKNOWN;

    SSD1306_HandleTypeDef ssd;
    ssd.hi2c = hi2c;
    ssd.addr = dev->addr;
    ssd.initialized = 0U;
    ssd.column_offset = 0U;

#ifdef OLED_USE_SH1106
    if (!SSD1306_InitSH1106(&ssd, hi2c)) return false;
    dev->controller = OLED_CONTROLLER_SH1106;
#else
    if (!SSD1306_Init(&ssd, hi2c)) return false;
    dev->controller = OLED_CONTROLLER_SSD1306;
#endif

    for (uint16_t i = 0U; i < sizeof(dev->buffer); i++)
    {
        dev->buffer[i] = ssd.buffer[i];
    }

    dev->initialized = 1U;
    return true;
}

void OLED_Clear(OLED_HandleTypeDef *dev)
{
    if (dev == NULL) return;

    for (uint16_t i = 0U; i < sizeof(dev->buffer); i++)
    {
        dev->buffer[i] = 0x00U;
    }
}

void OLED_Update(OLED_HandleTypeDef *dev)
{
    if ((dev == NULL) || (dev->initialized == 0U)) return;

    uint8_t data_buf[OLED_WIDTH + 1];
    uint8_t col_offset = (dev->controller == OLED_CONTROLLER_SH1106) ? 2U : 0U;
    uint8_t low_col  = col_offset & 0x0FU;
    uint8_t high_col = (col_offset >> 4U) & 0x0FU;

    for (uint8_t page = 0U; page < OLED_PAGES; page++)
    {
        if (OLED_WriteCmd(dev->hi2c, dev->addr, 0xB0U | page) != HAL_OK) return;
        if (OLED_WriteCmd(dev->hi2c, dev->addr, 0x00U | low_col) != HAL_OK) return;
        if (OLED_WriteCmd(dev->hi2c, dev->addr, 0x10U | high_col) != HAL_OK) return;

        data_buf[0] = 0x40U;
        for (uint8_t x = 0U; x < OLED_WIDTH; x++)
        {
            data_buf[x + 1] = dev->buffer[page * OLED_WIDTH + x];
        }
        if (HAL_I2C_Master_Transmit(dev->hi2c, dev->addr, data_buf, OLED_WIDTH + 1U, OLED_I2C_TIMEOUT_MS) != HAL_OK) return;
    }
}

void OLED_DrawPixel(OLED_HandleTypeDef *dev, uint8_t x, uint8_t y, uint8_t color)
{
    if (dev == NULL) return;
    if ((x >= OLED_WIDTH) || (y >= OLED_HEIGHT)) return;

    if (color)
    {
        dev->buffer[x + (y / 8U) * OLED_WIDTH] |= (1U << (y % 8U));
    }
    else
    {
        dev->buffer[x + (y / 8U) * OLED_WIDTH] &= ~(1U << (y % 8U));
    }
}

void OLED_DrawChar(OLED_HandleTypeDef *dev, uint8_t x, uint8_t y, char ch)
{
    if (dev == NULL) return;

    SSD1306_HandleTypeDef ssd;
    ssd.hi2c = dev->hi2c;
    ssd.addr = dev->addr;
    ssd.initialized = dev->initialized;
    ssd.column_offset = 0U;

    for (uint16_t i = 0U; i < sizeof(dev->buffer); i++)
    {
        ssd.buffer[i] = dev->buffer[i];
    }

    SSD1306_DrawChar(&ssd, x, y, ch);

    for (uint16_t i = 0U; i < sizeof(dev->buffer); i++)
    {
        dev->buffer[i] = ssd.buffer[i];
    }
}

void OLED_DrawString(OLED_HandleTypeDef *dev, uint8_t x, uint8_t y, const char *str)
{
    if (dev == NULL) return;

    SSD1306_HandleTypeDef ssd;
    ssd.hi2c = dev->hi2c;
    ssd.addr = dev->addr;
    ssd.initialized = dev->initialized;
    ssd.column_offset = 0U;

    for (uint16_t i = 0U; i < sizeof(dev->buffer); i++)
    {
        ssd.buffer[i] = dev->buffer[i];
    }

    SSD1306_DrawString(&ssd, x, y, str);

    for (uint16_t i = 0U; i < sizeof(dev->buffer); i++)
    {
        dev->buffer[i] = ssd.buffer[i];
    }
}

void OLED_TestPattern(OLED_HandleTypeDef *dev)
{
    if (dev == NULL) return;

    OLED_Clear(dev);

    for (uint16_t i = 0U; i < sizeof(dev->buffer); i++)
    {
        dev->buffer[i] = 0xFFU;
    }

    for (uint8_t y = 0U; y < OLED_HEIGHT; y++)
    {
        for (uint8_t x = 0U; x < OLED_WIDTH; x++)
        {
            uint8_t px = ((x / 8U) + (y / 8U)) & 1U;
            if (px == 0U)
            {
                uint8_t page = y / 8U;
                dev->buffer[x + page * OLED_WIDTH] &= ~(1U << (y % 8U));
            }
        }
    }

    OLED_Update(dev);
}

bool OLED_IsInitialized(const OLED_HandleTypeDef *dev)
{
    if (dev == NULL) return false;
    return (dev->initialized != 0U);
}