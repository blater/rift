CC     = gcc
CFLAGS = -Werror -Wall -Wextra -pedantic -I src
DEPFLAGS = -MMD -MP
SRC    = src/
LIB    = src/lib/
BUILD  = build/
OBJECTS = $(BUILD)alloc.o $(BUILD)ast.o $(BUILD)lexer.o $(BUILD)token.o \
          $(BUILD)parser.o $(BUILD)generator.o $(BUILD)generator_components.o $(BUILD)generator_ownership.o $(BUILD)generator_profile.o $(BUILD)generator_type_info.o $(BUILD)name_table.o \
          $(BUILD)stringview.o $(BUILD)error.o $(BUILD)component_manifest.o $(BUILD)asset_generator.o $(BUILD)typechecker.o \
          $(BUILD)semantic_resolve.o $(BUILD)semantic_ir.o $(BUILD)semantic_ir_lower.o \
          $(BUILD)ownership_plan.o $(BUILD)main.o
DRIVER_OBJECTS = $(BUILD)driver_main.o $(BUILD)driver_options.o \
                 $(BUILD)driver_paths.o $(BUILD)driver_process.o \
                 $(BUILD)driver_sidecar.o $(BUILD)driver_build_plan.o
DEPS = $(OBJECTS:.o=.d) $(DRIVER_OBJECTS:.o=.d)

.PHONY: all clean test-driver-locations test-driver-options test-driver-sidecar test-arena test-pools test-name-table test-semantic-ir test-ownership-plan test-type-method-autocast test-component-manifest test-asset-language test-negative test-refcount test-sprite-runtime test-zesarux-index-cache test-zesarux-zrcp test-zesarux-maintenance test-zxn-assets test-zxn-sprite test-zxn test-zxn-tiny-print test-zxn-light-core test-autolink test-memory-options test-memory-profile test-zxn-size check-rift-rename

all: $(BUILD) riftc rift $(BUILD)verify-zxn-assets

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)%.o: $(SRC)%.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)generator.o: $(SRC)generator/generator.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)generator_profile.o: $(SRC)generator/profile.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)generator_components.o: $(SRC)generator/components.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)generator_ownership.o: $(SRC)generator/ownership.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)generator_type_info.o: $(SRC)generator/type_info.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)semantic_resolve.o: $(SRC)semantic/resolve.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)semantic_ir.o: $(SRC)semantic_ir/semantic_ir.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)semantic_ir_lower.o: $(SRC)semantic_ir/lower.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)ownership_plan.o: $(SRC)ownership_plan/ownership_plan.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)driver_%.o: $(SRC)driver/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)asset_generator.o: $(SRC)generator/assets.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)alloc.o: $(LIB)alloc.c | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)arena_host.o: $(LIB)arena_host.c $(LIB)arena.h | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)pools.o: $(LIB)pools.c $(LIB)pools.h $(LIB)arena.h | $(BUILD)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

riftc: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

rift: $(DRIVER_OBJECTS) $(BUILD)component_manifest.o $(BUILD)alloc.o
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)verify-zxn-assets: tools/verify-zxn-assets.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

# Phase A.2 — pool runtime test harness.
# Standalone C test, no Rift language integration yet.
$(BUILD)pools_test: test/pools_test.c $(BUILD)pools.o $(BUILD)arena_host.o \
                     $(BUILD)error_sink_for_test.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/pools_test.c $(BUILD)pools.o \
		$(BUILD)arena_host.o \
		$(BUILD)error_sink_for_test.o

test-pools: $(BUILD)pools_test
	$(BUILD)pools_test

$(BUILD)arena_test: test/arena_test.c $(BUILD)arena_host.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

test-arena: $(BUILD)arena_test
	$(BUILD)arena_test

$(BUILD)name_table_test: test/name_table_test.c $(BUILD)name_table.o \
                         $(BUILD)ast.o $(BUILD)alloc.o $(BUILD)stringview.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/name_table_test.c $(BUILD)name_table.o \
		$(BUILD)ast.o $(BUILD)alloc.o $(BUILD)stringview.o

test-name-table: $(BUILD)name_table_test
	$(BUILD)name_table_test

$(BUILD)semantic_ir_test: test/semantic_ir_test.c $(BUILD)semantic_ir.o \
                           $(BUILD)semantic_ir_lower.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

test-semantic-ir: $(BUILD)semantic_ir_test
	$(BUILD)semantic_ir_test

$(BUILD)ownership_plan_test: test/ownership_plan_test.c \
                              $(BUILD)semantic_ir.o \
                              $(BUILD)semantic_ir_lower.o \
                              $(BUILD)ownership_plan.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

test-ownership-plan: $(BUILD)ownership_plan_test
	$(BUILD)ownership_plan_test

test-driver-locations: rift riftc
	sh test/build_locations_test.sh

$(BUILD)driver_options_test: test/driver_options_test.c \
                              $(BUILD)driver_options.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

test-driver-options: $(BUILD)driver_options_test
	$(BUILD)driver_options_test

$(BUILD)driver_sidecar_test: test/driver_sidecar_test.c \
                               $(BUILD)driver_sidecar.o \
                               $(BUILD)component_manifest.o $(BUILD)alloc.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

test-driver-sidecar: $(BUILD)driver_sidecar_test
	$(BUILD)driver_sidecar_test src/lib/components.manifest \
		test/fixtures/driver_sidecar_valid.txt \
		test/fixtures/driver_sidecar_legacy_v1.txt \
		test/fixtures/driver_sidecar_bad_profile.txt \
		test/fixtures/driver_sidecar_duplicate.txt

test-type-method-autocast: riftc
	sh test/test_type_method_autocast.sh

test-component-manifest: riftc
	bash test/test_component_manifest.sh

