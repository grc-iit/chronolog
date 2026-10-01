add_library(chronolog_keeper_archive STATIC KeeperArchive.cpp)
target_include_directories(chronolog_keeper_archive PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(chronolog_keeper_archive PUBLIC chronolog_keeper_lib chronolog_keeper_wal
    chronolog_internal_proto PRIVATE absl::crc32c chronolog_warnings Threads::Threads)
