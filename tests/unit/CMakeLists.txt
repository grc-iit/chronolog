cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

add_test(NAME Unit_Deploy_ArchiveCleanup
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/deploy_archive_cleanup_test.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Unit_Deploy_LocalWal
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/deploy_local_wal_test.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Unit_Deploy_WalDirProbe
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/wal_dir_probe_test.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Unit_Performance_ArchiveCleanup
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/performance_archive_cleanup_test.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Unit_NFS_ComposeOverride
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/nfs_compose_override_test.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Unit_NFS_ServerDryRun
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/nfs_server_dry_run_test.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Unit_NFS_Classify
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/nfs_classify_test.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Unit_NFS_Args
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/nfs_args_test.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Unit_LocalCI_BadNfsProfile
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/local_ci_bad_profile_test.sh ${CMAKE_SOURCE_DIR})

#------------------------------------------------------------------------------
# Unit test subdirectories (one component per folder)
#------------------------------------------------------------------------------
add_subdirectory(chrono-common)
add_subdirectory(chrono-grapher)
add_subdirectory(chrono-keeper)
add_subdirectory(chrono-player)
add_subdirectory(chrono-visor)
add_subdirectory(chronokvs)
add_subdirectory(chronolog-client)
add_subdirectory(chronopubsub)
add_subdirectory(chronosql)
