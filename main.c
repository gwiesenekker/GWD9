#include "draughts.h"
#include "game.h"
#include "logger.h"
#include "revision.h"
#include "search.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FEN_BUFFER_SIZE 512

typedef struct {
    size_t ordinal;
} PrintMoves;

typedef struct {
    const DraughtsGame *game;
    unsigned index;
    double time_limit;
    GwdSearchControl *control;
    GwdTranspositionTable *tt;
    GwdSearchResult result;
    bool ok;
    char error[256];
} ThreadWork;

typedef struct {
    ThreadWork *work;
    GwdLogger *logger;
} IterationOutput;

static bool print_move(const DraughtsMove *move, void *context)
{
    PrintMoves *output = context;

    printf("%3zu. ", ++output->ordinal);
    draughts_print_move(move);
    putchar('\n');
    return true;
}

static void format_pv(const GwdSearchResult *result, char *buffer, size_t size)
{
    size_t used = 0;

    if (size == 0)
        return;
    buffer[0] = '\0';
    for (size_t ply = 0; ply < result->pv_length; ++ply) {
        const DraughtsMove *move = &result->pv[ply];
        int written = snprintf(buffer + used, size - used, "%s%u%c%u",
                               ply == 0 ? "" : " ", move->from + 1,
                               move->capture_count == 0 ? '-' : 'x',
                               move->to + 1);
        if (written < 0 || (size_t)written >= size - used)
            break;
        used += (size_t)written;
    }
}

static void show_iteration(const GwdSearchResult *result, void *context)
{
    IterationOutput *output = context;
    char pv[1024];

    format_pv(result, pv, sizeof(pv));
    gwd_logger_log(output->logger,
                   "iteration depth %u score %d nodes %llu time %.3f PV %s",
                   result->depth, result->score,
                   (unsigned long long)result->nodes,
                   result->elapsed_seconds,
                   result->pv_length == 0 ? "(none)" : pv);
    if (output->work->index == 0) {
        printf("info depth %u score %d nodes %llu time %.3f pv %s\n",
               result->depth, result->score,
               (unsigned long long)result->nodes,
               result->elapsed_seconds,
               result->pv_length == 0 ? "(none)" : pv);
        fflush(stdout);
    }
}

static bool parse_count(const char *text, size_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed > 256)
        return false;
    *value = (size_t)parsed;
    return true;
}

static bool parse_time_limit(const char *text, double *value)
{
    char *end;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        parsed <= 0.0 || parsed > 86400.0)
        return false;
    *value = parsed;
    return true;
}

static void *read_game_state(void *opaque)
{
    ThreadWork *work = opaque;
    GwdLogger logger;
    char current_fen[FEN_BUFFER_SIZE];
    char best_move[32];
    char pv[1024];
    IterationOutput iteration = {work, &logger};

    work->ok = false;
    if (!gwd_logger_open(&logger, work->index, work->error,
                         sizeof(work->error)))
        return NULL;
    if (!draughts_board_to_fen(&work->game->board, current_fen,
                               sizeof(current_fen))) {
        snprintf(work->error, sizeof(work->error), "cannot format current FEN");
        gwd_logger_close(&logger);
        return NULL;
    }
    gwd_logger_log(&logger, "GWD9 revision %s started as %s", gwd9_revision,
                   work->index == 0 ? "master" : "slave");
    gwd_logger_log(&logger, "starting FEN: %s", work->game->starting_fen);
    gwd_logger_log(&logger, "root ply: %zu", draughts_game_ply(work->game));
    for (size_t ply = 0; ply < draughts_game_ply(work->game); ++ply)
        gwd_logger_log(&logger, "move %zu: %s", ply + 1,
                       draughts_game_move(work->game, ply));
    gwd_logger_log(&logger, "current FEN: %s", current_fen);
    gwd_logger_log(&logger, "Zobrist key: %016llx",
                   (unsigned long long)work->game->board.key);
    gwd_logger_log(&logger, "alpha-beta search started; limit %.3f seconds",
                   work->time_limit);
    if (!gwd_search(work->game, work->time_limit, work->control,
                    work->index == 0, work->tt,
                    show_iteration, &iteration, &work->result,
                    work->error, sizeof(work->error))) {
        gwd_logger_log(&logger, "search failed: %s", work->error);
        gwd_logger_close(&logger);
        return NULL;
    }
    if (work->result.has_best_move)
        snprintf(best_move, sizeof(best_move), "%u%c%u",
                 work->result.best_move.from + 1,
                 work->result.best_move.capture_count == 0 ? '-' : 'x',
                 work->result.best_move.to + 1);
    else
        snprintf(best_move, sizeof(best_move), "(none)");
    format_pv(&work->result, pv, sizeof(pv));
    gwd_logger_log(&logger,
                   "search finished: depth %u score %d nodes %llu time %.3f best %s",
                   work->result.depth, work->result.score,
                   (unsigned long long)work->result.nodes,
                   work->result.elapsed_seconds, best_move);
    gwd_logger_log(&logger,
                   "TT: probes %llu hits %llu cutoffs %llu stores %llu",
                   (unsigned long long)work->result.tt_probes,
                   (unsigned long long)work->result.tt_hits,
                   (unsigned long long)work->result.tt_cutoffs,
                   (unsigned long long)work->result.tt_stores);
    gwd_logger_log(&logger, "principal variation (%zu plies): %s",
                   work->result.pv_length,
                   work->result.pv_length == 0 ? "(none)" : pv);
    gwd_logger_close(&logger);
    work->ok = true;
    return NULL;
}

