#include "transposition.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    _Atomic uint64_t key_checksum;
    _Atomic uint64_t data;
} TtSlot;

typedef struct {
    TtSlot slots[GWD_TT_SLOTS];
} TtEntry;

struct GwdTranspositionTable {
    TtEntry *entries;
    size_t entry_count;
    size_t byte_count;
};

typedef struct {
    uint64_t key_checksum;
    uint64_t data;
} EncodedSlot;

static uint64_t checksum64(uint64_t key, uint64_t data)
{
    uint64_t checksum = key;

    checksum ^= data + UINT64_C(0x9e3779b97f4a7c15) +
                (checksum << 6) + (checksum >> 2);
    checksum ^= checksum >> 30;
    checksum *= UINT64_C(0xbf58476d1ce4e5b9);
    checksum ^= checksum >> 27;
    checksum *= UINT64_C(0x94d049bb133111eb);
    return checksum ^ (checksum >> 31);
}

static uint64_t mix_key(uint64_t key)
{
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    key *= UINT64_C(0xc4ceb9fe1a85ec53);
    return key ^ (key >> 33);
}

static uint64_t pack_record(const GwdTtRecord *record)
{
    uint64_t data = (uint16_t)(int16_t)record->score;

    data |= (uint64_t)(record->depth & 0xffU) << 16;
    data |= (uint64_t)(record->bound & 0x3U) << 24;
    if (record->has_move) {
        data |= UINT64_C(1) << 26;
        data |= (uint64_t)(record->from & 0x3fU) << 27;
        data |= (uint64_t)(record->to & 0x3fU) << 33;
        data |= (uint64_t)(record->capture_count & 0x3fU) << 39;
    }
    return data;
}

static bool unpack_record(uint64_t data, GwdTtRecord *record)
{
    unsigned bound = (unsigned)((data >> 24) & 0x3U);

    if (bound < GWD_TT_EXACT || bound > GWD_TT_UPPER_BOUND)
        return false;
    record->score = (int)(int16_t)(uint16_t)data;
    record->depth = (unsigned)((data >> 16) & 0xffU);
    record->bound = (GwdTtBound)bound;
    record->has_move = ((data >> 26) & 1U) != 0;
    record->from = (unsigned)((data >> 27) & 0x3fU);
    record->to = (unsigned)((data >> 33) & 0x3fU);
    record->capture_count = (unsigned)((data >> 39) & 0x3fU);
    return !record->has_move ||
           (record->from < 50 && record->to < 50 &&
            record->capture_count <= 20);
}

static EncodedSlot encode_slot(uint64_t board_key,
                               const GwdTtRecord *record)
{
    EncodedSlot encoded;
    uint64_t data = pack_record(record);
    uint64_t checksum = checksum64(board_key, data);

    encoded.key_checksum = board_key ^ checksum;
    encoded.data = data ^ board_key;
    return encoded;
}

static bool decode_slot(const TtSlot *slot, uint64_t board_key,
                        GwdTtRecord *record)
{
    EncodedSlot local;
    uint64_t confirm;
    uint64_t checksum;
    uint64_t data;

    local.key_checksum =
        atomic_load_explicit(&slot->key_checksum, memory_order_acquire);
    local.data = atomic_load_explicit(&slot->data, memory_order_relaxed);
    confirm = atomic_load_explicit(&slot->key_checksum, memory_order_acquire);
    if (local.key_checksum != confirm)
        return false;
    checksum = local.key_checksum ^ board_key;
    data = local.data ^ board_key;
    if (checksum64(board_key, data) != checksum)
        return false;
    return unpack_record(data, record);
}

bool gwd_tt_create(GwdTranspositionTable **out, size_t megabytes,
                   char *error, size_t error_size)
{
    GwdTranspositionTable *table;
    size_t requested;
    size_t entries = 1;

    if (out == NULL || megabytes == 0 ||
        megabytes > SIZE_MAX / (1024U * 1024U))
        return false;
    *out = NULL;
    requested = megabytes * 1024U * 1024U;
    while (entries <= requested / sizeof(TtEntry) / 2)
        entries *= 2;
    table = calloc(1, sizeof(*table));
    if (table != NULL)
        table->entries = calloc(entries, sizeof(*table->entries));
    if (table == NULL || table->entries == NULL) {
        free(table);
        if (error != NULL && error_size != 0)
            snprintf(error, error_size,
                     "cannot allocate %zu MiB transposition table", megabytes);
        return false;
    }
    if (!atomic_is_lock_free(&table->entries[0].slots[0].key_checksum) ||
        !atomic_is_lock_free(&table->entries[0].slots[0].data)) {
        free(table->entries);
        free(table);
        if (error != NULL && error_size != 0)
            snprintf(error, error_size,
                     "64-bit atomic operations are not lock-free");
        return false;
    }
    for (size_t entry = 0; entry < entries; ++entry)
        for (unsigned slot = 0; slot < GWD_TT_SLOTS; ++slot) {
            atomic_init(&table->entries[entry].slots[slot].key_checksum, 0);
            atomic_init(&table->entries[entry].slots[slot].data, 0);
        }
    table->entry_count = entries;
    table->byte_count = entries * sizeof(*table->entries);
    *out = table;
    return true;
}

void gwd_tt_destroy(GwdTranspositionTable *table)
{
    if (table == NULL)
        return;
    free(table->entries);
    free(table);
}

bool gwd_tt_probe(const GwdTranspositionTable *table, uint64_t board_key,
                  GwdTtRecord *record)
{
    const TtEntry *entry;

    if (table == NULL || record == NULL)
        return false;
    entry = &table->entries[mix_key(board_key) & (table->entry_count - 1)];
    for (unsigned slot = 0; slot < GWD_TT_SLOTS; ++slot)
        if (decode_slot(&entry->slots[slot], board_key, record))
            return true;
    return false;
}

void gwd_tt_store(GwdTranspositionTable *table, uint64_t board_key,
                  const GwdTtRecord *record)
{
    uint64_t mixed;
    TtEntry *entry;
    unsigned replacement;
    EncodedSlot local;
    GwdTtRecord old;

    if (table == NULL || record == NULL)
        return;
    mixed = mix_key(board_key);
    entry = &table->entries[mixed & (table->entry_count - 1)];
    replacement = (unsigned)((mixed >> 32) & (GWD_TT_SLOTS - 1));
    for (unsigned slot = 0; slot < GWD_TT_SLOTS; ++slot)
        if (decode_slot(&entry->slots[slot], board_key, &old)) {
            replacement = slot;
            break;
        }
    local = encode_slot(board_key, record);
    atomic_store_explicit(&entry->slots[replacement].data, local.data,
                          memory_order_relaxed);
    atomic_store_explicit(&entry->slots[replacement].key_checksum,
                          local.key_checksum, memory_order_release);
}

size_t gwd_tt_entries(const GwdTranspositionTable *table)
{
    return table == NULL ? 0 : table->entry_count;
}

size_t gwd_tt_bytes(const GwdTranspositionTable *table)
{
    return table == NULL ? 0 : table->byte_count;
}
