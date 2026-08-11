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

/* Erase geometry is supplied by the Platform; no erase-unit constant here. */
static bool s_initialized = false;
static uint32_t s_page_size = 0;
static uint32_t s_total_size = 0;
static uint32_t s_page_count = 0;

#define STORAGE_RECORD_TYPES 3U

/* Record types are CONFIG=1, IDENTITY=2, REGISTRATION=3. Their A/B slot
   pairs occupy consecutive pages starting at page 0: (rt-1)*2 and (rt-1)*2+1. */
static bool SlotPage(uint8_t record_type, uint8_t slot_index, uint32_t *page_out, uint32_t *offset_out)
{
    if (page_out == NULL || offset_out == NULL) return false;
    if (record_type < RECORD_TYPE_CONFIG || record_type > RECORD_TYPE_REGISTRATION)
        return false;
    if (slot_index > 1) return false;
    uint32_t base = (uint32_t)(record_type - RECORD_TYPE_CONFIG) * 2U;
    *page_out   = base + (uint32_t)slot_index;
    *offset_out = *page_out * s_page_size;
    return true;
}

const StorageRecordLayout *Storage_GetLayout(uint8_t record_type)
{
    static StorageRecordLayout layout;
    if (!s_initialized ||
        (record_type != RECORD_TYPE_CONFIG &&
         record_type != RECORD_TYPE_IDENTITY &&
         record_type != RECORD_TYPE_REGISTRATION))
        return NULL;

    uint32_t pa, pb, oa, ob;
    if (!SlotPage(record_type, 0, &pa, &oa)) return NULL;
    if (!SlotPage(record_type, 1, &pb, &ob)) return NULL;
    layout.slot_a_page = pa;
    layout.slot_b_page = pb;
    layout.slot_a_offset = oa;
    layout.slot_b_offset = ob;
    return &layout;
}

bool Storage_Init(void)
{
    const PlatformFlashInfo *info = Platform_FlashGetInfo();
    if (info == NULL) return false;

    /* Layout validation */
    if (info->page_size == 0) return false;
    if (info->page_count < STORAGE_MIN_PAGES) return false;
    if (info->total_size < (uint32_t)STORAGE_MIN_PAGES * info->page_size) return false;
    if (info->page_size < STORAGE_HEADER_SIZE + 1U) return false;

    s_page_size  = info->page_size;
    s_total_size = info->total_size;
    s_page_count = info->page_count;

    if (s_total_size == 0) return false;
    if ((uint64_t)s_page_count * (uint64_t)s_page_size != (uint64_t)s_total_size)
        return false;

    /* A/B slots must be distinct and within bounds. */
    for (uint8_t t = 0; t < STORAGE_RECORD_TYPES; t++)
    {
        uint8_t rt = (uint8_t)(t + RECORD_TYPE_CONFIG);
        uint32_t pa, pb, oa, ob;
        if (!SlotPage(rt, 0, &pa, &oa)) return false;
        if (!SlotPage(rt, 1, &pb, &ob)) return false;
        if (pa == pb) return false;
        if (pa >= s_page_count || pb >= s_page_count) return false;
        if (oa >= s_total_size || ob >= s_total_size) return false;
        if (oa + s_page_size > s_total_size || ob + s_page_size > s_total_size) return false;
    }

    s_initialized = true;
    return true;
}

