#include "draughts.h"
#include "game.h"
#include "search.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const DraughtsBoard *original;
    size_t checked;
    bool failed;
} RoundTrip;

static bool same_board(const DraughtsBoard *left, const DraughtsBoard *right)
{
    return left->side_to_move == right->side_to_move &&
           left->key == right->key &&
           left->position.white_men == right->position.white_men &&
           left->position.white_kings == right->position.white_kings &&
           left->position.black_men == right->position.black_men &&
           left->position.black_kings == right->position.black_kings;
}

static bool check_move_round_trip(const DraughtsMove *move, void *context)
{
    RoundTrip *test = context;
    DraughtsBoard changed = *test->original;
    DraughtsBoardUndo undo;
    EgtbSide expected_side = changed.side_to_move == EGTB_WHITE_TO_MOVE
                                 ? EGTB_BLACK_TO_MOVE
                                 : EGTB_WHITE_TO_MOVE;

    if (!draughts_board_do_move(&changed, move, &undo) ||
        changed.side_to_move != expected_side ||
        !draughts_position_is_valid(&changed.position) ||
        changed.key != draughts_board_recompute_key(&changed)) {
        test->failed = true;
        return true;
    }
    draughts_board_undo_move(&changed, &undo);
    if (!same_board(&changed, test->original))
        test->failed = true;
    ++test->checked;
    return true;
}

static bool check_position(const char *fen)
{
    DraughtsBoard board;
    RoundTrip test;
    char error[256];
    size_t generated = 0;

    if (!draughts_board_from_fen(&board, fen, error, sizeof(error))) {
        fprintf(stderr, "test FEN failed: %s: %s\n", fen, error);
        return false;
    }
    if (board.key != draughts_board_recompute_key(&board)) {
        fprintf(stderr, "initial Zobrist key failed for %s\n", fen);
        return false;
    }
    test.original = &board;
    test.checked = 0;
    test.failed = false;
    if (!draughts_generate_moves(&board.position, board.side_to_move,
                                 check_move_round_trip, &test, &generated)) {
        fprintf(stderr, "move generation failed: %s\n",
                draughts_movegen_last_error());
        return false;
    }
    if (test.failed || test.checked != generated) {
        fprintf(stderr, "do/undo round trip failed for %s\n", fen);
        return false;
    }
    return true;
}

static bool check_promotion(void)
{
    DraughtsBoard board;
    DraughtsBoardUndo undo;
    DraughtsMove move = {0, 5, 0, 0};
    char error[256];

    if (!draughts_board_from_fen(&board, "W:W6:B", error, sizeof(error)) ||
        !draughts_board_do_move(&board, &move, &undo) ||
        board.position.white_men != 0 ||
        board.position.white_kings != UINT64_C(1) ||
        board.side_to_move != EGTB_BLACK_TO_MOVE ||
        board.key != draughts_board_recompute_key(&board))
        return false;
    draughts_board_undo_move(&board, &undo);
    return board.position.white_men == (UINT64_C(1) << 5) &&
           board.position.white_kings == 0 &&
           board.side_to_move == EGTB_WHITE_TO_MOVE;
}

static bool check_zobrist_side(void)
{
    DraughtsBoard white;
    DraughtsBoard black;
    char error[256];

    return draughts_board_from_fen(&white, "W:WK10:BK41", error,
                                   sizeof(error)) &&
           draughts_board_from_fen(&black, "B:WK10:BK41", error,
                                   sizeof(error)) &&
           white.key != black.key;
}

static bool check_game(void)
{
    static const char *moves[] = {"31-26", "16-21"};
    DraughtsGame game = {0};
    DraughtsBoard after_first;
    char error[256];

    if (!draughts_game_replay(&game, "W:W31-50:B1-20", moves, 2,
                              error, sizeof(error)) ||
        draughts_game_ply(&game) != 2 ||
        strcmp(draughts_game_move(&game, 0), "31-26") != 0 ||
        strcmp(draughts_game_move(&game, 1), "16-21") != 0) {
        fprintf(stderr, "game replay failed: %s\n", error);
        draughts_game_destroy(&game);
        return false;
    }
    if (!draughts_game_pop(&game) || draughts_game_ply(&game) != 1) {
        fputs("game pop failed\n", stderr);
        draughts_game_destroy(&game);
        return false;
    }
    after_first = game.board;
    if (!draughts_game_push(&game, "16-21", error, sizeof(error)) ||
        !draughts_game_pop(&game) || !same_board(&game.board, &after_first) ||
        draughts_game_push(&game, "17-12", error, sizeof(error))) {
        fputs("game push validation or round trip failed\n", stderr);
        draughts_game_destroy(&game);
        return false;
    }
    draughts_game_destroy(&game);
    return true;
}