static bool run_threads(const DraughtsGame *game, size_t thread_count,
                        double time_limit, GwdSearchResult *master_result,
                        char *error, size_t error_size)
{
    pthread_t *threads = NULL;
    ThreadWork *work = calloc(thread_count, sizeof(*work));
    GwdSearchControl control;
    GwdTranspositionTable *tt = NULL;
    size_t created = 0;
    bool ok = true;

    if (work == NULL) {
        snprintf(error, error_size, "cannot allocate thread state");
        return false;
    }
    gwd_search_control_init(&control);
    if (!gwd_tt_create(&tt, 16, error, error_size)) {
        free(work);
        return false;
    }
    if (thread_count > 1) {
        threads = calloc(thread_count - 1, sizeof(*threads));
        if (threads == NULL) {
            gwd_tt_destroy(tt);
            free(work);
            snprintf(error, error_size, "cannot allocate thread handles");
            return false;
        }
    }
    for (size_t index = 0; index < thread_count; ++index) {
        work[index].game = game;
        work[index].index = (unsigned)index;
        work[index].time_limit = time_limit;
        work[index].control = &control;
        work[index].tt = tt;
    }
    for (size_t index = 1; index < thread_count; ++index) {
        int status = pthread_create(&threads[index - 1], NULL,
                                    read_game_state, &work[index]);
        if (status != 0) {
            snprintf(error, error_size, "cannot create slave %zu: %s", index,
                     strerror(status));
            ok = false;
            break;
        }
        ++created;
    }
    read_game_state(&work[0]);
    gwd_search_request_stop(&control);
    for (size_t index = 0; index < created; ++index)
        pthread_join(threads[index], NULL);
    for (size_t index = 0; index <= created; ++index) {
        if (!work[index].ok && ok) {
            snprintf(error, error_size, "thread %zu: %.220s", index,
                     work[index].error);
            ok = false;
        }
    }
    if (work[0].ok && master_result != NULL)
        *master_result = work[0].result;
    gwd_tt_destroy(tt);
    free(threads);
    free(work);
    return ok;
}

