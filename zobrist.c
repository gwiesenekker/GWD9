#include "zobrist.h"

#include <pthread.h>
#include <stdint.h>

static uint64_t piece_keys[2][2][50];
static uint64_t side_key;
static pthread_once_t zobrist_once = PTHREAD_ONCE_INIT;

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));

    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void initialize_zobrist(void)
{
    uint64_t state = UINT64_C(0x475744392d5a4f42); /* "GWD9-ZOB" */

    for (unsigned side = 0; side < 2; ++side)
        for (unsigned king = 0; king < 2; ++king)
            for (unsigned square = 0; square < 50; ++square)
                piece_keys[side][king][square] = splitmix64(&state);
    side_key = splitmix64(&state);
}

uint64_t draughts_zobrist_piece(EgtbSide side, bool king, unsigned square)
{
    pthread_once(&zobrist_once, initialize_zobrist);
    if ((side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE) ||
        square >= 50)
        return 0;
    return piece_keys[side][king ? 1 : 0][square];
}

uint64_t draughts_zobrist_side(void)
{
    pthread_once(&zobrist_once, initialize_zobrist);
    return side_key;
}

static uint64_t hash_pieces(uint64_t pieces, EgtbSide side, bool king)
{
    uint64_t hash = 0;

    while (pieces != 0) {
        unsigned square = (unsigned)__builtin_ctzll(pieces);
        hash ^= draughts_zobrist_piece(side, king, square);
        pieces &= pieces - 1;
    }
    return hash;
}

uint64_t draughts_zobrist_hash(const DraughtsPosition *position,
                               EgtbSide side_to_move)
{
    uint64_t hash;

    if (position == NULL)
        return 0;
    hash = hash_pieces(position->white_men, EGTB_WHITE_TO_MOVE, false) ^
           hash_pieces(position->white_kings, EGTB_WHITE_TO_MOVE, true) ^
           hash_pieces(position->black_men, EGTB_BLACK_TO_MOVE, false) ^
           hash_pieces(position->black_kings, EGTB_BLACK_TO_MOVE, true);
    if (side_to_move == EGTB_BLACK_TO_MOVE)
        hash ^= draughts_zobrist_side();
    return hash;
}
