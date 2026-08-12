#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "storage.h"
#include "platform_flash.h"
#include "fake_flash.h"

static int s_pass = 0, s_fail = 0, s_case = 0;

static void T(const char *name, int cond)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static uint32_t Crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint32_t)data[i];
        for (int b = 0; b < 8; b++)
        {
            if (crc & 1U) crc = (crc >> 1U) ^ 0xEDB88320U;
            else crc >>= 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

/* Craft an arbitrary record with a valid CRC and write it to slot `slot` of the
   record_type region. record_type_field and version mimic persistent bytes. */
static void SprayRawRecord(uint8_t region_type, uint8_t slot, uint8_t record_type_field,
                           uint16_t version, uint32_t seq, const uint8_t *payload, size_t size)
{
    const StorageRecordLayout *lo = Storage_GetLayout(region_type);
    if (lo == NULL) return;
    uint32_t offset = (slot == 0) ? lo->slot_a_offset : lo->slot_b_offset;
    uint32_t page   = (slot == 0) ? lo->slot_a_page   : lo->slot_b_page;

    StorageRecordHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = STORAGE_MAGIC;
    hdr.record_format_version = version;
    hdr.payload_size = (uint16_t)size;
    hdr.record_type = record_type_field;
    hdr.sequence = seq;

    uint8_t raw[STORAGE_HEADER_SIZE + STORAGE_PAYLOAD_MAX];
    memset(raw, 0xFF, sizeof(raw));
    memcpy(raw, &hdr, STORAGE_HEADER_SIZE);
    if (size > 0) memcpy(raw + STORAGE_HEADER_SIZE, payload, size);

    hdr.crc32 = 0;
    memcpy(raw, &hdr, STORAGE_HEADER_SIZE);
    hdr.crc32 = Crc32(raw, STORAGE_HEADER_SIZE + size);
    memcpy(raw, &hdr, STORAGE_HEADER_SIZE);

    Platform_FlashErase(page);
    size_t aligned = (STORAGE_HEADER_SIZE + size + 7U) & ~((size_t)7U);
    Platform_FlashWrite(offset, raw, aligned);
}

static void TestMatrix(void)
{
    printf("\n=== Storage slot-state matrix ===\n");

    /* 1. both erased -> NOT_FOUND */
    FakeFlash_Init();
    Storage_Init();
    {
        StoragePayload p;
        T("both erased -> NOT_FOUND",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_NOT_FOUND);
    }

    /* 2. valid A -> OK */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[8] = {1,2,3,4,5,6,7,8};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
        StoragePayload p;
        T("valid A -> OK", Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK);
        T("payload A returned",
          p.size == 8 && memcmp(p.data, a, 8) == 0);
    }

    /* 3. valid B -> OK (newest sequence in B) */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1,2,3,4};
        uint8_t b[4] = {9,9,9,9};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
        Storage_Write(RECORD_TYPE_CONFIG, b, sizeof(b));  /* newest -> B */
        StoragePayload p;
        T("valid B -> OK", Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK);
        T("newest (B) payload returned", p.size == 4 && memcmp(p.data, b, 4) == 0);
    }

    /* 4. valid + corrupt -> valid (A) */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1,2,3,4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
        FakeFlash_Corrupt(2048, 8);   /* corrupt slot B (offset 2048) */
        StoragePayload p;
        T("valid A + corrupt B -> OK A",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK);
        T("A payload intact", memcmp(p.data, a, 4) == 0);
    }

    /* 5. corrupt + corrupt -> CORRUPT */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t x[4] = {1,2,3,4};
        Storage_Write(RECORD_TYPE_CONFIG, x, sizeof(x));   /* A */
        Storage_Write(RECORD_TYPE_CONFIG, x, sizeof(x));   /* B */
        FakeFlash_Corrupt(0, 8);      /* corrupt slot A header */
        FakeFlash_Corrupt(2048, 8);   /* corrupt slot B header */
        StoragePayload p;
        T("corrupt A + corrupt B -> CORRUPT",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_CORRUPT);
    }

    /* 6. IO + erased -> IO_ERROR */
    FakeFlash_Init(); Storage_Init();
    {
        FakeFlash_SetReadFail(true, 0, 64);   /* slot A unreadable */
        StoragePayload p;
        T("IO A + erased B -> IO_ERROR",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_IO_ERROR);
        FakeFlash_SetReadFail(false, 0, 0);
    }

    /* 7. IO + IO -> IO_ERROR */
    FakeFlash_Init(); Storage_Init();
    {
        FakeFlash_SetReadFail(true, 0, 4096); /* cover both slots */
        StoragePayload p;
        T("IO A + IO B -> IO_ERROR",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_IO_ERROR);
        FakeFlash_SetReadFail(false, 0, 0);
    }

    /* 8. CRC wrong -> CORRUPT */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1,2,3,4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* A valid */
        SprayRawRecord(RECORD_TYPE_CONFIG, 1, RECORD_TYPE_CONFIG,
                       STORAGE_RECORD_FORMAT_VERSION, 2,
                       (const uint8_t[]){0xDE,0xAD,0xBE,0xEF}, 4);
        /* Corrupt a payload byte in slot B after a valid CRC was written. */
        {
            uint8_t *data = (uint8_t *)FakeFlash_GetData();
            data[2048 + STORAGE_HEADER_SIZE] ^= 0xFF;  /* invalidate CRC */
        }
        StoragePayload p;
        T("A valid + B bad-CRC -> OK A (valid preserved)",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK);
    }
    /* 8b. both bad CRC -> CORRUPT */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1,2,3,4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
        uint8_t *data = (uint8_t *)FakeFlash_GetData();
        data[0 + STORAGE_HEADER_SIZE] ^= 0xFF;
        data[2048 + STORAGE_HEADER_SIZE] ^= 0xFF;
        StoragePayload p;
        T("A bad-CRC + B bad-CRC -> CORRUPT",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_CORRUPT);
    }

    /* 9. wrong record type -> CORRUPT (P0) */
    FakeFlash_Init(); Storage_Init();
    {
        /* Craft a CRC-valid CONFIG record into the REGISTRATION region. */
        SprayRawRecord(RECORD_TYPE_REGISTRATION, 0, RECORD_TYPE_CONFIG,
                       STORAGE_RECORD_FORMAT_VERSION, 1,
                       (const uint8_t[]){0xAA}, 1);
        StoragePayload p;
        T("registration slot holding CONFIG record read as CORRUPT",
          Storage_Read(RECORD_TYPE_REGISTRATION, &p) == STORAGE_READ_CORRUPT);
    }

    /* 10. wrong schema/version -> CORRUPT */
    FakeFlash_Init(); Storage_Init();
    {
        SprayRawRecord(RECORD_TYPE_CONFIG, 0, RECORD_TYPE_CONFIG, 99, 1,
                       (const uint8_t[]){0x11}, 1);
        StoragePayload p;
        T("wrong format version -> CORRUPT",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_CORRUPT);
    }

    /* 11. write refuses on IO uncertainty -> false, no erase */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1,2,3,4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* A valid */
        /* Simulate IO error only on slot B so A stays valid. A write with an
           IO_ERROR slot must fail closed (it would erase the inactive B). */
        FakeFlash_SetReadFail(true, 2048, 4096);
        uint8_t w[4] = {5,6,7,8};
        T("write refused on IO uncertainty", !Storage_Write(RECORD_TYPE_CONFIG, w, sizeof(w)));
        /* A (offset 0) must still hold the valid record. */
        StoragePayload p;
        FakeFlash_SetReadFail(false, 0, 0);
        T("active slot A still valid after refused write",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK &&
          memcmp(p.data, a, 4) == 0);
    }

    /* 12. write never erases active valid page */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1,2,3,4};
        uint8_t b[4] = {5,6,7,8};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* A */
        Storage_Write(RECORD_TYPE_CONFIG, b, sizeof(b));  /* B */
        /* Re-read: newest is B. Write again -> must go to slot A (older),
           never erasing B. */
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* -> A again */
        StoragePayload p;
        Storage_Read(RECORD_TYPE_CONFIG, &p);
        T("active valid page preserved after write rotation",
          p.size == 4 && memcmp(p.data, a, 4) == 0);
    }
}

