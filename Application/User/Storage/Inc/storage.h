#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define STORAGE_MAGIC                  0x52534D43UL
#define STORAGE_RECORD_FORMAT_VERSION  1U
#define STORAGE_PAYLOAD_MAX            256U

/* Record types */
#define RECORD_TYPE_INVALID     0U
#define RECORD_TYPE_CONFIG      1U
#define RECORD_TYPE_IDENTITY    2U
#define RECORD_TYPE_REGISTRATION 3U

/* Minimum logical pages required: 3 record types x 2 A/B slots = 6. */
#define STORAGE_MIN_PAGES        6U

#define STORAGE_RECORD_TYPES     3U /* CONFIG, IDENTITY, REGISTRATION */

/* The number of pages Storage owns and manages (Config A/B, Identity A/B,
   Registration A/B). Storage_Format erases ONLY these owned pages and never
   pages beyond them, even if PlatformFlashInfo.page_count is larger (extra
   pages belong to a broader/reserved partition, not Storage). */
#define STORAGE_OWNED_PAGES      ((STORAGE_RECORD_TYPES) * 2U)

/* Maximum Flash program (write) granularity supported by the storage layer.
   PlatformFlashInfo.program_unit must be a power of two in (0, MAX]. */
#define STORAGE_PROGRAM_UNIT_MAX  8U

typedef struct
{
    uint32_t magic;
    uint16_t record_format_version;
    uint16_t payload_size;
    uint8_t  record_type;
    uint8_t  flags;
    uint32_t sequence;
    uint16_t reserved;
    uint16_t reserved2;
    uint32_t crc32;
} __attribute__((packed)) StorageRecordHeader;

#define STORAGE_HEADER_SIZE  sizeof(StorageRecordHeader)

/* Record on Flash = header + payload (no padding written on wire). */
#define STORAGE_RAW_MAX  (STORAGE_HEADER_SIZE + STORAGE_PAYLOAD_MAX)

/* Capacity of the program buffer. Writes are rounded up to program_unit, and
   program_unit <= STORAGE_PROGRAM_UNIT_MAX, so the aligned length is always
   <= this capacity. All padding bytes are initialized to erased (0xFF). */
#define STORAGE_PROGRAM_BUFFER_MAX \
    ((STORAGE_RAW_MAX + STORAGE_PROGRAM_UNIT_MAX - 1U) & \
     ~((size_t)(STORAGE_PROGRAM_UNIT_MAX - 1U)))

_Static_assert(STORAGE_HEADER_SIZE == 22U, "StorageRecordHeader size mismatch");
_Static_assert(STORAGE_RAW_MAX == 278U, "STORAGE_RAW_MAX mismatch");
_Static_assert(STORAGE_PROGRAM_BUFFER_MAX == 280U, "STORAGE_PROGRAM_BUFFER_MAX mismatch");
_Static_assert((STORAGE_PROGRAM_UNIT_MAX & (STORAGE_PROGRAM_UNIT_MAX - 1U)) == 0U,
               "STORAGE_PROGRAM_UNIT_MAX must be a power of two");

typedef struct
{
    uint8_t data[STORAGE_PAYLOAD_MAX];
    size_t  size;
    uint8_t record_type;
    uint32_t sequence;
} StoragePayload;

typedef struct
{
    bool slot_a_valid;
    bool slot_b_valid;
    uint32_t slot_a_sequence;
    uint32_t slot_b_sequence;
} StorageInfo;

typedef struct
{
    uint32_t slot_a_page;
    uint32_t slot_b_page;
    uint32_t slot_a_offset;
    uint32_t slot_b_offset;
} StorageRecordLayout;

/* Erased unit geometry is supplied by the Platform via Platform_FlashGetInfo().
   The portable Storage core does NOT hardcode any erase-unit / page size. */

typedef enum
{
    STORAGE_READ_OK = 0,
    STORAGE_READ_NOT_FOUND,
    STORAGE_READ_CORRUPT,
    STORAGE_READ_IO_ERROR,
    STORAGE_READ_INVALID_ARGUMENT
} StorageReadStatus;

/* Explicit per-slot state. A slot is ERASED only when the Flash read
   succeeded and the complete slot confirmed the erased representation. */
typedef enum
{
    SLOT_STATE_ERASED = 0,
    SLOT_STATE_VALID,
    SLOT_STATE_CORRUPT,
    SLOT_STATE_IO_ERROR
} SlotState;

/* A complete, single-read snapshot of one slot. Storage_Read and friends read
   each slot exactly ONCE into a SlotSnapshot; selection uses snapshots only
   (no re-read, no IO failure re-classification). */
typedef struct
{
    SlotState           state;
    StorageRecordHeader header;
    StoragePayload      payload;
} SlotSnapshot;

/* Aggregate storage health of a record region. */
typedef enum
{
    STORAGE_HEALTH_HEALTHY = 0,
    STORAGE_HEALTH_DEGRADED,      /* usable valid data but degraded mirror */
    STORAGE_HEALTH_DEGRADED_IO,   /* usable valid data + IO_ERROR peer */
    STORAGE_HEALTH_CORRUPT,       /* no usable valid record */
    STORAGE_HEALTH_IO_ERROR       /* cannot determine usable state */
} StorageHealth;

