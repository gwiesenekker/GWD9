#ifndef GWD9_SEARCH_H
#define GWD9_SEARCH_H

#include "game.h"
#include "transposition.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define GWD_SCORE_MAN 100
#define GWD_SCORE_KING 333
#define GWD_SCORE_WIN_ABSOLUTE 20000
#define GWD_SCORE_INFINITY 30000
#define GWD_PV_MAX 64

typedef struct {
    int score;
    unsigned depth;
    uint64_t nodes;
    uint64_t tt_probes;
    uint64_t tt_hits;
    uint64_t tt_cutoffs;
    uint64_t tt_stores;
    double elapsed_seconds;
    bool has_best_move;
    DraughtsMove best_move;
    DraughtsMove pv[GWD_PV_MAX];
    size_t pv_length;
    bool timed_out;
} GwdSearchResult;

typedef void (*GwdSearchIterationVisitor)(const GwdSearchResult *result,
                                          void *context);

typedef struct {
    atomic_bool stop_requested;
} GwdSearchControl;

void gwd_search_control_init(GwdSearchControl *control);
void gwd_search_request_stop(GwdSearchControl *control);

/*
 * Independent, iterative-deepening alpha-beta search of a game snapshot.
 * Only a master search checks the time limit. Slaves only poll control.
 */
bool gwd_search(const DraughtsGame *game, double time_limit_seconds,
                GwdSearchControl *control, bool master,
                GwdTranspositionTable *transposition_table,
                GwdSearchIterationVisitor iteration_visitor,
                void *iteration_context,
                GwdSearchResult *result, char *error, size_t error_size);

#endif
