#include "mqtt_utf8.h"
#include "mqtt_codec.h"

/* MQTT 3.1.1 UTF-8 / topic validation (Phase 17.1). RFC 3629 well-formed UTF-8
   plus MQTT MUST-NOT code points. Bounded, allocation-free, O(n). */

/* Verify one UTF-8 sequence starting at s[i], returning the number of bytes
   consumed (>0) or 0 if malformed. Rejects overlong, surrogates, U+FFFE/U+FFFF,
   > U+10FFFF, and too-short continuations. The caller has already ensured that
   at least one byte exists; `remaining` is how many bytes are left in the buffer. */
static size_t utf8_seq(const uint8_t *s, size_t i, size_t remaining)
{
    size_t n = remaining - i;
    uint8_t b0 = s[i];

    if (b0 <= 0x7FU)
    {
        if (b0 == 0x00U)
            return 0U;                 /* U+0000 forbidden */
        return 1U;
    }
    if (b0 >= 0xC2U && b0 <= 0xDFU)    /* 2-byte: U+0080..U+07FF */
    {
        if (n < 2U) return 0U;         /* truncated */
        uint8_t c1 = s[i + 1U];
        if (c1 < 0x80U || c1 > 0xBFU) return 0U;
        return 2U;
    }
    if (b0 >= 0xE0U && b0 <= 0xEFU)    /* 3-byte: U+0800..U+FFFF */
    {
        if (n < 3U) return 0U;         /* truncated */
        uint8_t c1 = s[i + 1U];
        uint8_t c2 = s[i + 2U];
        if (c1 < 0x80U || c1 > 0xBFU || c2 < 0x80U || c2 > 0xBFU) return 0U;
        /* overlong: E0 must be followed by [A0..BF] (≥ U+0800). */
        if (b0 == 0xE0U && c1 < 0xA0U) return 0U;
        /* surrogate range U+D800..U+DFFF: ED followed by [A0..BF]. */
        if (b0 == 0xEDU && c1 >= 0xA0U) return 0U;
        /* noncharacters U+FFFE/U+FFFF (EF BF BE / EF BF BF) forbidden. */
        if (b0 == 0xEFU && c1 == 0xBFU && (c2 == 0xBEU || c2 == 0xBFU)) return 0U;
        return 3U;
    }
    if (b0 >= 0xF0U && b0 <= 0xF4U)    /* 4-byte: U+10000..U+10FFFF */
    {
        if (n < 4U) return 0U;         /* truncated */
        uint8_t c1 = s[i + 1U];
        uint8_t c2 = s[i + 2U];
        uint8_t c3 = s[i + 3U];
        if (c1 < 0x80U || c1 > 0xBFU || c2 < 0x80U || c2 > 0xBFU ||
            c3 < 0x80U || c3 > 0xBFU)
            return 0U;
        /* F0 must be followed by [90..BF] (≥ U+10000), F4 by [80..8F] (≤ U+10FFFF). */
        if (b0 == 0xF0U && c1 < 0x90U) return 0U;
        if (b0 == 0xF4U && c1 > 0x8FU) return 0U;
        return 4U;
    }
    /* 0x80..0xBF (lone continuation) or 0xC0/0xC1 (overlong) or 0xF5..0xFF. */
    return 0U;
}

bool MqttUtf8_Valid(const uint8_t *s, size_t len)
{
    if (s == NULL || len == 0U)
        return false;
    size_t i = 0U;
    while (i < len)
    {
        size_t taken = utf8_seq(s, i, len);
        if (taken == 0U)
            return false;
        i += taken;
    }
    return true;
}

bool MqttUtf8_IsValidTopicName(const uint8_t *s, size_t len)
{
    if (s == NULL || len == 0U || len > MQTT_MAX_TOPIC_LENGTH)
        return false;
    if (!MqttUtf8_Valid(s, len))
        return false;
    /* Topic Name MUST NOT contain wildcard characters. */
    for (size_t i = 0U; i < len; i++)
    {
        if (s[i] == '#' || s[i] == '+')
            return false;
    }
    return true;
}

bool MqttUtf8_IsValidTopicFilter(const uint8_t *s, size_t len)
{
    if (len == 0U || len > MQTT_MAX_TOPIC_LENGTH)
        return false;
    if (s == NULL)
        return false;
    if (!MqttUtf8_Valid(s, len))
        return false;

    for (size_t i = 0U; i < len; i++)
    {
        if (s[i] == '#')
        {
            /* '#' must be the final character, and (if not alone) preceded by '/'. */
            if (i != len - 1U)
                return false;
            if (i > 0U && s[i - 1U] != '/')
                return false;
        }
        else if (s[i] == '+')
        {
            /* '+' must occupy an entire level: bounded by '/' or string ends. */
            bool left  = (i == 0U) || (s[i - 1U] == '/');
            bool right = (i == len - 1U) || (s[i + 1U] == '/');
            if (!(left && right))
                return false;
        }
    }
    return true;
}