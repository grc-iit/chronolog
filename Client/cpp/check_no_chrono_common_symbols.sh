#!/usr/bin/env bash
#
# CI guard for the client decoupling work (issue #619).
#
# After the client library was unlinked from chrono_common (#616 / PR #620),
# libchronolog_client.so should contain zero symbols defined by chrono_common's
# compiled translation units. This script asserts that invariant by grepping
# the demangled symbol table of the built shared library for known
# chrono_common-only class/function names. If any are present, the build fails
# with a descriptive message.
#
# Usage: check_no_chrono_common_symbols.sh /path/to/libchronolog_client.so

set -euo pipefail

LIB_PATH="${1:?usage: $0 /path/to/libchronolog_client.so}"

if [[ ! -f "$LIB_PATH" ]]; then
    echo "[check_no_chrono_common_symbols] Library not found: $LIB_PATH" >&2
    exit 2
fi

if ! command -v nm >/dev/null 2>&1; then
    echo "[check_no_chrono_common_symbols] nm not available; skipping symbol-leakage check" >&2
    exit 0
fi

# Patterns derived from chrono_common's compiled .cpp units:
#   StoryChunk.cpp, StoryChunkExtractor.cpp, StoryChunkWriter.cpp,
#   StoryPipeline.cpp, ChunkExtractorCSV.cpp, ChunkExtractorRDMA.cpp,
#   RDMATransferAgent.cpp, city.cpp.
# These are the class and function names that should never appear as defined
# symbols in libchronolog_client.so.
FORBIDDEN_PATTERNS=(
    'StoryChunk::'
    'StoryChunkExtractor'
    'StoryChunkWriter'
    'StoryPipeline'
    'ChunkExtractorCSV'
    'ChunkExtractorRDMA'
    'chronolog::RDMATransferAgent'
    'CityHash'
)

# nm -C demangles; --defined-only filters out undefined references; we only
# care about symbols actually shipped in this library.
SYMBOL_DUMP=$(nm -C --defined-only "$LIB_PATH" 2>/dev/null || true)

LEAKED=()
for pattern in "${FORBIDDEN_PATTERNS[@]}"; do
    if matches=$(echo "$SYMBOL_DUMP" | grep -F -- "$pattern" || true); [[ -n "$matches" ]]; then
        LEAKED+=("--- pattern: $pattern ---")
        LEAKED+=("$matches")
    fi
done

if (( ${#LEAKED[@]} > 0 )); then
    {
        echo "[check_no_chrono_common_symbols] FAIL: chrono_common-origin symbols leaked into $LIB_PATH"
        echo
        printf '%s\n' "${LEAKED[@]}"
        echo
        echo "The client library is supposed to be self-contained — it must not link any"
        echo "chrono_common compiled object code. If a new symbol leaked in, either:"
        echo "  - the offending header should be moved to Client/cpp/include/, or"
        echo "  - the dependency should be eliminated rather than re-introduced."
    } >&2
    exit 1
fi

echo "[check_no_chrono_common_symbols] OK: $(basename "$LIB_PATH") contains no chrono_common-origin symbols"
