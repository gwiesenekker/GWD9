#include "draughts.h"
#include "zobrist.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BIT(square) (UINT64_C(1) << (square))

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

static const char *skip_space(const char *text)
{
    while (isspace((unsigned char)*text))
        ++text;
    return text;
}

static bool parse_square(const char **cursor, unsigned *square)
{
    const char *text = *cursor;
    unsigned value = 0;

    if (!isdigit((unsigned char)*text))
        return false;
    while (isdigit((unsigned char)*text)) {
        value = value * 10 + (unsigned)(*text - '0');
        ++text;
    }
    if (value < 1 || value > 50)
        return false;
    *cursor = text;
    *square = value;
    return true;
}

static bool add_piece(DraughtsBoard *board, char colour, bool king,
                      unsigned number, char *error, size_t error_size)
{
    uint64_t bit = BIT(number - 1);
    uint64_t occupied = board->position.white_men |
                        board->position.white_kings |
                        board->position.black_men |
                        board->position.black_kings;
    uint64_t *pieces;

    if ((occupied & bit) != 0)
        return set_error(error, error_size,
                         "square %u occurs more than once", number);
    if (colour == 'W')
        pieces = king ? &board->position.white_kings :
                        &board->position.white_men;
    else
        pieces = king ? &board->position.black_kings :
                        &board->position.black_men;
    *pieces |= bit;
    return true;
}

static bool parse_piece_list(DraughtsBoard *board, const char **cursor,
                             char colour, char *error, size_t error_size)
{
    const char *text = *cursor;

    while (*text != '\0' && *text != ':' && *text != '"' && *text != ']') {
        bool king = false;
        unsigned first, last;

        text = skip_space(text);
        if (*text == 'K' || *text == 'k') {
            king = true;
            ++text;
        }
        if (!parse_square(&text, &first))
            return set_error(error, error_size,
                             "expected a square number in the %c list", colour);
        last = first;
        if (*text == '-') {
            ++text;
            if (!parse_square(&text, &last) || last < first)
                return set_error(error, error_size,
                                 "invalid square range beginning at %u", first);
        }
        for (unsigned number = first; number <= last; ++number)
            if (!add_piece(board, colour, king, number, error, error_size))
                return false;

        text = skip_space(text);
        if (*text != ',')
            break;
        ++text;
    }
    *cursor = text;
    return true;
}

bool draughts_board_from_fen(DraughtsBoard *board, const char *fen,
                            char *error, size_t error_size)
{
    const char *text;
    bool saw_white = false;
    bool saw_black = false;

    if (board == NULL || fen == NULL)
        return set_error(error, error_size, "missing board or FEN string");
    memset(board, 0, sizeof(*board));
    text = skip_space(fen);
    if (strncmp(text, "[FEN", 4) == 0) {
        text = skip_space(text + 4);
        if (*text++ != '"')
            return set_error(error, error_size,
                             "expected a quote after [FEN");
    }
    if (*text == 'W' || *text == 'w')
        board->side_to_move = EGTB_WHITE_TO_MOVE;
    else if (*text == 'B' || *text == 'b')
        board->side_to_move = EGTB_BLACK_TO_MOVE;
    else
        return set_error(error, error_size,
                         "FEN must begin with W or B");
    ++text;

    for (unsigned list = 0; list < 2; ++list) {
        char colour;

        text = skip_space(text);
        if (*text++ != ':')
            return set_error(error, error_size,
                             "expected ':' before a piece list");
        text = skip_space(text);
        colour = (char)toupper((unsigned char)*text++);
        if (colour != 'W' && colour != 'B')
            return set_error(error, error_size,
                             "piece list must begin with W or B");
        if ((colour == 'W' && saw_white) ||
            (colour == 'B' && saw_black))
            return set_error(error, error_size,
                             "duplicate %c piece list", colour);
        saw_white |= colour == 'W';
        saw_black |= colour == 'B';
        if (!parse_piece_list(board, &text, colour, error, error_size))
            return false;
    }
    text = skip_space(text);
    if (*text == '.')
        ++text;
    if (*text == '"') {
        ++text;
        text = skip_space(text);
        if (*text == ']')
            ++text;
    }
    if (*skip_space(text) != '\0')
        return set_error(error, error_size,
                         "unexpected text after the FEN position");
    if (!saw_white || !saw_black)
        return set_error(error, error_size,
                         "FEN needs one white and one black piece list");
    if (!draughts_position_is_valid(&board->position))
        return set_error(error, error_size,
                         "illegal position (overlap or unpromoted man on a promotion row)");
    board->key = draughts_zobrist_hash(&board->position,
                                       board->side_to_move);
    return true;
}

static bool append(char *buffer, size_t size, size_t *used,
                   const char *format, ...)
{
    va_list arguments;
    int written;

    if (*used >= size)
        return false;
    va_start(arguments, format);
    written = vsnprintf(buffer + *used, size - *used, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= size - *used)
        return false;
    *used += (size_t)written;
    return true;
}

