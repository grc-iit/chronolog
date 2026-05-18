---
sidebar_position: 2
title: "Code Style"
---

# Code Style

ChronoLog is a C++17 project. All formatting is enforced by **clang-format-18** in CI — pull requests that don't pass the format check cannot be merged.

## Formatting Tool

Install clang-format-18 from the default Ubuntu 24.04 repositories:

```bash
sudo apt-get update && sudo apt-get install -y clang-format-18
```

The project configuration lives at [`CodeStyleFiles/ChronoLog.clang-format`](https://github.com/grc-iit/ChronoLog/tree/develop/CodeStyleFiles/ChronoLog.clang-format). Key settings:

| Setting | Value |
|---|---|
| Column limit | 120 characters |
| Indent width | 4 spaces |
| Tabs | Never |
| Pointer alignment | Left (`int* p`) |
| Brace wrapping | Allman style (braces on their own line) |
| Sort includes | Never (preserves intentional ordering) |
| Bin-pack arguments/parameters | No (one per line when wrapped) |

## Running Locally

Format a single file:

```bash
clang-format-18 -i -style=file:CodeStyleFiles/ChronoLog.clang-format path/to/file.cpp
```

### Pre-commit Hook

The repository includes a ready-made hook at `CodeStyleFiles/pre-commit` that automatically formats staged C/C++ files before each commit. Install it by copying it into your local hooks directory:

```bash
cp CodeStyleFiles/pre-commit .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
```

The hook searches for `clang-format-18` in `~/.local/bin`, `/usr/local/bin`, `/usr/bin`, and `$PATH`. You can override the binary by setting the `CLANG_FORMAT` environment variable.

## IDE Integration

### CLion

1. Go to **File > Settings > Editor > Code Style > C/C++**
2. Click the gear icon and choose **Import Scheme**
3. Import `CodeStyleFiles/ChronoLog.xml`

### VS Code

Install the **Clang-Format** extension (or use clangd), then add to your workspace settings:

```json
{
  "C_Cpp.clang_format_path": "/usr/bin/clang-format-18",
  "C_Cpp.clang_format_style": "file:${workspaceFolder}/CodeStyleFiles/ChronoLog.clang-format",
  "editor.formatOnSave": true
}
```

### Other Editors

Point your editor's clang-format integration at the `CodeStyleFiles/ChronoLog.clang-format` file and ensure it uses version 18. Different versions may produce different output.

## CI Enforcement

The **Clang Format Check** workflow runs on every pull request that touches C/C++ files (`.cpp`, `.h`, `.hpp`, `.cc`, `.cxx`, `.c`). When formatting issues are found:

1. The workflow posts a comment on your PR listing the affected files
2. A `clang-format-patches` artifact is uploaded with patch files
3. The check fails, blocking the PR

To fix a failed check:

```bash
# Option A: download and apply the patch artifact
gh run download <run-id> -n clang-format-patches
git apply format_fixes.patch

# Option B: format the files manually
clang-format-18 -i -style=file:CodeStyleFiles/ChronoLog.clang-format <file>

# Then commit and push
git add -u && git commit -m "style: fix formatting"
git push
```

## Naming Conventions

| Element | Convention | Example |
|---|---|---|
| Classes / Structs | PascalCase | `StoryChunk`, `ChronoKeeperClient` |
| Methods / Functions | camelCase | `acquireStory()`, `processEvent()` |
| Member variables | camelCase, private members prefixed with `the` | `theStoryId`, `theEventQueue` |
| Local variables | camelCase | `storyName`, `eventCount` |
| Enums | PascalCase name, UPPER_CASE values | `enum class CLogStoryAcquisitionResult { CL_SUCCESS, CL_ERR_UNKNOWN }` |
| Typedefs / Aliases | PascalCase | `StoryId`, `ChunkMap` |
| Namespaces | lowercase | `chronolog` |
| Header guards | `#ifndef` with full path | `#ifndef CHRONO_COMMON_CHRONOLOG_ERRCODE_H` |
| Constants / Macros | UPPER_CASE | `MAX_STORY_CHUNK_SIZE` |

## Error Handling and Logging

### Error Codes

ChronoLog uses integer error codes defined in `chrono_common/include/chronolog_errcode.h`. Return `CL_SUCCESS` (0) on success and negative values on failure. The file provides `to_string()` conversion functions for human-readable log output.

### Logging

ChronoLog uses [spdlog](https://github.com/gabime/spdlog) through wrapper macros:

| Macro | Use |
|---|---|
| `LOG_TRACE(...)` | Verbose debugging (off in release) |
| `LOG_DEBUG(...)` | Developer-facing diagnostics |
| `LOG_INFO(...)` | Normal operational messages |
| `LOG_WARNING(...)` | Unexpected but recoverable situations |
| `LOG_ERROR(...)` | Failures that affect a single operation |
| `LOG_CRITICAL(...)` | Unrecoverable errors |

## Additional Static Analysis

The CMake build includes a **cpplint** target for additional style checking. This is not enforced in CI but can be useful during development:

```bash
cmake --build build --target cpplint
```
