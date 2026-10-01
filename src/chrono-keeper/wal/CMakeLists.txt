add_library(chronolog_keeper_wal STATIC FileSink.cpp Record.cpp WalJournal.cpp)
target_include_directories(chronolog_keeper_wal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(chronolog_keeper_wal PUBLIC chronolog_keeper_journal
    PRIVATE chronolog_proto absl::crc32c chronolog_warnings Threads::Threads)