static void TestHealth(void)
{
    printf("\n=== Storage health ===\n");
    FakeFlash_Init(); Storage_Init();
    T("blank region healthy",
      Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_HEALTHY);

    FakeFlash_Init(); Storage_Init();
    {
        SprayRawRecord(RECORD_TYPE_CONFIG, 0, RECORD_TYPE_CONFIG,
                       STORAGE_RECORD_FORMAT_VERSION, 1,
                       (const uint8_t[]){0x11}, 1);
        /* Corrupt the header magic so the record is structurally invalid. */
        {
            uint8_t *data = (uint8_t *)FakeFlash_GetData();
            data[0] ^= 0xFF;
        }
        T("corrupt region reports corrupt",
          Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_CORRUPT);
    }

    FakeFlash_Init(); Storage_Init();
    {
        FakeFlash_SetReadFail(true, 0, 64);
        T("IO region reports io_error",
          Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_IO_ERROR);
        FakeFlash_SetReadFail(false, 0, 0);
    }
}

/* Deterministic proof of the P0 invariant: for every valid payload size and
   every supported program unit (power of two in (0, STORAGE_PROGRAM_UNIT_MAX]),
   the aligned length never exceeds the program buffer capacity, so no aligned
   write can ever read past the buffer. */
static void TestProgramBounds(void)
{
    printf("\n=== Program-buffer capacity invariant ===\n");
    int ok = 1;
    for (uint32_t unit = 1; unit <= STORAGE_PROGRAM_UNIT_MAX; unit <<= 1)
    {
        for (size_t size = 0; size <= STORAGE_PAYLOAD_MAX; size++)
        {
            size_t total = STORAGE_HEADER_SIZE + size;
            size_t aligned = (total + unit - 1U) & ~((size_t)(unit - 1U));
            if (aligned < total || aligned > STORAGE_PROGRAM_BUFFER_MAX)
            {
                ok = 0;
                printf("  FAIL unit=%u size=%zu aligned=%zu cap=%u\n",
                       (unsigned)unit, size, aligned,
                       (unsigned)STORAGE_PROGRAM_BUFFER_MAX);
            }
        }
    }
    T("all sizes/units: aligned within program buffer", ok);
    T("max payload aligned == capacity (exact boundary)",
      ((STORAGE_HEADER_SIZE + STORAGE_PAYLOAD_MAX + STORAGE_PROGRAM_UNIT_MAX - 1U) &
       ~((size_t)(STORAGE_PROGRAM_UNIT_MAX - 1U))) == STORAGE_PROGRAM_BUFFER_MAX);
}