int main(int argc, char **argv)
{
    DraughtsGame game = {0};
    PrintMoves output = {0};
    char input[FEN_BUFFER_SIZE];
    char canonical[FEN_BUFFER_SIZE];
    char error[256];
    const char *fen;
    size_t thread_count = 1;
    double time_limit = 0.1;
    size_t pop_count = 0;
    int argument = 1;
    bool fen_from_stdin = false;
    size_t move_count;

    if (argc == 2 && strcmp(argv[1], "--revision") == 0) {
        printf("GWD9 revision %s\n", gwd9_revision);
        return 0;
    }
    while (argument < argc && argv[argument][0] == '-') {
        if ((strcmp(argv[argument], "-j") == 0 ||
             strcmp(argv[argument], "--threads") == 0) &&
            argument + 1 < argc &&
            parse_count(argv[argument + 1], &thread_count) &&
            thread_count != 0) {
            argument += 2;
        } else if (strncmp(argv[argument], "-j", 2) == 0 &&
                   argv[argument][2] != '\0' &&
                   parse_count(argv[argument] + 2, &thread_count) &&
                   thread_count != 0) {
            ++argument;
        } else if (strcmp(argv[argument], "-t") == 0 &&
                   argument + 1 < argc &&
                   parse_time_limit(argv[argument + 1], &time_limit)) {
            argument += 2;
        } else if (strncmp(argv[argument], "-t", 2) == 0 &&
                   argv[argument][2] != '\0' &&
                   parse_time_limit(argv[argument] + 2, &time_limit)) {
            ++argument;
        } else if (strcmp(argv[argument], "--pop") == 0 &&
                   argument + 1 < argc &&
                   parse_count(argv[argument + 1], &pop_count)) {
            argument += 2;
        } else {
            fprintf(stderr, "Invalid option or value: %s\n", argv[argument]);
            return 2;
        }
    }
    if (argument < argc) {
        fen = argv[argument++];
    } else {
        if (fgets(input, sizeof(input), stdin) == NULL) {
            fprintf(stderr, "Usage: %s [-j N] [-t N] [--pop N] FEN [MOVE ...]\n",
                    argv[0]);
            return 2;
        }
        input[strcspn(input, "\r\n")] = '\0';
        fen = input;
        fen_from_stdin = true;
    }
    if (!draughts_game_init(&game, fen, error, sizeof(error))) {
        fprintf(stderr, "Invalid FEN: %s\n", error);
        return 2;
    }
    for (; argument < argc; ++argument) {
        if (!draughts_game_push(&game, argv[argument], error, sizeof(error))) {
            fprintf(stderr, "Cannot replay ply %zu: %s\n",
                    draughts_game_ply(&game) + 1, error);
            draughts_game_destroy(&game);
            return 2;
        }
    }
    if (fen_from_stdin) {
        char move[64];
        while (scanf("%63s", move) == 1) {
            if (!draughts_game_push(&game, move, error, sizeof(error))) {
                fprintf(stderr, "Cannot replay ply %zu: %s\n",
                        draughts_game_ply(&game) + 1, error);
                draughts_game_destroy(&game);
                return 2;
            }
        }
    }
    if (pop_count > draughts_game_ply(&game)) {
        fprintf(stderr, "Cannot pop %zu moves from a %zu-ply game.\n",
                pop_count, draughts_game_ply(&game));
        draughts_game_destroy(&game);
        return 2;
    }
    while (pop_count-- != 0)
        draughts_game_pop(&game);
    if (!draughts_board_to_fen(&game.board, canonical, sizeof(canonical))) {
        fputs("Could not format the position as FEN.\n", stderr);
        draughts_game_destroy(&game);
        return 1;
    }
    GwdSearchResult master_result;
    if (!gwd_logs_prepare(error, sizeof(error)) ||
        !run_threads(&game, thread_count, time_limit, &master_result,
                     error, sizeof(error))) {
        fprintf(stderr, "Logging failed: %s\n", error);
        draughts_game_destroy(&game);
        return 1;
    }

    printf("GWD9 revision %s\n", gwd9_revision);
    printf("Threads: %zu (%zu slave%s)\n", thread_count, thread_count - 1,
           thread_count == 2 ? "" : "s");
    printf("Search time: %.3f seconds per thread\n", time_limit);
    printf("Starting FEN: %s\n", game.starting_fen);
    printf("Moves (%zu %s from root):", draughts_game_ply(&game),
           draughts_game_ply(&game) == 1 ? "ply" : "plies");
    for (size_t ply = 0; ply < draughts_game_ply(&game); ++ply)
        printf(" %s", draughts_game_move(&game, ply));
    putchar('\n');
    draughts_print_board(&game.board);
    printf("FEN: %s\n", canonical);
    printf("Zobrist key: %016llx\n",
           (unsigned long long)game.board.key);
    puts("Legal moves:");
    if (!draughts_generate_moves(&game.board.position, game.board.side_to_move,
                                 print_move, &output, &move_count)) {
        fprintf(stderr, "Move generation failed: %s\n",
                draughts_movegen_last_error());
        draughts_game_destroy(&game);
        return 1;
    }
    if (move_count == 0)
        puts("  (none)");
    printf("Total: %zu legal move%s\n", move_count,
           move_count == 1 ? "" : "s");
    printf("Alpha-beta: depth %u, score %d, nodes %llu, time %.3f s",
           master_result.depth, master_result.score,
           (unsigned long long)master_result.nodes,
           master_result.elapsed_seconds);
    if (master_result.has_best_move) {
        fputs(", best move ", stdout);
        draughts_print_move(&master_result.best_move);
    } else {
        fputs(", no legal move", stdout);
    }
    putchar('\n');
    printf("Transposition table: probes %llu, hits %llu, cutoffs %llu, stores %llu\n",
           (unsigned long long)master_result.tt_probes,
           (unsigned long long)master_result.tt_hits,
           (unsigned long long)master_result.tt_cutoffs,
           (unsigned long long)master_result.tt_stores);
    printf("Principal variation (%zu plies):", master_result.pv_length);
    for (size_t ply = 0; ply < master_result.pv_length; ++ply) {
        const DraughtsMove *move = &master_result.pv[ply];
        printf(" %u%c%u", move->from + 1,
               move->capture_count == 0 ? '-' : 'x', move->to + 1);
    }
    if (master_result.pv_length == 0)
        fputs(" (none)", stdout);
    putchar('\n');
    draughts_game_destroy(&game);
    return 0;
}
