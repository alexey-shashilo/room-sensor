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
StorageReadStatus Storage_Read(uint8_t record_type, StoragePayload *payload);
bool Storage_Write(uint8_t record_type, const uint8_t *data, size_t size);
bool Storage_Format(void);

/* Explicit destructive, per-record recovery. Erases ONLY the two pages that
   belong to `record_type`, then writes a fresh sequence-1 record. This repairs
   CORRUPT + CORRUPT storage. It never touches other record types and never
   falls back to a global format. Caller MUST gate on storage health (recovery
   is only valid for readable-but-corrupt; IO_ERROR must NOT be recovered). */
bool Storage_RecoverRecord(uint8_t record_type, const uint8_t *data, size_t size);

/* Erase ONLY the two pages of `record_type` (engineering/service operation).
   The record is left ERASED (both slots). Does not touch other records. */
bool Storage_FormatRecord(uint8_t record_type);

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