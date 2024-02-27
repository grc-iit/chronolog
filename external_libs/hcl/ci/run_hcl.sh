#!/bin/bash

pushd build

if [ "${HCL_ENABLE_RPCLIB}" = "ON" ]; then
    # TODO(chogan): Run tests via ctest and enable Thallium tests
    echo "Testing ordered map"
    ctest -V -R ^map_test || exit 1
    echo "Testing multimap"
    ctest -V -R ^multimap_test || exit 1
    echo "Testing priority queue"
    ctest -V -R ^priority_queue_test || exit 1
    echo "Testing queue"
    ctest -V -R ^queue_test || exit 1
    echo "Testing set"
    ctest -V -R ^set_test || exit 1
    echo "Testing unordered map test"
    ctest -V -R ^unordered_map_test || exit 1
    echo "Testing unordered map string test"
    ctest -V -R ^unordered_map_string_test || exit 1
fi

if [ "${HCL_ENABLE_THALLIUM_TCP}" = "ON" ]; then
    echo "Testing unordered map test"
    ctest -V -R ^unordered_map_test || exit 1
    echo "Testing unordered map string test"
    ctest -V -R ^unordered_map_string_test || exit 1
    echo "Testing ordered map"
    ctest -V -R ^map_test || exit 1
    echo "Testing multimap"
    ctest -V -R ^multimap_test || exit 1
    echo "Testing priority queue"
    ctest -V -R ^priority_queue_test || exit 1
    echo "Testing queue"
    ctest -V -R ^queue_test || exit 1
    echo "Testing set"
    ctest -V -R ^set_test || exit 1
fi
popd
