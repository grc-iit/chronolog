# Copyright 2013-2020 Lawrence Livermore National Security, LLC and other
# Spack Project Developers. See the top-level COPYRIGHT file for details.
#
# SPDX-License-Identifier: (Apache-2.0 OR MIT)

# ----------------------------------------------------------------------------
# If you submit this package back to Spack as a pull request,
# please first remove this boilerplate and all FIXME comments.
#
# This is a template package file for Spack.  We've put "FIXME"
# next to all the things you'll want to change. Once you've handled
# them, you can save this file and test your package like this:
#
#     spack install rpclib
#
# You can edit this file again by typing:
#
#     spack edit rpclib
#
# See the Spack documentation for more information on packaging.
# ----------------------------------------------------------------------------

from spack import *


class Hcl(CMakePackage):
    url = "https://github.com/HDFGroup/hcl/tarball/master"
    git = "https://github.com/HDFGroup/hcl.git"

    version('dev', branch='dev')
    version('0.1', branch='v0.1')
    variant('communication',
            default='rpclib',
            values=('rpclib', 'thallium'),
            multi=False,
            description='Which communication interface to build.')
    variant('protocol',
            default='tcp',
            values=('tcp', 'roce'),
            multi=True,
            description='Which communication protocol to use.')
    depends_on('gcc@8.3.0')
    depends_on('mpich@3.3.2~fortran')
    depends_on('rpclib@2.2.1', when='communication=rpclib')
    depends_on('mochi-thallium~cereal@0.8.3', when='communication=thallium')
    depends_on('boost@1.74.0')

    def cmake_args(self):
        spec = self.spec
        args = ['-DCMAKE_INSTALL_PREFIX={}'.format(self.prefix)]
        if 'communication=rpclib' in spec:
            args.append("-DBASKET_ENABLE_RPCLIB=ON")
        elif 'communication=thallium' in spec:
            if 'protocol=roce' in spec:
                args.append("-DHCL_ENABLE_THALLIUM_ROCE=ON")
            else:
                args.append("-DHCL_ENABLE_THALLIUM_TCP=ON")
        return args

    def set_include(self, env, path):
        env.append_flags('CFLAGS', '-I{}'.format(path))
        env.append_flags('CXXFLAGS', '-I{}'.format(path))

    def set_lib(self, env, path):
        env.prepend_path('LD_LIBRARY_PATH', path)
        env.append_flags('LDFLAGS', '-L{}'.format(path))

    def set_flags(self, env):
        self.set_include(env, '{}/include'.format(self.prefix))
        self.set_include(env, '{}/include'.format(self.prefix))
        self.set_lib(env, '{}/lib'.format(self.prefix))
        self.set_lib(env, '{}/lib64'.format(self.prefix))

    def setup_dependent_environment(self, spack_env, run_env, dependent_spec):
        self.set_flags(spack_env)

    def setup_run_environment(self, env):
        self.set_flags(env)
