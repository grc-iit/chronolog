# Versioning Changes Summary

This document summarizes the **versioning-related changes** for ChronoLog. Base comparison is against `develop`.

---

## 1. Single source of truth: CMake `project(VERSION)`

**Before (develop):** Version was hardcoded in `CMakeLists.txt`:
```cmake
project(ChronoLog LANGUAGES CXX C)

set(CHRONOLOG_VERSION_MAJOR "0")
set(CHRONOLOG_VERSION_MINOR "7")
set(CHRONOLOG_VERSION_PATCH "0")
set(CHRONOLOG_PACKAGE_VERSION "${CHRONOLOG_VERSION_MAJOR}.${CHRONOLOG_VERSION_MINOR}.${CHRONOLOG_VERSION_PATCH}")
# ... etc
```

**After (packaging-test):** Version is set once in `project()` and all variables are derived from CMake’s built-in `PROJECT_VERSION*`:

- **File:** `CMakeLists.txt`
- **Change:** Replace `project(ChronoLog LANGUAGES CXX C)` with:
  ```cmake
  project(ChronoLog VERSION 2.5.0 LANGUAGES CXX C)
  ```
- **Change:** Replace the manual version block with:
  ```cmake
  # Derive version variables from CMake project VERSION (set above)
  set(CHRONOLOG_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
  set(CHRONOLOG_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
  set(CHRONOLOG_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
  set(CHRONOLOG_PACKAGE "ChronoLog")
  set(CHRONOLOG_PACKAGE_NAME "CHRONOLOG")
  set(CHRONOLOG_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CHRONOLOG_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
  set(CHRONOLOG_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_PATCH}")
  set(CHRONOLOG_PACKAGE_STRING "${CHRONOLOG_PACKAGE_NAME} ${PROJECT_VERSION}")
  set(CHRONOLOG_PACKAGE_TARNAME "${CHRONOLOG_PACKAGE}")
  ```

To bump the version in the future: change only the `VERSION x.y.z` in the `project()` line; all `CHRONOLOG_*` variables and downstream uses stay in sync.

---

## 2. Runtime version header (`version.h`)

**Before:** No generated `version.h` on develop.

**After:** A generated header is installed so C/C++ code and downstream projects can use the project version at compile/runtime.

- **New file:** `cmake/version.h.in`
  ```c
  #ifndef CHRONOLOG_VERSION_H
  #define CHRONOLOG_VERSION_H

  #define CHRONOLOG_VERSION_MAJOR @PROJECT_VERSION_MAJOR@
  #define CHRONOLOG_VERSION_MINOR @PROJECT_VERSION_MINOR@
  #define CHRONOLOG_VERSION_PATCH @PROJECT_VERSION_PATCH@
  #define CHRONOLOG_VERSION_STRING "@PROJECT_VERSION@"

  #endif /* CHRONOLOG_VERSION_H */
  ```

- **In root `CMakeLists.txt`** (after pkg-config and before deploy scripts), add:
  ```cmake
  #------------------------------------------------------------------------------
  # Runtime Version Header
  #------------------------------------------------------------------------------
  configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
      "${CMAKE_CURRENT_BINARY_DIR}/chronolog_version.h"
      @ONLY
  )

  install(
      FILES "${CMAKE_CURRENT_BINARY_DIR}/chronolog_version.h"
      DESTINATION ${CHRONOLOG_INSTALL_INCLUDE_DIR}/chronolog
      RENAME version.h
  )
  ```

- **Install location:** `<prefix>/chronolog/include/chronolog/version.h`
- **Usage in code:** `#include <chronolog/version.h>` and use `CHRONOLOG_VERSION_STRING`, `CHRONOLOG_VERSION_MAJOR`, etc.

---

## 3. Client library versioning (VERSION / SOVERSION)

**File:** `Client/cpp/CMakeLists.txt`

- **Change:** On the `chronolog_client` target, set CMake target version properties so the shared library gets a proper soname and version:
  ```cmake
  set_target_properties(chronolog_client PROPERTIES
      PUBLIC_HEADER "${PUBLIC_HEADER_FILES}"
      VERSION       ${CHRONOLOG_PACKAGE_VERSION}
      SOVERSION     ${CHRONOLOG_VERSION_MAJOR}
  )
  ```
  (Add `VERSION` and `SOVERSION` to the existing `set_target_properties` for `chronolog_client`; keep `PUBLIC_HEADER` and any other properties you already have.)

- **Effect:** Shared library will have a versioned soname (e.g. `libchronolog_client.so.2`) and full version (e.g. `libchronolog_client.so.2.5.0`).

---

## 4. CPack / packaging

**File:** `cmake/CPackConfig.cmake`

- CPack version is already set from the single source of truth:
  ```cmake
  set(CPACK_PACKAGE_VERSION "${CHRONOLOG_PACKAGE_VERSION}")
  ```
