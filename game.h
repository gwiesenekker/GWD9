#ifndef GWD9_GAME_H
#define GWD9_GAME_H

#include "draughts.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *text;
    DraughtsMove move;
    DraughtsBoardUndo undo;
} DraughtsGamePly;

typedef struct {
    char *starting_fen;
    DraughtsBoard root_board;
    DraughtsBoard board;
    DraughtsGamePly *plies;
    size_t ply_count;
    size_t ply_capacity;
} DraughtsGame;

bool draughts_game_init(DraughtsGame *game, const char *fen,
                        char *error, size_t error_size);
void draughts_game_destroy(DraughtsGame *game);

/* Move text uses PDN numeric notation, for example 31-26 or 32x21. */
bool draughts_game_push(DraughtsGame *game, const char *move_text,
                        char *error, size_t error_size);
bool draughts_game_pop(DraughtsGame *game);

bool draughts_game_replay(DraughtsGame *game, const char *fen,
                          const char *const *moves, size_t move_count,
                          char *error, size_t error_size);

size_t draughts_game_ply(const DraughtsGame *game);
const char *draughts_game_move(const DraughtsGame *game, size_t ply);

#endif
