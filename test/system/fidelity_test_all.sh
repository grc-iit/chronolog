#!/bin/bash

# Check if a parameter has been provided
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <path>"
    exit 1
fi

# Store the path parameter
PATH_PARAM=$1

./fidelity_test_01.sh $PATH_PARAM

./fidelity_test_02.sh $PATH_PARAM

./fidelity_test_03.sh $PATH_PARAM

./fidelity_test_04.sh                   

echo "All tests completed!"