test-asset-language: riftc
	bash test/test_asset_language.sh

# Phase B — negative tests for the typechecker's acyclicity rule (ADR §9.4).
# Each test is a Rift program that MUST fail compilation with a specific
# diagnostic.
test-negative: riftc
	test/negative/run_negative.sh

# Phase E.a — string retain/release runtime helpers.
# Synthesises longlived backings to exercise the live refcount paths
# since no Rift program path populates `backing` until Phase H.
#
# fundefs.c and fundefs_internal.c are compiled with the same
# relaxed flags the `rift` script uses for runtime sources (the strict
# riftc flags would catch pre-existing sign-compare warnings unrelated
# to ADR-0003 work).
RUNTIME_CFLAGS = -Wall -Wno-unused-variable -I src -I src/ext/lib
$(BUILD)fundefs_for_test.o: $(LIB)fundefs.c $(LIB)fundefs.h $(LIB)pools.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)fundefs_internal_for_test.o: $(LIB)fundefs_internal.c $(LIB)fundefs_internal.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)handle_runtime_for_test.o: $(LIB)handle_runtime.c $(LIB)fundefs.h $(LIB)pools.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)termination_for_test.o: $(LIB)termination.c $(LIB)fundefs_internal.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)print_bytes_for_test.o: $(LIB)print_bytes.c $(LIB)fundefs.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)error_sink_for_test.o: $(LIB)error_sink.c $(LIB)error_sink.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)host_caps_for_test.o: $(LIB)host_caps.c $(LIB)host_caps.h | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
$(BUILD)termbox2_for_test.o: $(LIB)host/termbox2_impl.c | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
RUNTIME_TEST_SUPPORT_OBJECTS = $(BUILD)error_sink_for_test.o \
                               $(BUILD)host_caps_for_test.o \
                               $(BUILD)handle_runtime_for_test.o \
                               $(BUILD)termination_for_test.o \
                               $(BUILD)termbox2_for_test.o
$(BUILD)string_refcount_test: test/string_refcount_test.c $(BUILD)pools.o \
                              $(BUILD)arena_host.o \
                              $(BUILD)fundefs_for_test.o \
                              $(BUILD)fundefs_internal_for_test.o \
                              $(BUILD)print_bytes_for_test.o \
                              $(RUNTIME_TEST_SUPPORT_OBJECTS) | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -o $@ $^

$(BUILD)fixed_array_set_test: test/fixed_array_set_test.c $(BUILD)pools.o \
                              $(BUILD)arena_host.o \
                              $(BUILD)fundefs_for_test.o \
                              $(BUILD)fundefs_internal_for_test.o \
                              $(BUILD)print_bytes_for_test.o \
                              $(RUNTIME_TEST_SUPPORT_OBJECTS) | $(BUILD)
	$(CC) $(RUNTIME_CFLAGS) -o $@ $^

test-refcount: riftc $(BUILD)string_refcount_test $(BUILD)fixed_array_set_test
	$(BUILD)string_refcount_test
	$(BUILD)fixed_array_set_test
	@if $(BUILD)fixed_array_set_test gap >$(BUILD)fixed_array_set_gap.log 2>&1; then \
		echo 'FAIL: fixed array accepted a gap set' >&2; exit 1; \
	fi
	@grep -q 'INDEX OUT OF BOUNDS (2, limit: 1)' $(BUILD)fixed_array_set_gap.log
	@echo 'PASS: fixed array rejects a gap set before exposing skipped slots'
	sh test/test_fixed_array_ownership.sh

test-sprite-runtime:
	sh test/test_sprite_runtime.sh

test-zesarux-index-cache:
	tools/test-zesarux-index-cache

test-zesarux-zrcp:
	tools/test-zesarux-zrcp

test-zesarux-maintenance:
	bash test/test_zesarux_fork_maintenance.sh

test-zxn-assets: riftc $(BUILD)verify-zxn-assets
	sh test/test_zxn_asset_integration.sh

# Execute the sprite uploader and presentation API on the project-patched
# emulator, including mixed 4/8bpp attributes and rendered 4bpp halves.
test-zxn-sprite: riftc $(BUILD)verify-zxn-assets
	sh test/test_zxn_sprite_uploader_restore.sh
	sh test/test_zxn_sprite_smoke.sh

# Uses the canonical fork build by default; ZESARUX_BIN remains an override.
test-zxn: riftc
	tools/test-zxn --emulator-bin "$${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}"

# Verify the startup-31 tiny literal writer against ULA screen memory in the
# project-patched ZEsarUX build.
test-zxn-tiny-print: riftc
	sh test/test_zxn_tiny_print.sh

# Execute a dynamic string through pools/refcounting and the Rift-owned console
# without linking startup=1's stdio streams or terminal.
test-zxn-light-core: riftc
	sh test/test_zxn_light_core.sh

# Build a representative multi-component RTL program with both ZXN link modes.
# The script verifies that auto-linking resolves all required dependencies and
# leaves a materially smaller target artifact than the compatibility mode.
test-autolink: riftc
	sh test/test_rtl_autolink.sh

# Host execution under the exact constrained pool sizes used by the ZX Next
# target. Ownership loops must complete without pool exhaustion or UAF.
test-memory-profile: rift riftc
	sh test/test_memory_profile.sh

test-memory-options: rift riftc
	sh test/test_memory_options.sh

# Verify literal-print lowering, conservative CRT/tiny-core selection, and
# configurable target pool capacities without requiring an emulator.
test-zxn-size: riftc
	sh test/test_zxn_size_profile.sh

clean:
	rm -rf $(BUILD)
	rm -f rift riftc

check-rift-rename:
	tools/check-rift-rename.sh

-include $(DEPS)
