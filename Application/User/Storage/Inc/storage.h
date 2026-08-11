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

/* Aggregate storage health of a record region. */
typedef enum
{
    STORAGE_HEALTH_HEALTHY = 0,
    STORAGE_HEALTH_DEGRADED,
    STORAGE_HEALTH_CORRUPT,
    STORAGE_HEALTH_IO_ERROR
} StorageHealth;

const StorageRecordLayout *Storage_GetLayout(uint8_t record_type);

bool Storage_Init(void);
StorageReadStatus Storage_Read(uint8_t record_type, StoragePayload *payload);
bool Storage_Write(uint8_t record_type, const uint8_t *data, size_t size);
bool Storage_Format(void);
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