const StorageRecordLayout *Storage_GetLayout(uint8_t record_type);

bool Storage_Init(void);
/* Non-mutating query of the boot-time Storage subsystem initialization state.
   A diagnostic/self-test invocation MUST call this instead of Storage_Init(),
   which owns lifecycle/global initialization and must not be re-run. */
bool Storage_IsInitialized(void);
StorageReadStatus Storage_Read(uint8_t record_type, StoragePayload *payload);
bool Storage_Format(void);

/* Result of an explicit per-record destructive recovery attempt. */
typedef enum
{
    STORAGE_RECOVERY_OK = 0,
    STORAGE_RECOVERY_NOT_NEEDED,   /* region already healthy (valid/blank, no corruption) */
    STORAGE_RECOVERY_IO_ERROR,     /* cannot determine state; refused WITHOUT erasing */
    STORAGE_RECOVERY_INVALID_ARGUMENT,
    STORAGE_RECOVERY_FAILED
} StorageRecoveryStatus;

/* Explicit destructive, per-record recovery. Erases ONLY the two pages that
   belong to `record_type` and writes a fresh sequence-1 record.

   The caller MUST first inspect slot states (snapshots taken internally):
   - any IO_ERROR slot  -> STORAGE_RECOVERY_IO_ERROR, ZERO erase calls;
   - region fully healthy (both valid, or both erased) -> STORAGE_RECOVERY_NOT_NEEDED;
   - otherwise (at least one CORRUPT slot, no IO uncertainty) -> erase + reinit.

   Destructive power-loss policy: this cannot preserve an already-corrupt
   record. For registration / factory reset, if power is lost after the erase
   the safe result is unprovisioned / NOT_FOUND. It does NOT fall back to a
   global format and never touches other record types. */
StorageRecoveryStatus Storage_RecoverCorruptRecord(uint8_t record_type,
                                                   const uint8_t *data, size_t size);

/* Erase ONLY the two pages of `record_type` (engineering/service operation).
   The record is left ERASED (both slots). Does not touch other records. */
bool Storage_FormatRecord(uint8_t record_type);

/* Result of establishing A/B redundancy for a record. */
typedef enum
{
    STORAGE_REPAIR_DONE = 0,        /* mirror established -> both slots VALID */
    STORAGE_REPAIR_NOT_NEEDED,      /* already HEALTHY (both slots valid) */
    STORAGE_REPAIR_NOT_FOUND,       /* both slots erased (nothing to mirror) */
    STORAGE_REPAIR_REFUSED,         /* IO uncertainty or no valid source; no erase */
    STORAGE_REPAIR_INVALID_ARGUMENT
} StorageRepairStatus;

/* Establish the missing A/B mirror of an otherwise-valid record, preserving the
   valid source throughout. Reads the VALID slot's payload, erases ONLY the
   degraded peer (erased or corrupt), writes the mirrored copy (sequence +1),
   and verifies. The original valid slot is NEVER erased, so a power loss at any
   point leaves the valid source intact.

   VALID + ERASED          -> erase peer, mirror -> DONE (both valid)
   VALID + CORRUPT         -> erase peer, mirror   -> DONE (both valid)
   VALID + IO_ERROR        -> REFUSED, ZERO erase (cannot trust the peer)
   VALID + VALID           -> NOT_NEEDED
   ERASED + ERASED         -> NOT_FOUND (no data exists to mirror)
   no valid slot           -> REFUSED (explicit recovery path remains separate) */
StorageRepairStatus Storage_EnsureRedundancy(uint8_t record_type);

/* Classified result of a durable write attempt. Distinguishes an unsafe
   storage state (e.g. a no-valid CORRUPT+ERASED pair that requires explicit
   recovery) from a genuine Flash IO/physical error or a readback verification
   failure, so higher layers never collapse every failure into "IO_ERROR". */
typedef enum
{
    STORAGE_WRITE_OK = 0,
    STORAGE_WRITE_INVALID_ARGUMENT,
    STORAGE_WRITE_UNSAFE_STATE,   /* e.g. no VALID copy; explicit recovery required */
    STORAGE_WRITE_IO_ERROR,       /* Flash erase/program HAL failure */
    STORAGE_WRITE_VERIFY_FAILED   /* post-write readback mismatch */
} StorageWriteStatus;

/* Extended write API with a classified result. `data`/`size` must form exactly
   `size` payload bytes; NULL data is only valid with size==0. */
StorageWriteStatus Storage_WriteEx(uint8_t record_type, const uint8_t *data, size_t size);

/* Compatibility wrapper around Storage_WriteEx: true iff result is OK. */
bool Storage_Write(uint8_t record_type, const uint8_t *data, size_t size);

void Storage_GetInfo(StorageInfo *info);
bool Storage_GetPageInfo(uint8_t record_type, StorageInfo *info);

/* Per-slot classification. Owns ALL header/payload validation.
   Returns SLOT_STATE_ERASED / VALID / CORRUPT / IO_ERROR.
   header and/or payload may be NULL to skip filling. */
SlotState Storage_ReadSlot(
    uint8_t expected_record_type,
    uint8_t slot_index,
    StorageRecordHeader *header,
    StoragePayload *payload);

/* Aggregate storage health for a record region. */
StorageHealth Storage_GetHealth(uint8_t record_type);

#endif