SlotState Storage_ReadSlot(
    uint8_t expected_record_type,
    uint8_t slot_index,
    StorageRecordHeader *header,
    StoragePayload *payload)
{
    if (!s_initialized) return SLOT_STATE_IO_ERROR;
    if (slot_index > 1) return SLOT_STATE_IO_ERROR;

    uint32_t page, offset;
    if (!SlotPage(expected_record_type, slot_index, &page, &offset))
        return SLOT_STATE_IO_ERROR;

    StorageRecordHeader hdr;
    if (Platform_FlashRead(offset, &hdr, STORAGE_HEADER_SIZE) != PLATFORM_FLASH_OK)
        return SLOT_STATE_IO_ERROR;

    /* Confirm erased representation from complete header field set.
       Represents the full erased (0xFF) header, not just magic. */
    const uint8_t *raw_hdr = (const uint8_t *)&hdr;
    bool all_ff = true;
    for (size_t i = 0; i < STORAGE_HEADER_SIZE; i++)
    {
        if (raw_hdr[i] != 0xFF) { all_ff = false; break; }
    }

    if (all_ff)
    {
        /* Erased confirmed by the complete header. No payload expected. */
        if (header != NULL) memset(header, 0, sizeof(*header));
        return SLOT_STATE_ERASED;
    }

    /* Structural validation */
    if (hdr.magic != STORAGE_MAGIC) return SLOT_STATE_CORRUPT;
    if (hdr.record_format_version != STORAGE_RECORD_FORMAT_VERSION) return SLOT_STATE_CORRUPT;
    if (hdr.record_type != expected_record_type) return SLOT_STATE_CORRUPT;
    if (hdr.payload_size > STORAGE_PAYLOAD_MAX) return SLOT_STATE_CORRUPT;
    if (hdr.reserved != 0) return SLOT_STATE_CORRUPT;
    if (hdr.reserved2 != 0) return SLOT_STATE_CORRUPT;

    uint32_t expected_crc = hdr.crc32;
    hdr.crc32 = 0;

    uint8_t raw[STORAGE_HEADER_SIZE + STORAGE_PAYLOAD_MAX];
    memcpy(raw, &hdr, STORAGE_HEADER_SIZE);

    if (hdr.payload_size > 0)
    {
        if (Platform_FlashRead(offset + STORAGE_HEADER_SIZE,
                               raw + STORAGE_HEADER_SIZE, hdr.payload_size) != PLATFORM_FLASH_OK)
        {
            hdr.crc32 = expected_crc;
            return SLOT_STATE_IO_ERROR;
        }
    }

    uint32_t calc_crc = Storage_Crc32(raw, STORAGE_HEADER_SIZE + hdr.payload_size);
    hdr.crc32 = expected_crc;

    if (calc_crc != expected_crc) return SLOT_STATE_CORRUPT;

    if (header != NULL) *header = hdr;

    if (payload != NULL)
    {
        payload->record_type = hdr.record_type;
        payload->sequence = hdr.sequence;
        payload->size = hdr.payload_size;
        if (hdr.payload_size > 0)
            memcpy(payload->data, raw + STORAGE_HEADER_SIZE, hdr.payload_size);
    }

    return SLOT_STATE_VALID;
}

