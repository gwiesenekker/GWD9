#define _POSIX_C_SOURCE 200809L

#include "search.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    DraughtsMove *moves;
    size_t count;
    size_t capacity;
    bool failed;
} MoveVector;

#define PV_BUCKETS 64
#define PV_SLOTS 4

typedef struct {
    uint64_t key;
    DraughtsMove move;
    bool valid;
} PvSlot;

typedef struct {
    PvSlot slots[PV_SLOTS];
} PvBucket;

typedef struct {
    PvBucket buckets[PV_BUCKETS];
} PvTable;

typedef struct {
    DraughtsMove moves[GWD_PV_MAX];
    size_t length;
} PvLine;

typedef struct {
    DraughtsBoard board;
    size_t game_ply;
    unsigned search_ply;
    uint64_t nodes;
    struct timespec started;
    struct timespec deadline;
    GwdSearchControl *control;
    bool master;
    bool stopped;
    bool failed;
    PvTable *pv;
    GwdTranspositionTable *tt;
    uint64_t tt_probes;
    uint64_t tt_hits;
    uint64_t tt_cutoffs;
    uint64_t tt_stores;
} SearchContext;

static bool generate_moves(SearchContext *search, MoveVector *vector);

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

static int compare_time(const struct timespec *left,
                        const struct timespec *right)
{
    if (left->tv_sec != right->tv_sec)
        return left->tv_sec < right->tv_sec ? -1 : 1;
    if (left->tv_nsec != right->tv_nsec)
        return left->tv_nsec < right->tv_nsec ? -1 : 1;
    return 0;
}