/* Memory-safety & program-alignment boundary tests. The max-payload case is
   the exact aligned-buffer OOB regression (previously aligned=280 > raw[278]).
   These run under ASan/UBSan in CI. */
static void TestMemoryBoundaries(void)
{
    printf("\n=== Storage write boundaries (memory safety) ===\n");
    const size_t sizes[] = {0U, 1U, 7U, 8U, 9U,
                            STORAGE_PAYLOAD_MAX - 1U, STORAGE_PAYLOAD_MAX};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        FakeFlash_Init();
        Storage_Init();
        uint8_t buf[STORAGE_PAYLOAD_MAX];
        for (size_t j = 0; j < sizes[i]; j++) buf[j] = (uint8_t)(j + 1U);

        bool wok = Storage_Write(RECORD_TYPE_CONFIG, buf, sizes[i]);
        StoragePayload p;
        StorageReadStatus rs = Storage_Read(RECORD_TYPE_CONFIG, &p);
        char name[72];
        snprintf(name, sizeof(name), "write+read size=%zu no OOB round-trip", sizes[i]);
        T(name, wok && rs == STORAGE_READ_OK && p.size == sizes[i] &&
                (sizes[i] == 0 || memcmp(p.data, buf, sizes[i]) == 0));
    }
}

/* Record-scoped recovery: repairs both-corrupt registration without touching
   config or identity, and ordinary write remains fail-closed. */
