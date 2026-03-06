# Copyright 2013-2024 Lawrence Livermore National Security, LLC and other
# Spack Project Developers. See the top-level COPYRIGHT file for details.
#
# SPDX-License-Identifier: (Apache-2.0 OR MIT)

from spack.package import *


class Chronolog(CMakePackage):
    """Distributed Chronicle Logging System.

    ChronoLog is a high-performance, distributed, hierarchical, and tiered
    chronicle-based logging system designed for recording and playback of
    time-ordered event streams across HPC clusters.
    """

    homepage = "https://github.com/grc-iit/ChronoLog"
    git      = "https://github.com/grc-iit/ChronoLog.git"

    maintainers("enekogonzalez")

    # Versions — add tagged releases as they are cut
    version("main",    branch="main",    submodules=True)
    version("develop", branch="develop", submodules=True)

    # -------------------------------------------------------------------------
    # Variants
    # -------------------------------------------------------------------------
    variant("testing",   default=False, description="Build and install tests")
    variant("streaming", default=True,  description="Enable InfluxDB/Grafana streaming")
    variant("shared",    default=True,  description="Build shared libraries")

    # -------------------------------------------------------------------------
    # Dependencies
    # -------------------------------------------------------------------------
    # Build-time only
    depends_on("cmake@3.25:", type="build")
    depends_on("py-pybind11",  type="build")

    # Test-only
    depends_on("googletest@1.12: cxxstd=17", when="+testing", type="test")

    # Runtime
    depends_on("boost@1.80:")
    depends_on("spdlog@1.12:")
    depends_on("json-c")
    depends_on("hdf5@1.14: +cxx +threadsafe")
    depends_on("libfabric@1.16: fabrics=sockets,tcp,udp")
    depends_on("mercury@2.2: ~boostsys")
    depends_on("mochi-margo@0.13:")
    depends_on("mochi-thallium@0.10.1:")
    depends_on("mpich@4.0: ~fortran +argobots")

    # -------------------------------------------------------------------------
    # CMake arguments
    # -------------------------------------------------------------------------
    def cmake_args(self):
        return [
            self.define("CMAKE_BUILD_TYPE", "Release"),
            self.define_from_variant("BUILD_SHARED_LIBS",        "shared"),
            self.define_from_variant("CHRONOLOG_BUILD_TESTING",  "testing"),
            self.define_from_variant("CHRONOLOG_INSTALL_TESTS",  "testing"),
            self.define_from_variant("CHRONOLOG_WITH_STREAMING", "streaming"),
        ]
