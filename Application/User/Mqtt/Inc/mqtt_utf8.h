#ifndef MQTT_UTF8_H
#define MQTT_UTF8_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================
   MQTT 3.1.1 string / topic validation (Phase 17.1).

   RFC 3629 well-formed UTF-8 plus the MQTT-specific code-point MUST-NOT rules:
     - U+0000 MUST NOT appear.
     - encodings of code points U+D800..U+DFFF (UTF-16 surrogates) MUST NOT appear.
     - well-formed 1..4-byte sequences only (no overlong, no > U+10FFFF).
     - the noncharacters U+FFFE and U+FFFF are rejected (forbidden by MQTT).

   Nothing here allocates, uses locales, or normalizes. All functions are
   bounded, deterministic, O(n).
   ================================================================ */

#ifdef __cplusplus
extern "C" {
#endif

/* Validate an MQTT UTF-8 string by encoded bytes. `len` is BYTE length.
 * Returns true iff the byte sequence is valid MQTT UTF-8 non-empty content
 * (allows any non-zero code points; caller enforces other rules for topics). */
bool MqttUtf8_Valid(const uint8_t *s, size_t len);

/* Valid MQTT Topic Name for PUBLISH:
 *   - non-empty
 *   - valid MQTT UTF-8
 *   - within MQTT_MAX_TOPIC_LENGTH
 *   - MUST NOT contain wildcard characters '#' or '+'
 */
bool MqttUtf8_IsValidTopicName(const uint8_t *s, size_t len);

/* Valid MQTT Topic Filter for SUBSCRIBE:
 *   - non-empty
 *   - valid MQTT UTF-8
 *   - within MQTT_MAX_TOPIC_LENGTH
 *   - enforces the MQTT wildcard grammar:
 *       '#' may appear only as the entire final level ("#", "a/#").
 *       '+' must occupy an entire topic level ("+/a", "a/+/b", "/+").
 */
bool MqttUtf8_IsValidTopicFilter(const uint8_t *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif