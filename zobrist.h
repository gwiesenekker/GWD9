#ifndef GWD9_ZOBRIST_H
#define GWD9_ZOBRIST_H

#include "movegen.h"

#include <stdbool.h>
#include <stdint.h>

/* Deterministic keys: hashes remain stable across runs and thread counts. */
uint64_t draughts_zobrist_piece(EgtbSide side, bool king, unsigned square);
uint64_t draughts_zobrist_side(void);
uint64_t draughts_zobrist_hash(const DraughtsPosition *position,
                               EgtbSide side_to_move);

#endif
