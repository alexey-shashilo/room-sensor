#include "storage.h"
#include "platform_flash.h"
#include <string.h>

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

static const StorageRecordLayout s_layouts[4] = {
    [0] = { .slot_a_page = 0, .slot_b_page = 1, .slot_a_offset = 0,               .slot_b_offset = 1 * STORAGE_ERASE_UNIT },
    [RECORD_TYPE_CONFIG]      = { .slot_a_page = 0, .slot_b_page = 1, .slot_a_offset = 0,               .slot_b_offset = 1 * STORAGE_ERASE_UNIT },
    [RECORD_TYPE_IDENTITY]    = { .slot_a_page = 2, .slot_b_page = 3, .slot_a_offset = 2 * STORAGE_ERASE_UNIT, .slot_b_offset = 3 * STORAGE_ERASE_UNIT },
    [RECORD_TYPE_REGISTRATION] = { .slot_a_page = 4, .slot_b_page = 5, .slot_a_offset = 4 * STORAGE_ERASE_UNIT, .slot_b_offset = 5 * STORAGE_ERASE_UNIT },
};

const StorageRecordLayout *Storage_GetLayout(uint8_t record_type)
{
    if ((record_type != RECORD_TYPE_CONFIG) &&
        (record_type != RECORD_TYPE_IDENTITY) &&
        (record_type != RECORD_TYPE_REGISTRATION))
        return NULL;
    return &s_layouts[record_type];
}

static bool SlotReadRaw(uint32_t abs_offset, StorageRecordHeader *hdr, StoragePayload *payload)
{
    if (hdr == NULL) return false;

    memset(hdr, 0, sizeof(*hdr));

    if (Platform_FlashRead(abs_offset, hdr, STORAGE_HEADER_SIZE) != PLATFORM_FLASH_OK)
        return false;

    if (hdr->magic != STORAGE_MAGIC)
        return false;

    if (hdr->record_format_version != STORAGE_RECORD_FORMAT_VERSION)
        return false;

    uint16_t psize = hdr->payload_size;
    if (psize > STORAGE_PAYLOAD_MAX)
        return false;

    uint32_t expected_crc = hdr->crc32;
    hdr->crc32 = 0;

    uint8_t raw[STORAGE_HEADER_SIZE + STORAGE_PAYLOAD_MAX];
    memcpy(raw, hdr, STORAGE_HEADER_SIZE);

    if (psize > 0)
    {
        if (Platform_FlashRead(abs_offset + STORAGE_HEADER_SIZE, raw + STORAGE_HEADER_SIZE, psize) != PLATFORM_FLASH_OK)
            return false;
    }

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

static uint8_t SelectSlot(const StorageRecordLayout *layout, uint32_t *sequence_out)
{
    StorageRecordHeader hdr_a, hdr_b;
    bool valid_a = SlotReadRaw(layout->slot_a_offset, &hdr_a, NULL);
    bool valid_b = SlotReadRaw(layout->slot_b_offset, &hdr_b, NULL);

    if (!valid_a && !valid_b)
    {
        *sequence_out = 0;
        return 0xFF;
    }

    if (valid_a && !valid_b) { *sequence_out = hdr_a.sequence; return 0; }
    if (!valid_a && valid_b) { *sequence_out = hdr_b.sequence; return 1; }

    if ((int32_t)(hdr_a.sequence - hdr_b.sequence) >= 0)
    { *sequence_out = hdr_a.sequence; return 0; }

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

    const StorageRecordLayout *layout = Storage_GetLayout(record_type);
    if (layout == NULL) return false;

    StorageRecordHeader hdr_a, hdr_b;
    bool valid_a = false, valid_b = false;

    if (Platform_FlashRead(layout->slot_a_offset, &hdr_a, STORAGE_HEADER_SIZE) == PLATFORM_FLASH_OK)
        valid_a = (hdr_a.magic == STORAGE_MAGIC && hdr_a.record_type == record_type);

    if (Platform_FlashRead(layout->slot_b_offset, &hdr_b, STORAGE_HEADER_SIZE) == PLATFORM_FLASH_OK)
        valid_b = (hdr_b.magic == STORAGE_MAGIC && hdr_b.record_type == record_type);

    uint8_t best_slot = 0xFF;
    uint32_t best_seq = 0;

    if (valid_a)
    {
        StoragePayload p;
        if (SlotReadRaw(layout->slot_a_offset, &hdr_a, &p))
        {
            best_slot = 0;
            best_seq = hdr_a.sequence;
        }
    }

    if (valid_b)
    {
        StoragePayload p;
        if (SlotReadRaw(layout->slot_b_offset, &hdr_b, &p))
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
        return SlotReadRaw(layout->slot_a_offset, &hdr_a, payload);
    else
        return SlotReadRaw(layout->slot_b_offset, &hdr_b, payload);
}

bool Storage_Write(uint8_t record_type, const uint8_t *data, size_t size)
{
    if (size > STORAGE_PAYLOAD_MAX) return false;
    if ((data == NULL) && (size > 0)) return false;

    const StorageRecordLayout *layout = Storage_GetLayout(record_type);
    if (layout == NULL) return false;

    uint32_t current_seq;
    uint8_t active_slot = SelectSlot(layout, &current_seq);

    uint8_t write_slot = (active_slot == 0xFF) ? 0 : ((active_slot == 0) ? 1 : 0);

    uint32_t write_page = (write_slot == 0) ? layout->slot_a_page : layout->slot_b_page;
    uint32_t write_offset = (write_slot == 0) ? layout->slot_a_offset : layout->slot_b_offset;

    uint32_t next_seq = current_seq + 1;
    if (next_seq == 0) next_seq = 1;

    StorageRecordHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = STORAGE_MAGIC;
    hdr.record_format_version = STORAGE_RECORD_FORMAT_VERSION;
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

    /* Erase only the inactive slot page — old active slot remains intact */
    if (Platform_FlashErase(write_page) != PLATFORM_FLASH_OK)
        return false;

    if (Platform_FlashWrite(write_offset, raw, aligned) != PLATFORM_FLASH_OK)
        return false;

    uint8_t verify[STORAGE_HEADER_SIZE + STORAGE_PAYLOAD_MAX];
    if (Platform_FlashRead(write_offset, verify, total) != PLATFORM_FLASH_OK)
        return false;

    if (memcmp(raw, verify, total) != 0)
        return false;

    return true;
}

bool Storage_Format(void)
{
    for (uint32_t p = 0; p < 6; p++)
    {
        if (Platform_FlashErase(p) != PLATFORM_FLASH_OK)
            return false;
    }
    return true;
}

void Storage_GetInfo(StorageInfo *info)
{
    if (info == NULL) return;
    Storage_GetPageInfo(RECORD_TYPE_CONFIG, info);
}

bool Storage_GetPageInfo(uint8_t record_type, StorageInfo *info)
{
    if (info == NULL) return false;
    memset(info, 0, sizeof(*info));

    const StorageRecordLayout *layout = Storage_GetLayout(record_type);
    if (layout == NULL) return false;

    StorageRecordHeader hdr_a, hdr_b;
    info->slot_a_valid = SlotReadRaw(layout->slot_a_offset, &hdr_a, NULL);
    info->slot_b_valid = SlotReadRaw(layout->slot_b_offset, &hdr_b, NULL);
    info->slot_a_sequence = info->slot_a_valid ? hdr_a.sequence : 0;
    info->slot_b_sequence = info->slot_b_valid ? hdr_b.sequence : 0;
    return true;
}