static void TestRecovery(void)
{
    printf("\n=== Record recovery ===\n");

    FakeFlash_Init(); Storage_Init();

    uint8_t cfg[4] = {1, 2, 3, 4};
    Storage_Write(RECORD_TYPE_CONFIG, cfg, sizeof(cfg));

    uint8_t id[24];
    for (size_t i = 0; i < sizeof(id); i++) id[i] = (uint8_t)(i + 100);
    Storage_Write(RECORD_TYPE_IDENTITY, id, sizeof(id));

    /* Corrupt both registration slots (pages 4-5 -> offsets 8192, 10240). */
    FakeFlash_Corrupt(8192, 40);
    FakeFlash_Corrupt(10240, 40);

    uint8_t reg[60];
    memset(reg, 0xAB, sizeof(reg));

    StoragePayload pr;
    T("both-corrupt read -> CORRUPT",
      Storage_Read(RECORD_TYPE_REGISTRATION, &pr) == STORAGE_READ_CORRUPT);

    T("ordinary write refused on both-corrupt (fail closed)",
      !Storage_Write(RECORD_TYPE_REGISTRATION, reg, sizeof(reg)));

    /* Config/Identity remain untouched by the corruption and by recovery. */
    StoragePayload pi, pc;
    T("identity unaffected by registration corruption",
      Storage_Read(RECORD_TYPE_IDENTITY, &pi) == STORAGE_READ_OK &&
      memcmp(pi.data, id, sizeof(id)) == 0);
    T("config unaffected by registration corruption",
      Storage_Read(RECORD_TYPE_CONFIG, &pc) == STORAGE_READ_OK &&
      memcmp(pc.data, cfg, sizeof(cfg)) == 0);

    T("explicit corrupt-region recovery succeeds",
      Storage_RecoverCorruptRecord(RECORD_TYPE_REGISTRATION, reg, sizeof(reg)) ==
      STORAGE_RECOVERY_OK);

    T("registration readable after recovery",
      Storage_Read(RECORD_TYPE_REGISTRATION, &pr) == STORAGE_READ_OK &&
      pr.size == sizeof(reg) && memcmp(pr.data, reg, sizeof(reg)) == 0);
    T("identity still valid after registration recovery",
      Storage_Read(RECORD_TYPE_IDENTITY, &pi) == STORAGE_READ_OK &&
      memcmp(pi.data, id, sizeof(id)) == 0);
    T("config still valid after registration recovery",
      Storage_Read(RECORD_TYPE_CONFIG, &pc) == STORAGE_READ_OK &&
      memcmp(pc.data, cfg, sizeof(cfg)) == 0);

    /* Storage_FormatRecord erases ONLY the selected record's two pages. */
    T("FormatRecord(registration) succeeds",
      Storage_FormatRecord(RECORD_TYPE_REGISTRATION));
    T("registration erased after FormatRecord",
      Storage_Read(RECORD_TYPE_REGISTRATION, &pr) == STORAGE_READ_NOT_FOUND);
    T("identity unaffected after FormatRecord(registration)",
      Storage_Read(RECORD_TYPE_IDENTITY, &pi) == STORAGE_READ_OK &&
      memcmp(pi.data, id, sizeof(id)) == 0);
    T("config unaffected after FormatRecord(registration)",
      Storage_Read(RECORD_TYPE_CONFIG, &pc) == STORAGE_READ_OK &&
      memcmp(pc.data, cfg, sizeof(cfg)) == 0);
}

/* Each slot is read exactly once per Storage_Read (no re-read/re-classify). */
static void TestReadOnce(void)
{
    printf("\n=== Single-read slot snapshot ===\n");
    FakeFlash_Init(); Storage_Init();
    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot A */
    Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot B */

    FakeFlash_ResetReadCount();
    StoragePayload p;
    Storage_Read(RECORD_TYPE_CONFIG, &p);
    /* VALID+VALID: header+payload for each of 2 slots = exactly 4 reads. */
    T("each slot read once per Storage_Read (4 reads, no re-read)",
      FakeFlash_GetReadCount() == 4);
}

/* Extended health semantics (usable-degraded vs unrecoverable corruption). */
static void TestHealthExtended(void)
{
    printf("\n=== Health semantics ===\n");
    uint8_t a[4] = {1, 2, 3, 4};

    FakeFlash_Init(); Storage_Init();
    Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
    Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
    T("VALID+VALID -> healthy", Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_HEALTHY);

    FakeFlash_Init(); Storage_Init();
    Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
    T("VALID+ERASED -> degraded", Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_DEGRADED);

    FakeFlash_Init(); Storage_Init();
    Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
    FakeFlash_Corrupt(2048, 8);
    T("VALID+CORRUPT -> degraded", Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_DEGRADED);

    FakeFlash_Init(); Storage_Init();
    Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
    Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
    FakeFlash_Corrupt(0, 8);
    FakeFlash_Corrupt(2048, 8);
    T("both corrupt -> corrupt", Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_CORRUPT);

    FakeFlash_Init(); Storage_Init();
    Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
    FakeFlash_SetReadFail(true, 2048, 4096);   /* slot B IO_ERROR */
    T("VALID+IO -> degraded_io", Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_DEGRADED_IO);
    FakeFlash_SetReadFail(false, 0, 0);

    FakeFlash_Init(); Storage_Init();
    FakeFlash_SetReadFail(true, 0, 4096);      /* both slots IO_ERROR */
    T("both IO -> io_error", Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_IO_ERROR);
    FakeFlash_SetReadFail(false, 0, 0);
}

