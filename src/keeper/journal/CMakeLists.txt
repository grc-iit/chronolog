add_library(chronolog_keeper_journal STATIC RamJournal.cpp)
target_include_directories(chronolog_keeper_journal PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")
target_link_libraries(chronolog_keeper_journal
    PUBLIC chronolog_contracts chronolog_common_clock
    PRIVATE chronolog_warnings)
