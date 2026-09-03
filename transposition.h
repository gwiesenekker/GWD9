#ifndef GWD9_TRANSPOSITION_H
#define GWD9_TRANSPOSITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GWD_TT_SLOTS 4

typedef enum {
    GWD_TT_EXACT = 1,
    GWD_TT_LOWER_BOUND = 2,
    GWD_TT_UPPER_BOUND = 3
} GwdTtBound;

typedef struct {
    int score;
    unsigned depth;
    GwdTtBound bound;
    bool has_move;
    unsigned from;
    unsigned to;
    unsigned capture_count;
} GwdTtRecord;

typedef struct GwdTranspositionTable GwdTranspositionTable;

bool gwd_tt_create(GwdTranspositionTable **table, size_t megabytes,
                   char *error, size_t error_size);
void gwd_tt_destroy(GwdTranspositionTable *table);

bool gwd_tt_probe(const GwdTranspositionTable *table, uint64_t board_key,
                  GwdTtRecord *record);
void gwd_tt_store(GwdTranspositionTable *table, uint64_t board_key,
                  const GwdTtRecord *record);

size_t gwd_tt_entries(const GwdTranspositionTable *table);
size_t gwd_tt_bytes(const GwdTranspositionTable *table);

#endif