- Package file name uses the same variable, e.g. `chronolog-${CHRONOLOG_PACKAGE_VERSION}`; with the custom `package_release` target this becomes `chronolog-2.5.0-linux-x86_64.tar.gz` (see below).

**File:** Root `CMakeLists.txt` (after `include(CPack)`)

- **Custom target `package_release`:** Builds the CPack TGZ and renames it to include the platform suffix:
  - Base name: `chronolog-${CHRONOLOG_PACKAGE_VERSION}` (e.g. `chronolog-2.5.0`)
  - Final name: `chronolog-2.5.0-<system>-<processor>.tar.gz` (e.g. `chronolog-2.5.0-linux-x86_64.tar.gz`)
- Implementation uses `add_custom_target(package_release ...)` that runs `cpack` then renames the generated `.tar.gz` to the `-<system>-<processor>` form.

---

## 5. GitHub Actions release workflow

**File:** `.github/workflows/release.yml` (new on this branch)

- **Trigger:** Push of a tag matching `v[0-9]+.[0-9]+.[0-9]+` (e.g. `v2.5.0`).
- **Version extraction:** From the tag (e.g. `v2.5.0` → `2.5.0`) and architecture.
- **Version verification step (important for versioning):**
  ```yaml
  - name: Verify CMake version matches git tag
    run: |
      TAG_VERSION="${GITHUB_REF_NAME#v}"
      CMAKE_VERSION=$(grep -m1 'project(ChronoLog VERSION' CMakeLists.txt \
        | sed 's/.*VERSION[[:space:]]\+\([0-9][0-9.]*\).*/\1/')
      if [ "${CMAKE_VERSION}" != "${TAG_VERSION}" ]; then
        echo "❌ Version mismatch: CMakeLists.txt has ${CMAKE_VERSION} but tag is ${GITHUB_REF_NAME}"
        exit 1
      fi
      echo "✅ Version ${CMAKE_VERSION} matches tag ${GITHUB_REF_NAME}"
  ```
  So for tag `v2.5.0`, `CMakeLists.txt` must have `project(ChronoLog VERSION 2.5.0 ...)`.

- **Artifacts:** Build and upload tarballs (e.g. self-contained and devel) and **SHA256SUMS** (checksum generation step added for release artifacts).

---

## 6. Spack package (version 2.5.0)

**File:** `packaging/spack/packages/chronolog/package.py`

- Add a version that matches the release tag and single source of truth:
  ```python
  version("2.5.0", tag="v2.5.0")
  # version("2.5.0", sha256="<sha256>")  # preferred once tarball is available
  ```
- Optionally add a commented `url = "https://github.com/grc-iit/ChronoLog/archive/refs/tags/v2.5.0.tar.gz"` (and uncomment when release tarballs are published).

---

## 7. Config / find_package (ChronologConfig.cmake.in)

**File:** `cmake/ChronologConfig.cmake.in`

- Version variables in the generated config should come from the single source of truth (they are already substituted from `CHRONOLOG_*` / project version):
  - `Chronolog_VERSION` = `@CHRONOLOG_PACKAGE_VERSION@`
  - `Chronolog_VERSION_MAJOR`, `_MINOR`, `_PATCH` = `@CHRONOLOG_VERSION_MAJOR@`, etc.

No change needed if these are already set from the same CMake variables as in section 1.

---

## 8. Checklist to redo versioning on another branch

1. **CMakeLists.txt**
   - Add `VERSION x.y.z` to `project(ChronoLog ...)` (e.g. `2.5.0`).
   - Replace hardcoded `CHRONOLOG_VERSION_*` with `PROJECT_VERSION_MAJOR/MINOR/PATCH` and `PROJECT_VERSION` as above.
   - Add the “Runtime Version Header” block (configure_file + install of `chronolog_version.h` as `version.h`).
   - Ensure `package_release` custom target uses `CHRONOLOG_PACKAGE_VERSION` for the base package name.

2. **cmake/version.h.in**
   - Create with the four defines (`CHRONOLOG_VERSION_MAJOR/MINOR/PATCH` and `CHRONOLOG_VERSION_STRING`).

3. **Client/cpp/CMakeLists.txt**
   - Set `VERSION` and `SOVERSION` on `chronolog_client` from `CHRONOLOG_PACKAGE_VERSION` and `CHRONOLOG_VERSION_MAJOR`.

4. **Release workflow**
   - Add “Verify CMake version matches git tag” step so tag and `project(VERSION)` always match.
   - Ensure artifact names and SHA256SUMS use the version from the tag/step output.

5. **Spack**
   - Add `version("x.y.z", tag="vx.y.z")` (and optionally url/sha256 when using tarballs).

6. **Bumping version for a new release**
   - Update only: `project(ChronoLog VERSION x.y.z ...)` in `CMakeLists.txt`.
   - Update Spack: `version("x.y.z", tag="vx.y.z")` (and url/sha256 if applicable).
   - Tag as `vx.y.z`; the release workflow will verify and build with that version.

---

*Summary generated from the `packaging-test` branch; base comparison with `develop`.*
