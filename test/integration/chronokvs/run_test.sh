#!/bin/bash

# ChronoKVS Integration Test Runner
# This script can be used to run the integration test manually or in CI/CD

set -e  # Exit on any error

echo "ChronoKVS Integration Test Runner"
echo "================================="

# Check if we're in the right directory
if [ ! -f "chronokvs_integration_test.cpp" ]; then
    echo "Error: Please run this script from the test/integration/chronokvs/ directory"
    exit 1
fi

# Build the test
echo "Building ChronoKVS integration test..."
if [ -d "../../../build" ]; then
    cd ../../../build
    make chronokvs_integration_test
    cd test/integration/chronokvs
else
    echo "Error: Build directory not found. Please build the project first."
    exit 1
fi

# Run the test
echo "Running ChronoKVS integration test..."
./chronokvs_integration_test

echo "Test completed successfully!"
