# GWD9

GWD developed by GW and ChatGPT.

This is the new C implementation of GWD for International draughts. The main
program is the `master` thread; with the default of one thread it creates no
slaves and runs single-threaded.

The board uses four compact 50-bit bitboards, one for each colour and piece
kind. Legal move generation incorporates the generator developed and tested
for the endgame-tablebase project. It implements flying kings, backward
captures by men, mandatory capture, and the maximum-capture rule. Board-level
do/undo operations apply a
generated move, switch the side to move, and restore the complete prior state.

Build and run:

```sh
make
./gwd9 'W:W31-50:B1-20'
```

With no command-line FEN, the program reads one line from standard input:

```sh
echo 'W:W32:B27,17' | ./gwd9
```

Replay checked PDN moves after the starting FEN. The current ply count is the
distance from that root position:

```sh
./gwd9 'W:W31-50:B1-20' 31-26 16-21
```

The same game can be read from standard input with the FEN on the first line
and moves separated by whitespace afterward.

Use `--pop N` to undo the last `N` replayed moves. Use `-j N` to run with `N`
total threads, including the master. Thus `-j 1` is the single-threaded
default and creates no slave threads:

```sh
./gwd9 -j 4 'W:W31-50:B1-20' 31-26 16-21
```

Each thread reads the completed game state and writes timestamped messages to
its own file: `logs/log0.txt` for the master, followed by `log1.txt`, etc. The
game is not mutated after workers start, so these reads need no shared lock.

## Alpha-beta search

Every thread searches its own board copy with iterative-deepening negamax
alpha-beta. Search boards, node counts, ply counters, and results are private
to each thread. `-t N` sets the master wall-clock limit in seconds and accepts
fractions; the default is 0.1 seconds:

```sh
./gwd9 -j 4 -t 2.5 'W:W31-50:B1-20'
```

Only the master checks the clock. A small shared C11 atomic coordinator carries
the one-way stop request to the slaves. Slaves poll that flag at node
boundaries; message queues are deferred until thread commands need payloads.
The search ply starts at zero, increments after every search move, and is
decremented after undo. Terminal scores combine it with the game ply to retain
GWD8-compatible absolute win/loss distances.

Every fully completed depth is published immediately with its score,
cumulative node count, elapsed time, and checked principal variation. The
master prints these `info depth ...` lines to standard output; every thread
writes its own iterations to its private log. An interrupted depth is never
published.

## Zobrist keys

Every board carries a deterministic 64-bit Zobrist key covering all four
colour/piece classes and the side to move. FEN parsing computes the key from
scratch. Do-move updates it incrementally for the moving piece, captures,
promotion, and side-to-move; undo restores the saved key. The fixed SplitMix64
seed keeps keys stable between runs and across thread counts, ready for use by
a transposition table.

## Principal variation

Each search owns a 64-bucket, four-way associative principal-variation table,
following the compact design used by GWD8. Slots store the complete Zobrist key
and move. A completed iteration becomes the starting table for the next one,
providing PV move ordering; an interrupted iteration is discarded. After the
search, GWD9 reconstructs the line from the root and verifies every probed move
against the legal move generator before publishing it.

## Transposition table

All search threads share a 16 MiB lock-free transposition table with four slots
per entry. Each slot consists of two atomic 64-bit words. As in GWD8, the first
word overlays a 64-bit checksum with the board key and the second overlays the
packed search data with the board key. A reader copies the words locally,
rechecks the publication word, decodes them, and accepts the entry only when
the checksum matches. A writer prepares both encoded words locally, stores the
data, then publishes the key/checksum word with release ordering.

Records contain score, depth, exact/lower/upper bound, and a compact move hint.
Only systems reporting lock-free 64-bit atomics are accepted. TT activity is
reported per search thread in its log.

Run the smoke tests with `make check`.

Build with AddressSanitizer and UndefinedBehaviorSanitizer, then run the test
suite with LeakSanitizer enabled:

```sh
make check-sanitize
```

In an environment that runs programs under `ptrace`, LeakSanitizer cannot
start. The remaining sanitizer checks can still be run there with:

```sh
make check-sanitize-no-leaks
```

Run `make clean && make` afterward to restore the normal optimized build.

## Revisions

As in `endgame7`, the build supplies a source revision to a small revision
module. By default it comes from `git describe`; release or external builds
can set it explicitly:

```sh
make GWD9_REVISION=9.1
./gwd9 --revision
```
