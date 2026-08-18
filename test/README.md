# Rift Test Suite

This directory contains all unit and functional tests for the Rift compiler.

## Running Tests

From the project root, run:

```bash
./run_tests.sh
```

This will:
- Compile each test file
- Execute the compiled test
- Report pass/fail status
- Summarize results

## Test Files

All test files follow the naming convention: `*_test.rift`

### Core Type Tests
- `array_test.rift` - Array creation, append, indexing, length
- `byte_test.rift` - Byte type (unsigned 8-bit) operations
- `word_test.rift` - Word type (unsigned 16-bit) operations
- `dword_test.rift` - Dword type (unsigned 32-bit) operations
- `length_test.rift` - length() function for strings and arrays

### String/Substring Tests
- `concat_test.rift` - String concatenation
- `substring_test.rift` - substring() function with various indices
- `substring_advanced_test.rift` - Edge cases and advanced usage
- `substring_oob_test.rift` - Out-of-bounds error testing (expected failure)
- `tostring_test.rift` - to_string() conversion for various types

### Memory & Allocation Tests
- `memory_test.rift` - Memory allocation and cleanup
- `assign_test.rift` - Variable assignment operations

### Type Conversion Tests
- `byte_advanced_test.rift` - Advanced byte operations and casting
- `format_test.rift` - Function arguments with arrays and type conversion

### Embedding Tests
- `embed_test.rift` - Inline C code embedding with function calls
- `embed_inline_test.rift` - Inline C embedding
- `embed_simple_test.rift` - Simple C code embedding

### Error Handling Tests
- `error_test.rift` - Error condition testing (expected failure)
- `format_test.rift` - String formatting (expected failure)
- `enum_test.rift` - Enum type testing

## Expected Failures

The following tests are expected to fail or skip:
- `error_test.rift` - Tests compiler error messages
- `substring_oob_test.rift` - Tests out-of-bounds error handling

## Adding New Tests

To add a new test:

1. Create a file in `test/` named `your_feature_test.rift`
2. Implement the test with appropriate assertions via print statements
3. Run `./run_tests.sh` to verify it works
4. Add documentation above if it tests a new feature

## Test Output

Each test should produce minimal, deterministic output that can be visually verified or compared against expected output. Use:

```rift
print("✓ Feature test passed\n");
```

for success output, or error messages for failures.