/* CORRUPT+non-empty / ERASED+CORRUPT must fail closed in SelectWriteSlot:
   write returns false, with ZERO erase and ZERO write calls. No fallthrough
   into sequence logic; sequence is only ever read from VALID slots. */
static void TestWriteFailClosed(void)
{
    printf("\n=== Write fail-closed on no-valid pairs ===\n");

    /* A CORRUPT, B ERASED */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot A valid */
        FakeFlash_Corrupt(0, 8);                          /* corrupt slot A */
        uint8_t w[4] = {5, 6, 7, 8};
        FakeFlash_ResetIoCounters();
        T("CORRUPT+ERASED write refused", !Storage_Write(RECORD_TYPE_CONFIG, w, sizeof(w)));
        T("  zero erase on refused write", FakeFlash_GetEraseCount() == 0);
        T("  zero write on refused write", FakeFlash_GetWriteCount() == 0);
    }

    /* A ERASED, B CORRUPT */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* A */
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* B (newest) */
        Platform_FlashErase(0);                           /* erase slot A page -> ERASED */
        FakeFlash_Corrupt(2048, 8);                       /* corrupt slot B */
        uint8_t w[4] = {5, 6, 7, 8};
        FakeFlash_ResetIoCounters();
        T("ERASED+CORRUPT write refused", !Storage_Write(RECORD_TYPE_CONFIG, w, sizeof(w)));
        T("  zero erase on refused write", FakeFlash_GetEraseCount() == 0);
        T("  zero write on refused write", FakeFlash_GetWriteCount() == 0);
    }
}

/* Recovery hardening: healthy/blank -> NOT_NEEDED; IO_ERROR -> refused with
   ZERO erase calls. */
static void TestRecoveryHarden(void)
{
    printf("\n=== Recovery guards ===\n");

    /* both valid -> NOT_NEEDED, no erase */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
        FakeFlash_ResetIoCounters();
        uint8_t w[4] = {9, 9, 9, 9};
        T("healthy recovery NOT_NEEDED",
          Storage_RecoverCorruptRecord(RECORD_TYPE_CONFIG, w, sizeof(w)) ==
          STORAGE_RECOVERY_NOT_NEEDED);
        T("  zero erase on healthy recovery", FakeFlash_GetEraseCount() == 0);
    }

    /* blank (both erased) -> NOT_NEEDED, no erase */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t w[4] = {9, 9, 9, 9};
        T("blank recovery NOT_NEEDED",
          Storage_RecoverCorruptRecord(RECORD_TYPE_CONFIG, w, sizeof(w)) ==
          STORAGE_RECOVERY_NOT_NEEDED);
    }

    /* IO_ERROR -> refused, zero erase */
    FakeFlash_Init(); Storage_Init();
    {
        FakeFlash_SetReadFail(true, 0, 4096);   /* both slots unreadable */
        uint8_t w[4] = {9, 9, 9, 9};
        FakeFlash_ResetIoCounters();
        T("IO recovery refused",
          Storage_RecoverCorruptRecord(RECORD_TYPE_CONFIG, w, sizeof(w)) ==
          STORAGE_RECOVERY_IO_ERROR);
        T("  zero erase on IO refusal", FakeFlash_GetEraseCount() == 0);
        FakeFlash_SetReadFail(false, 0, 0);
    }
}

/* Storage_Format must NOT erase pages outside the Storage-owned partition,
   even when PlatformFlashInfo.page_count is larger (extra unrelated pages). */
