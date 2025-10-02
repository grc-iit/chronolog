# ChronoKVS Integration Test

This directory contains integration tests for the ChronoKVS component, designed to verify that the key-value store with temporal capabilities works correctly in a real-world scenario.

## Test Overview

The integration test (`chronokvs_integration_test.cpp`) comprehensively tests the ChronoKVS functionality including:

### Test Categories

1. **Basic Operations**
   - Put operations with timestamp generation
   - Get operations at specific timestamps
   - Multiple values for the same key

2. **Historical Data**
   - Retrieving complete history for a key
   - Timestamp ordering verification
   - Value retrieval at specific timestamps

3. **Timestamp Ordering**
   - Verification that timestamps are monotonically increasing
   - Correct value retrieval at intermediate timestamps

4. **Concurrent Operations**
   - Multi-threaded put operations
   - Thread safety verification
   - Unique timestamp generation under concurrency

5. **Error Handling**
   - Non-existent key handling
   - Invalid timestamp handling
   - Empty history for non-existent keys

6. **Performance Testing**
   - Bulk operations performance
   - Memory usage verification
   - Response time validation

7. **Edge Cases**
   - Empty keys and values
   - Very long keys and values
   - Special characters in keys and values

## Building and Running

### Prerequisites

- CMake 3.25 or higher
- C++17 compatible compiler
- ChronoKVS library built and available

### Build the Test

```bash
# From the project root
mkdir -p build
cd build
cmake ..
make chronokvs_integration_test
```

### Run the Test

```bash
# Option 1: Run directly
./test/integration/chronokvs/chronokvs_integration_test

# Option 2: Use the test runner script
cd test/integration/chronokvs
./run_test.sh

# Option 3: Run via CTest
ctest -R chronokvs_integration_test
```

## Test Output

The test provides detailed output showing:
- ✓ for passed tests
- ✗ for failed tests
- Performance metrics
- Final test summary

Example output:
```
Starting ChronoKVS Integration Tests...
========================================

--- Testing Basic Operations ---
✓ Put operation returns valid timestamp
✓ Get operation returns correct value
✓ Second timestamp is greater than first
✓ Get operation returns latest value
✓ Different keys work independently

--- Testing Historical Data ---
✓ History contains all values
✓ History is ordered by timestamp
✓ History value 0 matches
...

========================================
Test Results: 25/25 tests passed
🎉 All tests passed!
```

## CI/CD Integration

This test is designed to be run in GitHub Actions or other CI/CD systems. The test:

- Exits with code 0 on success, 1 on failure
- Has a timeout of 300 seconds
- Provides clear pass/fail indicators
- Includes performance benchmarks

### GitHub Actions Example

```yaml
- name: Run ChronoKVS Integration Test
  run: |
    cd build
    ctest -R chronokvs_integration_test --output-on-failure
```

## Test Design Principles

1. **Comprehensive Coverage**: Tests all public API methods and edge cases
2. **Real-world Scenarios**: Simulates actual usage patterns
3. **Performance Validation**: Ensures acceptable performance characteristics
4. **Error Resilience**: Verifies proper error handling
5. **Concurrency Safety**: Tests thread safety and concurrent operations
6. **Clear Reporting**: Provides detailed test results and failure information

## Maintenance

When adding new features to ChronoKVS:

1. Add corresponding test cases to verify the new functionality
2. Update performance benchmarks if needed
3. Ensure all tests still pass
4. Update this README if test structure changes
