#include "hex64.h"

static const char kHexDigits[16] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };

void Hex64_ToLower(char *out, uint64_t value)
{
    if (out == NULL)
        return;

    for (int i = 15; i >= 0; i--)
    {
        out[i] = kHexDigits[value & 0x0FU];
        value >>= 4U;
    }
    out[16] = '\0';
}