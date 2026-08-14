#ifndef HEX64_H
#define HEX64_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portable fixed-width 64-bit lowercase-hex encoder.

   Serializes a uint64_t as exactly 16 lowercase hexadecimal digits, without any
   libc 64-bit printf ("%llx") dependency. newlib-nano printf linked with
   --specs=nano.specs does not provide %ll support unless the `_printf_ll`
   module is explicitly pulled in, so "%016llx" mis-parses and can emit a literal
   trailing character (observed on the wire as boot_id "...000lx"). This helper
   always produces [0-9a-f]{16}, independent of libc printf long-long support.

   out must hold at least 17 bytes; the buffer is NUL-terminated. No heap, no
   locale, deterministic, endian-independent. */
void Hex64_ToLower(char *out, uint64_t value);

#ifdef __cplusplus
}
#endif

#endif /* HEX64_H */