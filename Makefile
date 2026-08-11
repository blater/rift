CC     = gcc
CFLAGS = -Werror -Wall -Wextra -pedantic -I src
DEPFLAGS = -MMD -MP
SRC    = src/
LIB    = src/lib/
BUILD  = build/
OBJECTS = $(BUILD)alloc.o $(BUILD)ast.o $(BUILD)lexer.o $(BUILD)token.o \
          $(BUILD)parser.o $(BUILD)generator.o $(BUILD)name_table.o \
          $(BUILD)stringview.o $(BUILD)error.o $(BUILD)typechecker.o \
          $(BUILD)main.o
DEPS = $(OBJECTS:.o=.d)

.PHONY: all clean test-pools test-negative test-refcount test-zxn

all: $(BUILD) rockc

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)%.o: $(SRC)%.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)alloc.o: $(LIB)alloc.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)pools.o: $(LIB)pools.c $(LIB)pools.h | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

rockc: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

# Phase A.2 — pool runtime test harness.
# Standalone C test, no Rock language integration yet.
$(BUILD)pools_test: test/pools_test.c $(BUILD)pools.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/pools_test.c $(BUILD)pools.o

test-pools: $(BUILD)pools_test
	$(BUILD)pools_test

# Phase B — negative tests for the typechecker's acyclicity rule (ADR §9.4).
# Each test is a Rock program that MUST fail compilation with a specific
# diagnostic.
test-negative: rockc
	test/negative/run_negative.sh

# Phase E.a — string retain/release runtime helpers.
# Synthesises longlived backings to exercise the live refcount paths
# since no Rock program path populates `backing` until Phase H.
#
# fundefs.c, alloc.c, fundefs_internal.c are compiled with the same
# relaxed flags the `rock` script uses for runtime sources (the strict
# rockc flags would catch pre-existing sign-compare warnings unrelated
# to ADR-0003 work).
RUNTIME_CFLAGS = -Wall -Wno-unused-variable -I src
$(BUILD)fundefs_for_test.o: $(LIB)fundefs.c $(LIB)fundefs.h $(LIB)pools.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)alloc_for_test.o: $(LIB)alloc.c $(LIB)alloc.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)fundefs_internal_for_test.o: $(LIB)fundefs_internal.c $(LIB)fundefs_internal.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)string_refcount_test: test/string_refcount_test.c $(BUILD)pools.o \
                              $(BUILD)fundefs_for_test.o \
                              $(BUILD)alloc_for_test.o \
                              $(BUILD)fundefs_internal_for_test.o | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -o $@ $^

test-refcount: $(BUILD)string_refcount_test
	$(BUILD)string_refcount_test

# Requires a ZEsarUX path through ZESARUX_BIN or
# `tools/test-zxn --emulator-bin ...`.
test-zxn: rockc
	tools/test-zxn

clean:
	rm -rf $(BUILD)
	rm -f rockc

-include $(DEPS)
