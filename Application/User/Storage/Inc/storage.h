#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define STORAGE_MAGIC              0x52534D43UL  /* "RSMC" */
#define STORAGE_RECORD_FORMAT_VERSION  1U

#define RECORD_TYPE_INVALID   0U
#define RECORD_TYPE_CONFIG    1U
#define RECORD_TYPE_IDENTITY  2U

#define STORAGE_SLOT_SIZE     2048U
#define STORAGE_PAYLOAD_MAX   256U

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

bool Storage_Init(void);
bool Storage_Read(uint8_t record_type, StoragePayload *payload);
bool Storage_Write(uint8_t record_type, const uint8_t *data, size_t size);
bool Storage_Format(void);
void Storage_GetInfo(StorageInfo *info);
bool Storage_GetPageInfo(uint8_t record_type, StorageInfo *info);

#endif