# ChronoLog Packaging Guide

ChronoLog supports two **packaging** (distribution) options and two **discoverability** (consumption) mechanisms.

- **Packaging** — how you get ChronoLog onto a system: Spack, CPack (tarball/RPM/DEB), or a normal repo clone and install.
- **Discoverability** — how downstream projects find and use an already-installed ChronoLog: CMake `find_package` or pkg-config. These are produced at install time regardless of how you installed (Spack, CPack, or manual build).

---

## Packaging (distribution)

### 1. Spack (recommended for HPC users)

Spack is the package manager used in HPC environments. It handles the full dependency tree automatically (Thallium, HDF5, MPI, spdlog, etc.).

**Location:** `packaging/spack/`

**Install (from this repo):**
```bash
spack repo add ./packaging/spack
spack install chronolog
spack load chronolog
```

If the package has been accepted into [upstream Spack](https://github.com/spack/spack), you can skip the `spack repo add` step and run `spack install chronolog` directly. See [Making the Spack package available to everyone](#making-the-spack-package-available-to-everyone) below for how to get the package into upstream or host your own repo.

**Variants:**
| Variant | Default | Description |
|---------|---------|-------------|
| `+streaming` | ON | InfluxDB/Grafana streaming via CURL |
| `+tests` | OFF | Build and install test executables |
| `+docs` | OFF | Build Doxygen documentation |
| `build_type=Release` | Release | `Debug` or `Release` |

**Test it:**
```bash
# Check the package info
spack info chronolog

# Dry-run (resolve dependencies without installing)
spack spec chronolog

# Install and verify
spack install chronolog
spack load chronolog
which chrono-visor
pkg-config --modversion chronolog
```

---

### 2. CPack — Tarball / RPM / DEB

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

### Making the Spack package available to everyone

Right now users must run `spack repo add ./packaging/spack` (after cloning ChronoLog). To make the package available without that step, you have two options.

#### Option A: Contribute to upstream Spack (recommended)

Once the package is in the official [Spack repository](https://github.com/spack/spack), anyone with Spack can run `spack install chronolog` with no repo add.

**Steps:**

1. **Fork and clone Spack**  
   Fork [spack/spack](https://github.com/spack/spack), then clone your fork and add the upstream remote. Use the `develop` branch.

2. **Add the package in Spack’s layout**  
   In the Spack clone, create:
   - `var/spack/repos/builtin/packages/chronolog/package.py`  
   Copy the contents of `packaging/spack/packages/chronolog/package.py`. Upstream prefers release tarball URLs with `sha256` when available; you can add a `url = "https://github.com/grc-iit/ChronoLog/archive/refs/tags/vX.Y.Z.tar.gz"` and `version("X.Y.Z", sha256="...")` for each release (and keep `git` for `main` if desired).

3. **Match upstream style and requirements**  
   Follow the [Packaging Guide](https://spack.readthedocs.io/en/latest/packaging_guide.html) and [Package Review Guide](https://spack.readthedocs.io/en/latest/package_review_guide.html). Run `spack unit-test` and any package tests (e.g. `spack install chronolog` and basic smoke checks).

4. **Open a pull request**  
   Submit a PR from your branch to `spack/develop` with a clear description and link to ChronoLog. Address review feedback; maintainers may request changes to variants, dependencies, or style.

5. **Maintain over time**  
   After merge, you (or the `maintainers("grc-iit")` in the recipe) can submit follow-up PRs for new versions and fixes.

**References:** [Spack Contribution Guide](https://spack.readthedocs.io/en/latest/contribution_guide.html), [Package Review Guide](https://spack.readthedocs.io/en/latest/package_review_guide.html).

#### Option B: Host a dedicated Spack repo (no upstream PR)

Users can add your package without cloning the full ChronoLog repo by pointing Spack at a **separate** Git repo whose **root** is the Spack repo (i.e. `repo.yaml` and `packages/` at the top level). Spack does not support adding a subdirectory of a repo via URL.

**Steps:**

1. **Create a new repository** (e.g. `grc-iit/chronolog-spack`) whose root contains:
   - `repo.yaml` (same content as `packaging/spack/repo.yaml`)
   - `packages/chronolog/package.py` (copy from `packaging/spack/packages/chronolog/package.py`)

2. **Keep it in sync**  
   When you add versions or change the recipe in ChronoLog’s `packaging/spack/`, update the dedicated repo (manually or via CI so both stay aligned).

3. **Document the one-time add**  
   Users run:
   ```bash
   spack repo add https://github.com/grc-iit/chronolog-spack.git
   spack install chronolog
   ```

You can still contribute to upstream Spack later; the dedicated repo is useful for early releases or if you prefer to maintain the recipe only in your own repo.

---

## Discoverability (consumption)

After ChronoLog is installed (via Spack, CPack, or a normal `cmake --build` + `cmake --install`), the install tree includes CMake config files and a pkg-config file. Downstream projects use one of these to discover headers, libraries, and link flags.

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
2. Builds and installs inside a Spack-configured Docker container
3. Produces two artifacts:
   - `chronolog-<version>-<arch>-self-contained.tar.gz` — runtime bundle
   - `chronolog-<version>-<arch>-devel.tar.gz` — CPack developer package
4. Generates `SHA256SUMS` for integrity verification
5. Publishes everything as a GitHub Release
