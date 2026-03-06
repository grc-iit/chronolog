# ChronoLog Packaging Guide

ChronoLog supports **packaging** (distribution) via CPack or a normal repo clone and install, and two **discoverability** (consumption) mechanisms.

- **Packaging** — how you get ChronoLog onto a system: CPack (tarball/RPM/DEB) or a normal repo clone and install. Spack packaging is planned for a future release.
- **Discoverability** — how downstream projects find and use an already-installed ChronoLog: CMake `find_package` or pkg-config. These are produced at install time regardless of how you installed (CPack or manual build).

---

## Packaging (distribution)

### 1. CPack — Tarball / RPM / DEB

CPack bundles the already-installed ChronoLog tree into a redistributable archive. Dependencies are bundled (RPATH-patched), so the tarball is self-contained.

**Generators:** TGZ always; RPM if `rpmbuild` is found; DEB if `dpkg-deb` is found.

**Produce a package:**
```bash
# 1. Build and install first
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --install build --prefix /tmp/cl-install

# 2. Package (adds platform suffix, e.g. linux-x86_64)
cmake --build build --target package_release

# Result
ls build/chronolog-*.tar.gz
```

**Test it:**
```bash
# Extract and check layout
tar xf build/chronolog-2.5.0-linux-x86_64.tar.gz -C /tmp/cl-pkg
ls /tmp/cl-pkg/chronolog-2.5.0/bin/
ls /tmp/cl-pkg/chronolog-2.5.0/lib/
ls /tmp/cl-pkg/chronolog-2.5.0/include/

# Verify shared libraries are self-contained (no broken rpaths)
ldd /tmp/cl-pkg/chronolog-2.5.0/bin/chrono-visor
```

---

## Discoverability (consumption)

After ChronoLog is installed (via CPack or a normal `cmake --build` + `cmake --install`), the install tree includes CMake config files and a pkg-config file. Downstream projects use one of these to discover headers, libraries, and link flags.

### 3. CMake `find_package`

For C++ projects that consume ChronoLog as a library. Downstream CMake projects use `find_package(Chronolog REQUIRED)`.

**Installed files:**
```
<prefix>/chronolog/lib/cmake/Chronolog/
    ChronologConfig.cmake
    ChronologConfigVersion.cmake
    ChronologTargets.cmake
```

**Consumer CMakeLists.txt:**
```cmake
find_package(Chronolog REQUIRED)
target_link_libraries(my_app PRIVATE Chronolog::chronolog_client)
```

**Test it:**
```bash
# Install ChronoLog
cmake --install build --prefix /tmp/cl-test

# Create a minimal test project
mkdir /tmp/cl-consumer && cat > /tmp/cl-consumer/CMakeLists.txt << 'EOF'
cmake_minimum_required(VERSION 3.25)
project(test)
find_package(Chronolog REQUIRED)
message(STATUS "Found Chronolog ${Chronolog_VERSION}")
EOF

cmake -B /tmp/cl-consumer/build \
      -S /tmp/cl-consumer \
      -DCMAKE_BUILD_TYPE=Release \
      -DChronolog_DIR=/tmp/cl-test/chronolog/lib/cmake/Chronolog
```

---

### 4. pkg-config

For C/C++ projects that use `pkg-config` instead of CMake (Makefiles, Meson, etc.).

**Installed file:** `<prefix>/chronolog/lib/pkgconfig/chronolog.pc`

**Test it:**
```bash
export PKG_CONFIG_PATH=/tmp/cl-test/chronolog/lib/pkgconfig

# Version
pkg-config --modversion chronolog

# Compiler and linker flags
pkg-config --cflags --libs chronolog
```

---

## Version Header

All installs (from any packaging or manual build) include a version header for compile-time detection:

```c
#include <chronolog/version.h>

printf("ChronoLog %s\n", CHRONOLOG_VERSION_STRING);
// CHRONOLOG_VERSION_MAJOR, _MINOR, _PATCH also available
```

**Location after install:** `<prefix>/chronolog/include/chronolog/version.h`

---

## Release Workflow (CI)

Pushing a tag matching `v*.*.*` triggers `.github/workflows/release.yml`, which:

1. Validates that the tag version matches `project(ChronoLog VERSION ...)` in `CMakeLists.txt`
2. Builds and installs inside a Docker container
3. Produces two artifacts:
   - `chronolog-<version>-<arch>-self-contained.tar.gz` — runtime bundle
   - `chronolog-<version>-<arch>-devel.tar.gz` — CPack developer package
4. Generates `SHA256SUMS` for integrity verification
5. Publishes everything as a GitHub Release
