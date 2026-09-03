#include "game.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned from;
    unsigned to;
    bool capture;
} ParsedMove;

typedef struct {
    ParsedMove wanted;
    DraughtsMove match;
    bool found;
    bool ambiguous;
} MoveMatch;

static bool set_error(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;

    if (buffer != NULL && size != 0) {
        va_start(arguments, format);
        vsnprintf(buffer, size, format, arguments);
        va_end(arguments);
    }
    return false;
}

static bool same_move(const DraughtsMove *left, const DraughtsMove *right)
{
    return left->from == right->from && left->to == right->to &&
           left->captured == right->captured &&
           left->capture_count == right->capture_count;
}

static bool parse_number(const char **cursor, unsigned *number)
{
    unsigned value = 0;
    const char *text = *cursor;

    if (!isdigit((unsigned char)*text))
        return false;
    while (isdigit((unsigned char)*text)) {
        value = value * 10 + (unsigned)(*text - '0');
        ++text;
    }
    if (value < 1 || value > 50)
        return false;
    *cursor = text;
    *number = value;
    return true;
}

static bool parse_move_text(const char *text, ParsedMove *move)
{
    char separator;

    while (isspace((unsigned char)*text))
        ++text;
    if (!parse_number(&text, &move->from))
        return false;
    separator = *text++;
    if (separator != '-' && separator != 'x' && separator != 'X')
        return false;
    move->capture = separator != '-';
    if (!parse_number(&text, &move->to))
        return false;
    while (isspace((unsigned char)*text))
        ++text;
    return *text == '\0';
}

static bool find_move(const DraughtsMove *move, void *context)
{
    MoveMatch *match = context;

    if ((unsigned)move->from + 1 != match->wanted.from ||
        (unsigned)move->to + 1 != match->wanted.to ||
        (move->capture_count != 0) != match->wanted.capture)
        return true;
    if (!match->found) {
        match->match = *move;
        match->found = true;
    } else if (!same_move(&match->match, move)) {
        match->ambiguous = true;
    }
    return true;
}

static char *copy_text(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);

    if (copy != NULL)
        memcpy(copy, text, length);
    return copy;
}

static bool reserve_ply(DraughtsGame *game, char *error, size_t error_size)
{
    DraughtsGamePly *grown;
    size_t capacity;

    if (game->ply_count < game->ply_capacity)
        return true;
    capacity = game->ply_capacity == 0 ? 64 : game->ply_capacity * 2;
    if (capacity < game->ply_capacity ||
        capacity > SIZE_MAX / sizeof(*game->plies))
        return set_error(error, error_size, "game is too long");
    grown = realloc(game->plies, capacity * sizeof(*game->plies));
    if (grown == NULL)
        return set_error(error, error_size, "cannot grow the game move list");
    game->plies = grown;
    game->ply_capacity = capacity;
    return true;
}

bool draughts_game_init(DraughtsGame *game, const char *fen,
                        char *error, size_t error_size)
{
    char canonical[512];

    if (game == NULL)
        return set_error(error, error_size, "missing game");
    memset(game, 0, sizeof(*game));
    if (!draughts_board_from_fen(&game->root_board, fen, error, error_size))
        return false;
    if (!draughts_board_to_fen(&game->root_board, canonical,
                               sizeof(canonical)))
        return set_error(error, error_size, "cannot format starting FEN");
    game->starting_fen = copy_text(canonical);
    if (game->starting_fen == NULL)
        return set_error(error, error_size, "cannot store starting FEN");
    game->board = game->root_board;
    return true;
}

void draughts_game_destroy(DraughtsGame *game)
{
    if (game == NULL)
        return;
    for (size_t ply = 0; ply < game->ply_count; ++ply)
        free(game->plies[ply].text);
    free(game->plies);
    free(game->starting_fen);
    memset(game, 0, sizeof(*game));
}

bool draughts_game_push(DraughtsGame *game, const char *move_text,
                        char *error, size_t error_size)
{
    MoveMatch match = {0};
    DraughtsGamePly *ply;
    size_t generated;
    char canonical[16];

    if (game == NULL || move_text == NULL)
        return set_error(error, error_size, "missing game or move");
    if (!parse_move_text(move_text, &match.wanted))
        return set_error(error, error_size,
                         "invalid move '%s' (expected 31-26 or 32x21)",
                         move_text);
    if (!draughts_generate_moves(&game->board.position,
                                 game->board.side_to_move, find_move, &match,
                                 &generated))
        return set_error(error, error_size, "move generation failed: %s",
                         draughts_movegen_last_error());
    (void)generated;
    if (!match.found)
        return set_error(error, error_size, "move '%s' is not legal",
                         move_text);
    if (match.ambiguous)
        return set_error(error, error_size,
                         "move '%s' is ambiguous in this position", move_text);
    if (!reserve_ply(game, error, error_size))
        return false;
    snprintf(canonical, sizeof(canonical), "%u%c%u", match.match.from + 1,
             match.match.capture_count == 0 ? '-' : 'x', match.match.to + 1);
    ply = &game->plies[game->ply_count];
    ply->text = copy_text(canonical);
    if (ply->text == NULL)
        return set_error(error, error_size, "cannot store move '%s'",
                         canonical);
    ply->move = match.match;
    if (!draughts_board_do_move(&game->board, &ply->move, &ply->undo)) {
        free(ply->text);
        ply->text = NULL;
        return set_error(error, error_size, "cannot play move '%s'", canonical);
    }
    ++game->ply_count;
    return true;
}

bool draughts_game_pop(DraughtsGame *game)
{
    DraughtsGamePly *ply;

    if (game == NULL || game->ply_count == 0)
        return false;
    ply = &game->plies[--game->ply_count];
    draughts_board_undo_move(&game->board, &ply->undo);
    free(ply->text);
    memset(ply, 0, sizeof(*ply));
    return true;
}

bool draughts_game_replay(DraughtsGame *game, const char *fen,
                          const char *const *moves, size_t move_count,
                          char *error, size_t error_size)
{
    DraughtsGame replay;

    if (!draughts_game_init(&replay, fen, error, error_size))
        return false;
    for (size_t ply = 0; ply < move_count; ++ply) {
        if (!draughts_game_push(&replay, moves[ply], error, error_size)) {
            char detail[256];
            snprintf(detail, sizeof(detail), "ply %zu: %s", ply + 1,
                     error != NULL ? error : "invalid move");
            draughts_game_destroy(&replay);
            return set_error(error, error_size, "%s", detail);
        }
    }
    draughts_game_destroy(game);
    *game = replay;
    return true;
}

size_t draughts_game_ply(const DraughtsGame *game)
{
    return game == NULL ? 0 : game->ply_count;
}

const char *draughts_game_move(const DraughtsGame *game, size_t ply)
{
    if (game == NULL || ply >= game->ply_count)
        return NULL;
    return game->plies[ply].text;
}
