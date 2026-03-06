# Copyright 2013-2024 Lawrence Livermore National Security, LLC and other
# Spack Project Developers. See the top-level COPYRIGHT file for details.
#
# SPDX-License-Identifier: (Apache-2.0 OR MIT)

import os

from spack.package import *


class Chronolog(CMakePackage):
    """ChronoLog: a scalable distributed chronicle-based log for HPC systems.

    ChronoLog provides high-performance, distributed logging infrastructure
    with chronicle (named log) management, distributed storage backed by HDF5,
    and optional real-time streaming to Grafana via InfluxDB.
    """

    homepage = "https://github.com/grc-iit/ChronoLog"
    git = "https://github.com/grc-iit/ChronoLog.git"
    # Uncomment and fill sha256 once a release tarball is published:
    # url = "https://github.com/grc-iit/ChronoLog/archive/refs/tags/v2.5.0.tar.gz"

    maintainers("grc-iit")
    license("BSD-2-Clause")

    version("main", branch="main")
    version("2.5.0", tag="v2.5.0")
    # version("2.5.0", sha256="<sha256>")  # preferred once tarball is available

    # ---------------------------------------------------------------------------
    # Variants
    # ---------------------------------------------------------------------------
    # ChronoLog's CMake only accepts Debug or Release — restrict accordingly.
    variant(
        "build_type",
        default="Release",
        values=("Debug", "Release"),
        description="CMake build type (RelWithDebInfo and MinSizeRel are not supported)",
        multi=False,
    )

    variant(
        "streaming",
        default=True,
        description="Enable real-time streaming to Grafana via InfluxDB (requires CURL)",
    )

    variant(
        "tests",
        default=False,
        description="Build and install test executables",
    )

    variant(
        "docs",
        default=False,
        description="Build Doxygen API documentation",
    )

    # ---------------------------------------------------------------------------
    # Dependencies
    # ---------------------------------------------------------------------------
    depends_on("cmake@3.25:", type="build")
    depends_on("pkgconfig", type="build")

    # RPC / messaging stack
    depends_on("mochi-thallium@0.10.1:")
    depends_on("mochi-margo@0.13:")

    # Storage
    depends_on("hdf5@1.14: +cxx +threadsafe")

    # Logging
    depends_on("spdlog@1.12:")

    # MPI (virtual — fulfilled by mpich, openmpi, etc.)
    depends_on("mpi")

    # JSON (used by the client library via pkg-config)
    depends_on("json-c")

    # Boost (headers + serialization used internally)
    depends_on("boost@1.80:")

    # Python bindings — always built; Client/python is unconditionally included.
    depends_on("python@3.8:", type=("build", "run"))
    depends_on("py-pybind11@2.11:", type=("build", "run"))

    # Optional: real-time streaming to InfluxDB/Grafana
    depends_on("curl", when="+streaming")

    # Optional: test suite
    depends_on("googletest cxxstd=17", when="+tests", type=("build", "test"))

    # Optional: API documentation
    depends_on("doxygen", when="+docs", type="build")

    # ---------------------------------------------------------------------------
    # CMake arguments
    # ---------------------------------------------------------------------------
    def cmake_args(self):
        args = [
            # Must be set explicitly; ChronoLog rejects anything else.
            self.define("CMAKE_BUILD_TYPE", self.spec.variants["build_type"].value),
            self.define_from_variant("CHRONOLOG_WITH_STREAMING", "streaming"),
            self.define_from_variant("CHRONOLOG_BUILD_TESTING", "tests"),
            self.define_from_variant("CHRONOLOG_INSTALL_TESTS", "tests"),
            self.define_from_variant("CHRONOLOG_ENABLE_DOXYGEN", "docs"),
        ]
        return args

    # ---------------------------------------------------------------------------
    # Runtime / build environment helpers
    # ---------------------------------------------------------------------------
    # ChronoLog installs into a non-standard sub-directory layout:
    #   <prefix>/chronolog/bin/
    #   <prefix>/chronolog/lib/
    #   <prefix>/chronolog/include/
    #   <prefix>/chronolog/lib/pkgconfig/
    #   <prefix>/chronolog/lib/cmake/Chronolog/
    # The helpers below expose these paths to Spack consumers.

    def _chronolog_prefix(self):
        return os.path.join(self.prefix, "chronolog")

    def setup_run_environment(self, env):
        cl = self._chronolog_prefix()
        env.prepend_path("PATH", os.path.join(cl, "bin"))
        env.prepend_path("LD_LIBRARY_PATH", os.path.join(cl, "lib"))
        env.prepend_path("PKG_CONFIG_PATH", os.path.join(cl, "lib", "pkgconfig"))
        env.set("Chronolog_DIR", os.path.join(cl, "lib", "cmake", "Chronolog"))

    def setup_dependent_build_environment(self, env, dependent_spec):
        cl = self._chronolog_prefix()
        env.prepend_path("PKG_CONFIG_PATH", os.path.join(cl, "lib", "pkgconfig"))
        env.set("Chronolog_DIR", os.path.join(cl, "lib", "cmake", "Chronolog"))