StorageReadStatus Storage_Read(uint8_t record_type, StoragePayload *payload)
{
    if (payload == NULL) return STORAGE_READ_INVALID_ARGUMENT;
    if (!s_initialized) return STORAGE_READ_IO_ERROR;
    if (Storage_GetLayout(record_type) == NULL) return STORAGE_READ_INVALID_ARGUMENT;

    StorageRecordHeader hdr_a, hdr_b;
    SlotState a = Storage_ReadSlot(record_type, 0, &hdr_a, NULL);
    SlotState b = Storage_ReadSlot(record_type, 1, &hdr_b, NULL);

    /* ================================================================
       Result matrix (documented policy)
       ================================================================
       A ERASED,  B ERASED  -> NOT_FOUND
       A VALID,   B ERASED  -> OK A
       A ERASED,  B VALID   -> OK B
       A VALID,   B CORRUPT -> OK A
       A CORRUPT, B VALID   -> OK B
       A VALID,   B VALID   -> newest sequence
       A CORRUPT, B CORRUPT -> CORRUPT
       A CORRUPT, B ERASED  -> CORRUPT
       A ERASED,  B CORRUPT -> CORRUPT
       A IO_ERROR,B ERASED  -> IO_ERROR   (cannot trust erased)
       A IO_ERROR,B CORRUPT -> IO_ERROR
       A IO_ERROR,B IO_ERROR-> IO_ERROR
       A IO_ERROR,B VALID   -> OK (valid B, degraded read)
       ----------------------------------------------------------------
       IO_ERROR policy: an I/O failure on a slot whose peer is NOT fully
       VALID yields IO_ERROR (state cannot be safely determined). When a
       peer slot is fully VALID we prefer the validated record and treat
       storage as degraded; the caller may consult Storage_GetHealth().
       In no case is a failed read reinterpreted as an erased page.
       ================================================================ */

    if (a == SLOT_STATE_IO_ERROR && b == SLOT_STATE_IO_ERROR) return STORAGE_READ_IO_ERROR;
    if (a == SLOT_STATE_IO_ERROR && b == SLOT_STATE_VALID)
        return Storage_ReadSlot(record_type, 1, NULL, payload) == SLOT_STATE_VALID
               ? STORAGE_READ_OK : STORAGE_READ_IO_ERROR;
    if (b == SLOT_STATE_IO_ERROR && a == SLOT_STATE_VALID)
        return Storage_ReadSlot(record_type, 0, NULL, payload) == SLOT_STATE_VALID
               ? STORAGE_READ_OK : STORAGE_READ_IO_ERROR;
    if (a == SLOT_STATE_IO_ERROR || b == SLOT_STATE_IO_ERROR) return STORAGE_READ_IO_ERROR;

    /* Neither slot has an I/O error from here on. */
    if (a == SLOT_STATE_ERASED && b == SLOT_STATE_ERASED) return STORAGE_READ_NOT_FOUND;

    if (a == SLOT_STATE_VALID && b == SLOT_STATE_CORRUPT)
        return Storage_ReadSlot(record_type, 0, NULL, payload) == SLOT_STATE_VALID
               ? STORAGE_READ_OK : STORAGE_READ_CORRUPT;
    if (a == SLOT_STATE_CORRUPT && b == SLOT_STATE_VALID)
        return Storage_ReadSlot(record_type, 1, NULL, payload) == SLOT_STATE_VALID
               ? STORAGE_READ_OK : STORAGE_READ_CORRUPT;

    if (a == SLOT_STATE_CORRUPT || b == SLOT_STATE_CORRUPT) return STORAGE_READ_CORRUPT;

    if (a == SLOT_STATE_ERASED && b == SLOT_STATE_VALID)
        return Storage_ReadSlot(record_type, 1, NULL, payload) == SLOT_STATE_VALID
               ? STORAGE_READ_OK : STORAGE_READ_CORRUPT;
    if (a == SLOT_STATE_VALID && b == SLOT_STATE_ERASED)
        return Storage_ReadSlot(record_type, 0, NULL, payload) == SLOT_STATE_VALID
               ? STORAGE_READ_OK : STORAGE_READ_CORRUPT;

    /* Both VALID -> newest sequence. */
    bool a_newer = (int32_t)(hdr_a.sequence - hdr_b.sequence) >= 0;
    uint8_t pick = a_newer ? 0U : 1U;
    return Storage_ReadSlot(record_type, pick, NULL, payload) == SLOT_STATE_VALID
           ? STORAGE_READ_OK : STORAGE_READ_CORRUPT;
}

StorageHealth Storage_GetHealth(uint8_t record_type)
{
    if (!s_initialized) return STORAGE_HEALTH_IO_ERROR;
    if (Storage_GetLayout(record_type) == NULL) return STORAGE_HEALTH_IO_ERROR;

    SlotState a = Storage_ReadSlot(record_type, 0, NULL, NULL);
    SlotState b = Storage_ReadSlot(record_type, 1, NULL, NULL);

    if (a == SLOT_STATE_IO_ERROR || b == SLOT_STATE_IO_ERROR)
        return STORAGE_HEALTH_IO_ERROR;
    if (a == SLOT_STATE_CORRUPT || b == SLOT_STATE_CORRUPT)
        return STORAGE_HEALTH_CORRUPT;
    if (a == SLOT_STATE_VALID && b == SLOT_STATE_VALID)
        return STORAGE_HEALTH_HEALTHY;
    /* one valid, other erased -> healthy (mirrored slots are optional) */
    return STORAGE_HEALTH_HEALTHY;
}

