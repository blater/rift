CC     = gcc
CFLAGS = -Werror -Wall -Wextra -pedantic -I src
DEPFLAGS = -MMD -MP
SRC    = src/
LIB    = src/lib/
BUILD  = build/
OBJECTS = $(BUILD)alloc.o $(BUILD)ast.o $(BUILD)lexer.o $(BUILD)token.o \
          $(BUILD)parser.o $(BUILD)generator.o $(BUILD)name_table.o \
          $(BUILD)stringview.o $(BUILD)error.o $(BUILD)component_manifest.o $(BUILD)typechecker.o \
          $(BUILD)main.o
DEPS = $(OBJECTS:.o=.d)

.PHONY: all clean test-pools test-name-table test-type-method-autocast test-component-manifest test-negative test-refcount test-zxn test-zxn-tiny-print test-zxn-light-core test-autolink test-memory-profile test-zxn-size

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

$(BUILD)name_table_test: test/name_table_test.c $(BUILD)name_table.o \
                         $(BUILD)ast.o $(BUILD)alloc.o $(BUILD)stringview.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/name_table_test.c $(BUILD)name_table.o \
		$(BUILD)ast.o $(BUILD)alloc.o $(BUILD)stringview.o

test-name-table: $(BUILD)name_table_test
	$(BUILD)name_table_test

test-type-method-autocast: rockc
	sh test/test_type_method_autocast.sh

test-component-manifest: rockc
	bash test/test_component_manifest.sh

# Phase B — negative tests for the typechecker's acyclicity rule (ADR §9.4).
# Each test is a Rock program that MUST fail compilation with a specific
# diagnostic.
test-negative: rockc
	test/negative/run_negative.sh

# Phase E.a — string retain/release runtime helpers.
# Synthesises longlived backings to exercise the live refcount paths
# since no Rock program path populates `backing` until Phase H.
#
# fundefs.c and fundefs_internal.c are compiled with the same
# relaxed flags the `rock` script uses for runtime sources (the strict
# rockc flags would catch pre-existing sign-compare warnings unrelated
# to ADR-0003 work).
RUNTIME_CFLAGS = -Wall -Wno-unused-variable -I src
$(BUILD)fundefs_for_test.o: $(LIB)fundefs.c $(LIB)fundefs.h $(LIB)pools.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)fundefs_internal_for_test.o: $(LIB)fundefs_internal.c $(LIB)fundefs_internal.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)print_bytes_for_test.o: $(LIB)print_bytes.c $(LIB)fundefs.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)string_refcount_test: test/string_refcount_test.c $(BUILD)pools.o \
                              $(BUILD)fundefs_for_test.o \
                              $(BUILD)fundefs_internal_for_test.o \
                              $(BUILD)print_bytes_for_test.o | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -o $@ $^

test-refcount: $(BUILD)string_refcount_test
	$(BUILD)string_refcount_test

# Requires a ZEsarUX path through ZESARUX_BIN or
# `tools/test-zxn --emulator-bin ...`.
test-zxn: rockc
	tools/test-zxn

# Verify the startup-31 tiny literal writer against ULA screen memory in the
# project-patched ZEsarUX build.
test-zxn-tiny-print: rockc
	sh test/test_zxn_tiny_print.sh

# Execute a dynamic string through pools/refcounting and the Rock-owned console
# without linking startup=1's stdio streams or terminal.
test-zxn-light-core: rockc
	sh test/test_zxn_light_core.sh

# Build a representative multi-component RTL program with both ZXN link modes.
# The script verifies that auto-linking resolves all required dependencies and
# leaves a materially smaller target artifact than the compatibility mode.
test-autolink: rockc
	sh test/test_rtl_autolink.sh

# Host execution under the exact constrained pool sizes used by the ZX Next
# target. Ownership loops must complete without pool exhaustion or UAF.
test-memory-profile: rockc
	sh test/test_memory_profile.sh

# Verify literal-print lowering, conservative CRT/tiny-core selection, and
# configurable target pool capacities without requiring an emulator.
test-zxn-size: rockc
	sh test/test_zxn_size_profile.sh

clean:
	rm -rf $(BUILD)
	rm -f rockc

-include $(DEPS)
