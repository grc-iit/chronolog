# ChronoKeeper: chronolog.v1.Journal and chronolog.internal.v1.Archive over a RAM journal.
# The legacy CMakeLists.txt in the parent directory is not built; sources are listed here.
set(KEEPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(chronolog_keeper_lib STATIC
    "${KEEPER_DIR}/KeeperConfig.cpp"
    "${KEEPER_DIR}/adapter/ArchiveService.cpp"
    "${KEEPER_DIR}/adapter/Convert.cpp"
    "${KEEPER_DIR}/adapter/JournalService.cpp"
    "${KEEPER_DIR}/membership/AcquisitionWatcher.cpp"
    "${KEEPER_DIR}/membership/ConfigMembership.cpp"
    "${KEEPER_DIR}/membership/RouteWatcher.cpp"
    "${KEEPER_DIR}/runtime/ClusterClient.cpp"
    "${KEEPER_DIR}/runtime/WorkerPool.cpp")
target_include_directories(chronolog_keeper_lib PUBLIC "${KEEPER_DIR}")
target_link_libraries(chronolog_keeper_lib
    PUBLIC chronolog_keeper_wal chronolog_keeper_journal chronolog_common_clock chronolog_contracts
           chronolog_common_rpc chronolog_internal_proto chronolog_proto nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE chronolog_warnings)

add_executable(chrono_keeper "${KEEPER_DIR}/main.cpp")
target_link_libraries(chrono_keeper PRIVATE chronolog_keeper_archive chronolog_keeper_lib chronolog_warnings)
set_target_properties(chrono_keeper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/chrono-keeper")
install(TARGETS chrono_keeper RUNTIME DESTINATION bin)
