#!/bin/bash
# Test-only real daemon regression; never a production stop entry.
set -eu
exec python3 "$(dirname "$0")/rpc_lifecycle_deadline_test.py" "$@"
