#include "storage.h"
#include "platform_flash.h"
#include <string.h>

#define PAGE_FOR_TYPE(t)  (((t) == RECORD_TYPE_IDENTITY) ? 1U : 0U)

static uint32_t Storage_Crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint32_t)data[i];
        for (int b = 0; b < 8; b++)
        {
            if (crc & 1U)
                crc = (crc >> 1U) ^ 0xEDB88320U;
            else
                crc >>= 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static bool SlotReadRaw(uint32_t abs_offset, StorageRecordHeader *hdr, StoragePayload *payload)
{
    memset(hdr, 0, sizeof(*hdr));

    if (Platform_FlashRead(abs_offset, hdr, STORAGE_HEADER_SIZE) != PLATFORM_FLASH_OK)
        return false;

    if (hdr->magic != STORAGE_MAGIC)
        return false;

    if (hdr->schema_version > STORAGE_SCHEMA_VERSION)
        return false;

    uint16_t psize = hdr->payload_size;
    if (psize > STORAGE_PAYLOAD_MAX)
        return false;

    uint8_t raw[STORAGE_HEADER_SIZE + STORAGE_PAYLOAD_MAX];
    memcpy(raw, hdr, STORAGE_HEADER_SIZE);

    if (psize > 0)
    {
        if (Platform_FlashRead(abs_offset + STORAGE_HEADER_SIZE, raw + STORAGE_HEADER_SIZE, psize) != PLATFORM_FLASH_OK)
            return false;
    }

    uint32_t expected_crc = hdr->crc32;
    hdr->crc32 = 0;

    uint32_t calc_crc = Storage_Crc32(raw, STORAGE_HEADER_SIZE + psize);
    hdr->crc32 = expected_crc;

    if (calc_crc != expected_crc)
        return false;

    if (payload)
    {
        payload->record_type = hdr->record_type;
        payload->sequence = hdr->sequence;
        payload->size = psize;
        if (psize > 0)
            memcpy(payload->data, raw + STORAGE_HEADER_SIZE, psize);
    }

    return true;
}

static uint8_t SelectSlot(uint32_t *sequence_out)
{
    StorageRecordHeader hdr_a, hdr_b;
    bool valid_a = SlotReadRaw(0, &hdr_a, NULL);
    bool valid_b = SlotReadRaw(STORAGE_SLOT_SIZE, &hdr_b, NULL);

    if (!valid_a && !valid_b)
    {
        *sequence_out = 0;
        return 0xFF;
    }

    if (valid_a && !valid_b)
    {
        *sequence_out = hdr_a.sequence;
        return 0;
    }

    if (!valid_a && valid_b)
    {
        *sequence_out = hdr_b.sequence;
        return 1;
    }

    if ((int32_t)(hdr_a.sequence - hdr_b.sequence) >= 0)
    {
        *sequence_out = hdr_a.sequence;
        return 0;
    }

    *sequence_out = hdr_b.sequence;
    return 1;
}

bool Storage_Init(void)
{
    return true;
}

bool Storage_Read(uint8_t record_type, StoragePayload *payload)
{
    if (payload == NULL) return false;

    uint32_t base_offset = (record_type == RECORD_TYPE_IDENTITY) ? STORAGE_SLOT_SIZE * 2 : 0U;

    StorageRecordHeader hdr_a, hdr_b;
    bool valid_a = false, valid_b = false;

    if (Platform_FlashRead(base_offset, &hdr_a, STORAGE_HEADER_SIZE) == PLATFORM_FLASH_OK)
        valid_a = (hdr_a.magic == STORAGE_MAGIC && hdr_a.record_type == record_type);

    if (Platform_FlashRead(base_offset + STORAGE_SLOT_SIZE, &hdr_b, STORAGE_HEADER_SIZE) == PLATFORM_FLASH_OK)
        valid_b = (hdr_b.magic == STORAGE_MAGIC && hdr_b.record_type == record_type);

    uint8_t best_slot = 0xFF;
    uint32_t best_seq = 0;

    if (valid_a)
    {
        StoragePayload p;
        if (SlotReadRaw(base_offset, &hdr_a, &p))
        {
            best_slot = 0;
            best_seq = hdr_a.sequence;
        }
    }

    if (valid_b)
    {
        StoragePayload p;
        if (SlotReadRaw(base_offset + STORAGE_SLOT_SIZE, &hdr_b, &p))
        {
            if ((best_slot == 0xFF) || ((int32_t)(hdr_b.sequence - best_seq) > 0))
            {
                best_slot = 1;
                best_seq = hdr_b.sequence;
            }
        }
    }

    if (best_slot == 0xFF)
        return false;

    if (best_slot == 0)
        return SlotReadRaw(base_offset, &hdr_a, payload);
    else
        return SlotReadRaw(base_offset + STORAGE_SLOT_SIZE, &hdr_b, payload);
}

bool Storage_Write(uint8_t record_type, const uint8_t *data, size_t size)
{
    if (size > STORAGE_PAYLOAD_MAX) return false;
    if ((record_type != RECORD_TYPE_CONFIG) && (record_type != RECORD_TYPE_IDENTITY))
        return false;

    uint32_t current_seq;
    uint8_t active_slot = SelectSlot(&current_seq);

    uint8_t write_slot;
    if (active_slot == 0xFF)
        write_slot = 0;
    else
        write_slot = (active_slot == 0) ? 1 : 0;

    uint32_t next_seq = current_seq + 1;
    if (next_seq == 0) next_seq = 1;

    StorageRecordHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = STORAGE_MAGIC;
    hdr.schema_version = STORAGE_SCHEMA_VERSION;
    hdr.payload_size = (uint16_t)size;
    hdr.record_type = record_type;
    hdr.sequence = next_seq;

    uint8_t raw[STORAGE_HEADER_SIZE + STORAGE_PAYLOAD_MAX];
    memset(raw, 0xFF, sizeof(raw));
    memcpy(raw, &hdr, STORAGE_HEADER_SIZE);
    if (size > 0)
        memcpy(raw + STORAGE_HEADER_SIZE, data, size);

    hdr.crc32 = 0;
    memcpy(raw, &hdr, STORAGE_HEADER_SIZE);

    uint32_t crc = Storage_Crc32(raw, STORAGE_HEADER_SIZE + size);
    hdr.crc32 = crc;
    memcpy(raw, &hdr, STORAGE_HEADER_SIZE);

    size_t total = STORAGE_HEADER_SIZE + size;
    size_t aligned = (total + 7U) & ~7U;
    if (aligned > STORAGE_SLOT_SIZE) aligned = STORAGE_SLOT_SIZE;

    uint32_t base_offset = (record_type == RECORD_TYPE_IDENTITY) ? STORAGE_SLOT_SIZE * 2 : 0;
    uint32_t write_offset = base_offset + (uint32_t)write_slot * STORAGE_SLOT_SIZE;

    if (Platform_FlashErase(PAGE_FOR_TYPE(record_type)) != PLATFORM_FLASH_OK)
        return false;

    if (Platform_FlashWrite(write_offset, raw, aligned) != PLATFORM_FLASH_OK)
    {
        Platform_FlashErase(PAGE_FOR_TYPE(record_type));
        return false;
    }

    uint8_t verify[STORAGE_HEADER_SIZE + STORAGE_PAYLOAD_MAX];
    if (Platform_FlashRead(write_offset, verify, total) != PLATFORM_FLASH_OK)
    {
        Platform_FlashErase(PAGE_FOR_TYPE(record_type));
        return false;
    }
    if (memcmp(raw, verify, total) != 0)
    {
        Platform_FlashErase(PAGE_FOR_TYPE(record_type));
        return false;
    }

    return true;
}

bool Storage_Format(void)
{
    if (Platform_FlashErase(0) != PLATFORM_FLASH_OK) return false;
    if (Platform_FlashErase(1) != PLATFORM_FLASH_OK) return false;
    return true;
}

void Storage_GetInfo(StorageInfo *info)
{
    if (info == NULL) return;
    memset(info, 0, sizeof(*info));

    StorageRecordHeader hdr_a, hdr_b;
    info->slot_a_valid = SlotReadRaw(0, &hdr_a, NULL);
    info->slot_b_valid = SlotReadRaw(STORAGE_SLOT_SIZE, &hdr_b, NULL);
    info->slot_a_sequence = info->slot_a_valid ? hdr_a.sequence : 0;
    info->slot_b_sequence = info->slot_b_valid ? hdr_b.sequence : 0;
}