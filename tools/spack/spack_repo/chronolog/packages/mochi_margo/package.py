# Margo 0.24.4 for ChronoLog's Spack environments.
#
# The builtin recipes are pinned to spack-packages releases/v2026.10, which
# offers Margo only up to 0.23.1. Margo 0.24.0 to 0.24.4 were added to
# spack-packages develop on 2026-10-07 (spack/spack-packages#6855); the
# sha256 below is the one from that commit. The builtin recipe already builds
# 0.22 and later with CMake, so this recipe only adds the version.
#
# Remove this repository once a spack-packages release offers 0.24.4.

from spack_repo.builtin.packages.mochi_margo.package import MochiMargo as BuiltinMochiMargo

from spack.package import *


class MochiMargo(BuiltinMochiMargo):
    """A library that provides Argobots bindings to the Mercury RPC
    implementation."""

    version("0.24.4", sha256="2dcf456b6aa4ee65f30a08a1bfa6d59602657e555074b91760fef893d7afbcf1")