static uint8_t SelectWriteSlot(uint8_t record_type, uint32_t *sequence_out)
{
    StorageRecordHeader hdr_a, hdr_b;
    SlotState a = Storage_ReadSlot(record_type, 0, &hdr_a, NULL);
    SlotState b = Storage_ReadSlot(record_type, 1, &hdr_b, NULL);

    /* Fail closed on any I/O uncertainty. */
    if (a == SLOT_STATE_IO_ERROR || b == SLOT_STATE_IO_ERROR)
        return 0xFF;

    /* Both erased -> write slot A. */
    if (a == SLOT_STATE_ERASED && b == SLOT_STATE_ERASED)
    {
        *sequence_out = 0;
        return 0;
    }

    /* Both corrupt -> unsafe to derive a newer state; fail closed. */
    if (a == SLOT_STATE_CORRUPT && b == SLOT_STATE_CORRUPT)
        return 0xFF;

    /* One valid + one erased/corrupt -> write the OTHER (non-active) slot. */
    if (a == SLOT_STATE_VALID && (b == SLOT_STATE_ERASED || b == SLOT_STATE_CORRUPT))
    {
        *sequence_out = hdr_a.sequence;
        return 1;
    }
    if (b == SLOT_STATE_VALID && (a == SLOT_STATE_ERASED || a == SLOT_STATE_CORRUPT))
    {
        *sequence_out = hdr_b.sequence;
        return 0;
    }

    /* Both valid -> write the older slot. */
    bool a_newer = (int32_t)(hdr_a.sequence - hdr_b.sequence) >= 0;
    uint32_t base = a_newer ? hdr_a.sequence : hdr_b.sequence;
    *sequence_out = base;
    return a_newer ? 1U : 0U;  /* overwrite the older (inactive) slot */
}

bool Storage_Write(uint8_t record_type, const uint8_t *data, size_t size)
{
    if (!s_initialized) return false;
    if (size > STORAGE_PAYLOAD_MAX) return false;
    if ((data == NULL) && (size > 0)) return false;

    const StorageRecordLayout *layout = Storage_GetLayout(record_type);
    if (layout == NULL) return false;

    uint32_t current_seq;
    uint8_t write_slot = SelectWriteSlot(record_type, &current_seq);
    if (write_slot == 0xFF)
        return false;  /* write fail-closed: slot state unknown or unsafe */

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
    size_t aligned = (total + 7U) & ~((size_t)7U);

    /* Erase only the inactive slot page — the active valid page is preserved. */
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
    if (!s_initialized) return false;
    for (uint32_t p = 0; p < s_page_count; p++)
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

    if (!s_initialized) return false;
    if (Storage_GetLayout(record_type) == NULL) return false;

    StorageRecordHeader hdr_a, hdr_b;
    SlotState a = Storage_ReadSlot(record_type, 0, &hdr_a, NULL);
    SlotState b = Storage_ReadSlot(record_type, 1, &hdr_b, NULL);

    info->slot_a_valid = (a == SLOT_STATE_VALID);
    info->slot_b_valid = (b == SLOT_STATE_VALID);
    info->slot_a_sequence = info->slot_a_valid ? hdr_a.sequence : 0;
    info->slot_b_sequence = info->slot_b_valid ? hdr_b.sequence : 0;
    return !(a == SLOT_STATE_IO_ERROR || b == SLOT_STATE_IO_ERROR);
}