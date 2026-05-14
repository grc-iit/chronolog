#!/bin/bash
# Print all variables used to set up a remote CLion environment

VARIABLES=(PKG_CONFIG_PATH CMAKE_PREFIX_PATH BOOST_ROOT PATH MPICXX MPICC)

for var in "${VARIABLES[@]}"
do
   echo "${var}=$(printenv "${var}")" >> /tmp/"${var}".env
done

paste -d ";" /tmp/*.env
rm /tmp/*.env