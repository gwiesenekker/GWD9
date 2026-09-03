CC ?= cc
CPPFLAGS ?= -I.
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?=
LDLIBS ?= -pthread

PROGRAM = gwd9
OBJECTS = main.o draughts.o game.o logger.o movegen.o revision.o search.o transposition.o zobrist.o
TEST_PROGRAM = test_draughts
TEST_OBJECTS = test_draughts.o draughts.o game.o movegen.o search.o transposition.o zobrist.o
GWD9_REVISION ?= $(shell git describe --always --dirty --tags 2>/dev/null || printf 9.0-dev)
SANITIZER_CFLAGS = -O1 -g -std=c11 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined
SANITIZER_LDFLAGS = -fsanitize=address,undefined

.PHONY: all clean check sanitize check-sanitize check-sanitize-no-leaks

all: $(PROGRAM)

$(PROGRAM): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

main.o: main.c draughts.h game.h logger.h revision.h search.h movegen.h
draughts.o: draughts.c draughts.h zobrist.h movegen.h
game.o: game.c game.h draughts.h movegen.h
logger.o: logger.c logger.h
search.o: search.c search.h transposition.h game.h draughts.h movegen.h
transposition.o: transposition.c transposition.h
zobrist.o: zobrist.c zobrist.h movegen.h
test_draughts.o: test_draughts.c draughts.h game.h search.h transposition.h movegen.h

.PHONY: FORCE
revision.o: revision.c revision.h FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) -DGWD9_REVISION='"$(GWD9_REVISION)"' -c -o $@ revision.c

movegen.o: movegen.c movegen.h

$(TEST_PROGRAM): $(TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJECTS) $(LDLIBS)

check: $(PROGRAM) $(TEST_PROGRAM)
	sh ./tests.sh
	./$(TEST_PROGRAM)

sanitize:
	$(MAKE) clean
	$(MAKE) $(PROGRAM) $(TEST_PROGRAM) CFLAGS='$(SANITIZER_CFLAGS)' LDFLAGS='$(SANITIZER_LDFLAGS)'

check-sanitize: sanitize
	ASAN_OPTIONS=detect_leaks=1 sh ./tests.sh
	ASAN_OPTIONS=detect_leaks=1 ./$(TEST_PROGRAM)

check-sanitize-no-leaks: sanitize
	ASAN_OPTIONS=detect_leaks=0 sh ./tests.sh
	ASAN_OPTIONS=detect_leaks=0 ./$(TEST_PROGRAM)

clean:
	$(RM) $(PROGRAM) $(TEST_PROGRAM) $(OBJECTS) test_draughts.o