static double elapsed(const struct timespec *start,
                      const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static bool should_stop(SearchContext *search)
{
    struct timespec now;

    if (atomic_load_explicit(&search->control->stop_requested,
                             memory_order_acquire)) {
        search->stopped = true;
        return true;
    }
    if (!search->master)
        return false;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (compare_time(&now, &search->deadline) < 0)
        return false;
    atomic_store_explicit(&search->control->stop_requested, true,
                          memory_order_release);
    search->stopped = true;
    return true;
}

static bool collect_move(const DraughtsMove *move, void *context)
{
    MoveVector *vector = context;
    DraughtsMove *grown;
    size_t capacity;

    if (vector->count == vector->capacity) {
        capacity = vector->capacity == 0 ? 32 : vector->capacity * 2;
        if (capacity < vector->capacity ||
            capacity > SIZE_MAX / sizeof(*vector->moves)) {
            vector->failed = true;
            return false;
        }
        grown = realloc(vector->moves, capacity * sizeof(*vector->moves));
        if (grown == NULL) {
            vector->failed = true;
            return false;
        }
        vector->moves = grown;
        vector->capacity = capacity;
    }
    vector->moves[vector->count++] = *move;
    return true;
}

static bool same_move(const DraughtsMove *left, const DraughtsMove *right)
{
    return left->from == right->from && left->to == right->to &&
           left->captured == right->captured &&
           left->capture_count == right->capture_count;
}

static uint64_t mix_key(uint64_t key)
{
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    key *= UINT64_C(0xc4ceb9fe1a85ec53);
    return key ^ (key >> 33);
}

static const DraughtsMove *pv_probe(const PvTable *table, uint64_t key)
{
    uint64_t mixed = mix_key(key);
    const PvBucket *bucket = &table->buckets[mixed & (PV_BUCKETS - 1)];

    for (unsigned slot = 0; slot < PV_SLOTS; ++slot)
        if (bucket->slots[slot].valid && bucket->slots[slot].key == key)
            return &bucket->slots[slot].move;
    return NULL;
}

static void pv_store(PvTable *table, uint64_t key, const DraughtsMove *move)
{
    uint64_t mixed = mix_key(key);
    PvBucket *bucket = &table->buckets[mixed & (PV_BUCKETS - 1)];
    unsigned replacement = (unsigned)((mixed >> 6) & (PV_SLOTS - 1));

    for (unsigned slot = 0; slot < PV_SLOTS; ++slot) {
        if (bucket->slots[slot].valid && bucket->slots[slot].key == key) {
            replacement = slot;
            break;
        }
        if (!bucket->slots[slot].valid) {
            replacement = slot;
            break;
        }
    }
    bucket->slots[replacement].key = key;
    bucket->slots[replacement].move = *move;
    bucket->slots[replacement].valid = true;
}

static void order_pv_move(const PvTable *table, uint64_t key,
                          MoveVector *vector)
{
    const DraughtsMove *pv_move = pv_probe(table, key);

    if (pv_move == NULL)
        return;
    for (size_t index = 0; index < vector->count; ++index) {
        if (same_move(pv_move, &vector->moves[index])) {
            DraughtsMove first = vector->moves[0];
            vector->moves[0] = vector->moves[index];
            vector->moves[index] = first;
            return;
        }
    }
}

static size_t find_tt_move(const GwdTtRecord *record,
                           const MoveVector *vector)
{
    if (!record->has_move)
        return SIZE_MAX;
    for (size_t index = 0; index < vector->count; ++index) {
        const DraughtsMove *move = &vector->moves[index];
        if (move->from == record->from && move->to == record->to &&
            move->capture_count == record->capture_count)
            return index;
    }
    return SIZE_MAX;
}

static void move_to_front(MoveVector *vector, size_t index)
{
    DraughtsMove first;

    if (index == SIZE_MAX || index == 0 || index >= vector->count)
        return;
    first = vector->moves[0];
    vector->moves[0] = vector->moves[index];
    vector->moves[index] = first;
}

static void follow_tt_line(GwdTranspositionTable *table,
                           const DraughtsBoard *start,
                           const GwdTtRecord *first, unsigned depth,
                           PvLine *line)
{
    DraughtsBoard board = *start;
    GwdTtRecord record = *first;

    line->length = 0;
    while (line->length < GWD_PV_MAX && line->length < depth) {
        SearchContext probe = {0};
        MoveVector vector;
        size_t move_index;
        DraughtsBoardUndo undo;

        probe.board = board;
        if (!generate_moves(&probe, &vector))
            break;
        move_index = find_tt_move(&record, &vector);
        if (move_index == SIZE_MAX) {
            free(vector.moves);
            break;
        }
        line->moves[line->length++] = vector.moves[move_index];
        if (!draughts_board_do_move(&board, &vector.moves[move_index], &undo)) {
            free(vector.moves);
            break;
        }
        free(vector.moves);
        if (!gwd_tt_probe(table, board.key, &record))
            break;
    }
}

static bool generate_moves(SearchContext *search, MoveVector *vector)
{
    size_t generated;

    memset(vector, 0, sizeof(*vector));
    if (!draughts_generate_moves(&search->board.position,
                                 search->board.side_to_move, collect_move,
                                 vector, &generated)) {
        search->failed = true;
        free(vector->moves);
        vector->moves = NULL;
        return false;
    }
    if (generated != vector->count) {
        search->failed = true;
        free(vector->moves);
        vector->moves = NULL;
        return false;
    }
    return true;
}

static unsigned bit_count(uint64_t bits)
{
    return (unsigned)__builtin_popcountll(bits);
}

static int evaluate(const SearchContext *search)
{
    int white = (int)bit_count(search->board.position.white_men) *
                    GWD_SCORE_MAN +
                (int)bit_count(search->board.position.white_kings) *
                    GWD_SCORE_KING;
    int black = (int)bit_count(search->board.position.black_men) *
                    GWD_SCORE_MAN +
                (int)bit_count(search->board.position.black_kings) *
                    GWD_SCORE_KING;
    int score = white - black;

    return search->board.side_to_move == EGTB_WHITE_TO_MOVE ? score : -score;
}

static int terminal_loss(const SearchContext *search)
{
    size_t absolute_ply = search->game_ply + search->search_ply;

    if (absolute_ply > (size_t)(GWD_SCORE_WIN_ABSOLUTE - 1))
        absolute_ply = GWD_SCORE_WIN_ABSOLUTE - 1;
    return -GWD_SCORE_WIN_ABSOLUTE + (int)absolute_ply;
}

static bool search_do_move(SearchContext *search, const DraughtsMove *move,
                           DraughtsBoardUndo *undo)
{
    if (!draughts_board_do_move(&search->board, move, undo)) {
        search->failed = true;
        return false;
    }
    ++search->search_ply;
    return true;
}

static void search_undo_move(SearchContext *search,
                             const DraughtsBoardUndo *undo)
{
    draughts_board_undo_move(&search->board, undo);
    --search->search_ply;
}

static int alpha_beta(SearchContext *search, unsigned depth,
                      int alpha, int beta, DraughtsMove *best_move,
                      PvLine *pv_line)
{
    MoveVector vector;
    GwdTtRecord cached;
    size_t cached_move = SIZE_MAX;
    int original_alpha = alpha;
    int best = -GWD_SCORE_INFINITY;

    if (pv_line != NULL)
        pv_line->length = 0;
    ++search->nodes;
    if ((search->nodes & UINT64_C(1023)) == 0 && should_stop(search))
        return 0;
    if (!generate_moves(search, &vector))
        return 0;
    if (vector.count == 0) {
        free(vector.moves);
        return terminal_loss(search);
    }
    if (depth == 0) {
        int score = evaluate(search);
        free(vector.moves);
        return score;
    }
    ++search->tt_probes;
    if (gwd_tt_probe(search->tt, search->board.key, &cached)) {
        ++search->tt_hits;
        cached_move = find_tt_move(&cached, &vector);
        if (search->search_ply != 0 && cached.depth >= depth &&
            (cached.bound == GWD_TT_EXACT ||
             (cached.bound == GWD_TT_LOWER_BOUND && cached.score >= beta) ||
             (cached.bound == GWD_TT_UPPER_BOUND &&
              cached.score <= alpha))) {
            if (pv_line != NULL && cached_move != SIZE_MAX) {
                follow_tt_line(search->tt, &search->board, &cached, depth,
                               pv_line);
            }
            ++search->tt_cutoffs;
            free(vector.moves);
            return cached.score;
        }
    }
    move_to_front(&vector, cached_move);
    order_pv_move(search->pv, search->board.key, &vector);

    for (size_t index = 0; index < vector.count; ++index) {
        DraughtsBoardUndo undo;
        PvLine child_line;
        int score;

        if (!search_do_move(search, &vector.moves[index], &undo))
            break;
        score = -alpha_beta(search, depth - 1, -beta, -alpha, NULL,
                            &child_line);
        search_undo_move(search, &undo);
        if (search->stopped || search->failed)
            break;
        if (score > best) {
            best = score;
            if (best_move != NULL)
                *best_move = vector.moves[index];
            if (pv_line != NULL) {
                size_t child_count = child_line.length;
                if (child_count >= GWD_PV_MAX)
                    child_count = GWD_PV_MAX - 1;
                pv_line->moves[0] = vector.moves[index];
                memcpy(&pv_line->moves[1], child_line.moves,
                       child_count * sizeof(*child_line.moves));
                pv_line->length = child_count + 1;
            }
        }
        if (score > alpha)
            alpha = score;
        if (alpha >= beta)
            break;
    }
    if (!search->stopped && !search->failed &&
        best != -GWD_SCORE_INFINITY) {
        GwdTtRecord stored;

        stored.score = best;
        stored.depth = depth;
        stored.bound = best <= original_alpha ? GWD_TT_UPPER_BOUND :
                       best >= beta ? GWD_TT_LOWER_BOUND : GWD_TT_EXACT;
        stored.has_move = pv_line != NULL && pv_line->length != 0;
        stored.from = stored.has_move ? pv_line->moves[0].from : 0;
        stored.to = stored.has_move ? pv_line->moves[0].to : 0;
        stored.capture_count =
            stored.has_move ? pv_line->moves[0].capture_count : 0;
        gwd_tt_store(search->tt, search->board.key, &stored);
        ++search->tt_stores;
    }
    free(vector.moves);
    return best;
}

static void make_pv_table(const DraughtsGame *game, const PvLine *line,
                          PvTable *table)
{
    DraughtsBoard board = game->board;

    memset(table, 0, sizeof(*table));
    for (size_t ply = 0; ply < line->length; ++ply) {
        DraughtsBoardUndo undo;

        pv_store(table, board.key, &line->moves[ply]);
        if (!draughts_board_do_move(&board, &line->moves[ply], &undo))
            break;
    }
}

void gwd_search_control_init(GwdSearchControl *control)
{
    atomic_init(&control->stop_requested, false);
}

void gwd_search_request_stop(GwdSearchControl *control)
{
    atomic_store_explicit(&control->stop_requested, true,
                          memory_order_release);
}

bool gwd_search(const DraughtsGame *game, double time_limit_seconds,
                GwdSearchControl *control, bool master,
                GwdTranspositionTable *transposition_table,
                GwdSearchIterationVisitor iteration_visitor,
                void *iteration_context,
                GwdSearchResult *result, char *error, size_t error_size)
{
    SearchContext search;
    PvTable completed_pv = {0};
    struct timespec finished;
    long seconds;
    long nanoseconds;

    if (game == NULL || control == NULL || transposition_table == NULL ||
        result == NULL ||
        time_limit_seconds <= 0.0)
        return set_error(error, error_size, "invalid search argument");
    memset(result, 0, sizeof(*result));
    memset(&search, 0, sizeof(search));
    search.board = game->board;
    search.game_ply = draughts_game_ply(game);
    search.control = control;
    search.master = master;
    search.pv = &completed_pv;
    search.tt = transposition_table;
    clock_gettime(CLOCK_MONOTONIC, &search.started);
    seconds = (long)time_limit_seconds;
    nanoseconds = (long)((time_limit_seconds - (double)seconds) * 1e9);
    search.deadline = search.started;
    search.deadline.tv_sec += seconds;
    search.deadline.tv_nsec += nanoseconds;
    if (search.deadline.tv_nsec >= 1000000000L) {
        ++search.deadline.tv_sec;
        search.deadline.tv_nsec -= 1000000000L;
    }

    for (unsigned depth = 1; depth <= 64; ++depth) {
        DraughtsMove best_move = {0};
        PvLine iteration_line;
        int score;

        best_move.from = UINT8_MAX;
        if (should_stop(&search))
            break;
        search.stopped = false;
        score = alpha_beta(&search, depth, -GWD_SCORE_INFINITY,
                           GWD_SCORE_INFINITY, &best_move, &iteration_line);
        if (search.failed)
            return set_error(error, error_size, "alpha-beta move failure");
        if (search.stopped)
            break;
        make_pv_table(game, &iteration_line, &completed_pv);
        result->score = score;
        result->depth = depth;
        if (best_move.from != UINT8_MAX) {
            result->best_move = best_move;
            result->has_best_move = true;
        }
        result->nodes = search.nodes;
        result->tt_probes = search.tt_probes;
        result->tt_hits = search.tt_hits;
        result->tt_cutoffs = search.tt_cutoffs;
        result->tt_stores = search.tt_stores;
        clock_gettime(CLOCK_MONOTONIC, &finished);
        result->elapsed_seconds = elapsed(&search.started, &finished);
        result->timed_out = false;
        result->pv_length = iteration_line.length;
        memcpy(result->pv, iteration_line.moves,
               result->pv_length * sizeof(*result->pv));
        if (result->pv_length != 0) {
            result->best_move = result->pv[0];
            result->has_best_move = true;
        }
        if (iteration_visitor != NULL)
            iteration_visitor(result, iteration_context);
        if (score >= GWD_SCORE_WIN_ABSOLUTE - 1024 ||
            score <= -GWD_SCORE_WIN_ABSOLUTE + 1024)
            break;
    }
    clock_gettime(CLOCK_MONOTONIC, &finished);
    result->nodes = search.nodes;
    result->tt_probes = search.tt_probes;
    result->tt_hits = search.tt_hits;
    result->tt_cutoffs = search.tt_cutoffs;
    result->tt_stores = search.tt_stores;
    result->elapsed_seconds = elapsed(&search.started, &finished);
    result->timed_out = search.stopped;
    if (search.search_ply != 0)
        return set_error(error, error_size,
                         "search ply did not return to root");
    return true;
}