static bool append_pieces(char *buffer, size_t size, size_t *used,
                          uint64_t men, uint64_t kings)
{
    bool first = true;

    for (unsigned kind = 0; kind < 2; ++kind) {
        uint64_t pieces = kind == 0 ? men : kings;
        while (pieces != 0) {
            unsigned square = (unsigned)__builtin_ctzll(pieces);
            if (!append(buffer, size, used, "%s%s%u",
                        first ? "" : ",", kind == 0 ? "" : "K",
                        square + 1))
                return false;
            first = false;
            pieces &= pieces - 1;
        }
    }
    return true;
}

bool draughts_board_to_fen(const DraughtsBoard *board, char *buffer,
                          size_t buffer_size)
{
    size_t used = 0;

    if (board == NULL || buffer == NULL || buffer_size == 0)
        return false;
    if (!append(buffer, buffer_size, &used, "%c:W",
                board->side_to_move == EGTB_WHITE_TO_MOVE ? 'W' : 'B') ||
        !append_pieces(buffer, buffer_size, &used,
                       board->position.white_men,
                       board->position.white_kings) ||
        !append(buffer, buffer_size, &used, ":B") ||
        !append_pieces(buffer, buffer_size, &used,
                       board->position.black_men,
                       board->position.black_kings))
        return false;
    return true;
}

bool draughts_board_do_move(DraughtsBoard *board, const DraughtsMove *move,
                            DraughtsBoardUndo *undo)
{
    EgtbSide side;
    EgtbSide opponent;
    uint64_t from_bit;
    uint64_t captured;
    uint64_t key;
    bool moving_king;
    bool resulting_king;

    if (board == NULL || move == NULL || undo == NULL)
        return false;
    side = board->side_to_move;
    opponent = side == EGTB_WHITE_TO_MOVE ? EGTB_BLACK_TO_MOVE :
                                             EGTB_WHITE_TO_MOVE;
    from_bit = BIT(move->from);
    moving_king = side == EGTB_WHITE_TO_MOVE
                      ? (board->position.white_kings & from_bit) != 0
                      : (board->position.black_kings & from_bit) != 0;
    resulting_king = moving_king ||
                     (side == EGTB_WHITE_TO_MOVE ? move->to < 5 :
                                                   move->to >= 45);
    key = board->key ^
          draughts_zobrist_piece(side, moving_king, move->from) ^
          draughts_zobrist_piece(side, resulting_king, move->to) ^
          draughts_zobrist_side();
    captured = move->captured;
    while (captured != 0) {
        unsigned square = (unsigned)__builtin_ctzll(captured);
        uint64_t bit = BIT(square);
        bool king = opponent == EGTB_WHITE_TO_MOVE
                        ? (board->position.white_kings & bit) != 0
                        : (board->position.black_kings & bit) != 0;
        key ^= draughts_zobrist_piece(opponent, king, square);
        captured &= captured - 1;
    }
    undo->side_to_move = board->side_to_move;
    undo->key = board->key;
    if (!draughts_do_move(&board->position, board->side_to_move, move,
                          &undo->position))
        return false;
    board->side_to_move = board->side_to_move == EGTB_WHITE_TO_MOVE
                              ? EGTB_BLACK_TO_MOVE
                              : EGTB_WHITE_TO_MOVE;
    board->key = key;
    return true;
}

void draughts_board_undo_move(DraughtsBoard *board,
                              const DraughtsBoardUndo *undo)
{
    if (board == NULL || undo == NULL)
        return;
    draughts_undo_move(&board->position, &undo->position);
    board->side_to_move = undo->side_to_move;
    board->key = undo->key;
}

uint64_t draughts_board_recompute_key(const DraughtsBoard *board)
{
    if (board == NULL)
        return 0;
    return draughts_zobrist_hash(&board->position, board->side_to_move);
}

static char piece_at(const DraughtsPosition *position, unsigned square)
{
    uint64_t bit = BIT(square);
    if ((position->white_men & bit) != 0)
        return 'w';
    if ((position->white_kings & bit) != 0)
        return 'W';
    if ((position->black_men & bit) != 0)
        return 'b';
    if ((position->black_kings & bit) != 0)
        return 'B';
    return '.';
}

void draughts_print_board(const DraughtsBoard *board)
{
    puts("    a b c d e f g h i j");
    puts("   +---------------------+");
    for (unsigned row = 0; row < 10; ++row) {
        printf("%2u |", 10 - row);
        for (unsigned column = 0; column < 10; ++column) {
            if (((row + column) & 1U) == 0)
                printf("  ");
            else {
                unsigned square = row * 5 + column / 2;
                printf("%c ", piece_at(&board->position, square));
            }
        }
        printf("| %2u\n", 10 - row);
    }
    puts("   +---------------------+");
    puts("    a b c d e f g h i j");
    printf("Side to move: %s\n",
           board->side_to_move == EGTB_WHITE_TO_MOVE ? "White" : "Black");
    puts("Pieces: w/W = white man/king, b/B = black man/king");
}

void draughts_print_move(const DraughtsMove *move)
{
    printf("%u%c%u", move->from + 1,
           move->capture_count == 0 ? '-' : 'x', move->to + 1);
    if (move->capture_count != 0) {
        uint64_t captured = move->captured;
        printf(" (captures");
        while (captured != 0) {
            unsigned square = (unsigned)__builtin_ctzll(captured);
            printf(" %u", square + 1);
            captured &= captured - 1;
        }
        putchar(')');
    }
}