static void TestFormatOwnership(void)
{
    printf("\n=== Storage_Format partition ownership ===\n");
    FakeFlash_Init();
    FakeFlash_SetPageCount(8);   /* expose 2 extra unrelated pages */
    Storage_Init();

    /* Mark the unrelated pages 6 and 7 so erasure is detectable. */
    uint8_t *raw = (uint8_t *)FakeFlash_GetData();
    raw[6 * FAKE_FLASH_SIZE] = 0x5A;
    raw[6 * FAKE_FLASH_SIZE + 1] = 0x5A;
    raw[7 * FAKE_FLASH_SIZE] = 0x5A;
    raw[7 * FAKE_FLASH_SIZE + 1] = 0x5A;

    T("Format claims only owned pages",
      Storage_Format());

    /* Owned pages 0..5 must be erased (0xFF); unrelated 6/7 preserved. */
    int all_erased = 1;
    for (uint32_t p = 0; p < STORAGE_OWNED_PAGES; p++)
    {
        for (uint32_t b = 0; b < 4U; b++)
            if (raw[p * FAKE_FLASH_SIZE + b] != 0xFF) { all_erased = 0; break; }
    }
    T("owned pages 0..5 erased", all_erased);
    T("unrelated page 6 not erased",
      raw[6 * FAKE_FLASH_SIZE] == 0x5A && raw[6 * FAKE_FLASH_SIZE + 1] == 0x5A);
    T("unrelated page 7 not erased",
      raw[7 * FAKE_FLASH_SIZE] == 0x5A && raw[7 * FAKE_FLASH_SIZE + 1] == 0x5A);

    FakeFlash_SetPageCount(FAKE_FLASH_PAGES);   /* restore default */
}

/* Unsupported Flash bank config (e.g. dual-bank) must fail closed: Storage_Init
   fails, and both erase and program refuse with ZERO side effects. */
static void TestBankValidation(void)
{
    printf("\n=== Unsupported Flash bank configuration ===\n");

    FakeFlash_Init();
    FakeFlash_SetBankSupported(false);
    FakeFlash_ResetIoCounters();

    T("Storage_Init fails on unsupported bank config", !Storage_Init());
    T("erase fails closed on unsupported config",
      Platform_FlashErase(0) == PLATFORM_FLASH_ERROR);
    T("  zero erase on unsupported config", FakeFlash_GetEraseCount() == 0);
    {
        uint8_t d[8] = {0};
        T("write fails closed on unsupported config",
          Platform_FlashWrite(0, d, sizeof(d)) == PLATFORM_FLASH_ERROR);
    }
    T("  zero write on unsupported config", FakeFlash_GetWriteCount() == 0);

    /* Restore supported config: storage works again. */
    FakeFlash_Init();   /* resets bank_supported to true */
    T("Storage_Init OK after restoring supported config", Storage_Init());
    {
        StoragePayload p;
        T("storage usable after restore",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_NOT_FOUND);
    }
}

/* Recovery must NOT destroy the final valid record. Any pair with a readable
   valid copy (VALID+CORRUPT, CORRUPT+VALID, VALID+ERASED, ERASED+VALID) is
   repairable by a normal Storage_Write, so explicit destructive recovery is
   NOT_NEEDED and performs ZERO erase calls. */
static void TestRecoveryPreserveValid(void)
{
    printf("\n=== Recovery preserves valid copy (zero-erase NOT_NEEDED) ===\n");

    /* VALID + CORRUPT -> NOT_NEEDED, zero erases, valid copy intact. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot A valid */
        FakeFlash_Corrupt(2048, 8);                       /* corrupt slot B */
        uint8_t w[4] = {9, 9, 9, 9};
        FakeFlash_ResetIoCounters();
        T("VALID+CORRUPT -> NOT_NEEDED",
          Storage_RecoverCorruptRecord(RECORD_TYPE_CONFIG, w, sizeof(w)) ==
          STORAGE_RECOVERY_NOT_NEEDED);
        T("  VALID+CORRUPT zero erase calls", FakeFlash_GetEraseCount() == 0);
        {
            StoragePayload p;
            T("  valid copy still readable",
              Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK);
            T("  valid payload preserved", memcmp(p.data, a, sizeof(a)) == 0);
        }
    }

    /* CORRUPT + VALID -> NOT_NEEDED, zero erases, valid copy intact. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot A valid */
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot B valid (newest) */
        FakeFlash_Corrupt(0, 8);                          /* corrupt slot A */
        uint8_t w[4] = {9, 9, 9, 9};
        FakeFlash_ResetIoCounters();
        T("CORRUPT+VALID -> NOT_NEEDED",
          Storage_RecoverCorruptRecord(RECORD_TYPE_CONFIG, w, sizeof(w)) ==
          STORAGE_RECOVERY_NOT_NEEDED);
        T("  CORRUPT+VALID zero erase calls", FakeFlash_GetEraseCount() == 0);
        {
            StoragePayload p;
            T("  valid copy still readable",
              Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK);
        }
    }

    /* VALID + ERASED -> NOT_NEEDED, zero erases. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        uint8_t w[4] = {9, 9, 9, 9};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot A valid, B erased */
        FakeFlash_ResetIoCounters();
        T("VALID+ERASED -> NOT_NEEDED",
          Storage_RecoverCorruptRecord(RECORD_TYPE_CONFIG, w, sizeof(w)) ==
          STORAGE_RECOVERY_NOT_NEEDED);
        T("  VALID+ERASED zero erase calls", FakeFlash_GetEraseCount() == 0);
    }

    /* ERASED + VALID -> NOT_NEEDED, zero erases. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        uint8_t w[4] = {9, 9, 9, 9};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot A valid */
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot B valid (newest) */
        Platform_FlashErase(0);                           /* erase slot A -> ERASED */
        FakeFlash_ResetIoCounters();
        T("ERASED+VALID -> NOT_NEEDED",
          Storage_RecoverCorruptRecord(RECORD_TYPE_CONFIG, w, sizeof(w)) ==
          STORAGE_RECOVERY_NOT_NEEDED);
        T("  ERASED+VALID zero erase calls", FakeFlash_GetEraseCount() == 0);
    }
}

