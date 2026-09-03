#ifndef MOVEGEN_H
#define MOVEGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRAUGHTS_BOARD_MASK ((UINT64_C(1) << 50) - 1)

typedef enum {
    EGTB_WHITE_TO_MOVE = 0,
    EGTB_BLACK_TO_MOVE = 1
} EgtbSide;

typedef struct {
    uint64_t white_men;
    uint64_t black_men;
    uint64_t white_kings;
    uint64_t black_kings;
} DraughtsPosition;

typedef struct {
    uint64_t captured;
    uint8_t from;
    uint8_t to;
    uint8_t capture_count;
} DraughtsMove;

typedef struct {
    uint64_t white_men;
    uint64_t black_men;
    uint64_t white_kings;
    uint64_t black_kings;
} DraughtsUndo;

typedef bool (*DraughtsMoveVisitor)(const DraughtsMove *move, void *context);

const char *draughts_movegen_last_error(void);
bool draughts_position_is_valid(const DraughtsPosition *position);
bool draughts_generate_moves(const DraughtsPosition *position, EgtbSide side,
                             DraughtsMoveVisitor visitor, void *context,
                             size_t *move_count);
bool draughts_has_capture(const DraughtsPosition *position, EgtbSide side);
bool draughts_do_move(DraughtsPosition *position, EgtbSide side,
                      const DraughtsMove *move, DraughtsUndo *undo);
void draughts_undo_move(DraughtsPosition *position, const DraughtsUndo *undo);

#endif
