#ifndef GWD9_DRAUGHTS_H
#define GWD9_DRAUGHTS_H

#include "movegen.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    DraughtsPosition position;
    EgtbSide side_to_move;
    uint64_t key;
} DraughtsBoard;

typedef struct {
    DraughtsUndo position;
    EgtbSide side_to_move;
    uint64_t key;
} DraughtsBoardUndo;

/* Parse and format portable draughts notation (PDN) FEN. */
bool draughts_board_from_fen(DraughtsBoard *board, const char *fen,
                            char *error, size_t error_size);
bool draughts_board_to_fen(const DraughtsBoard *board, char *buffer,
                          size_t buffer_size);

/* Apply a generated legal move, switch sides, and save exact undo state. */
bool draughts_board_do_move(DraughtsBoard *board, const DraughtsMove *move,
                            DraughtsBoardUndo *undo);
void draughts_board_undo_move(DraughtsBoard *board,
                              const DraughtsBoardUndo *undo);
uint64_t draughts_board_recompute_key(const DraughtsBoard *board);

void draughts_print_board(const DraughtsBoard *board);
void draughts_print_move(const DraughtsMove *move);

#endif