/* StorageWriteStatus classification: an unsafe (no-valid) state, a genuine
   Flash IO failure, and a post-write verification mismatch must be reported
   distinctly rather than collapsing everything into a generic "IO error". */
static void TestWriteClassification(void)
{
    printf("\n=== Write classification (StorageWriteStatus) ===\n");

    /* CORRUPT+ERASED (no valid copy) -> UNSAFE_STATE, not IO_ERROR. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot A valid */
        FakeFlash_Corrupt(0, 8);                          /* corrupt slot A -> no valid */
        uint8_t w[4] = {9, 9, 9, 9};
        FakeFlash_ResetIoCounters();
        T("no-valid pair -> UNSAFE_STATE",
          Storage_WriteEx(RECORD_TYPE_CONFIG, w, sizeof(w)) == STORAGE_WRITE_UNSAFE_STATE);
        T("  zero erase on unsafe write", FakeFlash_GetEraseCount() == 0);
    }

    /* Valid pair + Flash erase/program HAL failure -> IO_ERROR. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));  /* slot A valid, B erased */
        FakeFlash_SetWriteFail(true);                     /* erase of slot B fails */
        uint8_t w[4] = {9, 9, 9, 9};
        T("HAL erase failure -> IO_ERROR",
          Storage_WriteEx(RECORD_TYPE_CONFIG, w, sizeof(w)) == STORAGE_WRITE_IO_ERROR);
        FakeFlash_SetWriteFail(false);
    }

    /* Post-write readback mismatch -> VERIFY_FAILED (distinct from IO_ERROR). */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t w[4] = {9, 9, 9, 9};
        FakeFlash_SetVerifyFail(true);
        T("readback mismatch -> VERIFY_FAILED",
          Storage_WriteEx(RECORD_TYPE_CONFIG, w, sizeof(w)) == STORAGE_WRITE_VERIFY_FAILED);
        FakeFlash_SetVerifyFail(false);
        /* Region not left valid; a retry with verify ok succeeds. */
        StoragePayload rp;
        T("verify failure did not leave valid record",
          Storage_Read(RECORD_TYPE_CONFIG, &rp) != STORAGE_READ_OK);
    }

    /* Success -> OK. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t w[4] = {9, 9, 9, 9};
        T("clean write -> OK",
          Storage_WriteEx(RECORD_TYPE_CONFIG, w, sizeof(w)) == STORAGE_WRITE_OK);
    }
}

/* Safe A/B mirror establishment (Storage_EnsureRedundancy). The valid source
   is never erased; the degraded peer (erased/corrupt) is mirrored. IO
   uncertainty refuses without erasing. */
