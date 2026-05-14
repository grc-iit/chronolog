#!/bin/bash

BASE_DIR=${HOME}
cd "${BASE_DIR}" || exit
git clone -c feature.manyFiles=true https://github.com/spack/spack.git
echo ". ${BASE_DIR}/spack/share/spack/setup-env.sh" >> "${HOME}"/.bashrc
source "${HOME}"/.bashrc

yes | spack install lmod@8.6.5
echo "LMOD_PATH=$(spack location -i lmod)/lmod/8.6.5" >> "${HOME}"/.bashrc
echo "source ${LMOD_PATH}/init/bash" >> "${HOME}"/.bashrc
source "${HOME}"/.bashrc

git clone https://github.com/scs-lab/ChronoLog.git
cd ChronoLog || exit
CHRONOLOG_DIR=$(pwd)

spack env create chronolog "${CHRONOLOG_DIR}"/ci/environment/spack.lock
spack env activate -p chronolog
yes | spack install
echo "spack env activate -p chronolog" >> "${HOME}"/.bashrc

yes| spack module tcl refresh
spack env loads -r
echo "source ${CHRONOLOG_DIR}/ci/environment/loads" >> "${HOME}"/.bashrc
source "${HOME}"/.bashrc
