CC      := gcc
STD     := -std=c11
WARN    := -Wall -Wextra -Wpedantic

SRC_DIR   := src
INC_DIR   := include
TEST_DIR  := tests
BUILD_DIR := build

CFLAGS := $(STD) $(WARN) -g -I$(INC_DIR)

# Library code: every .c in src/ EXCEPT files named server*.c. Those are
# standalone entry points (each defines its own main()) rather than code
# meant to be linked into every test binary -- a server*.c sitting in
# LIB_OBJS would collide with each tests/test_*.c's own main() at link
# time. resp.c/store.c/cmd.c live here; server.c (and any future
# server_threaded.c) do not.
LIB_SRCS    := $(filter-out $(SRC_DIR)/server%.c,$(wildcard $(SRC_DIR)/*.c))
LIB_OBJS    := $(LIB_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# tests/test_*.c each become their own self-contained binary linked
# against all LIB_OBJS -- add a new test file and `make test` picks it up
# with no Makefile changes.
TEST_SRCS := $(wildcard $(TEST_DIR)/*.c)
TEST_BINS := $(TEST_SRCS:$(TEST_DIR)/%.c=$(BUILD_DIR)/%)

# src/server*.c each become their own standalone server binary, linked
# against the same LIB_OBJS -- add server_threaded.c later and `make
# server` picks it up the same way. This uses a *static* pattern rule
# (target list : pattern : prereq-pattern) rather than a plain implicit
# pattern rule, because GNU Make's implicit-rule matching requires a
# non-empty stem -- a generic "$(BUILD_DIR)/server%" rule can never match
# a binary literally named "server" (stem would have to be empty). A
# static pattern rule applies the % substitution only to the already-known
# SERVER_BINS list, so that restriction doesn't apply.
SERVER_SRCS := $(wildcard $(SRC_DIR)/server*.c)
SERVER_BINS := $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%)

.PHONY: all test server clean asan tsan
.PRECIOUS: $(BUILD_DIR)/%.o

all: test server

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%: $(TEST_DIR)/%.c $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LIB_OBJS) -o $@

$(SERVER_BINS): $(BUILD_DIR)/%: $(SRC_DIR)/%.c $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LIB_OBJS) -o $@

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "== $$t =="; \
		./$$t || exit 1; \
	done

server: $(SERVER_BINS)

# ASan+UBSan: catches buffer overruns / use-after-free / UB. Especially
# important for store.c's bucket-relinking during rehashing and for
# server.c's connection-buffer arithmetic.
asan: CFLAGS += -fsanitize=address,undefined
asan: clean test server

# TSan: not useful against server.c (single-threaded event loop, no data
# races possible by construction) -- wired up for when server_threaded.c
# shows up.
tsan: CFLAGS += -fsanitize=thread
tsan: clean test server

clean:
	rm -rf $(BUILD_DIR)