static void TestEnsureRedundancy(void)
{
    printf("\n=== EnsureRedundancy (safe mirror repair) ===\n");

    /* VALID+ERASED -> DONE, both valid -> HEALTHY. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));   /* slot A valid, B erased */
        T("health before repair == DEGRADED",
          Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_DEGRADED);
        T("VALID+ERASED -> DONE",
          Storage_EnsureRedundancy(RECORD_TYPE_CONFIG) == STORAGE_REPAIR_DONE);
        T("health after repair == HEALTHY",
          Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_HEALTHY);
        StoragePayload p;
        T("mirrored record still readable",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK);
        T("  mirrored payload preserved", p.size == sizeof(a) && memcmp(p.data, a, sizeof(a)) == 0);
    }

    /* VALID+CORRUPT -> DONE (peer rebuilt from valid source). */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));   /* slot A valid */
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));   /* slot B valid (newest) */
        FakeFlash_Corrupt(2048, 8);                        /* corrupt slot B, keep A valid */
        T("VALID+CORRUPT -> DONE",
          Storage_EnsureRedundancy(RECORD_TYPE_CONFIG) == STORAGE_REPAIR_DONE);
        T("  health became HEALTHY",
          Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_HEALTHY);
    }

    /* VALID+IO -> REFUSED, zero erase. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));   /* slot A valid */
        FakeFlash_SetReadFail(true, 2048, 4096);           /* slot B unreadable */
        FakeFlash_ResetIoCounters();
        T("VALID+IO -> REFUSED",
          Storage_EnsureRedundancy(RECORD_TYPE_CONFIG) == STORAGE_REPAIR_REFUSED);
        T("  zero erase on IO refusal", FakeFlash_GetEraseCount() == 0);
        FakeFlash_SetReadFail(false, 0, 0);
    }

    /* VALID+VALID -> NOT_NEEDED. */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {1, 2, 3, 4};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));   /* both valid */
        T("VALID+VALID -> NOT_NEEDED",
          Storage_EnsureRedundancy(RECORD_TYPE_CONFIG) == STORAGE_REPAIR_NOT_NEEDED);
    }

    /* ERASED+ERASED -> NOT_FOUND (nothing to mirror). */
    FakeFlash_Init(); Storage_Init();
    {
        T("ERASED+ERASED -> NOT_FOUND",
          Storage_EnsureRedundancy(RECORD_TYPE_CONFIG) == STORAGE_REPAIR_NOT_FOUND);
    }

    /* Valid source survives a failed peer write (verify-fail on peer). */
    FakeFlash_Init(); Storage_Init();
    {
        uint8_t a[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        Storage_Write(RECORD_TYPE_CONFIG, a, sizeof(a));   /* slot A valid */
        FakeFlash_SetVerifyFail(true);                     /* peer mirror write fails verify */
        T("peer verify failure -> REFUSED (source preserved)",
          Storage_EnsureRedundancy(RECORD_TYPE_CONFIG) == STORAGE_REPAIR_REFUSED);
        FakeFlash_SetVerifyFail(false);
        StoragePayload p;
        T("  valid source still readable",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK && memcmp(p.data, a, sizeof(a)) == 0);
    }

    /* Zero-length record: consistent contract. A valid zero-size record must
       be readable, degraded single-copy, mirrorable (DONE), and after mirror
       establishment both slots hold a still-readable size-0 record. This cements
       that Storage_EnsureRedundancy and Storage_WriteEx/Read share the same
       zero-length semantics (no mixed contract). */
    FakeFlash_Init(); Storage_Init();
    {
        T("zero-size write -> OK",
          Storage_WriteEx(RECORD_TYPE_CONFIG, NULL, 0) == STORAGE_WRITE_OK);
        StoragePayload p;
        T("zero-size read -> OK (size 0)",
          Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK && p.size == 0);
        T("zero-size single copy -> health DEGRADED",
          Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_DEGRADED);
        T("zero-size EnsureRedundancy -> DONE",
          Storage_EnsureRedundancy(RECORD_TYPE_CONFIG) == STORAGE_REPAIR_DONE);
        T("zero-size health -> HEALTHY",
          Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_HEALTHY);
        StoragePayload p2;
        T("zero-size still readable after mirror (size 0)",
          Storage_Read(RECORD_TYPE_CONFIG, &p2) == STORAGE_READ_OK && p2.size == 0);
    }
}

int main(void)
{
    printf("Storage Slot-State Host Tests\n");
    fflush(stdout);

    TestMatrix();
    TestHealth();
    TestProgramBounds();
    TestMemoryBoundaries();
    TestRecovery();
    TestReadOnce();
    TestHealthExtended();
    TestWriteFailClosed();
    TestRecoveryHarden();
    TestRecoveryPreserveValid();
    TestWriteClassification();
    TestEnsureRedundancy();
    TestFormatOwnership();
    TestBankValidation();

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}