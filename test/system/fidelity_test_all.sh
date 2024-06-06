#!/bin/bash

# Check if the correct number of arguments are provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <input_file> <directory>"
    exit 1
fi

# Store the path parameter
original_file=$1
directory=$2

./fidelity_test_01.sh $directory

./fidelity_test_02.sh $original_file $directory

./fidelity_test_03.sh $original_file $directory


echo "All tests completed!"
