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
static uint32_t s_program_unit = 0;

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

static bool SlotPagePair(uint8_t record_type, uint32_t *pa, uint32_t *pb,
                         uint32_t *oa, uint32_t *ob)
{
    if (pa == NULL || pb == NULL || oa == NULL || ob == NULL) return false;
    if (!SlotPage(record_type, 0, pa, oa)) return false;
    if (!SlotPage(record_type, 1, pb, ob)) return false;
    return true;
}

const StorageRecordLayout *Storage_GetLayout(uint8_t record_type)
{
    static StorageRecordLayout layout;
    if (!s_initialized) return NULL;

    uint32_t pa, pb, oa, ob;
    if (!SlotPagePair(record_type, &pa, &pb, &oa, &ob)) return NULL;
    layout.slot_a_page = pa;
    layout.slot_b_page = pb;
    layout.slot_a_offset = oa;
    layout.slot_b_offset = ob;
    return &layout;
}

bool Storage_Init(void)
{
    /* Deterministic: never leave stale "initialized" on a re-init that fails. */
    s_initialized = false;
    s_page_size = 0;
    s_total_size = 0;
    s_page_count = 0;
    s_program_unit = 0;

    const PlatformFlashInfo *info = Platform_FlashGetInfo();
    if (info == NULL) return false;

    /* Fail closed if the platform Flash mapping is unsupported (e.g. MCU in an
       unexpected bank mode). Storage must not initialize -> no erase/write. */
    if (Platform_FlashValidateConfiguration() != PLATFORM_FLASH_OK)
        return false;

    /* Program unit must be a supported power of two. */
    if (info->program_unit == 0) return false;
    if (info->program_unit > STORAGE_PROGRAM_UNIT_MAX) return false;
    if ((info->program_unit & (info->program_unit - 1U)) != 0U) return false;

    /* Geometry validation (overflow-safe). */
    if (info->page_size == 0) return false;
    if (info->page_count < STORAGE_MIN_PAGES) return false;
    if (info->page_size < STORAGE_HEADER_SIZE + 1U) return false;
    if (info->total_size == 0) return false;

    uint64_t min_total = (uint64_t)info->page_count * (uint64_t)info->page_size;
    if (min_total != (uint64_t)info->total_size) return false;

    s_page_size   = info->page_size;
    s_total_size  = info->total_size;
    s_page_count  = info->page_count;
    s_program_unit = info->program_unit;

    /* A/B slots must be distinct, in bounds, and program-buffer safe. */
    for (uint8_t t = 0; t < STORAGE_RECORD_TYPES; t++)
    {
        uint8_t rt = (uint8_t)(t + RECORD_TYPE_CONFIG);
        uint32_t pa, pb, oa, ob;
        if (!SlotPagePair(rt, &pa, &pb, &oa, &ob)) return false;
        if (pa == pb) return false;
        if (pa >= s_page_count || pb >= s_page_count) return false;
        if (oa > s_total_size || ob > s_total_size) return false;
        if (s_page_size > s_total_size - oa) return false;
        if (s_page_size > s_total_size - ob) return false;
        /* Largest aligned record must fit in a page. */
        if (STORAGE_PROGRAM_BUFFER_MAX > s_page_size) return false;
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

    const uint8_t *raw_hdr = (const uint8_t *)&hdr;
    bool all_ff = true;
    for (size_t i = 0; i < STORAGE_HEADER_SIZE; i++)
    {
        if (raw_hdr[i] != 0xFF) { all_ff = false; break; }
    }

    if (all_ff)
    {
        if (header != NULL) memset(header, 0, sizeof(*header));
        return SLOT_STATE_ERASED;
    }

    if (hdr.magic != STORAGE_MAGIC) return SLOT_STATE_CORRUPT;
    if (hdr.record_format_version != STORAGE_RECORD_FORMAT_VERSION) return SLOT_STATE_CORRUPT;
    if (hdr.record_type != expected_record_type) return SLOT_STATE_CORRUPT;
    if (hdr.payload_size > STORAGE_PAYLOAD_MAX) return SLOT_STATE_CORRUPT;
    if (hdr.reserved != 0) return SLOT_STATE_CORRUPT;
    if (hdr.reserved2 != 0) return SLOT_STATE_CORRUPT;

    uint32_t expected_crc = hdr.crc32;
    hdr.crc32 = 0;

    uint8_t raw[STORAGE_RAW_MAX];
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

/* Read one slot into a full snapshot with a single hardware read. */
static SlotSnapshot ReadSlotSnapshot(uint8_t record_type, uint8_t slot_index)
{
    SlotSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.state = Storage_ReadSlot(record_type, slot_index, &snap.header, &snap.payload);
    return snap;
}

static void CopyPayload(const SlotSnapshot *snap, StoragePayload *payload)
{
    if (payload == NULL || snap == NULL) return;
    *payload = snap->payload;
}

/* Pick the newest-valid payload from the two snapshots. Returns 0/1. */
static uint8_t SelectNewest(const SlotSnapshot *a, const SlotSnapshot *b)
{
    return ((int32_t)(a->header.sequence - b->header.sequence) >= 0) ? 0U : 1U;
}

StorageReadStatus Storage_Read(uint8_t record_type, StoragePayload *payload)
{
    if (payload == NULL) return STORAGE_READ_INVALID_ARGUMENT;
    if (!s_initialized) return STORAGE_READ_IO_ERROR;
    if (Storage_GetLayout(record_type) == NULL) return STORAGE_READ_INVALID_ARGUMENT;

    SlotSnapshot a = ReadSlotSnapshot(record_type, 0);
    SlotSnapshot b = ReadSlotSnapshot(record_type, 1);

    /* ================================================================
       Result matrix (documented policy). Each slot is read exactly once.
       IO_ERROR fully dominates unless a peer slot is VALID.
       ================================================================
       ERASED  + ERASED   -> NOT_FOUND
       VALID   + ERASED   -> OK A
       ERASED  + VALID    -> OK B
       VALID   + CORRUPT  -> OK A          (health = DEGRADED)
       CORRUPT + VALID    -> OK B          (health = DEGRADED)
       VALID   + VALID    -> newest
       CORRUPT + CORRUPT  -> CORRUPT
       CORRUPT + ERASED   -> CORRUPT
       ERASED  + CORRUPT  -> CORRUPT
       IO_ERROR+ ERASED   -> IO_ERROR
       IO_ERROR+ CORRUPT  -> IO_ERROR
       IO_ERROR+ IO_ERROR -> IO_ERROR
       VALID   + IO_ERROR -> OK (valid peer)
                          (health = DEGRADED_IO; degradation is surfaced)
       ERASED  + IO_ERROR -> IO_ERROR
       ================================================================
       IO_ERROR is NEVER reclassified as CORRUPT or ERASED. When a peer is
       fully VALID we return the validated payload and record DEGRADED_IO in
       Storage_GetHealth() so the degradation is not hidden.
       ================================================================ */

    bool a_valid = (a.state == SLOT_STATE_VALID);
    bool b_valid = (b.state == SLOT_STATE_VALID);

    if (a_valid && b_valid)
    {
        CopyPayload((SelectNewest(&a, &b) == 0U) ? &a : &b, payload);
        return STORAGE_READ_OK;
    }
    if (a_valid && b.state == SLOT_STATE_CORRUPT) { CopyPayload(&a, payload); return STORAGE_READ_OK; }
    if (b_valid && a.state == SLOT_STATE_CORRUPT) { CopyPayload(&b, payload); return STORAGE_READ_OK; }
    if (a_valid && b.state == SLOT_STATE_ERASED)  { CopyPayload(&a, payload); return STORAGE_READ_OK; }
    if (b_valid && a.state == SLOT_STATE_ERASED)  { CopyPayload(&b, payload); return STORAGE_READ_OK; }
    if (a_valid && b.state == SLOT_STATE_IO_ERROR){ CopyPayload(&a, payload); return STORAGE_READ_OK; }
    if (b_valid && a.state == SLOT_STATE_IO_ERROR){ CopyPayload(&b, payload); return STORAGE_READ_OK; }

    if (a.state == SLOT_STATE_IO_ERROR || b.state == SLOT_STATE_IO_ERROR)
        return STORAGE_READ_IO_ERROR;

    if (a.state == SLOT_STATE_ERASED && b.state == SLOT_STATE_ERASED)
        return STORAGE_READ_NOT_FOUND;

    /* Any remaining non-erased, no-valid case is corrupt. */
    return STORAGE_READ_CORRUPT;
}

StorageHealth Storage_GetHealth(uint8_t record_type)
{
    if (!s_initialized) return STORAGE_HEALTH_IO_ERROR;
    if (Storage_GetLayout(record_type) == NULL) return STORAGE_HEALTH_IO_ERROR;

    SlotSnapshot a = ReadSlotSnapshot(record_type, 0);
    SlotSnapshot b = ReadSlotSnapshot(record_type, 1);

    bool a_valid = (a.state == SLOT_STATE_VALID);
    bool b_valid = (b.state == SLOT_STATE_VALID);

    if (a_valid && b_valid) return STORAGE_HEALTH_HEALTHY;
    if (a_valid && b.state == SLOT_STATE_IO_ERROR) return STORAGE_HEALTH_DEGRADED_IO;
    if (b_valid && a.state == SLOT_STATE_IO_ERROR) return STORAGE_HEALTH_DEGRADED_IO;
    if (a.state == SLOT_STATE_IO_ERROR || b.state == SLOT_STATE_IO_ERROR)
        return STORAGE_HEALTH_IO_ERROR;

    if (a_valid || b_valid)
    {
        /* one usable copy; mirror erased or corrupt -> degraded */
        return STORAGE_HEALTH_DEGRADED;
    }

    if (a.state == SLOT_STATE_CORRUPT || b.state == SLOT_STATE_CORRUPT)
        return STORAGE_HEALTH_CORRUPT;

    if (a.state == SLOT_STATE_ERASED && b.state == SLOT_STATE_ERASED)
        return STORAGE_HEALTH_HEALTHY;

    return STORAGE_HEALTH_CORRUPT;
}

static uint8_t SelectWriteSlot(uint8_t record_type, uint32_t *sequence_out)
{
    SlotSnapshot a = ReadSlotSnapshot(record_type, 0);
    SlotSnapshot b = ReadSlotSnapshot(record_type, 1);

    /* everything + IO_ERROR -> FAIL (cannot trust any state). */
    if (a.state == SLOT_STATE_IO_ERROR || b.state == SLOT_STATE_IO_ERROR)
        return 0xFF;

    /* Both erased -> write slot A with sequence 1 (seq_out=0 base). */
    if (a.state == SLOT_STATE_ERASED && b.state == SLOT_STATE_ERASED)
    {
        *sequence_out = 0;
        return 0;
    }

    /* One valid + one erased/corrupt -> write the OTHER (inactive) slot.
       Sequence is read ONLY from the VALID slot. */
    if (a.state == SLOT_STATE_VALID && b.state == SLOT_STATE_ERASED)
    {
        *sequence_out = a.header.sequence;
        return 1;
    }
    if (b.state == SLOT_STATE_VALID && a.state == SLOT_STATE_ERASED)
    {
        *sequence_out = b.header.sequence;
        return 0;
    }
    if (a.state == SLOT_STATE_VALID && b.state == SLOT_STATE_CORRUPT)
    {
        *sequence_out = a.header.sequence;
        return 1;   /* overwrite the corrupt slot B */
    }
    if (b.state == SLOT_STATE_VALID && a.state == SLOT_STATE_CORRUPT)
    {
        *sequence_out = b.header.sequence;
        return 0;   /* overwrite the corrupt slot A */
    }

    /* Both valid -> write the older slot (sequence from valid headers). */
    if (a.state == SLOT_STATE_VALID && b.state == SLOT_STATE_VALID)
    {
        bool a_newer = (int32_t)(a.header.sequence - b.header.sequence) >= 0;
        *sequence_out = a_newer ? a.header.sequence : b.header.sequence;
        return a_newer ? 1U : 0U;
    }

    /* Remaining pairs have NO valid slot:
         CORRUPT+CORRUPT, CORRUPT+ERASED, ERASED+CORRUPT
       -> fail closed. Never fall through into sequence comparison. */
    return 0xFF;
}

/* Build a program-aligned record in the padded buffer and program it to the
   (already erased) target slot. Guarantees no OOB read: aligned <= buffer. */
static bool WriteProgramRecord(uint32_t write_offset,
                               uint8_t record_type, const uint8_t *data, size_t size,
                               uint32_t seq)
{
    if (size > STORAGE_PAYLOAD_MAX) return false;
    if ((data == NULL) && (size > 0)) return false;
    if (!s_initialized) return false;
    /* program_unit must be a supported power of two (validated at Init; the
       guard makes the alignment computation safe on every code path). */
    if (s_program_unit == 0 ||
        (s_program_unit & (s_program_unit - 1U)) != 0U)
        return false;

    size_t total = STORAGE_HEADER_SIZE + size;
    size_t aligned = (total + s_program_unit - 1U) & ~((size_t)(s_program_unit - 1U));

    /* Program-buffer capacity invariant (defensive, also asserted statically). */
    if (aligned < total) return false;
    if (aligned > STORAGE_PROGRAM_BUFFER_MAX) return false;
    /* Record must fit inside the slot page. */
    if (aligned > s_page_size) return false;

    StorageRecordHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = STORAGE_MAGIC;
    hdr.record_format_version = STORAGE_RECORD_FORMAT_VERSION;
    hdr.payload_size = (uint16_t)size;
    hdr.record_type = record_type;
    hdr.sequence = seq;

    /* Entire program buffer initialized to erased (0xFF) so all padding bytes
       between `total` and `aligned` are explicitly set, never uninitialized. */
    uint8_t program_buf[STORAGE_PROGRAM_BUFFER_MAX];
    memset(program_buf, 0xFF, sizeof(program_buf));

    memcpy(program_buf, &hdr, STORAGE_HEADER_SIZE);
    if (size > 0)
        memcpy(program_buf + STORAGE_HEADER_SIZE, data, size);

    hdr.crc32 = 0;
    memcpy(program_buf, &hdr, STORAGE_HEADER_SIZE);

    uint32_t crc = Storage_Crc32(program_buf, total);
    hdr.crc32 = crc;
    memcpy(program_buf, &hdr, STORAGE_HEADER_SIZE);

    if (Platform_FlashWrite(write_offset, program_buf, aligned) != PLATFORM_FLASH_OK)
        return false;

    /* Verify the program-aligned bytes as written. */
    uint8_t verify[STORAGE_PROGRAM_BUFFER_MAX];
    if (Platform_FlashRead(write_offset, verify, total) != PLATFORM_FLASH_OK)
        return false;
    if (memcmp(program_buf, verify, total) != 0)
        return false;

    return true;
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
        return false;  /* fail closed: slot state unknown or unsafe */

    uint32_t write_page = (write_slot == 0) ? layout->slot_a_page : layout->slot_b_page;
    uint32_t write_offset = (write_slot == 0) ? layout->slot_a_offset : layout->slot_b_offset;

    uint32_t next_seq = current_seq + 1;
    if (next_seq == 0) next_seq = 1;

    /* Erase only the inactive slot page; the active valid page is preserved. */
    if (Platform_FlashErase(write_page) != PLATFORM_FLASH_OK)
        return false;

    return WriteProgramRecord(write_offset, record_type, data, size, next_seq);
}

StorageRecoveryStatus Storage_RecoverCorruptRecord(uint8_t record_type,
                                                   const uint8_t *data, size_t size)
{
    if (!s_initialized) return STORAGE_RECOVERY_FAILED;
    if (size > STORAGE_PAYLOAD_MAX) return STORAGE_RECOVERY_INVALID_ARGUMENT;
    if ((data == NULL) && (size > 0)) return STORAGE_RECOVERY_INVALID_ARGUMENT;

    const StorageRecordLayout *layout = Storage_GetLayout(record_type);
    if (layout == NULL) return STORAGE_RECOVERY_INVALID_ARGUMENT;

    /* Inspect slot states FIRST, before any destructive erase. */
    SlotSnapshot a = ReadSlotSnapshot(record_type, 0);
    SlotSnapshot b = ReadSlotSnapshot(record_type, 1);

    /* IO uncertainty: never erase. */
    if (a.state == SLOT_STATE_IO_ERROR || b.state == SLOT_STATE_IO_ERROR)
        return STORAGE_RECOVERY_IO_ERROR;

    /* No corruption to repair: healthy (both valid) or blank (both erased).
       Do not erase a healthy/empty region as an ordinary operation. */
    bool a_valid = (a.state == SLOT_STATE_VALID);
    bool b_valid = (b.state == SLOT_STATE_VALID);
    bool a_corrupt = (a.state == SLOT_STATE_CORRUPT);
    bool b_corrupt = (b.state == SLOT_STATE_CORRUPT);
    if ((a_valid && b_valid) || (a.state == SLOT_STATE_ERASED && b.state == SLOT_STATE_ERASED))
        return STORAGE_RECOVERY_NOT_NEEDED;

    /* Known readable-but-corrupt region with no IO uncertainty: repair it.
       Erase ONLY the two owned pages; never touch other records. */
    if (!a_corrupt && !b_corrupt)
        return STORAGE_RECOVERY_NOT_NEEDED;

    if (Platform_FlashErase(layout->slot_a_page) != PLATFORM_FLASH_OK)
        return STORAGE_RECOVERY_FAILED;
    if (Platform_FlashErase(layout->slot_b_page) != PLATFORM_FLASH_OK)
        return STORAGE_RECOVERY_FAILED;

    /* Both slots now ERASED; write a fresh sequence-1 record into slot A. */
    if (!WriteProgramRecord(layout->slot_a_offset, record_type, data, size, 1U))
        return STORAGE_RECOVERY_FAILED;

    return STORAGE_RECOVERY_OK;
}

bool Storage_FormatRecord(uint8_t record_type)
{
    if (!s_initialized) return false;
    const StorageRecordLayout *layout = Storage_GetLayout(record_type);
    if (layout == NULL) return false;

    if (Platform_FlashErase(layout->slot_a_page) != PLATFORM_FLASH_OK)
        return false;
    if (Platform_FlashErase(layout->slot_b_page) != PLATFORM_FLASH_OK)
        return false;
    return true;
}

bool Storage_Format(void)
{
    if (!s_initialized) return false;

    /* Storage_Format is the engineering/service destructive operation. It
       erases ONLY the pages Storage owns (STORAGE_OWNED_PAGES = Config A/B,
       Identity A/B, Registration A/B). It NEVER erases pages beyond that owned
       partition, even if PlatformFlashInfo.page_count is larger (extra pages
       belong to a broader partition and are not Storage-managed). */
    for (uint32_t p = 0; p < STORAGE_OWNED_PAGES; p++)
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

    SlotSnapshot a = ReadSlotSnapshot(record_type, 0);
    SlotSnapshot b = ReadSlotSnapshot(record_type, 1);

    info->slot_a_valid = (a.state == SLOT_STATE_VALID);
    info->slot_b_valid = (b.state == SLOT_STATE_VALID);
    info->slot_a_sequence = info->slot_a_valid ? a.header.sequence : 0;
    info->slot_b_sequence = info->slot_b_valid ? b.header.sequence : 0;
    return !(a.state == SLOT_STATE_IO_ERROR || b.state == SLOT_STATE_IO_ERROR);
}