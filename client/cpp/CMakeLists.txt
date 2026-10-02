include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(chronolog_client_objects OBJECT lib/client.cpp lib/writer.cpp lib/streams.cpp lib/clock.cpp)
target_include_directories(chronolog_client_objects PRIVATE include "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(chronolog_client_objects PRIVATE chronolog_proto absl::status absl::statusor chronolog_warnings Threads::Threads)

add_library(chronolog_client SHARED $<TARGET_OBJECTS:chronolog_client_objects>)
add_library(chronolog_client_static STATIC $<TARGET_OBJECTS:chronolog_client_objects>)
add_library(chronolog::client ALIAS chronolog_client)
add_library(chronolog::client_static ALIAS chronolog_client_static)
foreach(target chronolog_client chronolog_client_static)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    target_link_libraries(${target} PUBLIC absl::status absl::statusor PRIVATE chronolog_proto Threads::Threads)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME chronolog_client)
endforeach()
set_target_properties(chronolog_client PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 4 EXPORT_NAME client)
set_target_properties(chronolog_client_static PROPERTIES EXPORT_NAME client_static)
set_target_properties(chronolog_proto PROPERTIES EXPORT_NAME client_wire)
install(TARGETS chronolog_client chronolog_client_static chronolog_proto EXPORT chronologClientTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT client
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT client
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT client)
install(DIRECTORY include/chronolog/client DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/chronolog COMPONENT client)
install(FILES "${PROJECT_SOURCE_DIR}/include/chronolog/types.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/chronolog COMPONENT client)
configure_package_config_file(cmake/chronologConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/chronologConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chronolog)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/chronologConfigVersion.cmake" VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/chronologConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/chronologConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chronolog COMPONENT client)
install(EXPORT chronologClientTargets NAMESPACE chronolog:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chronolog COMPONENT client)
configure_file(cmake/chronolog.pc.in "${CMAKE_CURRENT_BINARY_DIR}/chronolog.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/chronolog.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT client)

include(GoogleTest)
add_executable(chronolog_client_tests tests/clock_test.cpp tests/identity_test.cpp tests/physical_read_test.cpp
    "${PROJECT_SOURCE_DIR}/tests/contract/client/client_test.cpp")
target_link_libraries(chronolog_client_tests PRIVATE chronolog::client chronolog_proto chronolog_warnings GTest::gtest_main Threads::Threads)
gtest_discover_tests(chronolog_client_tests TEST_PREFIX "sdk." DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
add_executable(chronolog_client_acceptance tests/acceptance.cpp)
target_link_libraries(chronolog_client_acceptance PRIVATE chronolog::client chronolog_warnings)
if(TARGET chrono_visor AND TARGET chrono_keeper AND TARGET chrono_player)
    add_test(NAME sdk.acceptance COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance.sh"
        $<TARGET_FILE:chrono_visor> $<TARGET_FILE:chrono_keeper> $<TARGET_FILE:chrono_player> $<TARGET_FILE:chronolog_client_acceptance>)
    set_tests_properties(sdk.acceptance PROPERTIES TIMEOUT 90)
endif()
# The installed-package check builds an uninstrumented consumer, so it runs only in unsanitized builds.
if(NOT CHRONOLOG_SANITIZERS)
    add_test(NAME sdk.install COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/install.sh"
        "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    set_tests_properties(sdk.install PROPERTIES TIMEOUT 180)
endif()
