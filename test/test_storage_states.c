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

    T("explicit record recovery succeeds",
      Storage_RecoverRecord(RECORD_TYPE_REGISTRATION, reg, sizeof(reg)));

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

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}