typedef struct {
    unsigned count;
    unsigned last_depth;
    bool failed;
} IterationCheck;

static void check_iteration(const GwdSearchResult *result, void *context)
{
    IterationCheck *check = context;

    if (result->depth <= check->last_depth ||
        result->pv_length > result->depth)
        check->failed = true;
    check->last_depth = result->depth;
    ++check->count;
}

static bool check_search(void)
{
    DraughtsGame game = {0};
    GwdSearchControl control;
    GwdSearchResult result;
    GwdTranspositionTable *tt = NULL;
    IterationCheck iterations = {0};
    char error[256];

    if (!gwd_tt_create(&tt, 1, error, sizeof(error)))
        return false;
    if (!draughts_game_init(&game, "W:W32:B17,27", error, sizeof(error))) {
        gwd_tt_destroy(tt);
        return false;
    }
    gwd_search_control_init(&control);
    if (!gwd_search(&game, 0.05, &control, true, tt,
                    check_iteration, &iterations, &result,
                    error, sizeof(error)) ||
        result.depth == 0 || !result.has_best_move ||
        result.pv_length == 0 ||
        result.best_move.from != 31 || result.best_move.to != 11 ||
        result.best_move.capture_count != 2 || result.tt_probes == 0 ||
        result.tt_stores == 0 || iterations.count == 0 ||
        iterations.failed || iterations.last_depth != result.depth) {
        fprintf(stderr, "forced-move search failed: %s\n", error);
        draughts_game_destroy(&game);
        gwd_tt_destroy(tt);
        return false;
    }
    draughts_game_destroy(&game);

    if (!draughts_game_init(&game, "W:W:BK1", error, sizeof(error))) {
        gwd_tt_destroy(tt);
        return false;
    }
    memset(&iterations, 0, sizeof(iterations));
    gwd_search_control_init(&control);
    if (!gwd_search(&game, 0.05, &control, true, tt,
                    check_iteration, &iterations, &result,
                    error, sizeof(error)) ||
        result.depth != 1 || result.has_best_move ||
        result.pv_length != 0 ||
        result.score != -GWD_SCORE_WIN_ABSOLUTE || iterations.count != 1 ||
        iterations.failed) {
        fprintf(stderr, "terminal search failed: %s\n", error);
        draughts_game_destroy(&game);
        gwd_tt_destroy(tt);
        return false;
    }
    draughts_game_destroy(&game);
    gwd_tt_destroy(tt);
    return true;
}

static bool check_transposition_table(void)
{
    GwdTranspositionTable *table = NULL;
    GwdTtRecord stored = {-123, 17, GWD_TT_LOWER_BOUND,
                          true, 31, 11, 2};
    GwdTtRecord probed;
    char error[256];
    uint64_t key = UINT64_C(0x123456789abcdef0);

    if (!gwd_tt_create(&table, 1, error, sizeof(error)) ||
        gwd_tt_entries(table) == 0 || gwd_tt_bytes(table) == 0 ||
        gwd_tt_probe(table, key, &probed)) {
        gwd_tt_destroy(table);
        return false;
    }
    gwd_tt_store(table, key, &stored);
    if (!gwd_tt_probe(table, key, &probed) ||
        probed.score != stored.score || probed.depth != stored.depth ||
        probed.bound != stored.bound ||
        probed.has_move != stored.has_move || probed.from != stored.from ||
        probed.to != stored.to ||
        probed.capture_count != stored.capture_count ||
        gwd_tt_probe(table, key ^ UINT64_C(1), &probed)) {
        gwd_tt_destroy(table);
        return false;
    }
    gwd_tt_destroy(table);
    return true;
}

int main(void)
{
    static const char *positions[] = {
        "W:W31-50:B1-20",
        "B:W31-50:B1-20",
        "W:W32:B17,27",
        "W:WK36:B22",
        "B:W29,34:BK12,18",
    };

    for (size_t index = 0; index < sizeof(positions) / sizeof(positions[0]);
         ++index)
        if (!check_position(positions[index]))
            return 1;
    if (!check_promotion()) {
        fputs("promotion do/undo test failed\n", stderr);
        return 1;
    }
    if (!check_zobrist_side()) {
        fputs("Zobrist side-to-move test failed\n", stderr);
        return 1;
    }
    if (!check_game())
        return 1;
    if (!check_search())
        return 1;
    if (!check_transposition_table()) {
        fputs("transposition-table test failed\n", stderr);
        return 1;
    }
    puts("Game and search tests passed.");
    return 0;
}
